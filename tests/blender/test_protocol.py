"""Tests for blender/patina_bridge/protocol.py (no Blender needed).

Run with ``python3 -m pytest tests/blender`` or ``python3 tests/blender/test_protocol.py``.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
sys.path.insert(0, PKG_DIR)

import protocol  # noqa: E402  (blender/patina_bridge/protocol.py, imported standalone)

try:
    import numpy as np
except Exception:  # pragma: no cover
    np = None


# A unit quad in the XY plane: 4 verts, 1 face, 4 loops, 4 edges.
QUAD_POS = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0]
QUAD_FACE_SIZES = [4]
QUAD_LOOP_VERTS = [0, 1, 2, 3]
QUAD_UVS = [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0]
QUAD_NORMALS = [0.0, 0.0, 1.0] * 4
QUAD_EDGES = [0, 1, 1, 2, 2, 3, 3, 0]
QUAD_SEAMS = [1, 0, 0, 1]
QUAD_SHARP = [0, 0, 1, 0]
QUAD_FMAT = [2]


def hand_built_quad() -> bytes:
    """Build the quad .ptmesh byte by byte, independently of protocol.py."""
    blocks = [
        ("positions", "f32", struct.pack("<12f", *QUAD_POS)),
        ("face_sizes", "u32", struct.pack("<I", 4)),
        ("loop_verts", "u32", struct.pack("<4I", *QUAD_LOOP_VERTS)),
        ("loop_uvs", "f32", struct.pack("<8f", *QUAD_UVS)),
        ("loop_normals", "f32", struct.pack("<12f", *QUAD_NORMALS)),
        ("edges", "u32", struct.pack("<8I", *QUAD_EDGES)),
        ("edge_seams", "u8", bytes(QUAD_SEAMS)),
        ("edge_sharp", "u8", bytes(QUAD_SHARP)),
        ("face_material", "u32", struct.pack("<I", 2)),
    ]
    sizes = {"f32": 4, "u32": 4, "u8": 1}
    header = {
        "version": 1, "name": "Quad", "up": "Z",
        "vertex_count": 4, "face_count": 1, "loop_count": 4, "edge_count": 4,
        "blocks": [{"name": n, "type": t, "count": len(b) // sizes[t]} for n, t, b in blocks],
    }
    hs = json.dumps(header, separators=(",", ":"), sort_keys=True).encode()
    return b"PTMESH01" + struct.pack("<I", len(hs)) + hs + b"".join(b for _, _, b in blocks)


def quad_kwargs():
    return dict(loop_uvs=QUAD_UVS, loop_normals=QUAD_NORMALS, edges=QUAD_EDGES, edge_seams=QUAD_SEAMS,
                edge_sharp=QUAD_SHARP, face_material=QUAD_FMAT)


class ContainerTests(unittest.TestCase):
    def test_round_trip_all_types(self):
        blocks = [
            ("a", "f32", [1.5, -2.25, 3.0]),
            ("b", "u32", [0, 1, 4294967295]),
            ("c", "i32", [-1, 0, 2147483647]),
            ("d", "u8", [0, 255, 7]),
            ("empty", "f32", []),
        ]
        buf = protocol.encode_container(b"TESTMAG1", {"version": 1, "name": "x"}, blocks)
        header, out = protocol.decode_container(buf, b"TESTMAG1")
        self.assertEqual(header["name"], "x")
        self.assertEqual([b["name"] for b in header["blocks"]], ["a", "b", "c", "d", "empty"])
        self.assertEqual(list(out), ["a", "b", "c", "d", "empty"])
        self.assertEqual(out["a"].to_list(), [1.5, -2.25, 3.0])
        self.assertEqual(out["b"].to_list(), [0, 1, 4294967295])
        self.assertEqual(out["c"].to_list(), [-1, 0, 2147483647])
        self.assertEqual(out["d"].to_list(), [0, 255, 7])
        self.assertEqual(out["empty"].count, 0)

    def test_exact_byte_layout(self):
        buf = protocol.encode_container(b"PTUV0001", {"version": 1, "name": "Q", "loop_count": 2, "edge_count": 1},
                                        [("loop_uvs", "f32", [0.25, 0.5, 0.75, 1.0]), ("edge_seams", "u8", [1])])
        self.assertEqual(buf[:8], b"PTUV0001")
        (hlen,) = struct.unpack("<I", buf[8:12])
        hs = buf[12:12 + hlen]
        header = json.loads(hs.decode("utf-8"))
        # compact + sorted keys == nlohmann::json::dump()
        self.assertEqual(hs, json.dumps(header, separators=(",", ":"), sort_keys=True).encode())
        self.assertEqual(header["blocks"], [{"count": 4, "name": "loop_uvs", "type": "f32"},
                                            {"count": 1, "name": "edge_seams", "type": "u8"}])
        body = buf[12 + hlen:]
        self.assertEqual(body, struct.pack("<4f", 0.25, 0.5, 0.75, 1.0) + b"\x01")
        self.assertEqual(len(buf), 12 + hlen + 16 + 1)

    def test_little_endian_u32(self):
        buf = protocol.encode_container(b"TESTMAG1", {}, [("x", "u32", [0x01020304])])
        self.assertTrue(buf.endswith(b"\x04\x03\x02\x01"))

    def test_raw_bytes_and_numpy_inputs(self):
        raw = struct.pack("<3f", 1, 2, 3)
        b = protocol.Block("r", "f32", raw)
        self.assertEqual(b.count, 3)
        if np is not None:
            b2 = protocol.Block("n", "f32", np.array([[1, 2], [3, 4]], dtype=np.float64))
            self.assertEqual(b2.data, struct.pack("<4f", 1, 2, 3, 4))
            b3 = protocol.Block("bools", "u8", np.array([True, False, True]))
            self.assertEqual(b3.data, b"\x01\x00\x01")
            b4 = protocol.Block("ints", "u32", np.array([1, 2], dtype=np.int32))
            self.assertEqual(b4.data, struct.pack("<2I", 1, 2))

    def test_errors(self):
        good = protocol.encode_container(b"PTUV0001", {"loop_count": 1}, [("loop_uvs", "f32", [0, 0])])
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(good, b"PTMESH01")  # wrong magic
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(good[:-1])  # truncated block
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(good[:10])  # too short
        bad_json = b"PTUV0001" + struct.pack("<I", 3) + b"{x}"
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(bad_json)
        huge = b"PTUV0001" + struct.pack("<I", 1000) + b"{}"
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(huge)
        with self.assertRaises(protocol.ProtocolError):
            protocol.Block("x", "f64", [1.0])
        with self.assertRaises(protocol.ProtocolError):
            protocol.encode_container(b"SHORT", {}, [])

    def test_unknown_block_type_in_file(self):
        hs = json.dumps({"blocks": [{"name": "x", "type": "f16", "count": 1}]}).encode()
        buf = b"PTUV0001" + struct.pack("<I", len(hs)) + hs + b"\0\0"
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_container(buf)

    def test_write_container_atomic(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "sub", "x.ptuv")
            n = protocol.write_container(path, b"PTUV0001", {"loop_count": 1}, [("loop_uvs", "f32", [1, 2])])
            self.assertEqual(os.path.getsize(path), n)
            self.assertEqual(os.listdir(os.path.dirname(path)), ["x.ptuv"])  # no temp file left behind
            header, blocks = protocol.read_container(path, b"PTUV0001")
            self.assertEqual(blocks["loop_uvs"].to_list(), [1.0, 2.0])


class PtMeshTests(unittest.TestCase):
    def test_quad_matches_hand_built_bytes(self):
        buf = protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, **quad_kwargs())
        self.assertEqual(buf, hand_built_quad())

    def test_quad_block_order_and_counts(self):
        header, blocks = protocol.decode_ptmesh(hand_built_quad())
        self.assertEqual([b["name"] for b in header["blocks"]], [n for n, _ in protocol.PTMESH_BLOCK_ORDER])
        self.assertEqual((header["vertex_count"], header["face_count"], header["loop_count"], header["edge_count"]),
                         (4, 1, 4, 4))
        self.assertEqual(header["up"], "Z")
        self.assertEqual(blocks["positions"].to_list(), QUAD_POS)
        self.assertEqual(blocks["loop_verts"].to_list(), QUAD_LOOP_VERTS)
        self.assertEqual(blocks["edge_seams"].to_list(), QUAD_SEAMS)
        self.assertEqual(blocks["face_material"].to_list(), QUAD_FMAT)

    def test_minimal_and_extra_header(self):
        buf = protocol.encode_ptmesh("Tri", [0, 0, 0, 1, 0, 0, 0, 1, 0], [3], [0, 1, 2],
                                     extra_header={"had_uvs": False, "blocks": "ignored"})
        header, blocks = protocol.decode_ptmesh(buf)
        self.assertFalse(header["had_uvs"])
        self.assertEqual(list(blocks), ["positions", "face_sizes", "loop_verts"])
        self.assertEqual(header["edge_count"], 0)

    def test_validation(self):
        with self.assertRaises(protocol.ProtocolError):  # sizes don't sum to loops
            protocol.encode_ptmesh("x", QUAD_POS, [3], QUAD_LOOP_VERTS)
        with self.assertRaises(protocol.ProtocolError):  # vertex index out of range
            protocol.encode_ptmesh("x", QUAD_POS, [4], [0, 1, 2, 4])
        with self.assertRaises(protocol.ProtocolError):  # uv count
            protocol.encode_ptmesh("x", QUAD_POS, [4], QUAD_LOOP_VERTS, loop_uvs=[0.0] * 6)
        with self.assertRaises(protocol.ProtocolError):  # seams without matching edges
            protocol.encode_ptmesh("x", QUAD_POS, [4], QUAD_LOOP_VERTS, edges=QUAD_EDGES, edge_seams=[1, 0])

    def test_loop_colors(self):
        cols = [1.0, 0.0, 0.0, 1.0] * 2 + [0.0, 0.0, 1.0, 1.0] * 2
        buf = protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, **quad_kwargs(), loop_colors=cols)
        header, blocks = protocol.decode_ptmesh(buf)
        names = [b["name"] for b in header["blocks"]]
        # after the usual blocks, so older readers see the same file
        self.assertEqual(names, [n for n, _ in protocol.PTMESH_BLOCK_ORDER] + ["loop_colors"])
        self.assertEqual(blocks["loop_colors"].to_list(), cols)
        with self.assertRaises(protocol.ProtocolError):  # four values per corner
            protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, loop_colors=[1.0] * 12)

    def test_vertex_groups(self):
        buf = protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, **quad_kwargs(),
                                     vertex_groups=[("Head", [0, 1]), ("Body", [2, 3, 1])])
        header, blocks = protocol.decode_ptmesh(buf)
        self.assertEqual(header["vertex_groups"], ["Head", "Body"])
        self.assertEqual(blocks["group_sizes"].to_list(), [2, 3])
        self.assertEqual(blocks["group_verts"].to_list(), [0, 1, 2, 3, 1])
        # the usual blocks come first, in their order, so older readers see the same file
        names = [b["name"] for b in header["blocks"]]
        self.assertEqual(names[:len(protocol.PTMESH_BLOCK_ORDER)], [n for n, _ in protocol.PTMESH_BLOCK_ORDER])
        self.assertEqual(names[len(protocol.PTMESH_BLOCK_ORDER):], [n for n, _ in protocol.PTMESH_GROUP_BLOCKS])
        with self.assertRaises(protocol.ProtocolError):
            protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, vertex_groups=[("Bad", [4])])
        # no groups: no blocks, no header key
        header, blocks = protocol.decode_ptmesh(protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES,
                                                                       QUAD_LOOP_VERTS, vertex_groups=[]))
        self.assertNotIn("vertex_groups", header)
        self.assertNotIn("group_sizes", blocks)

    def test_write_ptmesh_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "Quad", "Quad.ptmesh")
            info = protocol.write_ptmesh(path, "Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, **quad_kwargs())
            with open(path, "rb") as f:
                self.assertEqual(f.read(), hand_built_quad())
            self.assertEqual(info["bytes"], len(hand_built_quad()))
            self.assertEqual(info["loop_count"], 4)

    @unittest.skipIf(np is None, "numpy not available")
    def test_numpy_inputs_identical(self):
        buf = protocol.encode_ptmesh(
            "Quad", np.array(QUAD_POS, np.float32), np.array(QUAD_FACE_SIZES, np.int32),
            np.array(QUAD_LOOP_VERTS, np.int32), loop_uvs=np.array(QUAD_UVS, np.float32),
            loop_normals=np.array(QUAD_NORMALS, np.float32), edges=np.array(QUAD_EDGES, np.int32),
            edge_seams=np.array(QUAD_SEAMS, np.bool_), edge_sharp=np.array(QUAD_SHARP, np.bool_),
            face_material=np.array(QUAD_FMAT, np.int32))
        self.assertEqual(buf, hand_built_quad())


class PtUVTests(unittest.TestCase):
    def test_round_trip(self):
        buf = protocol.encode_ptuv("Quad", QUAD_UVS, QUAD_SEAMS)
        self.assertEqual(buf[:8], b"PTUV0001")
        uv = protocol.decode_ptuv(buf)
        self.assertEqual((uv.name, uv.loop_count, uv.edge_count), ("Quad", 4, 4))
        self.assertEqual(list(uv.loop_uvs), QUAD_UVS)
        self.assertEqual([int(v) for v in uv.edge_seams], QUAD_SEAMS)

    def test_matches_cpp_writer_layout(self):
        # savePtUV(): header {version,name,loop_count,edge_count}, blocks loop_uvs then edge_seams.
        hs = json.dumps({"blocks": [{"count": 8, "name": "loop_uvs", "type": "f32"},
                                    {"count": 4, "name": "edge_seams", "type": "u8"}],
                         "edge_count": 4, "loop_count": 4, "name": "Quad", "version": 1},
                        separators=(",", ":")).encode()
        expected = b"PTUV0001" + struct.pack("<I", len(hs)) + hs + struct.pack("<8f", *QUAD_UVS) + bytes(QUAD_SEAMS)
        self.assertEqual(protocol.encode_ptuv("Quad", QUAD_UVS, QUAD_SEAMS), expected)

    def test_without_seams_and_bad_counts(self):
        uv = protocol.decode_ptuv(protocol.encode_ptuv("Q", [0.5, 0.5]))
        self.assertIsNone(uv.edge_seams)
        self.assertEqual(uv.edge_count, 0)
        hs = json.dumps({"loop_count": 3, "blocks": [{"name": "loop_uvs", "type": "f32", "count": 4}]}).encode()
        with self.assertRaises(protocol.ProtocolError):
            protocol.decode_ptuv(b"PTUV0001" + struct.pack("<I", len(hs)) + hs + bytes(16))

    def test_read_ptuv_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "q.ptuv")
            with open(p, "wb") as f:
                f.write(protocol.encode_ptuv("Q", QUAD_UVS, QUAD_SEAMS))
            self.assertEqual(protocol.read_ptuv(p).loop_count, 4)


class LineTests(unittest.TestCase):
    def test_line_buffer_partial(self):
        lb = protocol.LineBuffer()
        self.assertEqual(lb.feed(b'{"a":'), [])
        self.assertEqual(lb.feed(b'1}\r\n\n{"b"'), [b'{"a":1}'])
        self.assertEqual(lb.feed(b':2}\n'), [b'{"b":2}'])
        self.assertEqual(lb.pending, 0)

    def test_encode_parse(self):
        line = protocol.encode_line({"id": 1, "cmd": "x", "args": {"path": "a\nb ü"}})
        self.assertEqual(line.count(b"\n"), 1)
        self.assertTrue(line.endswith(b"\n"))
        self.assertEqual(protocol.parse_message(line.strip())["args"]["path"], "a\nb ü")
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_message(b"[1,2]")
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_message(b"{nope")


class _EchoServer:
    """Threaded TCP server answering every request line with {"id":..,"ok":true,"result":{"pong":1}}.

    Replies are deliberately sent in small fragments to exercise partial reads.
    Commands: "fail" -> ok:false, "slow" -> waits 1 s, "close" -> drops the connection,
    "stale" -> sends an answer for a wrong id first.
    """

    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.port = self.sock.getsockname()[1]
        self.requests = []
        self.connections = 0
        self._stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        self.sock.settimeout(0.2)
        while not self._stop:
            try:
                conn, _ = self.sock.accept()
            except (socket.timeout, OSError):
                continue
            self.connections += 1
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn):
        buf = b""
        with conn:
            while not self._stop:
                try:
                    data = conn.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    msg = json.loads(line)
                    self.requests.append(msg)
                    cmd = msg.get("cmd")
                    if cmd == "close":
                        return
                    if cmd == "slow":
                        time.sleep(1.0)
                    if cmd == "fail":
                        reply = {"id": msg.get("id"), "ok": False, "error": "nope"}
                    else:
                        reply = {"id": msg.get("id"), "ok": True, "result": {"pong": 1}}
                    out = b""
                    if cmd == "stale":
                        out += (json.dumps({"id": -5, "ok": True, "result": {"stale": 1}}) + "\n").encode()
                    out += (json.dumps(reply) + "\n").encode()
                    for i in range(0, len(out), 5):  # fragmentary writes
                        conn.sendall(out[i:i + 5])
                        time.sleep(0.001)

    def close(self):
        self._stop = True
        self.sock.close()


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.server = _EchoServer()

    def tearDown(self):
        self.server.close()

    def test_request_and_ids(self):
        with protocol.JsonLineClient(port=self.server.port, connect_timeout=3, response_timeout=60) as c:
            self.assertEqual(c.request("ping"), {"pong": 1})
            self.assertEqual(c.request("bridge_open_mesh", {"path": "/x"}), {"pong": 1})
            self.assertEqual(c.request("stale"), {"pong": 1})  # skips the answer for a foreign id
        ids = [r["id"] for r in self.server.requests]
        self.assertEqual(ids, [1, 2, 3])
        self.assertEqual(self.server.requests[0]["args"], {})
        self.assertEqual(self.server.requests[1]["args"], {"path": "/x"})
        self.assertEqual(self.server.connections, 1)  # connection reused

    def test_remote_error(self):
        with protocol.JsonLineClient(port=self.server.port) as c:
            with self.assertRaises(protocol.RemoteError) as cm:
                c.request("fail")
            self.assertIn("nope", str(cm.exception))
            self.assertEqual(c.request("ping"), {"pong": 1})

    def test_timeout(self):
        c = protocol.JsonLineClient(port=self.server.port, response_timeout=0.2)
        with self.assertRaises(protocol.BridgeConnectionError):
            c.request("slow")
        self.assertFalse(c.connected)
        self.assertEqual(c.request("ping", timeout=5), {"pong": 1})  # fresh connection afterward
        c.close()

    def test_peer_closes(self):
        c = protocol.JsonLineClient(port=self.server.port)
        with self.assertRaises(protocol.BridgeConnectionError):
            c.request("close")
        self.assertEqual(c.request("ping"), {"pong": 1})
        c.close()

    def test_request_once(self):
        self.assertEqual(protocol.request_once("ping", port=self.server.port), {"pong": 1})

    def test_connection_refused(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        t0 = time.monotonic()
        with self.assertRaises(protocol.BridgeConnectionError):
            protocol.request_once("ping", port=port, connect_timeout=3)
        self.assertLess(time.monotonic() - t0, 3.5)


class PollingServerTests(unittest.TestCase):
    """The non-blocking server used inside Blender, polled here from a loop like a timer would."""

    def setUp(self):
        self.calls = []

        def handler(cmd, args, msg):
            self.calls.append((cmd, args, threading.current_thread().name))
            if cmd == "ping":
                return {"app": "blender"}
            if cmd == "bad":
                raise protocol.CommandError("bad args")
            if cmd == "crash":
                raise RuntimeError("boom")
            return {"echo": args}

        self.srv = protocol.PollingLineServer(handler, port=0)
        self.srv.start()
        self.stop = threading.Event()
        self.poller = threading.Thread(target=self._poll, name="PollThread", daemon=True)
        self.poller.start()

    def _poll(self):
        while not self.stop.is_set():
            self.srv.poll()
            time.sleep(0.005)

    def tearDown(self):
        self.stop.set()
        self.poller.join(1)
        self.srv.stop()

    def test_requests(self):
        with protocol.JsonLineClient(port=self.srv.port) as c:
            self.assertEqual(c.request("ping"), {"app": "blender"})
            self.assertEqual(c.request("patina_textures_updated", {"object": "Quad"}), {"echo": {"object": "Quad"}})
            with self.assertRaises(protocol.RemoteError):
                c.request("bad")
            with self.assertRaises(protocol.RemoteError) as cm:
                c.request("crash")
            self.assertIn("boom", str(cm.exception))
            self.assertEqual(c.request("ping"), {"app": "blender"})
        self.assertTrue(all(t == "PollThread" for _, _, t in self.calls))  # handled on the polling thread

    def test_raw_lines_partial_and_pipelined(self):
        s = socket.create_connection(("127.0.0.1", self.srv.port), timeout=3)
        s.sendall(b'{"id":7,"cmd":"pi')
        time.sleep(0.05)
        s.sendall(b'ng"}\n{"cmd":"x","args":{"a":1}}\nnot json\n')
        buf = b""
        deadline = time.monotonic() + 3
        while buf.count(b"\n") < 3 and time.monotonic() < deadline:
            buf += s.recv(4096)
        s.close()
        replies = [json.loads(l) for l in buf.splitlines()]
        self.assertEqual(replies[0], {"id": 7, "ok": True, "result": {"app": "blender"}})
        self.assertEqual(replies[1], {"id": None, "ok": True, "result": {"echo": {"a": 1}}})
        self.assertFalse(replies[2]["ok"])

    def test_poll_never_blocks(self):
        s = socket.create_connection(("127.0.0.1", self.srv.port), timeout=3)
        s.sendall(b'{"id":1,"cmd":"pi')  # incomplete line, then silence
        self.stop.set()
        self.poller.join(1)
        t0 = time.monotonic()
        for _ in range(50):
            self.srv.poll()
        self.assertLess(time.monotonic() - t0, 0.5)
        s.close()

    def test_port_in_use(self):
        other = protocol.PollingLineServer(lambda *a: None, port=self.srv.port)
        with self.assertRaises(OSError):
            other.start()
        self.assertFalse(other.running)


class NoNumpyTests(unittest.TestCase):
    """The array-module fallback must produce identical bytes (Blender builds without numpy are rare)."""

    def setUp(self):
        self._np = protocol.np
        protocol.np = None

    def tearDown(self):
        protocol.np = self._np

    def test_quad_bytes(self):
        self.assertEqual(protocol.encode_ptmesh("Quad", QUAD_POS, QUAD_FACE_SIZES, QUAD_LOOP_VERTS, **quad_kwargs()),
                         hand_built_quad())
        header, blocks = protocol.decode_ptmesh(hand_built_quad())
        self.assertEqual(blocks["loop_uvs"].to_list(), QUAD_UVS)

    def test_ptuv(self):
        uv = protocol.decode_ptuv(protocol.encode_ptuv("Q", QUAD_UVS, QUAD_SEAMS))
        self.assertEqual(list(uv.loop_uvs), QUAD_UVS)
        self.assertEqual(list(uv.edge_seams), QUAD_SEAMS)
        with self.assertRaises(protocol.ProtocolError):
            protocol.encode_ptmesh("x", QUAD_POS, [3], QUAD_LOOP_VERTS)


def _no_bpy_imports():
    with open(os.path.join(PKG_DIR, "protocol.py"), encoding="utf-8") as f:
        src = f.read()
    return "import bpy" not in src and "from ." not in src


class StandaloneTests(unittest.TestCase):
    def test_protocol_is_standalone(self):
        self.assertTrue(_no_bpy_imports())
        self.assertFalse(hasattr(protocol, "bpy"))
        self.assertEqual(protocol.__name__, "protocol")  # imported standalone, not as a package member


if __name__ == "__main__":
    unittest.main(verbosity=2)
