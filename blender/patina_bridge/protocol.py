"""Patina bridge wire protocol.

Pure Python with no ``bpy`` imports and no relative imports, so it can be
tested outside Blender (``tests/blender/test_protocol.py``).

It contains:

* The binary container used by ``.ptmesh`` (Blender -> Patina) and ``.ptuv``
  (Patina -> Blender) files::

      8 bytes   magic ("PTMESH01" / "PTUV0001")
      uint32 LE length N of the JSON header
      N bytes   UTF-8 JSON header (contains "blocks": [{name, type, count}, ...])
      ...       raw little-endian blocks, in the order of header["blocks"]

  ``count`` is the number of *elements* (not bytes). Types: f32, u32, i32 (4
  bytes) and u8 (1 byte). The header is serialized compactly with sorted keys,
  which is exactly what nlohmann::json's ``dump()`` produces on the C++ side.

* Newline-delimited JSON messaging: a blocking :class:`JsonLineClient` (used
  from a worker thread inside Blender) and a :class:`PollingLineServer` that
  never blocks and is meant to be polled from Blender's main thread by a timer.

numpy is used when available (Blender always ships it); otherwise the standard
``array`` module is used.
"""

from __future__ import annotations

import array
import errno
import itertools
import json
import os
import select
import socket
import struct
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple, Union

try:  # Blender bundles numpy; plain Python may not have it.
    import numpy as np  # type: ignore
except Exception:  # pragma: no cover - depends on the environment
    np = None  # type: ignore

__all__ = [
    "PROTOCOL_VERSION", "MAGIC_PTMESH", "MAGIC_PTUV", "DEFAULT_PATINA_PORT", "DEFAULT_BLENDER_PORT",
    "ProtocolError", "RemoteError", "BridgeConnectionError", "CommandError",
    "Block", "encode_container", "decode_container", "write_container", "read_container",
    "PTMESH_BLOCK_ORDER", "PTMESH_COLOR_BLOCK", "PTMESH_UV2_BLOCK", "PTMESH_GROUP_BLOCKS", "build_ptmesh", "encode_ptmesh", "write_ptmesh", "decode_ptmesh", "read_ptmesh",
    "PtUV", "encode_ptuv", "decode_ptuv", "read_ptuv",
    "encode_line", "parse_message", "make_request", "make_response", "LineBuffer",
    "JsonLineClient", "request_once", "PollingLineServer",
]

PROTOCOL_VERSION = 1
MAGIC_PTMESH = b"PTMESH01"
MAGIC_PTUV = b"PTUV0001"
DEFAULT_PATINA_PORT = 7860
DEFAULT_BLENDER_PORT = 7861
LOCALHOST = "127.0.0.1"

#: Sanity limits so a corrupt file or a misbehaving peer cannot exhaust memory.
MAX_HEADER_BYTES = 16 * 1024 * 1024
MAX_LINE_BYTES = 64 * 1024 * 1024

# type name -> (item size, array typecode, numpy little-endian dtype)
_TYPES: Dict[str, Tuple[int, str, str]] = {
    "f32": (4, "f", "<f4"),
    "u32": (4, "I", "<u4"),
    "i32": (4, "i", "<i4"),
    "u8": (1, "B", "u1"),
}

# On every mainstream platform 'I' is 4 bytes; keep a fallback anyway.
if array.array("I").itemsize != 4:  # pragma: no cover
    _TYPES["u32"] = (4, "L" if array.array("L").itemsize == 4 else "I", "<u4")
if array.array("i").itemsize != 4:  # pragma: no cover
    _TYPES["i32"] = (4, "l" if array.array("l").itemsize == 4 else "i", "<i4")

_BIG_ENDIAN = sys.byteorder == "big"

ArrayLike = Union[bytes, bytearray, memoryview, "array.array", Sequence[Any], Any]


# --------------------------------------------------------------------------- errors


class ProtocolError(Exception):
    """Malformed container data or a malformed message."""


class RemoteError(ProtocolError):
    """The peer answered ``{"ok": false, "error": ...}``."""


class BridgeConnectionError(ProtocolError, ConnectionError):
    """The peer could not be reached, closed the connection or timed out."""


class CommandError(Exception):
    """Raised by a :class:`PollingLineServer` handler to answer ``ok: false``."""


# --------------------------------------------------------------------------- blocks


def type_size(type_name: str) -> int:
    """Return the byte size of one element of ``type_name`` (raises on unknown types)."""
    try:
        return _TYPES[type_name][0]
    except KeyError:
        raise ProtocolError("Unknown block type %r" % (type_name,)) from None


