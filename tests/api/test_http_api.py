#!/usr/bin/env python3
"""End-to-end tests of the HTTP API and batch mode against a real headless Patina.

    PATINA_BIN=build/patina python3 tests/api/test_http_api.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "clients", "python"))
from patina_api import Patina, PatinaError, JobFailed  # noqa: E402

BIN = os.environ.get("PATINA_BIN", os.path.join(ROOT, "build", "patina"))
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def own_address():
    """an address of this machine that is not loopback (to test the local-network filter)"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.123", 9))  # no packet is sent for UDP connect
        a = s.getsockname()[0]
        s.close()
        return None if a.startswith("127.") else a
    except OSError:
        return None


class Server:
    def __init__(self, *extra, home):
        self.port = free_port()
        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=os.path.join(home, "cfg"), EGL_LOG_LEVEL="fatal")
        self.proc = subprocess.Popen([BIN, "--headless", "--port", "0", "--http-port", str(self.port), *extra],
                                     env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        for _ in range(100):
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/api/v1/health" % self.port, timeout=1).read()
                return
            except Exception:
                if self.proc.poll() is not None:
                    raise SystemExit("patina exited: " + self.proc.stderr.read())
                time.sleep(0.15)
        raise SystemExit("patina did not start")

    def url(self, host="127.0.0.1"):
        return "http://%s:%d" % (host, self.port)

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except Exception:
            self.proc.kill()


def raw_status(url, headers=None, method="GET", data=None):
    req = urllib.request.Request(url, headers=headers or {}, method=method, data=data)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return -1


def main():
    home = tempfile.mkdtemp(prefix="patina_api_test_")
    ws = os.path.join(home, "ws")
    try:
        print("localhost server with a token and a sandboxed workspace")
        s = Server("--token", "s3cret", "--workspace", ws, "--sandbox", home=home)
        try:
            pt = Patina(s.url(), token="s3cret")
            check(pt.health()["ok"], "health")
            check(raw_status(s.url() + "/api/v1/info") == 401, "no token: 401")
            check(raw_status(s.url() + "/api/v1/info", {"Authorization": "Bearer nope"}) == 401, "wrong token: 401")
            check(raw_status(s.url() + "/api/v1/health") == 200, "health needs no token")
            check(raw_status(s.url() + "/api/v1/info", {"Authorization": "Bearer s3cret", "Origin": "https://evil.example"}) == 403,
                  "requests from web pages are refused")
            check(raw_status(s.url() + "/api/v1/health", {"Host": "evil.example:80"}) == 403, "foreign Host header refused")
            info = pt.info()
            check(info["sandbox"] and os.path.samefile(info["workspace"], ws), "info: workspace and sandbox")
            cmds = {c["name"] for c in pt.commands()}
            check({"bake_from_high_poly", "export_textures", "render_image"} <= cmds, "command list")
            api = pt.openapi()
            check("/api/v1/commands/bake_from_high_poly" in api["paths"] and "/api/v1/jobs" in api["paths"], "OpenAPI paths")
            r = pt.call("new_project", primitive="cube", resolution=512)
            check(r["faces"] == 6, "call new_project")
            try:
                pt.call("no_such_command")
                check(False, "unknown command raises")
            except PatinaError as e:
                check(e.status == 404, "unknown command: 404")
            try:
                pt.call("save_project", path="/tmp/outside.patina")
                check(False, "sandbox refuses absolute paths")
            except PatinaError as e:
                check(e.status == 403, "sandbox: path outside the workspace refused (403)")
            try:
                pt.call("set_brush", alpha={"shape": "round", "image": "/etc/hostname"})
                check(False, "sandbox refuses nested paths")
            except PatinaError as e:
                check(e.status == 403, "sandbox: nested file paths (brush tip image) refused too")
            # path tricks on the file endpoints, sent raw (no client-side normalization)
            for trick in ("/api/v1/files/%2e%2e/%2e%2e/etc/passwd", "/api/v1/files/..%2f..%2fetc%2fpasswd",
                          "/api/v1/files/in/../../outside.txt"):
                c = socket.create_connection(("127.0.0.1", s.port))
                c.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer s3cret\r\nConnection: close\r\n\r\n" % trick).encode())
                resp = b""
                while True:
                    chunk = c.recv(65536)
                    if not chunk:
                        break
                    resp += chunk
                c.close()
                code = int(resp.split(b" ")[1]) if resp.startswith(b"HTTP/") else -1
                check(code in (400, 403, 404) and b"root:" not in resp, "path trick %s refused (%d)" % (trick, code))
            # files
            src = os.path.join(home, "upload.bin")
            with open(src, "wb") as f:
                f.write(os.urandom(300000))
            rel = pt.upload(src, "in/sub/upload.bin")
            check(rel == "in/sub/upload.bin", "upload")
            got = pt.download("in/sub/upload.bin", os.path.join(home, "back.bin"))
            check(open(got, "rb").read() == open(src, "rb").read(), "download returns the same bytes")
            check(raw_status(s.url() + "/api/v1/files/../../etc/passwd", {"Authorization": "Bearer s3cret"}) in (403, 404),
                  "no path escape through /files")
            check(any(f["path"] == "in/sub/upload.bin" for f in pt.files("in", recursive=True)), "list files")
            pt.delete("in/sub/upload.bin")
            check(not any(f["path"] == "in/sub/upload.bin" for f in pt.files("in", recursive=True)), "delete file")
            # a job: bake, export, render
            job = pt.run([("new_project", {"primitive": "sphere", "resolution": 512}),
                          ("bake_mesh_maps", {"ao": True, "curvature": True, "ao_samples": 32}),
                          ("add_layer", {"type": "fill", "material": {"id": "patina_rust_heavy"},
                                         "generator": {"type": "edges"}}),
                          ("export_textures", {"directory": "out/sphere", "preset": "gltf"}),
                          ("render_image", {"path": "out/sphere/preview.png", "width": 160, "height": 120})],
                         name="sphere", timeout=300)
            check(job["status"] == "done" and len(job["results"]) == 5, "job ran all steps")
            outs = {f["path"] for f in pt.files("out/sphere")}
            check("out/sphere/preview.png" in outs and any("baseColor" in p for p in outs), "job outputs in the workspace")
            # a failing step stops the job
            try:
                pt.run([("get_state", {}), ("open_file", {"path": "in/missing.glb"}), ("get_state", {})], timeout=60)
                check(False, "failing job raises")
            except JobFailed as e:
                check(e.job["status"] == "failed" and len(e.job["results"]) == 2 and "step 2" in e.job["error"],
                      "failing step: job failed, later steps skipped")
            # queue + cancel + busy
            # several slow bakes, so the job is still running after the pause below even on a fast GPU
            bake = ("bake_mesh_maps", {"ao": True, "curvature": False, "ao_samples": 1024})
            long_id = pt.submit([("new_project", {"primitive": "sphere", "resolution": 4096})] + [bake] * 8, name="long")
            queued_id = pt.submit([("get_state", {})], name="queued")
            time.sleep(1.5)
            check(pt.job(queued_id)["status"] == "queued", "second job waits in the queue")
            check(pt.cancel(queued_id)["status"] == "cancelled", "cancel a queued job")
            try:
                pt.call("new_project", primitive="cube", busy_wait=0)
                check(False, "busy engine refuses")
            except PatinaError as e:
                check(e.status == 409, "engine busy: 409")
            j = pt.job(long_id)
            check(j["status"] == "running" and 0 <= j["progress"] <= 1, "running job reports progress (%.2f, %s)" % (j["progress"], j.get("label")))
            pt.cancel(long_id)
            j = pt.wait(long_id, timeout=120, raise_on_failure=False)
            check(j["status"] == "cancelled", "cancel a running bake (%s)" % j["status"])
            check(pt.call("get_state")["app"] == "patina", "engine usable after a cancel")
        finally:
            s.close()

        print("local network mode")
        s = Server("--lan", "--workspace", ws, home=home)
        try:
            tok_file = os.path.join(home, "cfg", "patina", "api_token")
            check(os.path.exists(tok_file), "token generated and stored")
            tok = open(tok_file).read().strip()
            check(raw_status(s.url() + "/api/v1/info") == 401, "lan: token required")
            check(Patina(s.url(), token=tok).info()["network"] == "local network", "lan: works with the token")
            addr = own_address()
            if addr:
                pub = not (addr.startswith("10.") or addr.startswith("192.168.") or addr.startswith("169.254.") or
                           (addr.startswith("172.") and 16 <= int(addr.split(".")[1]) <= 31))
                code = raw_status(s.url(addr) + "/api/v1/health")
                if pub:
                    check(code == 403, "lan: client with a public address (%s) refused" % addr)
                else:
                    check(code == 200, "lan: client with a private address (%s) answered" % addr)
            else:
                print("  skip own-address check (no non-loopback address)")
        finally:
            s.close()
        if own_address():
            addr = own_address()
            s = Server("--lan", "--allow", addr + "/32", "--token", "t0k", home=home)
            try:
                check(raw_status(s.url(addr) + "/api/v1/health") == 200, "--allow admits a configured range")
            finally:
                s.close()

        print("batch mode")
        jobdir = os.path.join(home, "batch")
        os.makedirs(jobdir)
        with open(os.path.join(jobdir, "job.json"), "w") as f:
            json.dump({"name": "batch test", "vars": {"res": "256"},
                       "steps": [{"cmd": "new_project", "args": {"primitive": "${shape}", "resolution": 256}},
                                 {"cmd": "export_textures", "args": {"directory": "out/${shape}", "preset": "blender"}},
                                 {"cmd": "save_project", "args": {"path": "out/${shape}.patina"}}]}, f)
        env = dict(os.environ, HOME=home, EGL_LOG_LEVEL="fatal")
        p = subprocess.run([BIN, "run", os.path.join(jobdir, "job.json"), "--set", "shape=torus", "--quiet"],
                           env=env, capture_output=True, text=True, timeout=300)
        check(p.returncode == 0, "patina run: exit 0 (%s)" % p.stderr.strip()[-200:])
        rep = json.loads(p.stdout) if p.stdout.strip().startswith("{") else {}
        check(rep.get("ok") and len(rep.get("steps", [])) == 3, "patina run: report on stdout")
        check(os.path.exists(os.path.join(jobdir, "out", "torus.patina")), "relative paths resolve next to the job file")
        check(any(n.endswith("basecolor.png") for n in os.listdir(os.path.join(jobdir, "out", "torus"))), "batch export written")
        p = subprocess.run([BIN, "run", os.path.join(jobdir, "job.json"), "--quiet"], env=env, capture_output=True,
                           text=True, timeout=60)
        check(p.returncode == 2 and "shape" in p.stderr, "patina run: missing variable -> exit 2")
        with open(os.path.join(jobdir, "bad.json"), "w") as f:
            json.dump([{"cmd": "open_file", "args": {"path": "nope.glb"}}, {"cmd": "get_state"}], f)
        rpt = os.path.join(jobdir, "report.json")
        p = subprocess.run([BIN, "run", os.path.join(jobdir, "bad.json"), "--report", rpt, "--quiet"], env=env,
                           capture_output=True, text=True, timeout=120)
        r = json.load(open(rpt)) if os.path.exists(rpt) else {}
        check(p.returncode == 1 and r.get("ok") is False and len(r.get("steps", [])) == 1, "failing batch: exit 1, report written")
        p = subprocess.run([BIN, "call", "new_project", '{"primitive": "plane"}', "--port", str(free_port())], env=env,
                           capture_output=True, text=True, timeout=120)
        check(p.returncode == 0 and '"faces"' in p.stdout, "patina call without a server runs headless")
    finally:
        shutil.rmtree(home, ignore_errors=True)
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
