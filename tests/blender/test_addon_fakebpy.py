"""Smoke test of the bpy-facing modules against a minimal fake ``bpy``.

This does not replace testing in Blender; it catches typos, import errors and
logic bugs in registration, mesh export, UV import, material building and
message dispatch. Run with ``python3 tests/blender/test_addon_fakebpy.py``.
"""

import json
import os
import socket
import sys
import tempfile
import time
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BLENDER_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "blender"))
sys.path.insert(0, HERE)

import test_protocol as tp  # noqa: E402  (quad fixtures)


# --------------------------------------------------------------------------- fake bpy


class _Coll(list):
    """List with foreach_get/foreach_set over a flat attribute, like bpy_prop_collection."""

    def __init__(self, items=(), new=None):
        super().__init__(items)
        self._new = new
        self.active = None

    def foreach_get(self, attr, buf):
        flat = []
        for item in self:
            v = getattr(item, attr)
            flat.extend(v if isinstance(v, (list, tuple)) else [v])
        assert len(flat) == len(buf), (attr, len(flat), len(buf))
        for i, v in enumerate(flat):
            buf[i] = v

    def foreach_set(self, attr, buf):
        vals = list(buf)
        width = len(vals) // max(1, len(self))
        assert width * len(self) == len(vals), (attr, len(vals), len(self))
        for i, item in enumerate(self):
            chunk = vals[i * width:(i + 1) * width]
            setattr(item, attr, chunk if width > 1 else chunk[0])

    def get(self, name, default=None):
        for item in self:
            if getattr(item, "name", None) == name:
                return item
        return default

    def new(self, *args, **kw):
        item = self._new(*args, **kw)
        self.append(item)
        return item

    def remove(self, item):
        list.remove(self, item)


def ns(**kw):
    return types.SimpleNamespace(**kw)


class FakeUVLoop:
    """One UV corner. ``uv`` is MeshUVLoop.uv (layer.data); ``vector`` is Float2AttributeValue.vector
    (layer.uv, Blender 3.5+). Both views read the same float2 attribute, as in Blender 4.x."""

    def __init__(self, uv=(0.0, 0.0)):
        self.uv = list(uv)

    @property
    def vector(self):
        return self.uv

    @vector.setter
    def vector(self, v):
        self.uv = list(v)


class FakeUVLayer:
    def __init__(self, name, n):
        self.name = name
        self.data = _Coll(FakeUVLoop() for _ in range(n))
        self.uv = self.data  # same storage, different attribute name


class FakeMesh:
    def __init__(self, with_uvs=True, legacy_normals=False):
        self.name = "QuadMesh"
        self.is_editmode = False
        p = tp.QUAD_POS
        self.vertices = _Coll(ns(co=p[i * 3:i * 3 + 3], index=i, groups=[]) for i in range(4))
        self.polygons = _Coll([ns(loop_start=0, loop_total=4, material_index=2)])
        self.loops = _Coll(ns(vertex_index=v) for v in tp.QUAD_LOOP_VERTS)
        e = tp.QUAD_EDGES
        self.edges = _Coll(ns(vertices=e[i * 2:i * 2 + 2], use_seam=bool(tp.QUAD_SEAMS[i]),
                              use_edge_sharp=bool(tp.QUAD_SHARP[i])) for i in range(4))
        if legacy_normals:  # Blender 3.6 - 4.0: calc_normals_split() + MeshLoop.normal
            self.split_calls = []
            for lp in self.loops:
                lp.normal = [0.0, 0.0, 1.0]
            self.calc_normals_split = lambda: self.split_calls.append("calc")
            self.free_normals_split = lambda: self.split_calls.append("free")
        else:  # Blender 4.1+
            self.corner_normals = _Coll(ns(vector=[0.0, 0.0, 1.0]) for _ in range(4))
        self.uv_layers = _Coll(new=self._new_uv)
        if with_uvs:
            layer = self._new_uv("UVMap")
            for i, d in enumerate(layer.data):
                d.uv = tp.QUAD_UVS[i * 2:i * 2 + 2]
            self.uv_layers.append(layer)
            self.uv_layers.active = layer
        self.materials = _Coll()
        self.updated = False

    def _new_uv(self, name="UVMap"):
        layer = FakeUVLayer(name, len(self.loops))
        if self.uv_layers.active is None:
            self.uv_layers.active = layer
        return layer

    def update(self):
        self.updated = True


class _Slot:
    def __init__(self, mats, i):
        self._mats, self._i = mats, i

    @property
    def material(self):
        return self._mats[self._i]

    @material.setter
    def material(self, m):
        self._mats[self._i] = m


class FakeObject(dict):
    """Object: identity semantics, dict for custom properties (obj["key"])."""

    __eq__ = object.__eq__
    __hash__ = object.__hash__

    def __init__(self, name, data):
        super().__init__()
        self.name, self.type, self.mode, self.data, self.modifiers = name, "MESH", "OBJECT", data, []
        self.editmode_flushes = 0

    def update_from_editmode(self):
        self.editmode_flushes += 1
        return True

    @property
    def material_slots(self):  # mirrors data.materials, like Blender (object-linked slots aside)
        return [_Slot(self.data.materials, i) for i in range(len(self.data.materials))]


class FakeSocket:
    def __init__(self, name, node, identifier=None):
        self.name = name
        self.identifier = identifier or name
        self.node = node
        self.links = []
        self.default_value = 0.0


class _Sockets(list):
    def __getitem__(self, key):
        if isinstance(key, str):
            sock = self.get(key)
            if sock is None:
                raise KeyError(key)
            return sock
        return list.__getitem__(self, key)

    def get(self, name, default=None):
        for s in self:
            if s.name == name:
                return s
        return default


NODE_SOCKETS = {
    "ShaderNodeBsdfPrincipled": (["Base Color", "Roughness", "Metallic", "Normal", "Emission Color",
                                  "Emission Strength"], ["BSDF"]),
    "ShaderNodeOutputMaterial": (["Surface", "Volume", "Displacement"], []),
    "ShaderNodeTexImage": (["Vector"], ["Color", "Alpha"]),
    "ShaderNodeUVMap": ([], ["UV"]),
    "ShaderNodeNormalMap": (["Strength", "Color"], ["Normal"]),
    "ShaderNodeDisplacement": (["Height", "Midlevel", "Scale", "Normal"], ["Displacement"]),
    # Real socket order of ShaderNodeMix in 4.x (names repeat; identifiers are unique).
    "ShaderNodeMix": ([("Factor", "Factor_Float"), ("Factor", "Factor_Vector"), ("A", "A_Float"),
                       ("B", "B_Float"), ("A", "A_Vector"), ("B", "B_Vector"), ("A", "A_Color"), ("B", "B_Color"),
                       ("A", "A_Rotation"), ("B", "B_Rotation")],
                      [("Result", "Result_Float"), ("Result", "Result_Vector"), ("Result", "Result_Color"),
                       ("Result", "Result_Rotation")]),
}


