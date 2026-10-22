#!/usr/bin/env python3
"""Patina Web end to end in headless Chromium (Playwright): tools, layers, smart masks, projects,
export presets, the Island brush. Run scripts/build_web.py first.

    python3 tests/web/test_web_e2e.py
"""
import os
import sys
import tempfile
import zipfile

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
URL = "file://" + os.path.join(ROOT, "web", "dist", "index.html")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


PAINTED = """(() => { const e = patinaWeb.engine; const d = e.readLayer(e.layers[e.selected], 'color');
  let n = 0; for (let i = 3; i < d.length; i += 4) if (d[i] > 128) n++; return n; })()"""
SIDES = """(() => { const e = patinaWeb.engine, n = e.size; const d = e.readLayer(e.layers[e.selected], 'color');
  const pos = new Float32Array(n * n * 4); e.renderer.readRenderTargetPixels(e.posRT, 0, 0, n, n, pos);
  let l = 0, r = 0; for (let i = 0; i < n * n; ++i) if (d[i * 4 + 3] > 128 && pos[i * 4 + 3] > 0) {
    if (pos[i * 4] > 0.02) r++; else if (pos[i * 4] < -0.02) l++; } return [l, r]; })()"""
HASH = """(() => { const e = patinaWeb.engine; e.composite(); const out = []; for (const w of ['color', 'orm']) {
  const m = e.readMap(w); let h = 0; for (let i = 0; i < m.data.length; i += 7) h = (h * 31 + m.data[i]) >>> 0; out.push(h); } return out; })()"""


