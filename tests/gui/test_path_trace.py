#!/usr/bin/env python3
"""(0.39) The path tracer, in the real window (Xvfb or PATINA_SHOTS_DISPLAY): the Path traced view mode adds samples
while nothing changes and starts again when something does, stops at its maximum, shows the model's shadow on the
ground; the Render image dialog renders path traced in steps; render_image {path_traced} over MCP.

    python3 tests/gui/test_path_trace.py [path/to/patina]
"""
import io
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import click, layout, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def lum(px):
    return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]


def main():
    from PIL import Image
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_path_trace_test"), display=":97", port=7889)
    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_tool", tool="paint")
        s("set_camera", view="persp", frame=True)
        s("set_camera", yaw=0.4, pitch=0.35, distance=s("set_camera")["distance"] * 2.4)  # (room round it for the ground)
        s("path_trace", max_samples=48, ground=True, bounces=4)
        r = s("set_view", mode="path_traced")
        check(r["mode"] == "path_traced", "set_view {mode: path_traced}")
        time.sleep(0.4)
        a = s("path_trace")["samples"]
        time.sleep(1.5)
        b = s("path_trace")
        check(b["shown"] and b["samples"] > a, "samples add up while nothing changes (%d -> %d)" % (a, b["samples"]))
        time.sleep(3.0)
        b = s("path_trace")
        check(b["done"] and b["samples"] == 48, "it stops at max_samples (%d, done %s)" % (b["samples"], b["done"]))
        time.sleep(0.5)
        check(s("path_trace")["samples"] == 48, "and stays there")
        # the shadow on the ground: pixels around the sphere's foot darker with the ground than without it, the
        # background far out the same
        x0, y0, x1, y1 = manual_scenes.VIEW
        box = (x0, y0 + (y1 - y0) // 3, x1, y1)
        im = s.image().crop(box).convert("L")
        if os.environ.get("PT_DEBUG"):
            im.save(os.path.join(os.environ["PT_DEBUG"], "pt_test_view.png"))
        s("path_trace", ground=False)
        time.sleep(3.5)
        im2 = s.image().crop(box).convert("L")
        a1, a2 = im.tobytes(), im2.tobytes()
        darker = sum(1 for p, q in zip(a1, a2) if q - p > 12)
        corner = abs(im.getpixel((8, im.height - 8)) - im2.getpixel((8, im2.height - 8)))
        check(darker > 1500 and corner < 8, "the ground's shadow round the model, gone with ground off (%d pixels darker, corner %d)" % (darker, corner))
        s("path_trace", ground=True)
        # orbiting starts again
        time.sleep(2.5)
        p0 = vp(0.5, 0.5)
        s.x.drag([p0] + [(p0[0] + 6 * i, p0[1]) for i in range(1, 6)], b=2, dt=0.03)
        n = s("path_trace")["samples"]
        check(n < 48, "orbiting starts the samples again (%d)" % n)
        # ---- the Render image dialog, path traced, in steps
        out = os.path.join(s.home, "patina_renders")
        s("open_window", window="render")
        time.sleep(0.6)
        click(s, "render.path_traced")
        time.sleep(0.3)
        click(s, "render.go")
        t0 = time.time()
        files = []
        while time.time() - t0 < 120:
            time.sleep(0.5)
            files = [f for f in os.listdir(out)] if os.path.isdir(out) else []
            if files:
                break
        check(len(files) == 1, "the dialog renders path traced to a PNG (%s, %.1fs)" % (files, time.time() - t0))
        if files:
            time.sleep(0.5)
            png = Image.open(os.path.join(out, files[0]))
            check(png.size == (1920, 1080), "at the dialog's size (%s)" % (png.size,))
        # ---- MCP: render_image {path_traced}, transparent: the shadow is in the alpha
        p = os.path.join(s.home, "pt_alpha.png")
        r = s("render_image", path=p, width=320, height=240, transparent=True, path_traced=True, samples=32)
        png = Image.open(r["files"][0]).convert("RGBA")
        alpha = png.getchannel("A").tobytes()
        part = sum(1 for v in alpha if 20 < v < 230)
        check(png.getpixel((160, 120))[3] == 255 and png.getpixel((4, 4))[3] == 0 and part > 0.025 * len(alpha),
              "render_image {path_traced, transparent}: model opaque, background clear, the shadow partly (%d pixels)" % part)
        check(s("set_view")["mode"] == "path_traced", "the view keeps its mode")
    finally:
        s.close()
    print("path trace: %d failed" % len(failures) if failures else "path trace: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