def _to_le_bytes(data: ArrayLike, type_name: str) -> Tuple[bytes, int]:
    """Convert ``data`` to little-endian bytes of ``type_name``; return (bytes, element count)."""
    type_size(type_name)  # raises ProtocolError for unknown types
    size, typecode, dtype = _TYPES[type_name]
    if isinstance(data, (bytes, bytearray, memoryview)):
        raw = bytes(data)
        if len(raw) % size:
            raise ProtocolError("Raw data length %d is not a multiple of %d (%s)" % (len(raw), size, type_name))
        return raw, len(raw) // size
    if np is not None and isinstance(data, np.ndarray):
        arr = np.ascontiguousarray(data.reshape(-1), dtype=dtype)
        return arr.tobytes(), int(arr.size)
    if isinstance(data, array.array) and data.typecode == typecode:
        arr = data
    else:
        if np is not None and not isinstance(data, array.array):
            try:
                arr_np = np.asarray(data)
            except Exception:
                arr_np = None
            if arr_np is not None and arr_np.dtype != object:
                if type_name != "f32" and arr_np.dtype.kind == "f":
                    raise ProtocolError("Float data given for integer block type %s" % type_name)
                return _to_le_bytes(arr_np, type_name)
        try:
            arr = array.array(typecode, data)  # type: ignore[arg-type]
        except (TypeError, OverflowError) as exc:
            raise ProtocolError("Cannot convert data to %s: %s" % (type_name, exc)) from None
    if _BIG_ENDIAN and size > 1:  # pragma: no cover
        arr = array.array(typecode, arr)
        arr.byteswap()
    return arr.tobytes(), len(arr)


class Block:
    """One named, typed data block of a container."""

    __slots__ = ("name", "type", "data")

    def __init__(self, name: str, type: str, data: ArrayLike):  # noqa: A002 - mirrors the file format
        if not isinstance(name, str) or not name:
            raise ProtocolError("Block name must be a non-empty string")
        type_size(type)
        self.name = name
        self.type = type
        raw, _count = _to_le_bytes(data, type)
        self.data = raw

    @classmethod
    def from_raw(cls, name: str, type: str, raw: bytes) -> "Block":  # noqa: A002
        """Create a block from bytes that are already little-endian."""
        blk = cls.__new__(cls)
        blk.name, blk.type, blk.data = name, type, bytes(raw)
        if len(blk.data) % type_size(type):
            raise ProtocolError("Block %s: byte length not a multiple of the element size" % name)
        return blk

    @property
    def count(self) -> int:
        """Number of elements in the block."""
        return len(self.data) // type_size(self.type)

    def to_array(self, use_numpy: Optional[bool] = None):
        """Return the values as a numpy array (if available) or ``array.array``."""
        size, typecode, dtype = _TYPES[self.type]
        if use_numpy is None:
            use_numpy = np is not None
        if use_numpy and np is not None:
            return np.frombuffer(self.data, dtype=dtype).astype(np.dtype(dtype).newbyteorder("="), copy=True)
        arr = array.array(typecode)
        arr.frombytes(self.data)
        if _BIG_ENDIAN and size > 1:  # pragma: no cover
            arr.byteswap()
        return arr

    def to_list(self) -> list:
        """Return the values as a Python list."""
        return list(self.to_array(use_numpy=False))

    def __repr__(self) -> str:
        return "Block(%r, %r, count=%d)" % (self.name, self.type, self.count)


BlockSpec = Union[Block, Tuple[str, str, ArrayLike]]


def _as_block(spec: BlockSpec) -> Block:
    if isinstance(spec, Block):
        return spec
    name, type_name, data = spec
    return Block(name, type_name, data)


def _dump_header(header: Dict[str, Any]) -> bytes:
    # Compact, sorted keys: byte-identical to nlohmann::json::dump().
    return json.dumps(header, separators=(",", ":"), sort_keys=True, ensure_ascii=False, allow_nan=False).encode("utf-8")


def _header_with_blocks(header: Dict[str, Any], blocks: List[Block]) -> Dict[str, Any]:
    h = dict(header)
    h["blocks"] = [{"name": b.name, "type": b.type, "count": b.count} for b in blocks]
    return h


def _check_magic(magic: bytes) -> bytes:
    if isinstance(magic, str):
        magic = magic.encode("ascii")
    if len(magic) != 8:
        raise ProtocolError("Magic must be exactly 8 bytes, got %r" % (magic,))
    return bytes(magic)