def main():
    if not os.path.exists(URL[len("file://"):]):
        print("web/dist not built (python3 scripts/build_web.py); skipping")
        sys.exit(77)
    tmp = tempfile.mkdtemp(prefix="patina_web_e2e_")
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
        pg = b.new_page(viewport={"width": 1500, "height": 900}, accept_downloads=True)
        errors = []
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.on("console", lambda m: m.type == "error" and errors.append(m.text[:300]))
        pg.goto(URL)
        pg.wait_for_timeout(2500)
        # mirror painting: a one-sided stroke lands on both halves (a sphere, so every texel is unique;
        # the Suzanne sample's left half overlaps the right in UV space)
        pg.evaluate("patinaWeb.primitive('sphere')")
        pg.wait_for_timeout(2500)
        pg.evaluate("patinaWeb.selectMaterial(''); patinaWeb.state.brush.size = 40; patinaWeb.state.brush.flow = 1")
        pg.evaluate("patinaWeb.paintStroke([[0.56,0.47],[0.62,0.5]])")
        one = pg.evaluate(SIDES)
        pg.evaluate("patinaWeb.engine.undo()")
        pg.check("#brSymX")
        pg.evaluate("patinaWeb.paintStroke([[0.56,0.47],[0.62,0.5]])")
        two = pg.evaluate(SIDES)
        pg.uncheck("#brSymX")
        check(min(one) < max(one) * 0.1 and min(two) > max(two) * 0.6,
              f"Mirror X paints both sides (left/right {one} -> {two} texels)")

        pg.evaluate("patinaWeb.loadSample('Suzanne')")
        pg.wait_for_timeout(3500)
        pg.evaluate("patinaWeb.selectMaterial(''); patinaWeb.state.brush.size = 40; patinaWeb.state.brush.flow = 1")

        # polygon fill: a UV island
        pg.evaluate("patinaWeb.setTool('fill')")
        pg.select_option("#fillMode", "island")
        before = pg.evaluate(PAINTED)
        pg.evaluate("patinaWeb.fillAt(0.5, 0.3)")
        after = pg.evaluate(PAINTED)
        check(after > before * 3, f"Fill covers the island ({before} -> {after} texels)")

        # blur changes the layer, one undo step
        pg.evaluate("patinaWeb.setTool('blur'); patinaWeb.state.material.flat.color = '#000000'")
        depth = pg.evaluate("patinaWeb.engine.undoStack.length")
        h0 = pg.evaluate(HASH)
        pg.evaluate("patinaWeb.setTool('paint'); patinaWeb.paintStroke([[0.4,0.6],[0.6,0.6]]); patinaWeb.setTool('blur'); patinaWeb.state.brush.size = 90")
        h1 = pg.evaluate(HASH)
        pg.evaluate("patinaWeb.paintStroke([[0.4,0.6],[0.6,0.6]])")
        h2 = pg.evaluate(HASH)
        check(h1 != h0 and h2 != h1, "Blur softens what was painted")
        check(pg.evaluate("patinaWeb.engine.undoStack.length") == depth + 2, "each stroke is one undo step")

        # picker takes the color under the cursor
        pg.evaluate("patinaWeb.setTool('pick'); patinaWeb.pickAt(0.5, 0.3)")
        check(pg.evaluate("patinaWeb.state.material.flat.color").startswith("#"), "Pick sets the flat color (%s)" % pg.evaluate("patinaWeb.state.material.flat.color"))

        # layers: fill with an edge mask and multiply, adjustment layer
        pg.evaluate("patinaWeb.setTool('paint'); patinaWeb.selectMaterial('patina_rust_heavy')")
        pg.wait_for_timeout(1500)
        pg.click("#lyAddFill")
        g0 = pg.evaluate(HASH)
        pg.select_option("#genType", "edges")
        g1 = pg.evaluate(HASH)
        pg.select_option("#lyBlend", "multiply")
        g2 = pg.evaluate(HASH)
        pg.click("#lyAddAdj")
        pg.fill("#adjHue", "0.3")
        pg.dispatch_event("#adjHue", "input")
        g3 = pg.evaluate(HASH)
        check(len({tuple(g0), tuple(g1), tuple(g2), tuple(g3)}) == 4, "smart mask, blend mode and adjustment layer each change the result")

        # project round trip
        with pg.expect_download() as dl:
            pg.click("#btnSave")
        proj = os.path.join(tmp, "t.patinaweb")
        dl.value.save_as(proj)
        names = zipfile.ZipFile(proj).namelist()
        check("project.json" in names and "mesh.bin" in names, "Save project writes a .patinaweb (%d files)" % len(names))
        pg.close()  # one WebGL page at a time: software GL in headless Chromium is slow with two
        pg2 = b.new_page(viewport={"width": 1500, "height": 900})
        pg2.on("pageerror", lambda e: errors.append("page 2: " + str(e)))
        pg2.goto(URL, wait_until="domcontentloaded", timeout=90000)
        pg2.wait_for_timeout(2500)
        pg2.set_input_files("#fileInput", proj)
        for _ in range(80):
            pg2.wait_for_timeout(250)
            if not pg2.inner_text("#statusText").startswith("Opening"):
                break
        check(pg2.evaluate(HASH) == g3, "the reopened project gives the same maps, texel for texel")
        check(pg2.evaluate("patinaWeb.engine.layers.map(l => l.kind).join(',')") == "paint,fill,adjust", "and the same layers")

        # export presets
        for preset, want in [("unreal", "T_Suzanne_ORM.png"), ("unity", "Suzanne_MetallicSmoothness.png")]:
            pg2.select_option("#selPreset", preset)
            with pg2.expect_download() as dl:
                pg2.click("#btnExportZip")
            f = os.path.join(tmp, preset + ".zip")
            dl.value.save_as(f)
            check(want in zipfile.ZipFile(f).namelist(), f"export for {preset} has {want}")

        pg = pg2  # the reopened project (the same Suzanne) from here on
        # Island brush (mirrored, pinned) still works
        pg.evaluate("patinaWeb.setTool('island'); patinaWeb.state.brush.size = 50")
        pg.evaluate("patinaWeb.islandStroke([[0.75,0.36],[0.82,0.38],[0.86,0.42],[0.82,0.45],[0.76,0.44],[0.8,0.4]])")
        pg.evaluate("patinaWeb.islands.makeIsland()")
        pg.wait_for_timeout(800)
        info = pg.evaluate("(() => { const f = patinaWeb.state.island.flags; return [f.filter(x => x & 1).length, f.filter(x => x & 16).length, patinaWeb.islands.info().unstacked]; })()")
        check(info[0] > 20 and info[1] > 40 and info[2] == 0, "Island brush: mirrored half stacked, pinned (%s)" % info)
        # (0.37.1) the Island panel's Pin and Unpin, and Alt+P in the 3D view
        pinned = "patinaWeb.state.island.flags.filter(x => x & 16).length"
        pg.evaluate("patinaWeb.islands.setSelection([], 'set')")
        pg.evaluate("patinaWeb.islandStroke([[0.75,0.36],[0.82,0.38],[0.86,0.42]])")
        check(pg.is_enabled("#islUnpinSel") and "pinned" in pg.inner_text("#islSelCount"),
              "Unpin is on with pinned triangles painted (%s)" % pg.inner_text("#islSelCount"))
        pg.click("#islUnpinSel")
        check(pg.evaluate(pinned) == 0 and pg.is_disabled("#islUnpinSel"), "Unpin unpins them (%d pinned)" % pg.evaluate(pinned))
        pg.click("#islPinSel")
        n = pg.evaluate(pinned)
        check(n > 40, "Pin pins them again (%d)" % n)
        vp = pg.locator("#viewport").bounding_box()
        pg.mouse.move(vp["x"] + vp["width"] * 0.3, vp["y"] + vp["height"] * 0.8)
        pg.keyboard.press("Alt+P")
        pg.wait_for_timeout(200)
        check(pg.evaluate(pinned) == 0, "Alt+P in the 3D view unpins (%d pinned)" % pg.evaluate(pinned))
        pg.keyboard.press("Control+z")
        pg.wait_for_timeout(200)
        check(pg.evaluate(pinned) == n, "undo pins them again (%d)" % pg.evaluate(pinned))
        # (0.37) the axis gizmo: a click on X looks along it, a second click from the other side, Y from the top
        eye = "patinaWeb.camera.position.clone().sub(patinaWeb.controls.target).normalize().toArray()"

        def click_end(name):
            e = next(e for e in pg.evaluate("patinaWeb.gizmo.ends()") if e["name"] == name)
            pg.mouse.move(e["x"], e["y"])
            pg.mouse.down()
            pg.mouse.up()
            pg.wait_for_timeout(600)
            return pg.evaluate(eye)
        d = click_end("X")
        check(d[0] > 0.999, "gizmo: X looks from the right (%s)" % d)
        d = click_end("X")
        check(d[0] < -0.999, "gizmo: X again from the left (%s)" % d)
        d = click_end("Y")
        check(d[1] > 0.999, "gizmo: Y from the top (%s)" % d)
        check(not errors, "no page errors %s" % errors[:3])
        b.close()
    print("web e2e: %d failed" % len(failures) if failures else "web e2e: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
