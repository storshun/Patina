#!/usr/bin/env python3
"""Screenshots of Patina Web for the manual (headless Chromium via Playwright).

    python3 scripts/manual_web_shots.py [--out docs/manual/img]

Run scripts/build_web.py first.
"""
import argparse
import os

from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "manual", "img"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    url = "file://" + os.path.join(ROOT, "web", "dist", "index.html")
    with sync_playwright() as p:
        b = p.chromium.launch(args=["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
        pg = b.new_page(viewport={"width": 1500, "height": 900})
        errors = []
        pg.on("pageerror", lambda e: errors.append(str(e)))
        pg.goto(url)
        pg.wait_for_timeout(2500)
        pg.evaluate("patinaWeb.loadSample('Suzanne')")
        pg.wait_for_timeout(3500)
        pg.evaluate("patinaWeb.selectMaterial('patina_paint_chipped_red')")
        pg.wait_for_timeout(1500)
        pg.click("#lyAddFill")
        pg.wait_for_timeout(800)
        pg.click("#lyAddPaint")
        pg.evaluate("patinaWeb.selectMaterial('patina_gold_polished')")
        pg.wait_for_timeout(300)
        pg.evaluate("patinaWeb.state.brush.size = 46; patinaWeb.state.brush.hardness = 0.85; patinaWeb.state.brush.flow = 1")
        pg.evaluate("patinaWeb.paintStroke([[0.36,0.3],[0.43,0.26],[0.5,0.25],[0.57,0.26],[0.64,0.3]])")
        pg.evaluate("patinaWeb.selectMaterial('patina_rust_heavy')")
        pg.wait_for_timeout(1500)
        pg.evaluate("patinaWeb.state.brush.size = 110; patinaWeb.state.brush.hardness = 0.4; patinaWeb.state.brush.flow = 0.7")
        pg.evaluate("patinaWeb.paintStroke([[0.38,0.68],[0.46,0.72],[0.56,0.7],[0.62,0.64]])")
        pg.wait_for_timeout(600)
        pg.mouse.move(900, 520)
        pg.wait_for_timeout(300)
        pg.screenshot(path=os.path.join(a.out, "web_overview.png"))
        pg.locator("#side").screenshot(path=os.path.join(a.out, "web_sidebar.png"))
        pg.evaluate("patinaWeb.toggleUV()")
        pg.wait_for_timeout(1500)
        pg.screenshot(path=os.path.join(a.out, "web_uvview.png"))
        # a smart mask: dirt in the cavities over steel
        pg.goto(url)
        pg.wait_for_timeout(2500)
        pg.evaluate("patinaWeb.loadSample('Suzanne')")
        pg.wait_for_timeout(3500)
        pg.evaluate("patinaWeb.selectMaterial('patina_steel_brushed')")
        pg.wait_for_timeout(1200)
        pg.click("#lyAddFill")
        pg.wait_for_timeout(800)
        pg.evaluate("patinaWeb.selectMaterial('patina_dirt_mud')")
        pg.wait_for_timeout(1200)
        pg.click("#lyAddFill")
        pg.wait_for_timeout(800)
        pg.select_option("#genType", "cavity")
        pg.fill("#genLevel", "0.55")
        pg.dispatch_event("#genLevel", "input")
        pg.wait_for_timeout(1500)
        pg.mouse.move(900, 520)
        pg.screenshot(path=os.path.join(a.out, "web_smart_mask.png"))
        # the Island brush: paint over the ear (mirrored), then the whole head as one mirrored island
        pg.goto(url)
        pg.wait_for_timeout(2500)
        pg.evaluate("patinaWeb.loadSample('Suzanne')")
        pg.wait_for_timeout(3500)
        pg.evaluate("patinaWeb.selectMaterial('patina_gold_polished')")
        pg.wait_for_timeout(1200)
        pg.evaluate("patinaWeb.state.brush.size = 60; patinaWeb.state.brush.flow = 1")
        pg.evaluate("patinaWeb.paintStroke([[0.3,0.42],[0.4,0.3],[0.5,0.27],[0.6,0.3],[0.7,0.42]])")
        pg.evaluate("patinaWeb.selectMaterial('patina_rust_heavy')")
        pg.wait_for_timeout(1200)
        pg.evaluate("patinaWeb.paintStroke([[0.42,0.7],[0.5,0.74],[0.58,0.7]])")
        pg.evaluate("patinaWeb.setTool('island'); patinaWeb.state.brush.size = 50")
        pg.evaluate("patinaWeb.islandStroke([[0.75,0.36],[0.82,0.38],[0.86,0.42],[0.82,0.45],[0.76,0.44],[0.8,0.4]])")
        pg.mouse.move(1000, 330)
        pg.wait_for_timeout(500)
        pg.locator("#viewport").screenshot(path=os.path.join(a.out, "web_island_brush.png"))
        pg.keyboard.press("a")
        pg.select_option("#islDensity", "fit")
        pg.evaluate("patinaWeb.islands.makeIsland()")
        pg.wait_for_timeout(1500)
        pg.evaluate("patinaWeb.islands.setSelection([], 'set')")
        pg.evaluate("patinaWeb.toggleUV()")
        pg.select_option("#selUVMode", "islands")
        pg.wait_for_timeout(1500)
        box = pg.locator("#uvCanvas").bounding_box()
        isl = pg.evaluate("(() => { const p = patinaWeb.islands.info().per.filter(p => p.stacked).sort((a, b) => (b.x1-b.x0)-(a.x1-a.x0))[0]; return [p.x0, p.y0, p.x1, p.y1]; })()")
        cx = box["x"] + (isl[0] * 0.7 + isl[2] * 0.3) * box["width"]
        cy = box["y"] + (1 - (isl[1] + isl[3]) / 2) * box["height"]
        pg.mouse.click(cx, cy)
        pg.wait_for_timeout(500)
        pg.screenshot(path=os.path.join(a.out, "web_islands.png"))
        pg.mouse.click(cx, cy, button="right")
        pg.wait_for_timeout(500)
        mb = pg.locator("#uvMenu").bounding_box()
        pg.screenshot(path=os.path.join(a.out, "web_uv_menu.png"),
                      clip={"x": box["x"], "y": box["y"] - 40, "width": max(box["width"], mb["x"] + mb["width"] - box["x"] + 10),
                            "height": max(box["height"] + 40, mb["y"] + mb["height"] - box["y"] + 50)})
        if errors:
            print("page errors:", errors)
        b.close()
    print("web screenshots written to", a.out)


if __name__ == "__main__":
    main()
