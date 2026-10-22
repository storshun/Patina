#!/usr/bin/env python3
"""Bakes many assets on several Patina servers at once (a small render farm).

Each server is `patina --headless --lan --workspace /srv/patina` on a machine of the
local network. Give their URLs and the token, and a folder with <name>_low.glb /
<name>_high.glb pairs; finished textures land in ./baked/<name>/.

    python3 farm_bake.py --token $PATINA_TOKEN \\
        --server http://render-01:7870 --server http://render-02:7870 assets/
"""
import argparse
import glob
import os
import queue
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "clients", "python"))
from patina_api import Patina, PatinaError  # noqa: E402


def bake(pt, name, low, high, out, resolution, preset, log):
    base = "farm/" + name
    pt.upload(low, base + "/" + os.path.basename(low))
    pt.upload(high, base + "/" + os.path.basename(high))
    job = pt.run([
        ("open_file", {"path": base + "/" + os.path.basename(low)}),
        ("set_resolution", {"resolution": resolution}),
        ("bake_from_high_poly", {"path": base + "/" + os.path.basename(high)}),
        ("export_textures", {"directory": base + "/textures", "preset": preset}),
        ("render_image", {"path": base + "/textures/preview.png", "width": 960, "height": 540}),
    ], name=name, on_progress=lambda j: log("%-20s %3.0f%% %s" % (name, 100 * j.get("progress", 0), j.get("label", ""))))
    hit = job["results"][2]["result"].get("hit_fraction", 1.0)
    for f in pt.files(base + "/textures"):
        pt.download(f["path"], os.path.join(out, name, os.path.basename(f["path"])))
    pt.delete(base)
    return hit


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--server", action="append", required=True)
    ap.add_argument("--token", default=os.environ.get("PATINA_TOKEN"))
    ap.add_argument("--out", default="baked")
    ap.add_argument("--resolution", type=int, default=2048)
    ap.add_argument("--preset", default="unreal", choices=["blender", "gltf", "unreal", "unity"])
    a = ap.parse_args()
    work = queue.Queue()
    for low in sorted(glob.glob(os.path.join(a.folder, "*_low.*"))):
        name = os.path.basename(low).rsplit("_low.", 1)[0]
        highs = glob.glob(os.path.join(a.folder, name + "_high.*"))
        if highs:
            work.put((name, low, highs[0]))
    total = work.qsize()
    print("%d assets, %d servers" % (total, len(a.server)))
    lock = threading.Lock()
    failed = []

    def log(msg):
        with lock:
            print(msg, flush=True)

    def worker(url):
        pt = Patina(url, a.token)
        while True:
            try:
                name, low, high = work.get_nowait()
            except queue.Empty:
                return
            try:
                hit = bake(pt, name, low, high, a.out, a.resolution, a.preset, log)
                log("%-20s done on %s (%.0f%% of texels found the sculpt)" % (name, url, hit * 100))
            except PatinaError as e:
                log("%-20s FAILED on %s: %s" % (name, url, e))
                failed.append(name)

    threads = [threading.Thread(target=worker, args=(u,)) for u in a.server]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print("%d of %d baked" % (total - len(failed), total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
