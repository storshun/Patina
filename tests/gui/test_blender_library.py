#!/usr/bin/env python3
"""Materials in Blender (0.25) from the real window: the Blender Link window's "Add Patina's materials to Blender"
button
- with no Blender listening, says to start Blender's live link;
- with a (fake) Blender live link on port 7861, asks it for the library (1024, box, all) and says where it goes.

    python3 tests/gui/test_blender_library.py [path/to/patina]
"""
import os
import socket
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "blender", "patina_bridge"))
import manual_shots  # noqa: E402
import protocol  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def port_free(p):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    if not port_free(7861):
        print("SKIP: port 7861 is in use (a Blender with the live link?)")
        return 0
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_blender_library_test"), display=":97", port=7898)

    def click_button():
        r = s("ui_rect", name="blender.material_library")
        check(r.get("found") and r["y"] > 0, "the button is in the Blender Link window")
        if r.get("found"):
            s.x.click(int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] / 2))
            time.sleep(1.0)

    def log_has(text):
        return any(text in (e.get("text", "") if isinstance(e, dict) else str(e)) for e in s("get_log", count=60))

    try:
        s("new_project", primitive="sphere", resolution=256)
        s("open_window", window="blender")
        time.sleep(1.0)
        click_button()
        check(log_has("Blender is not listening on port 7861"), "no Blender: it says to start the live link")

        asked = []

        def handler(cmd, args, msg):
            if cmd == "patina_material_library":
                asked.append(args)
                return {"started": True, "directory": "/home/someone/Documents/Patina Materials"}
            raise protocol.CommandError("Unknown command %r" % cmd)

        server = protocol.PollingLineServer(handler, port=7861)
        server.start()
        stop = threading.Event()

        def pump():
            while not stop.is_set():
                server.poll()
                time.sleep(0.02)

        threading.Thread(target=pump, daemon=True).start()
        try:
            click_button()
        finally:
            stop.set()
            server.stop()
        check(asked == [{"size": 1024, "mapping": "box", "which": "all"}], "Blender is asked for the library (%s)" % asked)
        check(log_has("Blender is building the library in /home/someone/Documents/Patina Materials"),
              "it says where the library goes")
    finally:
        s.close()
    print("blender library: " + ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
