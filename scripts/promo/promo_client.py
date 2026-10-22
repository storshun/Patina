"""A headless Patina with its own home (so the real library is never touched), and a small client."""
import base64
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time

from promo_paths import EXE


class Patina:
    def __init__(self, port=7889):
        self.home = tempfile.mkdtemp(prefix="patina_promo_home_")
        env = dict(os.environ, HOME=self.home, XDG_DATA_HOME=self.home + "/d", XDG_CONFIG_HOME=self.home + "/c",
                   XDG_CACHE_HOME=self.home + "/k")
        self.proc = subprocess.Popen([str(EXE), "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=600)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("Patina did not start (%s)" % EXE)
        self.f = self.s.makefile("rwb")
        self.n = 0

    def __call__(self, cmd, **a):
        self.n += 1
        self.f.write((json.dumps({"id": self.n, "cmd": cmd, "args": a}) + "\n").encode())
        self.f.flush()
        r = json.loads(self.f.readline())
        if not r.get("ok", True) and "error" in r:
            raise RuntimeError("%s: %s" % (cmd, r["error"]))
        return r.get("result")

    def png(self, path, **a):
        r = self("render_view", **a)
        open(path, "wb").write(base64.b64decode(r["image_png_base64"]))

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        shutil.rmtree(self.home, ignore_errors=True)