class FakeNode:
    def __init__(self, bl_idname):
        self.bl_idname = bl_idname
        self.name = bl_idname
        self.label = ""
        self.location = (0, 0)
        self.is_active_output = True
        ins, outs = NODE_SOCKETS[bl_idname]
        mk = lambda s: FakeSocket(s, self) if isinstance(s, str) else FakeSocket(s[0], self, s[1])  # noqa: E731
        self.inputs = _Sockets(mk(s) for s in ins)
        self.outputs = _Sockets(mk(s) for s in outs)
        if bl_idname == "ShaderNodeDisplacement":
            self.inputs.get("Scale").default_value = 1.0


class FakeLink:
    def __init__(self, a, b):
        self.from_socket, self.to_socket, self.from_node = a, b, a.node


class FakeLinks(list):
    def new(self, a, b):
        link = FakeLink(a, b)
        b.links.append(link)
        self.append(link)
        return link

    def remove(self, link):
        link.to_socket.links.remove(link)
        list.remove(self, link)


class FakeTree:
    def __init__(self):
        self.nodes = _Coll(new=FakeNode)
        self.links = FakeLinks()


class FakeIDCollection:
    """Looks items up by their *current* name, like bpy.data collections."""

    def __init__(self, factory):
        self._items = []
        self._factory = factory

    def __iter__(self):
        return iter(list(self._items))

    def __len__(self):
        return len(self._items)

    def get(self, name, default=None):
        return next((i for i in self._items if i.name == name), default)

    def __getitem__(self, name):
        item = self.get(name)
        if item is None:
            raise KeyError(name)
        return item

    def add(self, item):
        self._items.append(item)
        return item

    def new(self, name):
        return self.add(self._factory(name))


class FakeID(dict):
    """ID datablock: identity semantics, dict for custom properties."""

    __eq__ = object.__eq__
    __hash__ = object.__hash__

    def __init__(self, name):
        super().__init__()
        self.name = name
        self.library = None


class FakeMaterial(FakeID):
    def __init__(self, name):
        super().__init__(name)
        self.use_nodes = False
        self.node_tree = FakeTree()
        self.users = 1
        self.use_fake_user = False
        self.displacement_method = "BUMP"


class FakeColorspace:
    def __init__(self):
        self.name = "sRGB"

    @property
    def is_data(self):  # follows the color space (setting it is only a fallback in the add-on)
        return self.name == "Non-Color"


class _FakeTiles(list):
    """Image.tiles (UDIMTiles): 1001 exists from the start."""

    def __init__(self):
        super().__init__([ns(number=1001)])

    def new(self, tile_number=1002, label=""):
        t = ns(number=tile_number)
        self.append(t)
        return t


class FakeImage(FakeID):
    def __init__(self, name, filepath=""):
        super().__init__(name)
        self._filepath = filepath
        self.source = "FILE"
        self.colorspace_settings = FakeColorspace()
        self.alpha_mode = "STRAIGHT"
        self.packed_file = None
        self.reloads = 0
        self.tiles = _FakeTiles()

    @property
    def filepath(self):
        return self._filepath

    @filepath.setter
    def filepath(self, value):  # like Blender: assigning the path reloads the image
        self._filepath = value
        self.reloads += 1

    def reload(self):
        self.reloads += 1

    def unpack(self, method="USE_LOCAL"):
        assert method == "REMOVE"
        self.packed_file = None

    def update(self):
        pass


def make_fake_bpy():
    bpy = types.ModuleType("bpy")
    registered = []
    timers = {}

    class _Base:
        def report(self, level, msg):
            self.reports = getattr(self, "reports", []) + [(level, msg)]

    def prop(**kw):
        return ("prop", kw)

    bpy.props = types.ModuleType("bpy.props")
    for n in ("BoolProperty", "EnumProperty", "IntProperty", "StringProperty", "FloatProperty"):
        setattr(bpy.props, n, prop)
    bpy.types = types.ModuleType("bpy.types")
    bpy.types.Operator = type("Operator", (_Base,), {})
    bpy.types.Panel = type("Panel", (_Base,), {})
    bpy.types.AddonPreferences = type("AddonPreferences", (_Base,), {})
    bpy.types.Image = FakeImage
    bpy.utils = ns(register_class=registered.append, unregister_class=lambda c: registered.remove(c))
    bpy.app = ns(version=(4, 2, 0), version_string="4.2.0", background=False,
                 timers=ns(register=lambda fn, first_interval=0.0, persistent=False: timers.__setitem__(fn, 1),
                           unregister=lambda fn: timers.pop(fn, None),
                           is_registered=lambda fn: fn in timers),
                 handlers=ns(depsgraph_update_post=[], load_post=[], persistent=lambda fn: fn))

    images = FakeIDCollection(FakeImage)

    def load(path, check_existing=False):
        return images.add(FakeImage(os.path.basename(path), path))

    images.load = load
    bpy.data = ns(objects={}, images=images, materials=FakeIDCollection(FakeMaterial))
    bpy.path = ns(abspath=lambda p: p)
    bpy.context = ns(preferences=ns(addons={}), window_manager=ns(windows=[]), window=None,
                     evaluated_depsgraph_get=lambda: None)
    bpy.ops = ns(patina=ns(report=lambda **kw: None))
    bpy._registered = registered
    bpy._timers = timers
    return bpy


class FakeBMLoop:
    def __init__(self, vert):
        self.vert = vert
        self._data = {}

    def __getitem__(self, layer):
        return self._data.setdefault(layer, ns(uv=(0.0, 0.0)))


class _BMVerts(list):
    def index_update(self):
        for i, v in enumerate(self):
            v.index = i


class _BMUVLayers:
    def __init__(self):
        self.active = None
        self.names = []

    def new(self, name):
        self.names.append(name)
        layer = ("uv", name)
        if self.active is None:
            self.active = layer
        return layer


