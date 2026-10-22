#!/usr/bin/env python3
"""End-to-end bridge test: plays Blender's side with the add-on's real protocol
module against a running `patina --headless` process.

  PATINA_BIN=build/patina python3 tests/blender/test_live_bridge.py
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402  (bpy-free module of the add-on)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def cube_mesh():
    """A Blender-style cube: Z up, 8 verts, 6 quads, 12 edges, cross-layout UVs."""
    v = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1), (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
    faces = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    cells = [(1, 0), (1, 2), (1, 1), (2, 1), (3, 1), (0, 1)]
    positions = [c for p in v for c in p]
    face_sizes = [4] * 6
    loop_verts = [i for f in faces for i in f]
    uvs = []
    for (cx, cy) in cells:
        for (dx, dy) in [(0, 0), (1, 0), (1, 1), (0, 1)]:
            uvs += [(cx + 0.05 + dx * 0.9) / 4.0, (cy + 0.05 + dy * 0.9) / 3.0]
    edges = []
    seen = set()
    for f in faces:
        for a, b in zip(f, f[1:] + f[:1]):
            k = (min(a, b), max(a, b))
            if k not in seen:
                seen.add(k)
                edges += list(k)
    E = len(edges) // 2
    return dict(positions=positions, face_sizes=face_sizes, loop_verts=loop_verts, loop_uvs=uvs, edges=edges,
                edge_seams=[0] * E, edge_sharp=[0] * E, face_material=[0] * 6)


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    assert head[:8] == b"\x89PNG\r\n\x1a\n", path
    return struct.unpack(">II", head[16:24])


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    exe = os.path.abspath(exe)
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    patina_port, blender_port = free_port(), free_port()
    tmp = tempfile.mkdtemp(prefix="patina_bridge_test_")
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(tmp, "cache"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"))
    proc = subprocess.Popen([exe, "--headless", "--port", str(patina_port), "--resolution", "256"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    received = []
    got = threading.Event()
    retopo_msgs = []
    got_retopo = threading.Event()

    def handler(cmd, args, msg):
        if cmd == "patina_retopo_mesh":
            retopo_msgs.append(args)
            got_retopo.set()
            return {"queued": True, "object": args.get("name")}
        if cmd == "patina_textures_updated":
            received.append(args)
            got.set()
            return {"queued": True, "object": args.get("object")}
        if cmd == "ping":
            return {"app": "blender-test"}
        raise protocol.CommandError("unknown command " + cmd)

    server = protocol.PollingLineServer(handler, port=blender_port)
    server.start()
    stop = threading.Event()

    def pump():
        while not stop.is_set():
            server.poll()
            time.sleep(0.02)

    threading.Thread(target=pump, daemon=True).start()
    failures = 0

    def check(cond, what):
        nonlocal failures
        print(("  ok   " if cond else "  FAIL ") + what)
        failures += 0 if cond else 1

    try:
        client = protocol.JsonLineClient(port=patina_port, connect_timeout=1.0, response_timeout=120.0)
        for _ in range(100):
            try:
                client.connect()
                break
            except Exception:
                time.sleep(0.1)
        check(client.request("ping").get("app") == "patina", "ping")

        exchange = os.path.join(tmp, "bridge")
        obj = "Test Cube"
        mesh_path = os.path.join(exchange, obj, obj + ".ptmesh")
        os.makedirs(os.path.dirname(mesh_path), exist_ok=True)
        m = cube_mesh()
        protocol.write_ptmesh(mesh_path, obj, m.pop("positions"), m.pop("face_sizes"), m.pop("loop_verts"), **m,
                              extra_header={"had_uvs": True, "source": "test"})
        r = client.request("bridge_open_mesh", {"path": mesh_path, "object": obj, "blender_port": blender_port,
                                                "exchange_dir": exchange, "resolution": 256, "keep_layers": True})
        check(r.get("faces") == 6 and r.get("islands") == 6, "bridge_open_mesh: 6 faces, 6 islands")

        # paint something and pull (no UV edits yet -> no uv file)
        client.request("set_paint_material", {"id": "rust_heavy"})
        client.request("fill", {"mode": "mesh"})
        res = client.request("bridge_export", {"object": obj})
        check(res.get("object") == obj, "bridge_export echoes the object name")
        check(res.get("uv_path") is None, "no UV file before UV edits")
        tex = res.get("textures", {})
        for key in ("basecolor", "roughness", "metallic", "normal", "height", "emission"):
            ok = key in tex and os.path.isabs(tex[key]) and os.path.exists(tex[key])
            check(ok and png_size(tex[key]) == (256, 256), "texture %s written (256x256)" % key)

        # edit UVs and seams in Patina, then push to "Blender"
        client.request("uv_seams", {"mode": "from_islands"})
        client.request("uv_unwrap", {"method": "smart"})
        client.request("send_to_blender", {})
        check(got.wait(30), "Blender listener received patina_textures_updated")
        if received:
            msg = received[-1]
            check(msg.get("object") == obj, "push names the object")
            uvp = msg.get("uv_path")
            check(bool(uvp) and os.path.exists(uvp), "push carries a .ptuv file")
            if uvp:
                uv = protocol.read_ptuv(uvp)
                check(uv.loop_count == 24, "ptuv has 24 loops (cube)")
                check(uv.edge_count == 12, "ptuv keeps Blender's 12 edges")
                vals = list(uv.loop_uvs)
                check(all(0.0 <= x <= 1.0 for x in vals), "UVs inside 0..1")
                check(sum(1 for s in uv.edge_seams if s) >= 7, "seams were sent back")
            check(os.path.exists(msg.get("textures", {}).get("basecolor", "")), "pushed basecolor exists")

        # the project is saved, closed and opened again (or Patina restarts): the edited UVs must
        # still go back with the textures, which only fit them
        proj = os.path.join(tmp, "cube.patina")
        client.request("save_project", {"path": proj})
        client.request("open_file", {"path": proj})
        res = client.request("bridge_export", {"object": obj})
        uvp = res.get("uv_path")
        check(bool(uvp) and os.path.exists(uvp), "after reopening the project, the edited UVs still go back")

        # texture sets: a cube whose two material slots use the same UV space (body / clothes) gets a
        # texture set per slot, named after the Blender materials, and the push lists them
        obj2 = "Set Cube"
        mesh2 = os.path.join(exchange, obj2, obj2 + ".ptmesh")
        os.makedirs(os.path.dirname(mesh2), exist_ok=True)
        m2 = cube_mesh()
        uvs2 = []
        for f in range(6):
            u0, u1 = (f % 3) / 3.0 + 0.02, (f % 3 + 1) / 3.0 - 0.02
            for (u, v) in [(u0, 0.1), (u1, 0.1), (u1, 0.9), (u0, 0.9)]:
                uvs2 += [u, v]
        m2["loop_uvs"] = uvs2
        m2["face_material"] = [0, 0, 0, 1, 1, 1]
        protocol.write_ptmesh(mesh2, obj2, m2.pop("positions"), m2.pop("face_sizes"), m2.pop("loop_verts"), **m2,
                              extra_header={"had_uvs": True, "source": "test", "material_names": ["Body", "Clothes"]})
        client.request("bridge_open_mesh", {"path": mesh2, "object": obj2, "blender_port": blender_port,
                                            "exchange_dir": exchange, "resolution": 256, "keep_layers": False})
        tt = client.request("texture_tiles", {})
        check(tt.get("mode") == "material_slots", "overlapping material slots get a texture set each")
        res = client.request("bridge_export", {"object": obj2})
        sets = res.get("texture_sets") or []
        check([s.get("set") for s in sets] == ["Body", "Clothes"], "texture sets named after the materials")
        check(all(os.path.exists(s.get("textures", {}).get("basecolor", "")) for s in sets), "each set has its maps")
        check(all(png_size(s["textures"]["basecolor"]) == (256, 256) for s in sets if s.get("textures")),
              "each set at the texture set resolution")
        check((res.get("tile_mode") or {}).get("material_slots") is True, "push says the sets are per material slot")
        check(not res.get("slots_changed"), "the slots Blender sent are not sent back")

        # 0.11.1: a texture set made in Patina (the top face gets one of its own) goes back to
        # Blender with each face's slot and the slots' names, for the add-on to add the slot
        r = client.request("texture_set_from_faces", {"faces": [5], "name": "Lid"})
        check(r.get("set") == "Lid" and len(r.get("tiles", [])) == 3, "the new set is a third texture set")
        res = client.request("bridge_export", {"object": obj2})
        check(res.get("slots_changed") is True and bool(res.get("uv_path")), "the push says the slots changed")
        uv = protocol.read_ptuv(res["uv_path"])
        check(uv.face_slots is not None and list(uv.face_slots) == [0, 0, 0, 1, 1, 2], "each face's slot goes back")
        check(uv.slot_names == ["Body", "Clothes", "Lid"], "with the slots' names")
        check([s.get("set") for s in res.get("texture_sets") or []] == ["Body", "Clothes", "Lid"], "and a texture set each")

        # 0.13: retopology of a linked object goes back as a new object of quads (Blender's axes)
        obj3 = "Ball"
        mesh3 = os.path.join(exchange, obj3, obj3 + ".ptmesh")
        os.makedirs(os.path.dirname(mesh3), exist_ok=True)
        import math
        seg, rings, rad = 64, 32, 0.5
        pos = [0.0, 0.0, rad]
        for r in range(1, rings):
            th = math.pi * r / rings
            for k in range(seg):
                ph = 2 * math.pi * k / seg
                pos += [rad * math.sin(th) * math.cos(ph), rad * math.sin(th) * math.sin(ph), rad * math.cos(th)]
        pos += [0.0, 0.0, -rad]
        bottom = 1 + (rings - 1) * seg
        fs, lv = [], []
        for k in range(seg):
            fs.append(3), lv.extend([0, 1 + k, 1 + (k + 1) % seg])
        for r in range(rings - 2):
            for k in range(seg):
                a, b = 1 + r * seg + k, 1 + r * seg + (k + 1) % seg
                fs.append(4), lv.extend([a, a + seg, b + seg, b])
        for k in range(seg):
            a, b = 1 + (rings - 2) * seg + k, 1 + (rings - 2) * seg + (k + 1) % seg
            fs.append(3), lv.extend([a, bottom, b])
        protocol.write_ptmesh(mesh3, obj3, pos, fs, lv, extra_header={"had_uvs": False, "source": "test"})
        client.request("bridge_open_mesh", {"path": mesh3, "object": obj3, "blender_port": blender_port,
                                            "exchange_dir": exchange, "resolution": 256, "keep_layers": False})
        client.request("retopo_auto_patches", {"count": 6})
        client.request("retopo_density", {"quads_per_m2": 800})
        b = client.request("retopo_build", {})
        check(b.get("quads", 0) > 200 and not b.get("problems"), "the ball fills with quads (%s)" % b.get("quads"))
        sent = client.request("retopo_send_to_blender", {})
        check(sent.get("object") == "Ball_retopo", "the quads go as Ball_retopo")
        check(got_retopo.wait(30), "Blender listener received patina_retopo_mesh")
        if retopo_msgs:
            msg = retopo_msgs[-1]
            check(msg.get("object") == obj3 and msg.get("name") == "Ball_retopo", "it names the linked object and the new one")
            h, blocks = protocol.read_ptmesh(msg["path"])
            sizes = list(blocks["face_sizes"].to_array(use_numpy=False))
            check(h.get("up") == "Z" and sizes and all(n == 4 for n in sizes), "a .ptmesh of quads in Blender's axes")
            p = list(blocks["positions"].to_array(use_numpy=False))
            radii = [math.sqrt(p[i] ** 2 + p[i + 1] ** 2 + p[i + 2] ** 2) for i in range(0, len(p), 3)]
            check(all(abs(r - rad) < 0.02 for r in radii), "on the ball's surface (radius %.3f..%.3f)" % (min(radii), max(radii)))
            zs = [p[i + 2] for i in range(0, len(p), 3)]
            check(max(zs) > 0.45 and min(zs) < -0.45, "Z is up, as Blender sent it")
    finally:
        stop.set()
        server.stop()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    print("%s (%d failure%s)" % ("OK" if failures == 0 else "FAILED", failures, "" if failures == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
