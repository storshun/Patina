#!/usr/bin/env python3
"""(1.2.12) Patina in 5 Minutes in the real window: the welcome window has one button that opens the tutorial
(docs/tutorial/index.html, found next to Patina's data; PATINA_NO_BROWSER keeps the browser closed), an x beside it
hides the button for good (session.json tutorial_dismissed), and MCP open_window {window: tutorial} opens it too. Every
screenshot the page shows is in docs/tutorial/shots.

    python3 tests/gui/test_tutorial.py [path/to/patina]
"""
import json
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import click  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
OUT = os.path.join("/tmp", "patina_tutorial_test")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def found(s, name):
    return s("ui_rect", name=name).get("found") is True


def main():
    # ---- the page and its screenshots
    page = os.path.join(ROOT, "docs", "tutorial", "index.html")
    html = open(page, encoding="utf-8").read()
    shots = set(re.findall(r'src: "([0-9a-z_]+)"', html)) | {"01_pie_q_tutorial_set"}  # (q(): the Quick Favorites pie)
    missing = [n for n in shots if not os.path.exists(os.path.join(ROOT, "docs", "tutorial", "shots", n + ".webp"))]
    check(len(shots) > 30 and not missing, "every screenshot the page shows is there (%d, missing %s)" % (len(shots), missing))
    check(html.lstrip().lower().startswith("<!doctype html>"), "the page is a whole HTML document")
    # ---- a first start: the welcome window with the tutorial button
    s = manual_shots.Session(BIN, OUT, display=":97", port=7893, session=False)
    try:
        s.settle(10)
        check(found(s, "welcome.tutorial") and found(s, "welcome.tutorial_dismiss"), "the welcome window has the tutorial button and its x")
        click(s, "welcome.tutorial")
        s.settle(4)
        p = s("get_state")["last_page"]
        check(p.endswith("tutorial/index.html") and os.path.exists(p), "the button opens the tutorial (%s)" % p)
        r = s("open_window", window="tutorial")
        check(r["opened"] and r["page"] == p, "open_window {window: tutorial} opens it too (%s)" % r)
        # the x: gone for good
        click(s, "welcome.tutorial_dismiss")
        s.settle(4)
        sess = json.load(open(os.path.join(s.home, ".config", "patina", "session.json")))
        check(sess.get("tutorial_dismissed") is True, "its x saves tutorial_dismissed in the session")
    finally:
        s.close()
    # ---- the next start: the welcome window without the button
    s = manual_shots.Session(BIN, OUT, display=":97", port=7894, welcome=True, session={"tutorial_dismissed": True})
    try:
        s("open_window", window="welcome")
        s.settle(10)
        check(found(s, "window.Welcome to Patina") and not found(s, "welcome.tutorial"),
              "dismissed: the welcome window shows no tutorial button")
        r = s("open_window", window="tutorial")
        check(r["opened"], "the tutorial still opens from Help (open_window tutorial)")
    finally:
        s.close()
    print("tutorial: %d failed" % len(failures) if failures else "tutorial: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