class FakeBMesh:
    """Edit-mode BMesh built in mesh order, like entering edit mode does."""

    def __init__(self, mesh):
        self.verts = _BMVerts(ns(index=i, co=list(mesh.vertices[i].co)) for i in range(len(mesh.vertices)))
        self.faces = []
        for poly in mesh.polygons:
            corners = mesh.loops[poly.loop_start:poly.loop_start + poly.loop_total]
            self.faces.append(ns(loops=[FakeBMLoop(self.verts[c.vertex_index]) for c in corners]))
        self.edges = [ns(seam=e.use_seam) for e in mesh.edges]
        self.loops = ns(layers=ns(uv=_BMUVLayers()))
        if mesh.uv_layers.active is not None:
            layer = ("uv", mesh.uv_layers.active.name)
            self.loops.layers.uv.active = layer
            for poly, face in zip(mesh.polygons, self.faces):  # entering edit mode copies the UVs
                for k, lp in enumerate(face.loops):
                    lp[layer].uv = tuple(mesh.uv_layers.active.data[poly.loop_start + k].uv)
        self.updates = 0


def make_fake_bmesh():
    bmesh = types.ModuleType("bmesh")

    def from_edit_mesh(mesh):
        assert mesh.is_editmode
        if getattr(mesh, "_bm", None) is None:
            mesh._bm = FakeBMesh(mesh)
        return mesh._bm

    def update_edit_mesh(mesh, loop_triangles=True, destructive=True):
        mesh._bm.updates += 1

    bmesh.from_edit_mesh = from_edit_mesh
    bmesh.update_edit_mesh = update_edit_mesh
    return bmesh


def install_fake_bpy():
    bpy = make_fake_bpy()
    sys.modules["bpy"] = bpy
    sys.modules["bpy.props"] = bpy.props
    sys.modules["bpy.types"] = bpy.types
    sys.modules["bmesh"] = make_fake_bmesh()
    return bpy


# --------------------------------------------------------------------------- tests


class FakeBpyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bpy = install_fake_bpy()
        sys.path.insert(0, BLENDER_DIR)
        import patina_bridge  # noqa: F401

        cls.pkg = patina_bridge
        from patina_bridge import materials, mesh_io, protocol, server

        cls.materials, cls.mesh_io, cls.protocol, cls.server = materials, mesh_io, protocol, server
        cls.tmp = tempfile.TemporaryDirectory()
        prefs = ns(patina_port=7860, blender_port=0, exchange_dir=cls.tmp.name, default_resolution="2048",
                   auto_start=False, use_displacement=False, apply_modifiers=False, patina_executable="patina",
                   replace_materials=True, auto_update_mesh=False, auto_update_delay=1.5)
        cls.bpy.context.preferences.addons["patina_bridge"] = ns(preferences=prefs)
        cls.prefs = prefs

    @classmethod
    def tearDownClass(cls):
        cls.server.stop_listener()
        cls.tmp.cleanup()

    def make_obj(self, name="Quad", with_uvs=True, **kw):
        obj = FakeObject(name, FakeMesh(with_uvs, **kw))
        self.bpy.data.objects[name] = obj
        return obj

    def test_register_unregister(self):
        self.pkg.register()
        names = {c.__name__ for c in self.bpy._registered}
        self.assertIn("PATINA_OT_send", names)
        self.assertIn("PATINA_PT_main", names)
        self.assertIn("PatinaBridgePreferences", names)
        for cls in self.bpy._registered:
            if cls.__name__.startswith("PATINA_OT"):
                self.assertTrue(cls.bl_idname.startswith("patina."))
        self.pkg.unregister()
        self.assertEqual(self.bpy._registered, [])

    def test_manifest(self):
        import tomllib

        with open(os.path.join(BLENDER_DIR, "patina_bridge", "blender_manifest.toml"), "rb") as f:
            m = tomllib.load(f)
        self.assertEqual(m["id"], "patina_bridge")
        self.assertEqual(m["schema_version"], "1.0.0")
        self.assertIn("network", m["permissions"])
        self.assertIn("files", m["permissions"])
        self.assertEqual(m["version"], ".".join(map(str, self.pkg.bl_info["version"])))
        # Blender 4.2 extension validation rules
        self.assertEqual(m["type"], "add-on")
        self.assertEqual(m["blender_version_min"], "4.2.0")
        self.assertEqual(m["license"], ["SPDX:GPL-3.0-or-later"])
        for key in ("name", "tagline", "maintainer"):
            self.assertIsInstance(m[key], str)
        self.assertLessEqual(len(m["tagline"]), 64)
        self.assertNotIn(m["tagline"][-1], ".!?,;:")
        for perm, reason in m["permissions"].items():
            self.assertIn(perm, {"files", "network", "clipboard", "camera", "microphone"})
            self.assertIsInstance(reason, str)
            self.assertLessEqual(len(reason), 64, perm)
            self.assertNotIn(reason[-1], ".!?,;:", perm)
        valid_tags = {"3D View", "Add Curve", "Add Mesh", "Animation", "Bake", "Camera", "Compositing",
                      "Development", "Game Engine", "Geometry Nodes", "Grease Pencil", "Import-Export",
                      "Lighting", "Material", "Modeling", "Mesh", "Node", "Object", "Paint", "Pipeline",
                      "Physics", "Render", "Rigging", "Scene", "Sculpt", "Sequencer", "System",
                      "Text Editor", "Tracking", "User Interface", "UV"}
        self.assertLessEqual(set(m.get("tags", [])), valid_tags)

    def test_export_quad_matches_protocol_bytes(self):
        obj = self.make_obj()
        info = self.mesh_io.export_object(obj, self.tmp.name)
        self.assertEqual(info["path"], os.path.join(self.tmp.name, "Quad", "Quad.ptmesh"))
        self.assertTrue(info["had_uvs"])
        header, blocks = self.protocol.read_ptmesh(info["path"])
        self.assertEqual([b["name"] for b in header["blocks"]], [n for n, _ in self.protocol.PTMESH_BLOCK_ORDER])
        _, ref = self.protocol.decode_ptmesh(tp.hand_built_quad())
        for name, blk in ref.items():
            self.assertEqual(blocks[name].data, blk.data, name)
        self.assertTrue(header["had_uvs"])

    def test_export_sends_vertex_groups(self):
        obj = self.make_obj()
        obj.vertex_groups = [ns(index=0, name="Head"), ns(index=1, name="Unused"), ns(index=2, name="Body")]
        v = obj.data.vertices
        v[0].groups = [ns(group=0, weight=1.0)]
        v[1].groups = [ns(group=0, weight=0.6), ns(group=2, weight=0.4)]
        v[2].groups = [ns(group=0, weight=0.3), ns(group=2, weight=0.9)]
        v[3].groups = [ns(group=2, weight=0.5)]
        info = self.mesh_io.export_object(obj, self.tmp.name)
        header, blocks = self.protocol.read_ptmesh(info["path"])
        self.assertEqual(header["vertex_groups"], ["Head", "Body"])  # empty groups are left out
        self.assertEqual(blocks["group_sizes"].to_list(), [2, 2])
        self.assertEqual(blocks["group_verts"].to_list(), [0, 1, 2, 3])
        # an object without groups sends neither the header key nor the blocks
        plain = self.mesh_io.export_object(self.make_obj(), self.tmp.name)
        header, blocks = self.protocol.read_ptmesh(plain["path"])
        self.assertNotIn("vertex_groups", header)
        self.assertNotIn("group_verts", blocks)

    def test_export_without_uvs(self):
        obj = self.make_obj("NoUV", with_uvs=False)
        info = self.mesh_io.export_object(obj, self.tmp.name)
        header, blocks = self.protocol.read_ptmesh(info["path"])
        self.assertFalse(header["had_uvs"])
        self.assertEqual(blocks["loop_uvs"].to_list(), [0.0] * 8)
        self.assertEqual(len(obj.data.uv_layers), 0)  # sending doesn't modify the mesh

    def test_apply_ptuv(self):
        obj = self.make_obj("UVQ", with_uvs=False)
        path = os.path.join(self.tmp.name, "uvq.ptuv")
        uvs = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        with open(path, "wb") as f:
            f.write(self.protocol.encode_ptuv("UVQ", uvs, [0, 1, 1, 0]))
        ok, msg = self.mesh_io.apply_ptuv(obj, path)
        self.assertTrue(ok, msg)
        layer = obj.data.uv_layers.active
        self.assertEqual(layer.name, "UVMap")
        got = [c for d in layer.data for c in d.uv]
        for a, b in zip(got, uvs):
            self.assertAlmostEqual(a, b, places=6)
        self.assertEqual([bool(e.use_seam) for e in obj.data.edges], [False, True, True, False])
        # mismatched loop count -> warning, nothing raised
        with open(path, "wb") as f:
            f.write(self.protocol.encode_ptuv("UVQ", [0.0] * 6))
        ok, msg = self.mesh_io.apply_ptuv(obj, path)
        self.assertFalse(ok)
        self.assertIn("3 loops", msg)

    def _write_pngs(self, name, keys):
        d = os.path.join(self.tmp.name, name)
        os.makedirs(d, exist_ok=True)
        out = {}
        for k in keys:
            p = os.path.join(d, "%s_%s.png" % (name, k))
            open(p, "wb").close()
            out[k] = p
        return out

    def test_texture_sets_made_in_patina_become_slots(self):
        # 0.6.1: Patina's "New texture set from selection" sends each face's material slot
        obj = self.make_obj("Sets")
        body = self.bpy.data.materials.add(FakeMaterial("Body"))
        obj.data.materials[:] = [body]
        obj.data.polygons[0].material_index = 0
        info = self.mesh_io.export_object(obj, self.tmp.name)
        path = os.path.join(self.tmp.name, "Sets.ptuv")
        with open(path, "wb") as f:
            f.write(self.protocol.encode_ptuv("Sets", tp.QUAD_UVS, None, [1], ["Body", "Head"]))
        uv = self.protocol.read_ptuv(path)
        self.assertEqual(list(uv.face_slots), [1])
        self.assertEqual(uv.slot_names, ["Body", "Head"])
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)
        self.assertEqual([m.name for m in obj.data.materials], ["Body", "Head"])
        self.assertIs(obj.data.materials[0], body)
        self.assertEqual(obj.data.polygons[0].material_index, 1)
        self.assertIn("Head", msg)
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])  # again: nothing new
        self.assertEqual(len(obj.data.materials), 2)
        self.assertEqual(msg, "material slots updated")
        with open(path, "wb") as f:  # another face count: left alone, and said so
            f.write(self.protocol.encode_ptuv("Sets", tp.QUAD_UVS, None, [0, 1], ["Body", "Head"]))
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertTrue(ok)
        self.assertIn("skipped", msg)
        self.assertEqual(obj.data.polygons[0].material_index, 1)
        # a .ptuv without slots (older Patina) changes no slot
        with open(path, "wb") as f:
            f.write(self.protocol.encode_ptuv("Sets", tp.QUAD_UVS))
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertEqual(obj.data.polygons[0].material_index, 1)

    def test_textures_and_material(self):
        obj = self.make_obj("Mat")
        textures = self._write_pngs("Mat", ["basecolor", "roughness", "metallic", "normal", "height", "emission",
                                            "ao"])
        textures["bogus"] = "/nonexistent/x.png"
        self.prefs.use_displacement = True
        try:
            self.assertTrue(self.server.apply_patina_result("Mat", textures, None))
        finally:
            self.prefs.use_displacement = False
        img = self.bpy.data.images["Patina_Mat_basecolor"]
        self.assertEqual(img.colorspace_settings.name, "sRGB")
        self.assertEqual(self.bpy.data.images["Patina_Mat_normal"].colorspace_settings.name, "Non-Color")
        mat = self.bpy.data.materials["Patina_Mat"]
        self.assertEqual(obj.data.materials, [mat])
        nodes = mat.node_tree.nodes
        bsdf = nodes.get("Principled BSDF")
        self.assertEqual(bsdf.inputs.get("Base Color").links[0].from_node.name, "Patina AO Multiply")
        self.assertEqual(bsdf.inputs.get("Normal").links[0].from_node.bl_idname, "ShaderNodeNormalMap")
        self.assertEqual(bsdf.inputs.get("Emission Strength").default_value, 1.0)
        out = nodes.get("Material Output")
        disp = out.inputs.get("Displacement").links[0].from_node
        self.assertEqual(disp.inputs.get("Midlevel").default_value, 0.5)
        self.assertIn("missing", self.server.state.message)  # the bogus texture is reported

        # second push: images reloaded in place, no duplicate nodes, displacement removed
        n_nodes = len(nodes)
        self.server.apply_patina_result("Mat", textures, None)
        self.assertEqual(self.bpy.data.images["Patina_Mat_basecolor"].reloads, 1)
        self.assertEqual(len(nodes), n_nodes - 1)
        self.assertEqual(out.inputs.get("Displacement").links, [])
        self.assertEqual(len(obj.data.materials), 1)

    def test_ao_multiply_uses_color_sockets(self):
        """ShaderNodeMix repeats socket names; the color sockets must be chosen by identifier."""
        self.make_obj("AO")
        textures = self._write_pngs("AO", ["basecolor", "ao"])
        self.assertTrue(self.server.apply_patina_result("AO", textures, None))
        nodes = self.bpy.data.materials["Patina_AO"].node_tree.nodes
        mix = nodes.get("Patina AO Multiply")
        self.assertEqual((mix.data_type, mix.blend_type), ("RGBA", "MULTIPLY"))
        by_id = {s.identifier: s for s in mix.inputs}
        self.assertEqual(by_id["Factor_Float"].default_value, 1.0)
        self.assertEqual(by_id["A_Color"].links[0].from_node.name, "Patina basecolor")
        self.assertEqual(by_id["B_Color"].links[0].from_node.name, "Patina ao")
        for ident in ("A_Float", "B_Float", "A_Vector", "B_Vector"):
            self.assertEqual(by_id[ident].links, [], ident)
        base = nodes.get("Principled BSDF").inputs.get("Base Color")
        self.assertEqual(base.links[0].from_socket.identifier, "Result_Color")

    def test_packed_image_and_path_change_reload_once(self):
        self.make_obj("Pk")
        textures = self._write_pngs("Pk", ["roughness"])
        self.server.apply_patina_result("Pk", textures, None)
        img = self.bpy.data.images["Patina_Pk_roughness"]
        self.assertTrue(img.colorspace_settings.is_data)
        self.assertEqual(img.alpha_mode, "NONE")
        img.packed_file = object()
        img.reloads = 0
        moved = self._write_pngs("Pk2", ["roughness"])
        self.server.apply_patina_result("Pk", moved, None)
        self.assertIsNone(img.packed_file)  # packed copy dropped, the file is used again
        self.assertEqual(img.filepath, os.path.normpath(os.path.abspath(moved["roughness"])))
        self.assertEqual(img.reloads, 1)  # the filepath assignment reloads; no second reload()

    def test_legacy_split_normals(self):
        obj = self.make_obj("Legacy", legacy_normals=True)
        info = self.mesh_io.export_object(obj, self.tmp.name)
        _, blocks = self.protocol.read_ptmesh(info["path"])
        self.assertEqual(blocks["loop_normals"].to_list(), tp.QUAD_NORMALS)
        self.assertEqual(obj.data.split_calls, ["calc", "free"])  # temporary data freed again

    def _write_ptuv(self, name, uvs, seams=None):
        path = os.path.join(self.tmp.name, "%s.ptuv" % name)
        with open(path, "wb") as f:
            f.write(self.protocol.encode_ptuv(name, uvs, seams))
        return path

    def test_apply_ptuv_refuses_reordered_corners(self):
        obj = self.make_obj("Ord")
        info = self.mesh_io.export_object(obj, self.tmp.name)
        uvs = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        path = self._write_ptuv("Ord", uvs)
        for lp, v in zip(obj.data.loops, [1, 2, 3, 0]):  # same loop count, rotated corners
            lp.vertex_index = v
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertFalse(ok)
        self.assertIn("no longer match", msg)
        self.assertEqual([c for d in obj.data.uv_layers.active.data for c in d.uv], tp.QUAD_UVS)  # untouched
        for lp, v in zip(obj.data.loops, tp.QUAD_LOOP_VERTS):
            lp.vertex_index = v
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)

    def test_apply_ptuv_refuses_evaluated_send(self):
        """After a Blender restart state.sent is empty; the .ptmesh header still knows."""
        obj = self.make_obj("Ev")
        obj.modifiers = [ns(show_viewport=True)]
        ev = FakeMesh()
        for lp, v in zip(ev.loops, [1, 2, 3, 0]):  # a modifier that changed the face corners
            lp.vertex_index = v
        obj.evaluated_get = lambda dg: ns(to_mesh=lambda **kw: ev, to_mesh_clear=lambda: None)
        info = self.mesh_io.export_object(obj, self.tmp.name, apply_modifiers=True, context=self.bpy.context)
        self.assertTrue(info["evaluated"])
        self.server.state.sent.pop("Ev", None)
        path = self._write_ptuv("Ev", [0.5] * 8)
        self.assertFalse(self.server.apply_patina_result("Ev", {}, path))
        self.assertIn("Apply Modifiers When Sending", self.server.state.message)
        self.assertEqual([c for d in obj.data.uv_layers.active.data for c in d.uv], tp.QUAD_UVS)

    def test_deform_only_modifiers_keep_the_uv_round_trip(self):
        """An Armature (or any modifier that only moves vertices) leaves every face corner in place."""
        obj = self.make_obj("Rig")
        obj.modifiers = [ns(show_viewport=True)]
        ev = FakeMesh()  # same corners, moved vertices
        obj.evaluated_get = lambda dg: ns(to_mesh=lambda **kw: ev, to_mesh_clear=lambda: None)
        info = self.mesh_io.export_object(obj, self.tmp.name, apply_modifiers=True, context=self.bpy.context)
        self.assertFalse(info["evaluated"])
        self.server.state.sent["Rig"] = info
        uvs = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        self.assertTrue(self.server.apply_patina_result("Rig", {}, self._write_ptuv("Rig", uvs)))
        for a, b in zip([c for d in obj.data.uv_layers.active.data for c in d.uv], uvs):
            self.assertAlmostEqual(a, b, places=6)

    def test_textures_wait_for_their_uvs(self):
        """Textures painted on Patina's UVs are not put on a mesh whose UVs could not be updated."""
        obj = self.make_obj("Wait")
        self.server.state.sent["Wait"] = self.mesh_io.export_object(obj, self.tmp.name)
        textures = self._write_pngs("Wait", ["basecolor"])
        bad = self._write_ptuv("Wait", [0.5] * 6)  # 3 corners: the topology changed
        self.assertFalse(self.server.apply_patina_result("Wait", textures, bad))
        self.assertIsNone(self.bpy.data.materials.get("Patina_Wait"))
        self.assertIn("left as they were", self.server.state.message)
        self.assertEqual([c for d in obj.data.uv_layers.active.data for c in d.uv], tp.QUAD_UVS)
        # Patina's UVs equal Blender's: nothing to write, the textures come through
        same = self._write_ptuv("Wait", tp.QUAD_UVS)
        self.assertTrue(self.server.apply_patina_result("Wait", textures, same))
        self.assertIsNotNone(self.bpy.data.materials.get("Patina_Wait"))
        self.assertNotIn("UVs", self.server.state.message)

    def test_uv_edits_made_in_blender_after_sending_are_kept(self):
        obj = self.make_obj("Kept")
        info = self.mesh_io.export_object(obj, self.tmp.name)
        a = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        b = [0.2, 0.2, 0.4, 0.4, 0.6, 0.6, 0.8, 0.8]
        # Patina pushes twice: the second push replaces what the first one wrote
        ok, msg = self.mesh_io.apply_ptuv(obj, self._write_ptuv("Kept", a), sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)
        ok, msg = self.mesh_io.apply_ptuv(obj, self._write_ptuv("Kept", b), sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)
        # the user moves UVs in Blender, then Patina pushes again: refused, Blender's edit stays
        obj.data.uv_layers.active.data[0].uv = [0.9, 0.9]
        ok, msg = self.mesh_io.apply_ptuv(obj, self._write_ptuv("Kept", a), sent_ptmesh=info["path"])
        self.assertFalse(ok)
        self.assertIn("changed in Blender", msg)
        self.assertEqual(obj.data.uv_layers.active.data[0].uv, [0.9, 0.9])
        # after sending again, Blender's UVs are what Patina knows: pushes work again
        info = self.mesh_io.export_object(obj, self.tmp.name)
        ok, msg = self.mesh_io.apply_ptuv(obj, self._write_ptuv("Kept", a), sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)

    def test_material_shown_on_every_slot_and_restored(self):
        obj = self.make_obj("Char")
        skin, eyes = (self.bpy.data.materials.add(FakeMaterial(n)) for n in ("Skin", "Eyes"))
        obj.data.materials[:] = [skin, eyes]
        textures = self._write_pngs("Char", ["basecolor"])
        self.assertTrue(self.server.apply_patina_result("Char", textures, None))
        mat = self.bpy.data.materials["Patina_Char"]
        self.assertEqual(list(obj.data.materials), [mat, mat])  # face assignments untouched
        self.assertTrue(skin.use_fake_user and eyes.use_fake_user)  # kept in the file
        self.assertIn("Restore Materials", self.server.state.message)
        self.server.apply_patina_result("Char", textures, None)  # second push: nothing more to do
        self.assertEqual(list(obj.data.materials), [mat, mat])
        self.assertEqual(self.materials.restore_materials(obj), 2)
        self.assertEqual(list(obj.data.materials), [skin, eyes])
        self.assertFalse(skin.use_fake_user or eyes.use_fake_user)
        self.assertFalse(self.materials.has_original_materials(obj))
        # with the preference off, the material is added as a slot of its own
        self.prefs.replace_materials = False
        try:
            self.server.apply_patina_result("Char", textures, None)
        finally:
            self.prefs.replace_materials = True
        self.assertEqual(list(obj.data.materials), [skin, eyes, mat])

    def test_texture_sets_get_a_material_per_slot(self):
        obj = self.make_obj("Char2")
        skin, cloth = (self.bpy.data.materials.add(FakeMaterial(n)) for n in ("SkinT", "ClothT"))
        obj.data.materials[:] = [skin, cloth]
        info = self.mesh_io.export_object(obj, self.tmp.name)
        header, _ = self.protocol.read_ptmesh(info["path"])
        self.assertEqual(header["material_names"], ["SkinT", "ClothT"])  # texture sets are named after them
        body = self._write_pngs("Char2", ["Char2_SkinT_basecolor", "Char2_SkinT_normal"])
        clothes = self._write_pngs("Char2c", ["Char2_ClothT_basecolor"])
        sets = [{"index": 0, "name": "SkinT", "set": "SkinT", "slot": 0, "udim": 1001,
                 "textures": {"basecolor": body["Char2_SkinT_basecolor"], "normal": body["Char2_SkinT_normal"]}},
                {"index": 1, "name": "ClothT", "set": "ClothT", "slot": 1, "udim": 1001,
                 "textures": {"basecolor": clothes["Char2_ClothT_basecolor"]}}]
        self.assertTrue(self.server.apply_patina_result("Char2", {}, None, sets, {"material_slots": True}))
        m0, m1 = self.bpy.data.materials["Patina_Char2_SkinT"], self.bpy.data.materials["Patina_Char2_ClothT"]
        self.assertEqual(list(obj.data.materials), [m0, m1])
        self.assertIn("2 texture set", self.server.state.message)
        self.assertIsNotNone(self.bpy.data.images.get("Patina_Char2_SkinT_normal"))
        self.assertTrue(skin.use_fake_user and cloth.use_fake_user)
        # sent again, the sets keep the user's names, not Patina's
        self.assertEqual(self.materials.slot_names(obj), ["SkinT", "ClothT"])
        self.assertEqual(self.materials.restore_materials(obj), 2)
        self.assertEqual(list(obj.data.materials), [skin, cloth])

    def test_udim_tiles_become_tiled_images(self):
        obj = self.make_obj("Udim")
        d = os.path.join(self.tmp.name, "Udim")
        os.makedirs(d, exist_ok=True)
        paths = {}
        for u in (1001, 1002, 1011):
            paths[u] = os.path.join(d, "Udim_basecolor.%d.png" % u)
            open(paths[u], "wb").close()
        sets = [{"index": i, "name": str(u), "slot": 0, "udim": u, "textures": {"basecolor": paths[u]}}
                for i, u in enumerate((1001, 1002, 1011))]
        self.assertTrue(self.server.apply_patina_result("Udim", {}, None, sets, {"udim": True}))
        img = self.bpy.data.images["Patina_Udim_basecolor"]
        self.assertEqual(img.source, "TILED")
        self.assertEqual(img.filepath, os.path.join(d, "Udim_basecolor.<UDIM>.png"))
        self.assertEqual(sorted(t.number for t in img.tiles), [1001, 1002, 1011])
        mat = self.bpy.data.materials["Patina_Udim"]
        self.assertEqual(list(obj.data.materials), [mat])
        # pushed again: the same image, no duplicate tiles
        self.server.apply_patina_result("Udim", {}, None, sets, {"udim": True})
        self.assertEqual(sorted(t.number for t in img.tiles), [1001, 1002, 1011])
        # a layout without 1001: the empty 1001 tile goes
        later = [dict(st, index=i) for i, st in enumerate(sets[1:])]
        self.server.apply_patina_result("Udim", {}, None, later, {"udim": True})
        self.assertEqual(sorted(t.number for t in img.tiles), [1002, 1011])
        self.assertEqual(img.filepath, os.path.join(d, "Udim_basecolor.<UDIM>.png"))

    def test_edit_mode_send_and_bmesh_uvs(self):
        obj = self.make_obj("Edit")
        obj.mode, obj.data.is_editmode = "EDIT", True
        info = self.mesh_io.export_object(obj, self.tmp.name)
        self.assertEqual(obj.editmode_flushes, 1)  # edit-mode changes flushed before reading
        uvs = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        path = self._write_ptuv("Edit", uvs, [1, 1, 0, 0])
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertTrue(ok, msg)
        bm = obj.data._bm
        layer = bm.loops.layers.uv.active
        got = [c for f in bm.faces for lp in f.loops for c in lp[layer].uv]
        for a, b in zip(got, uvs):
            self.assertAlmostEqual(a, b, places=6)
        self.assertEqual([e.seam for e in bm.edges], [True, True, False, False])
        self.assertEqual(bm.updates, 1)
        # obj.data is not written in edit mode (leaving edit mode would overwrite it anyway)
        self.assertEqual([c for d in obj.data.uv_layers.active.data for c in d.uv], tp.QUAD_UVS)
        # corners re-ordered in edit mode -> refused
        bm.faces[0].loops.append(bm.faces[0].loops.pop(0))
        ok, msg = self.mesh_io.apply_ptuv(obj, path, sent_ptmesh=info["path"])
        self.assertFalse(ok)
        # shared mesh: another object is in edit mode -> still the BMesh path
        other = FakeObject("Other", obj.data)
        self.assertTrue(self.mesh_io.in_edit_mode(other))

    def test_send_selected_objects_as_a_scene(self):
        # (0.14) Send: the active mesh first (a scene of its own unless Patina has it), the other selected ones
        # beside it; Add to the Patina Scene: all of them beside what Patina has
        from patina_bridge import operators

        body, shirt, hair = self.make_obj("Body"), self.make_obj("Shirt"), self.make_obj("Hair")
        lamp = FakeObject("Lamp", None)
        lamp.type = "LIGHT"
        ctx = ns(active_object=shirt, selected_objects=[body, lamp, shirt, hair])
        self.assertEqual([o.name for o in operators._selected_meshes(ctx)], ["Shirt", "Body", "Hair"])
        sent = []
        orig = operators.send_object

        def fake_send(o, context, update, keep_layers, quiet=False, scene="auto"):
            sent.append((o.name, scene))
            return True, "ok"

        operators.send_object = fake_send
        try:
            op = operators.PATINA_OT_send()
            op.report = lambda *a: None
            op.keep_layers = True
            self.assertEqual(op.execute(ctx), {"FINISHED"})
            self.assertEqual(sent, [("Shirt", "auto"), ("Body", "add"), ("Hair", "add")])
            sent.clear()
            op = operators.PATINA_OT_add_to_scene()
            op.report = lambda *a: None
            self.assertEqual(op.execute(ctx), {"FINISHED"})
            self.assertEqual([s for _, s in sent], ["add", "add", "add"])
        finally:
            operators.send_object = orig
        # the placement goes along: Blender's matrix_world, column by column
        body.matrix_world = [[1, 0, 0, 2], [0, 1, 0, 3], [0, 0, 1, 4], [0, 0, 0, 1]]
        self.assertEqual(operators._matrix(body)[12:15], [2.0, 3.0, 4.0])
        self.assertIsNone(operators._matrix(lamp))

    def test_auto_update_mesh_after_edits_settle(self):
        from patina_bridge import autosync, operators

        s = self.server
        obj = self.make_obj("Auto")
        calls = []
        orig = operators.send_object

        scenes = []

        def fake_send(o, context, update, keep_layers, quiet=False, scene="auto"):
            calls.append((o.name, update, keep_layers, quiet))
            scenes.append(scene)
            autosync.sent(o.name, o)  # as the real one does
            return True, "ok"

        operators.send_object = fake_send
        try:
            self.prefs.auto_update_mesh = False
            self.prefs.auto_update_delay = 1.0
            s.state.sent["Auto"] = {"path": "x"}
            self.assertFalse(autosync.note_change("Auto", now=100.0))  # switched off
            self.prefs.auto_update_mesh = True
            autosync.sent("Auto", obj)  # what Patina has
            dg = ns(updates=[ns(is_updated_geometry=True, id=obj)])
            # an update that changes nothing Patina has (selection, a mode switch): not sent
            autosync.on_depsgraph_update(None, dg)
            self.assertIn("Auto", autosync.pending())
            autosync._dirty["Auto"] = 100.0
            autosync.tick(now=101.5)
            self.assertEqual(calls, [])
            self.assertEqual(autosync.pending(), {})
            # a real edit: sent once the edits settle
            obj.data.vertices[0].co[0] += 0.25
            autosync.on_depsgraph_update(None, dg)
            autosync._dirty["Auto"] = 200.0
            autosync.tick(now=200.5)
            self.assertEqual(calls, [])  # still being edited
            autosync.tick(now=201.2)
            self.assertEqual(calls, [("Auto", True, True, True)])
            self.assertEqual(scenes, ["update"])  # (0.14) only where Patina's scene has it
            self.assertNotIn("Auto", autosync.pending())
            # Patina writes its UVs back: Blender matches Patina, nothing to send
            layer = obj.data.uv_layers.active
            layer.data[0].uv = [0.9, 0.9]
            autosync.uvs_written("Auto", obj)
            autosync.note_change("Auto", now=300.0)
            autosync.tick(now=302.0)
            self.assertEqual(len(calls), 1)
            # a UV edit made in Blender is sent
            layer.data[1].uv = [0.33, 0.66]
            autosync.note_change("Auto", now=400.0)
            autosync.tick(now=402.0)
            self.assertEqual(len(calls), 2)
            # objects that were never sent are not watched
            other = self.make_obj("NotSent")
            autosync.on_depsgraph_update(None, ns(updates=[ns(is_updated_geometry=True, id=other)]))
            self.assertNotIn("NotSent", autosync.pending())
            # edit mode reports the mesh datablock
            autosync.on_depsgraph_update(None, ns(updates=[ns(is_updated_geometry=True, id=obj.data)]))
            self.assertIn("Auto", autosync.pending())
            # material-only updates are not geometry
            autosync._dirty.clear()
            autosync.on_depsgraph_update(None, ns(updates=[ns(is_updated_geometry=False, id=obj)]))
            self.assertEqual(autosync.pending(), {})
            # Edit Mode reads the BMesh, Object Mode the mesh: the same mesh gives the same digests
            flat = autosync.mesh_digests(obj)
            obj.mode, obj.data.is_editmode = "EDIT", True
            self.assertEqual(autosync.mesh_digests(obj), flat)
            obj.mode, obj.data.is_editmode = "OBJECT", False
            obj.data._bm = None
            # a send that fails is tried again on the next change, even if the shape is the same
            autosync.sent("Auto", obj)
            autosync.send_failed("Auto")
            autosync.note_change("Auto", now=500.0)
            autosync.tick(now=502.0)
            self.assertEqual(len(calls), 3)
            # Patina not answering: changes wait
            s.state.patina_online = False
            obj.data.vertices[1].co[1] += 0.5
            autosync.note_change("Auto", now=600.0)
            autosync.tick(now=602.0)
            self.assertEqual(len(calls), 3)
            self.assertIn("Auto", autosync.pending())
            s.state.patina_online = True
            autosync.tick(now=603.0)
            self.assertEqual(len(calls), 4)
            # opening another .blend forgets what was sent (an object there may have the same name)
            autosync.on_load(None)
            self.assertNotIn("Auto", s.state.sent)
            autosync.on_depsgraph_update(None, dg)
            self.assertEqual(autosync.pending(), {})
            # the handlers come and go with the add-on
            self.pkg.register()
            self.assertIn(autosync.on_depsgraph_update, self.bpy.app.handlers.depsgraph_update_post)
            self.assertIn(autosync.on_load, self.bpy.app.handlers.load_post)
            self.pkg.unregister()
            self.assertNotIn(autosync.on_depsgraph_update, self.bpy.app.handlers.depsgraph_update_post)
            self.assertNotIn(autosync.on_load, self.bpy.app.handlers.load_post)
        finally:
            operators.send_object = orig
            self.prefs.auto_update_mesh = False
            autosync._dirty.clear()
            s.state.sent.pop("Auto", None)
            s.state.patina_online = None

    def test_select_asymmetric_vertices(self):
        from patina_bridge import operators

        obj = self.make_obj("Sym")
        cb = operators.symmetry_result_handler("Sym", True, 4)
        cb({"object": "Sym", "vertices": 4, "verdict": "asymmetric", "tolerance": 0.002, "max": 0.05,
            "beyond_vertices": [0, 1], "unmatched_vertices": [3]}, None)
        self.assertEqual([bool(getattr(v, "select", False)) for v in obj.data.vertices], [True, True, False, True])
        e = tp.QUAD_EDGES
        want = [e[i * 2] in (0, 1, 3) and e[i * 2 + 1] in (0, 1, 3) for i in range(4)]
        self.assertEqual([bool(x.select) for x in obj.data.edges], want)
        self.assertIn("3 vertices selected", self.server.state.message)
        # without the unmatched ones
        operators.symmetry_result_handler("Sym", False, 4)(
            {"object": "Sym", "vertices": 4, "beyond_vertices": [2], "unmatched_vertices": [3]}, None)
        self.assertEqual([bool(v.select) for v in obj.data.vertices], [False, False, True, False])
        # Patina has another object open, or a different vertex count: nothing changes
        operators.symmetry_result_handler("Sym", True, 4)({"object": "Other", "beyond_vertices": [0]}, None)
        self.assertEqual([bool(v.select) for v in obj.data.vertices], [False, False, True, False])
        operators.symmetry_result_handler("Sym", True, 4)({"object": "Sym", "vertices": 9, "beyond_vertices": [0]}, None)
        self.assertEqual([bool(v.select) for v in obj.data.vertices], [False, False, True, False])

    def test_background_mode_has_no_listener(self):
        s = self.server
        self.bpy.app.background = True
        try:
            self.assertFalse(s.start_listener(port=0))
            self.assertIn("background", s.state.listener_error)
            self.assertFalse(s.state.listening)
        finally:
            self.bpy.app.background = False
            s.state.listener_error = ""

    def test_unregister_clears_timers_and_launch(self):
        s = self.server
        self.pkg.register()
        self.assertIn(s._tick, self.bpy._timers)
        s.state.launch = s._Launch(ns(poll=lambda: None), "patina", 7860)
        s.queue_result("Nobody", {}, None)
        self.assertIn(s._apply_pending, self.bpy._timers)
        self.pkg.unregister()
        for fn in (s._tick, s._autostart, s._launch_tick, s._apply_pending):
            self.assertNotIn(fn, self.bpy._timers)
        self.assertFalse(s.is_launching())
        s.state.launch = None

    def test_assign_material_rules(self):
        m = self.materials
        obj = self.make_obj("Slots")
        mat = FakeMaterial("Patina_Slots")
        self.assertEqual(m.assign_material(obj, mat), "set")  # no slots
        obj.data.materials[:] = [FakeMaterial("Material.001")]
        self.assertEqual(m.assign_material(obj, mat), "replaced")  # single default slot
        self.assertEqual(m.assign_material(obj, mat), "present")
        obj.data.materials[:] = [None]
        self.assertEqual(m.assign_material(obj, mat), "replaced")  # single empty slot
        obj.data.materials[:] = [FakeMaterial("Wood")]
        self.assertEqual(m.assign_material(obj, mat), "appended")  # user material is kept
        self.assertEqual(obj.data.materials[0].name, "Wood")

    def test_incoming_dispatch_and_listener(self):
        s = self.server
        self.make_obj("Live")
        self.assertTrue(s.start_listener(port=0))
        port = s.state.listener.port
        conn = socket.create_connection(("127.0.0.1", port), timeout=3)
        msgs = [
            {"id": 1, "cmd": "ping"},
            {"id": 2, "cmd": "patina_textures_updated", "args": {"object": "Live", "textures": {}, "uv_path": None}},
            {"id": 3, "cmd": "patina_textures_updated", "args": {"object": "Ghost", "textures": {}}},
            {"id": 4, "cmd": "nope"},
        ]
        conn.sendall(b"".join(self.protocol.encode_line(m) for m in msgs))
        buf = b""
        deadline = time.monotonic() + 3
        while buf.count(b"\n") < 4 and time.monotonic() < deadline:
            s._tick()  # what the bpy timer does
            conn.settimeout(0.05)
            try:
                buf += conn.recv(65536)
            except socket.timeout:
                pass
        conn.close()
        replies = [json.loads(line) for line in buf.splitlines()]
        self.assertEqual([r["id"] for r in replies], [1, 2, 3, 4])
        self.assertEqual(replies[0]["result"]["app"], "blender")
        self.assertEqual(replies[1]["result"], {"queued": True, "object": "Live"})
        self.assertFalse(replies[2]["ok"])
        self.assertIn("Ghost", replies[2]["error"])
        self.assertFalse(replies[3]["ok"])
        self.assertIn(s._apply_pending, self.bpy._timers)  # deferred to a main-thread timer
        s._apply_pending()
        s.stop_listener()

    def test_send_command_worker_reports_unreachable(self):
        s = self.server
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        free_port = sock.getsockname()[1]
        sock.close()
        self.prefs.patina_port = free_port
        got = []
        try:
            s.send_command("ping", {}, on_done=lambda r, e: got.append((r, e)))
            deadline = time.monotonic() + 5
            while not got and time.monotonic() < deadline:
                s._tick()
                time.sleep(0.02)
        finally:
            self.prefs.patina_port = 7860
        self.assertEqual(len(got), 1)
        self.assertIsInstance(got[0][1], self.protocol.BridgeConnectionError)
        self.assertFalse(s.state.patina_online)
        self.assertIn("not reachable", s.describe_error(got[0][1]))

    def test_resolve_executable(self):
        with self.assertRaises(FileNotFoundError):
            self.server.resolve_executable("definitely-not-a-patina-binary")
        self.assertTrue(os.path.isabs(self.server.resolve_executable("python3")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
