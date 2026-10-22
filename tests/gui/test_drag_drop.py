#!/usr/bin/env python3
"""Drag and drop onto the layer stack in the real window (0.22.5), with synthetic mouse drags:
- a material from the Materials tab dropped in the middle of a fill layer's row replaces its material; its
  smart mask stays, a layer named after the old material takes the new one's name; one undo step;
- dropped near the top edge of a row, it makes a new fill layer above that layer; in the middle of a
  folder's row, a new fill layer in the folder; on the model in the viewport, one above the selection;
- a fill layer's thumbnail dropped on another fill layer gives it that material, its tweaks too;
- a smart material dropped under the rows goes in at the bottom of the stack;
- a smart mask dropped on a layer gives it that smart mask;
- a layer's mask dropped on another layer is copied there;
- a layer dragged with Ctrl held is copied, not moved.

    python3 tests/gui/test_drag_drop.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import layout, panel, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_drag_drop_test"), display=":96", port=7897)

    def layers():
        return s("list_layers")

    def stack():  # (0.41) without the baked map layers, which have their own section under the list
        return [l for l in layers() if l["type"] != "baked"]

    def by(uid):
        return next((l for l in layers() if l["uid"] == uid), None)

    def rect(name):
        r = s("ui_rect", name=name)
        return r if r.get("found") else None

    def center(r, fy=0.5):
        return int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] * fy)

    def drag(a, b, mods=()):
        # (a few pixels first: the drag starts; then on to the target in steps)
        pts = [a, (a[0] + 6, a[1] + 6)] + [(int(a[0] + (b[0] - a[0]) * k / 12), int(a[1] + (b[1] - a[1]) * k / 12)) for k in range(1, 13)] + [b]
        s.x.drag(pts, dt=0.06, mods=mods)
        time.sleep(0.8)

    def to_bottom():
        # (the list scrolled to its foot, where the space under the rows is)
        r = rect("layers.list")  # (0.41: over the list itself; the baked maps' section is under it)
        s.x.move(int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] / 2))
        for _ in range(10):
            s.x.button(5, True); s.x.button(5, False); time.sleep(0.03)
        time.sleep(0.6)

    def tile(prefix):
        rs = s("ui_rect", prefix=prefix)["rects"]
        return sorted(rs.items(), key=lambda kv: (kv[1]["y"], kv[1]["x"]))

    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_camera", view="front", frame=True)
        panel(s, "properties")
        panel(s, "materials")
        time.sleep(1.5)
        tiles = tile("materials.tile:")
        check(len(tiles) >= 3, "the material tiles are there (%d)" % len(tiles))
        (k0, t0), (k1, t1) = tiles[0], tiles[1]
        id0, id1 = k0.split(":", 1)[1], k1.split(":", 1)[1]
        names = {m["id"]: m["name"] for m in s("list_materials")}
        name0, name1 = names[id0], names[id1]
        r = s("add_layer", type="fill", name=name1, material={"id": id1}, generator={"type": "edges", "level": 0.4})
        fill = r["uid"]
        s("update_layer", index=r["index"], material={"tint": [1.0, 0.5, 0.5]})
        paint = s("add_layer", type="paint", name="Paint")["uid"]
        folder = s("add_layer", type="folder", name="Folder")["uid"]
        time.sleep(1.0)
        n0 = len(layers())

        # a material onto the middle of the fill layer: replaced, the smart mask stays
        drag(center(t0), center(rect("layers.row:%d" % fill)))
        f = by(fill)
        check(f["material"]["id"] == id0 and len(layers()) == n0, "a material dropped on a fill layer replaces its material (%s)" % f["material"]["id"])
        check(f["generator"]["type"] == "edges", "its smart mask stays (%s)" % f["generator"]["type"])
        check(f["name"] == name0, "named after the new material, as it was after the old one (%s)" % f["name"])
        check(abs(f["material"]["tint"][1] - 0.5) < 1e-3, "its tint stays")
        s.x.key("Control_L", True); s.x.key("z"); s.x.key("Control_L", False)
        time.sleep(0.8)
        check(by(fill)["material"]["id"] == id1, "one undo step puts the old material back")

        # near the top edge of the paint layer: a new fill layer above it
        drag(center(t0), center(rect("layers.row:%d" % paint), 0.1))
        L = layers()
        pi = next(i for i, l in enumerate(L) if l["uid"] == paint)
        check(len(L) == n0 + 1 and pi + 1 < len(L) and L[pi + 1]["type"] == "fill" and L[pi + 1]["material"]["id"] == id0,
              "near a row's top edge: a new fill layer above that layer")
        # in the middle of the folder's row: in the folder
        drag(center(t1), center(rect("layers.row:%d" % folder)))
        L = layers()
        check(len(L) == n0 + 2 and any(l["parent"] == folder and l["type"] == "fill" and l["material"]["id"] == id1 for l in L),
              "in the middle of a folder's row: a new fill layer in the folder")
        # on the model: above the selection
        s("select_layer", index=next(i for i, l in enumerate(layers()) if l["uid"] == paint))
        time.sleep(0.5)
        drag(center(t1), vp(0.5, 0.5))
        L = layers()
        pi = next(i for i, l in enumerate(L) if l["uid"] == paint)
        check(len(L) == n0 + 3 and L[pi + 1]["type"] == "fill" and L[pi + 1]["material"]["id"] == id1, "on the model: a new fill layer above the selected layer")

        # a fill layer's thumbnail onto another fill layer: that material, its tweaks too
        other = L[pi + 1]["uid"]
        time.sleep(0.8)
        c = rect("layers.content:%d" % fill)
        drag(center(c), center(rect("layers.row:%d" % other)))
        o = by(other)
        check(o["material"]["id"] == id1 and abs(o["material"]["tint"][1] - 0.5) < 1e-3, "a fill layer's material dragged onto another, with its tint")

        # a layer's mask onto another layer: copied
        m = rect("layers.mask:%d" % fill)
        check(m is not None, "the fill layer's mask is on its row")
        if m:
            drag(center(m), center(rect("layers.row:%d" % paint)))
            check(by(paint)["generator"]["type"] == "edges", "a mask dragged onto another layer is copied there (%s)" % by(paint)["generator"]["type"])

        # Ctrl+drag a layer onto the bottom: a copy
        n = len(stack())
        name_r = rect("layers.row:%d" % other)
        to_bottom()
        name_r = rect("layers.row:%d" % other)
        drag((int(name_r["x"] + name_r["w"] * 0.55), int(name_r["y"] + name_r["h"] / 2)), center(rect("layers.bottom"), 0.5), mods=("Control_L",))
        L = stack()
        check(len(L) == n + 1 and L[0]["type"] == "fill" and L[0]["material"]["id"] == id1 and by(other) is not None,
              "a layer dragged with Ctrl is copied (%d layers, was %d)" % (len(L), n))

        # smart masks: onto the paint layer
        s.x.click(*center(rect("materials.tab:masks")))
        time.sleep(0.8)
        drag(center(rect("smartmask.tile:cavity")), center(rect("layers.row:%d" % paint)))
        check(by(paint)["generator"]["type"] == "cavity", "a smart mask dropped on a layer gives it that smart mask (%s)" % by(paint)["generator"]["type"])

        # smart materials: under the rows = the bottom of the stack
        s.x.click(*center(rect("materials.tab:smart")))
        time.sleep(1.2)
        smart = tile("smart.tile:")
        check(len(smart) >= 3, "the smart material tiles are there (%d)" % len(smart))
        n = len(stack())
        before = set(l["uid"] for l in layers())
        to_bottom()
        drag(center(smart[0][1]), center(rect("layers.bottom"), 0.5))
        L = stack()
        new = [l["uid"] for l in L if l["uid"] not in before]
        check(len(L) > n and [l["uid"] for l in L[:len(new)]] == new,
              "a smart material dropped under the rows goes in at the bottom (%d layers, was %d)" % (len(L), n))
        s.x.click(*center(rect("materials.tab:materials")))
        time.sleep(0.5)
    finally:
        s.close()
    print("drag and drop: %d failed" % len(failures) if failures else "drag and drop: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