def encode_container(magic: bytes, header: Dict[str, Any], blocks: Iterable[BlockSpec]) -> bytes:
    """Serialize a container to bytes. ``header["blocks"]`` is generated from ``blocks``."""
    magic = _check_magic(magic)
    blks = [_as_block(b) for b in blocks]
    hs = _dump_header(_header_with_blocks(header, blks))
    return b"".join([magic, struct.pack("<I", len(hs)), hs] + [b.data for b in blks])


def write_container(path: str, magic: bytes, header: Dict[str, Any], blocks: Iterable[BlockSpec]) -> int:
    """Write a container atomically (temp file + rename). Returns the file size in bytes."""
    magic = _check_magic(magic)
    blks = [_as_block(b) for b in blocks]
    hs = _dump_header(_header_with_blocks(header, blks))
    directory = os.path.dirname(os.path.abspath(path))
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".tmp_", suffix=os.path.splitext(path)[1], dir=directory)
    size = 0
    try:
        with os.fdopen(fd, "wb") as f:
            for part in [magic, struct.pack("<I", len(hs)), hs] + [b.data for b in blks]:
                f.write(part)
                size += len(part)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return size


def decode_container(buf: Union[bytes, bytearray, memoryview], expected_magic: Optional[bytes] = None
                     ) -> Tuple[Dict[str, Any], Dict[str, Block]]:
    """Parse a container. Returns ``(header, {block name: Block})`` (blocks keep file order)."""
    mv = memoryview(buf).cast("B")
    if len(mv) < 12:
        raise ProtocolError("File too short (%d bytes)" % len(mv))
    magic = bytes(mv[:8])
    if expected_magic is not None and magic != _check_magic(expected_magic):
        raise ProtocolError("Bad file magic %r (expected %r)" % (magic, expected_magic))
    (hlen,) = struct.unpack_from("<I", mv, 8)
    if hlen > MAX_HEADER_BYTES or 12 + hlen > len(mv):
        raise ProtocolError("Header length %d exceeds the file size" % hlen)
    try:
        header = json.loads(bytes(mv[12:12 + hlen]).decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise ProtocolError("Bad header JSON: %s" % exc) from None
    if not isinstance(header, dict):
        raise ProtocolError("Header is not a JSON object")
    specs = header.get("blocks", [])
    if not isinstance(specs, list):
        raise ProtocolError("header['blocks'] is not a list")
    blocks: Dict[str, Block] = {}
    off = 12 + hlen
    for spec in specs:
        try:
            name, type_name, count = spec["name"], spec["type"], spec["count"]
        except (KeyError, TypeError):
            raise ProtocolError("Malformed block entry %r" % (spec,)) from None
        if not isinstance(name, str) or not isinstance(count, int) or isinstance(count, bool) or count < 0:
            raise ProtocolError("Malformed block entry %r" % (spec,))
        nbytes = count * type_size(type_name)
        if off + nbytes > len(mv):
            raise ProtocolError("Truncated block %s" % name)
        blocks[name] = Block.from_raw(name, type_name, mv[off:off + nbytes])
        off += nbytes
    return header, blocks


def read_container(path: str, expected_magic: Optional[bytes] = None) -> Tuple[Dict[str, Any], Dict[str, Block]]:
    """Read and parse a container file."""
    with open(path, "rb") as f:
        data = f.read()
    try:
        return decode_container(data, expected_magic)
    except ProtocolError as exc:
        raise ProtocolError("%s: %s" % (os.path.basename(path), exc)) from None


# --------------------------------------------------------------------------- .ptmesh

PTMESH_BLOCK_ORDER = (
    ("positions", "f32"),
    ("face_sizes", "u32"),
    ("loop_verts", "u32"),
    ("loop_uvs", "f32"),
    ("loop_normals", "f32"),
    ("edges", "u32"),
    ("edge_seams", "u8"),
    ("edge_sharp", "u8"),
    ("face_material", "u32"),
)
# Optional (add-on 0.10, Patina 0.32): the active color attribute per face corner, linear RGBA, after the usual
# blocks. Older Patina versions ignore it.
PTMESH_COLOR_BLOCK = ("loop_colors", "f32")
# Optional (add-on 0.11, Patina 0.35): a second UV map (lightmaps), after the colors.
PTMESH_UV2_BLOCK = ("loop_uvs2", "f32")
_REQUIRED_PTMESH = ("positions", "face_sizes", "loop_verts")
# Optional (Patina 0.11): vertex groups. Their names go in the header ("vertex_groups"), the
# number of vertices in each in group_sizes, and the vertex indices one group after another in
# group_verts. Older Patina versions ignore both blocks.
PTMESH_GROUP_BLOCKS = (("group_sizes", "u32"), ("group_verts", "u32"))


def _sum_and_max(block: Block) -> Tuple[int, int]:
    vals = block.to_array()
    if block.count == 0:
        return 0, -1
    if np is not None and isinstance(vals, np.ndarray):
        return int(vals.sum(dtype=np.uint64)), int(vals.max())
    return sum(vals), max(vals)


def build_ptmesh(name: str, positions: ArrayLike, face_sizes: ArrayLike, loop_verts: ArrayLike, *,
                 loop_uvs: Optional[ArrayLike] = None, loop_normals: Optional[ArrayLike] = None,
                 edges: Optional[ArrayLike] = None, edge_seams: Optional[ArrayLike] = None,
                 edge_sharp: Optional[ArrayLike] = None, face_material: Optional[ArrayLike] = None,
                 loop_colors: Optional[ArrayLike] = None, loop_uvs2: Optional[ArrayLike] = None,
                 vertex_groups: Optional[List[Tuple[str, ArrayLike]]] = None,
                 face_sets: Optional[ArrayLike] = None,
                 extra_header: Optional[Dict[str, Any]] = None) -> Tuple[Dict[str, Any], List[Block]]:
    """Validate mesh arrays and return ``(header, blocks)`` for a ``.ptmesh`` container.

    All arrays are flat (``positions`` is V*3 floats, ``edges`` E*2 indices, ...).
    ``vertex_groups`` is a list of ``(name, vertex indices)``. ``face_sets`` (0.14): Blender's sculpt face set of
    each face (0: none), for Patina 1.2's face sets.
    """
    given = {
        "positions": positions, "face_sizes": face_sizes, "loop_verts": loop_verts,
        "loop_uvs": loop_uvs, "loop_normals": loop_normals, "edges": edges,
        "edge_seams": edge_seams, "edge_sharp": edge_sharp, "face_material": face_material,
        "loop_colors": loop_colors, "loop_uvs2": loop_uvs2,
    }
    blocks = [Block(n, t, given[n]) for n, t in PTMESH_BLOCK_ORDER + (PTMESH_COLOR_BLOCK, PTMESH_UV2_BLOCK)
              if given[n] is not None]
    by_name = {b.name: b for b in blocks}

    pos = by_name["positions"]
    if pos.count % 3:
        raise ProtocolError("positions length %d is not a multiple of 3" % pos.count)
    V, F, L = pos.count // 3, by_name["face_sizes"].count, by_name["loop_verts"].count
    total, _ = _sum_and_max(by_name["face_sizes"])
    if total != L:
        raise ProtocolError("face_sizes sum to %d but there are %d loops" % (total, L))
    _, vmax = _sum_and_max(by_name["loop_verts"])
    if vmax >= V:
        raise ProtocolError("loop_verts references vertex %d but there are only %d vertices" % (vmax, V))

    E = 0
    if "edges" in by_name:
        if by_name["edges"].count % 2:
            raise ProtocolError("edges length is not a multiple of 2")
        E = by_name["edges"].count // 2
    expected = {"loop_uvs": 2 * L, "loop_normals": 3 * L, "edge_seams": E, "edge_sharp": E, "face_material": F,
                "loop_colors": 4 * L, "loop_uvs2": 2 * L}
    for key, n in expected.items():
        if key in by_name and by_name[key].count != n:
            raise ProtocolError("%s has %d values, expected %d" % (key, by_name[key].count, n))
    if E == 0 and ("edge_seams" in by_name or "edge_sharp" in by_name):
        if by_name.get("edge_seams", by_name.get("edge_sharp")).count:
            raise ProtocolError("edge flags given without edges")

    header: Dict[str, Any] = {
        "version": PROTOCOL_VERSION, "name": str(name), "up": "Z",
        "vertex_count": V, "face_count": F, "loop_count": L, "edge_count": E,
    }
    if vertex_groups:
        names, sizes, verts = [], [], []
        for gname, idx in vertex_groups:
            idx = [int(i) for i in idx]
            if any(i < 0 or i >= V for i in idx):
                raise ProtocolError("vertex group %r references a vertex that does not exist" % gname)
            names.append(str(gname))
            sizes.append(len(idx))
            verts.extend(idx)
        header["vertex_groups"] = names
        blocks.append(Block("group_sizes", "u32", sizes))
        blocks.append(Block("group_verts", "u32", verts))
    if face_sets is not None:
        b = Block("face_sets", "u32", face_sets)
        if b.count != F:
            raise ProtocolError("face_sets has %d values, expected %d" % (b.count, F))
        blocks.append(b)
    if extra_header:
        for k, v in extra_header.items():
            if k != "blocks":
                header.setdefault(k, v)
    return header, blocks


def encode_ptmesh(name: str, positions: ArrayLike, face_sizes: ArrayLike, loop_verts: ArrayLike, **kw) -> bytes:
    """Return the bytes of a ``.ptmesh`` file (see :func:`build_ptmesh` for arguments)."""
    header, blocks = build_ptmesh(name, positions, face_sizes, loop_verts, **kw)
    return encode_container(MAGIC_PTMESH, header, blocks)


def write_ptmesh(path: str, name: str, positions: ArrayLike, face_sizes: ArrayLike, loop_verts: ArrayLike,
                 **kw) -> Dict[str, Any]:
    """Write a ``.ptmesh`` file atomically and return its header (plus ``"bytes"``)."""
    header, blocks = build_ptmesh(name, positions, face_sizes, loop_verts, **kw)
    size = write_container(path, MAGIC_PTMESH, header, blocks)
    out = _header_with_blocks(header, blocks)
    out["bytes"] = size
    return out


def decode_ptmesh(buf: Union[bytes, bytearray, memoryview]) -> Tuple[Dict[str, Any], Dict[str, Block]]:
    """Parse and validate a ``.ptmesh`` buffer."""
    header, blocks = decode_container(buf, MAGIC_PTMESH)
    for req in _REQUIRED_PTMESH:
        if req not in blocks:
            raise ProtocolError("Missing block %s" % req)
    try:
        V, F, L = int(header["vertex_count"]), int(header["face_count"]), int(header["loop_count"])
    except (KeyError, TypeError, ValueError):
        raise ProtocolError("Header lacks vertex_count/face_count/loop_count") from None
    if blocks["positions"].count != 3 * V or blocks["face_sizes"].count != F or blocks["loop_verts"].count != L:
        raise ProtocolError("Block sizes do not match the header counts")
    return header, blocks


def read_ptmesh(path: str) -> Tuple[Dict[str, Any], Dict[str, Block]]:
    with open(path, "rb") as f:
        return decode_ptmesh(f.read())


# --------------------------------------------------------------------------- .ptuv


class PtUV:
    """Decoded ``.ptuv`` contents: per-loop UVs, optional per-edge seams and (texture sets made in
    Patina) optional per-face material slots with the slots' names."""

    __slots__ = ("name", "loop_count", "edge_count", "loop_uvs", "edge_seams", "face_slots", "slot_names", "header")

    def __init__(self, name: str, loop_count: int, edge_count: int, loop_uvs, edge_seams, header: Dict[str, Any],
                 face_slots=None, slot_names: Optional[List[str]] = None):
        self.name = name
        self.loop_count = loop_count
        self.edge_count = edge_count
        self.loop_uvs = loop_uvs  # flat float array, 2*loop_count
        self.edge_seams = edge_seams  # flat uint8 array of edge_count, or None
        self.face_slots = face_slots  # flat uint32 array of face_count, or None
        self.slot_names = slot_names or []
        self.header = header

    def __repr__(self) -> str:
        return "PtUV(%r, loops=%d, edges=%d, seams=%s)" % (
            self.name, self.loop_count, self.edge_count, self.edge_seams is not None)


def encode_ptuv(name: str, loop_uvs: ArrayLike, edge_seams: Optional[ArrayLike] = None,
                face_slots: Optional[ArrayLike] = None, slot_names: Optional[List[str]] = None) -> bytes:
    """Return the bytes of a ``.ptuv`` file (normally written by Patina; used for tests)."""
    uv = Block("loop_uvs", "f32", loop_uvs)
    if uv.count % 2:
        raise ProtocolError("loop_uvs length is not a multiple of 2")
    blocks = [uv]
    E = 0
    if edge_seams is not None:
        seams = Block("edge_seams", "u8", edge_seams)
        blocks.append(seams)
        E = seams.count
    header = {"version": PROTOCOL_VERSION, "name": str(name), "loop_count": uv.count // 2, "edge_count": E}
    if face_slots is not None:
        fs = Block("face_slots", "u32", face_slots)
        blocks.append(fs)
        header["face_count"] = fs.count
        header["slot_names"] = [str(n) for n in (slot_names or [])]
    return encode_container(MAGIC_PTUV, header, blocks)


def decode_ptuv(buf: Union[bytes, bytearray, memoryview]) -> PtUV:
    """Parse and validate a ``.ptuv`` buffer."""
    header, blocks = decode_container(buf, MAGIC_PTUV)
    try:
        L = int(header["loop_count"])
        E = int(header.get("edge_count", 0) or 0)
    except (KeyError, TypeError, ValueError):
        raise ProtocolError("Header lacks loop_count") from None
    uv = blocks.get("loop_uvs")
    if uv is None:
        raise ProtocolError("Missing block loop_uvs")
    if uv.type != "f32" or uv.count != 2 * L:
        raise ProtocolError("loop_uvs has %d values, expected %d" % (uv.count, 2 * L))
    seams = None
    sb = blocks.get("edge_seams")
    if sb is not None:
        if sb.type != "u8" or sb.count != E:
            raise ProtocolError("edge_seams has %d values, expected %d" % (sb.count, E))
        seams = sb.to_array()
    slots = None
    fb = blocks.get("face_slots")
    if fb is not None:
        if fb.type != "u32" or fb.count != int(header.get("face_count", -1)):
            raise ProtocolError("face_slots has %d values, expected %s" % (fb.count, header.get("face_count")))
        slots = fb.to_array()
    names = header.get("slot_names") or []
    return PtUV(str(header.get("name", "")), L, E, uv.to_array(), seams, header, slots,
                [str(n) for n in names] if isinstance(names, list) else [])


def read_ptuv(path: str) -> PtUV:
    with open(path, "rb") as f:
        data = f.read()
    try:
        return decode_ptuv(data)
    except ProtocolError as exc:
        raise ProtocolError("%s: %s" % (os.path.basename(path), exc)) from None


# --------------------------------------------------------------------------- JSON lines


def encode_line(message: Dict[str, Any]) -> bytes:
    """Serialize one message as a single UTF-8 JSON line (json.dumps never emits raw newlines)."""
    return json.dumps(message, separators=(",", ":"), ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n"


def parse_message(line: Union[bytes, str]) -> Dict[str, Any]:
    """Parse one JSON line into a dict (raises :class:`ProtocolError`)."""
    if isinstance(line, (bytes, bytearray)):
        try:
            line = bytes(line).decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ProtocolError("Message is not UTF-8: %s" % exc) from None
    try:
        msg = json.loads(line)
    except ValueError as exc:
        raise ProtocolError("Invalid JSON: %s" % exc) from None
    if not isinstance(msg, dict):
        raise ProtocolError("Message is not a JSON object")
    return msg


def make_request(msg_id: Optional[int], cmd: str, args: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    return {"id": msg_id, "cmd": cmd, "args": args if args is not None else {}}


def make_response(msg_id: Any, ok: bool = True, result: Any = None, error: Optional[str] = None) -> Dict[str, Any]:
    if ok:
        return {"id": msg_id, "ok": True, "result": result if result is not None else {}}
    return {"id": msg_id, "ok": False, "error": str(error or "error")}


class LineBuffer:
    """Accumulates stream bytes and yields complete lines (handles partial reads and CRLF)."""

    def __init__(self, max_line: int = MAX_LINE_BYTES):
        self._buf = bytearray()
        self._max = max_line

    def feed(self, data: bytes) -> List[bytes]:
        self._buf += data
        lines: List[bytes] = []
        while True:
            i = self._buf.find(b"\n")
            if i < 0:
                break
            line = bytes(self._buf[:i]).rstrip(b"\r")
            del self._buf[:i + 1]
            if line.strip():
                lines.append(line)
        if len(self._buf) > self._max:
            self._buf.clear()
            raise ProtocolError("Line exceeds %d bytes" % self._max)
        return lines

    @property
    def pending(self) -> int:
        return len(self._buf)


class JsonLineClient:
    """Blocking JSON-lines request/response client.

    Keeps one connection open and reuses it; a connection the peer has closed
    while idle is detected and transparently re-opened. Thread-safe (one request
    at a time). Inside Blender, only call it from a worker thread.
    """

    def __init__(self, host: str = LOCALHOST, port: int = DEFAULT_PATINA_PORT,
                 connect_timeout: float = 3.0, response_timeout: float = 60.0):
        self.host = host
        self.port = int(port)
        self.connect_timeout = connect_timeout
        self.response_timeout = response_timeout
        self._sock: Optional[socket.socket] = None
        self._lines = LineBuffer()
        self._ids = itertools.count(1)
        self._lock = threading.RLock()

    # -- connection management
    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self) -> None:
        with self._lock:
            if self._sock is not None:
                return
            try:
                sock = socket.create_connection((self.host, self.port), timeout=self.connect_timeout)
            except socket.timeout:
                raise BridgeConnectionError("Timed out connecting to %s:%d" % (self.host, self.port)) from None
            except OSError as exc:
                raise BridgeConnectionError("Cannot connect to %s:%d (%s)" % (
                    self.host, self.port, exc.strerror or exc)) from None
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            self._sock = sock
            self._lines = LineBuffer()

    def close(self) -> None:
        with self._lock:
            if self._sock is not None:
                try:
                    self._sock.close()
                except OSError:
                    pass
            self._sock = None

    def __enter__(self) -> "JsonLineClient":
        self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _is_stale(self) -> bool:
        """True if the idle connection was closed by the peer (EOF or error pending)."""
        sock = self._sock
        if sock is None:
            return True
        try:
            readable, _, errored = select.select([sock], [], [sock], 0)
        except (OSError, ValueError):
            return True
        if errored:
            return True
        if not readable:
            return False
        try:
            sock.settimeout(0)
            peek = sock.recv(1, socket.MSG_PEEK)
        except (BlockingIOError, InterruptedError):
            return False
        except OSError:
            return True
        return peek == b""  # unsolicited data is left for the response reader to skip

    def next_id(self) -> int:
        return next(self._ids)

    # -- requests
    def request(self, cmd: str, args: Optional[Dict[str, Any]] = None, timeout: Optional[float] = None) -> Any:
        """Send ``cmd`` and return the ``result`` of the matching response.

        Raises :class:`RemoteError` for ``ok: false``, :class:`BridgeConnectionError`
        for network problems and :class:`ProtocolError` for garbage replies.
        """
        with self._lock:
            msg_id = self.next_id()
            payload = encode_line(make_request(msg_id, cmd, args))
            reused = self._sock is not None
            if reused and self._is_stale():
                self.close()
                reused = False
            self.connect()
            try:
                self._send(payload)
            except BridgeConnectionError:
                self.close()
                if not reused:
                    raise
                self.connect()  # stale keep-alive connection: retry once on a fresh one
                self._send(payload)
            try:
                return self._await(msg_id, self.response_timeout if timeout is None else timeout)
            except (BridgeConnectionError, ProtocolError):
                self.close()  # stream position unknown after a failure; start fresh next time
                raise

    def _send(self, payload: bytes) -> None:
        assert self._sock is not None
        try:
            self._sock.settimeout(self.connect_timeout + 10.0)
            self._sock.sendall(payload)
        except OSError as exc:
            raise BridgeConnectionError("Send to %s:%d failed (%s)" % (self.host, self.port, exc)) from None

    def _await(self, msg_id: int, timeout: float) -> Any:
        deadline = time.monotonic() + timeout
        while True:
            assert self._sock is not None
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BridgeConnectionError("No response from %s:%d within %.0f s" % (self.host, self.port, timeout))
            self._sock.settimeout(remaining)
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                raise BridgeConnectionError("Connection to %s:%d failed (%s)" % (self.host, self.port, exc)) from None
            if not chunk:
                raise BridgeConnectionError("Connection closed by %s:%d before a response arrived" % (
                    self.host, self.port))
            for line in self._lines.feed(chunk):
                msg = parse_message(line)
                if msg.get("id") != msg_id and msg.get("id") is not None:
                    continue  # a late answer to an earlier (timed-out) request
                if msg.get("ok") is True:
                    return msg.get("result")
                if msg.get("ok") is False:
                    raise RemoteError(str(msg.get("error") or "unknown error"))
                raise ProtocolError("Response without 'ok': %r" % (msg,))


def request_once(cmd: str, args: Optional[Dict[str, Any]] = None, host: str = LOCALHOST,
                 port: int = DEFAULT_PATINA_PORT, connect_timeout: float = 3.0, timeout: float = 60.0) -> Any:
    """Open a connection, send one request, return its result and close."""
    with JsonLineClient(host, port, connect_timeout, timeout) as client:
        return client.request(cmd, args)


# --------------------------------------------------------------------------- polling server

Handler = Callable[[str, Dict[str, Any], Dict[str, Any]], Any]


class _Conn:
    __slots__ = ("sock", "addr", "lines", "out", "closing")

    def __init__(self, sock: socket.socket, addr):
        self.sock = sock
        self.addr = addr
        self.lines = LineBuffer()
        self.out = bytearray()
        self.closing = False


class PollingLineServer:
    """Non-blocking JSON-lines server: call :meth:`poll` regularly; it never blocks.

    ``handler(cmd, args, message)`` runs inside :meth:`poll` (i.e. on the thread
    that polls - Blender's main thread). Its return value becomes ``result``;
    raising :class:`CommandError` (or any exception) produces ``ok: false``.
    """

    def __init__(self, handler: Handler, host: str = LOCALHOST, port: int = DEFAULT_BLENDER_PORT,
                 max_clients: int = 16, on_error: Optional[Callable[[str], None]] = None):
        self.handler = handler
        self.host = host
        self.requested_port = int(port)
        self.max_clients = max_clients
        self.on_error = on_error
        self._listener: Optional[socket.socket] = None
        self._conns: List[_Conn] = []
        self.messages_handled = 0

    @property
    def running(self) -> bool:
        return self._listener is not None

    @property
    def port(self) -> int:
        """The bound port (useful when started with port 0)."""
        if self._listener is None:
            return self.requested_port
        return self._listener.getsockname()[1]

    @property
    def client_count(self) -> int:
        return len(self._conns)

    def start(self) -> None:
        """Bind and listen. Raises ``OSError`` (e.g. EADDRINUSE) on failure."""
        if self._listener is not None:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if os.name == "nt":
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            else:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((self.host, self.requested_port))
            sock.listen(8)
            sock.setblocking(False)
        except OSError:
            sock.close()
            raise
        self._listener = sock

    def stop(self) -> None:
        for c in self._conns:
            self._close(c)
        self._conns = []
        if self._listener is not None:
            try:
                self._listener.close()
            except OSError:
                pass
        self._listener = None

    def _report(self, text: str) -> None:
        if self.on_error is not None:
            try:
                self.on_error(text)
            except Exception:
                pass

    @staticmethod
    def _close(c: _Conn) -> None:
        try:
            c.sock.close()
        except OSError:
            pass

    def poll(self, max_messages: int = 64) -> int:
        """Accept connections, read requests, dispatch them and flush replies. Returns messages handled."""
        if self._listener is None:
            return 0
        handled = 0
        rlist = [self._listener] + [c.sock for c in self._conns if not c.closing]
        wlist = [c.sock for c in self._conns if c.out]
        try:
            readable, writable, _ = select.select(rlist, wlist, [], 0)
        except (OSError, ValueError):
            readable, writable = rlist, wlist  # a socket died; per-socket errors are handled below
        readable_set = set(readable)

        if self._listener in readable_set:
            self._accept()

        for c in list(self._conns):
            if c.sock not in readable_set or c.closing:
                continue
            while True:
                try:
                    chunk = c.sock.recv(65536)
                except (BlockingIOError, InterruptedError):
                    break
                except OSError:
                    c.closing = True
                    c.out.clear()
                    break
                if not chunk:
                    c.closing = True  # peer finished sending; flush pending replies then close
                    break
                try:
                    lines = c.lines.feed(chunk)
                except ProtocolError as exc:
                    c.out += encode_line(make_response(None, False, error=str(exc)))
                    c.closing = True
                    break
                for line in lines:
                    c.out += self._dispatch(line)
                    handled += 1
                if handled >= max_messages:
                    break

        for c in list(self._conns):
            if c.out:
                self._flush(c)
            if c.closing and not c.out:
                self._close(c)
                self._conns.remove(c)
        self.messages_handled += handled
        return handled

    def _accept(self) -> None:
        assert self._listener is not None
        while True:
            try:
                sock, addr = self._listener.accept()
            except (BlockingIOError, InterruptedError):
                return
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    return
                self._report("accept failed: %s" % exc)
                return
            if len(self._conns) >= self.max_clients:
                sock.close()
                self._report("too many bridge connections; refusing %s" % (addr,))
                continue
            sock.setblocking(False)
            self._conns.append(_Conn(sock, addr))

    def _flush(self, c: _Conn) -> None:
        try:
            n = c.sock.send(bytes(c.out[:1 << 20]))
            del c.out[:n]
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            c.out.clear()
            c.closing = True

    def _dispatch(self, line: bytes) -> bytes:
        try:
            msg = parse_message(line)
        except ProtocolError as exc:
            return encode_line(make_response(None, False, error=str(exc)))
        msg_id = msg.get("id")
        cmd = msg.get("cmd")
        args = msg.get("args") or {}
        if not isinstance(cmd, str) or not cmd:
            return encode_line(make_response(msg_id, False, error="missing 'cmd'"))
        if not isinstance(args, dict):
            return encode_line(make_response(msg_id, False, error="'args' must be an object"))
        try:
            result = self.handler(cmd, args, msg)
            return encode_line(make_response(msg_id, True, result=result))
        except CommandError as exc:
            return encode_line(make_response(msg_id, False, error=str(exc)))
        except Exception as exc:  # never let a handler bug kill the poll loop
            self._report("handler for %r failed: %r" % (cmd, exc))
            return encode_line(make_response(msg_id, False, error="%s: %s" % (type(exc).__name__, exc)))
