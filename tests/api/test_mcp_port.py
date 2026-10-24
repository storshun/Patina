#!/usr/bin/env python3
"""(1.2.5) `patina --mcp` and the command port, against real Patinas:
- with no Patina window running, `--mcp` starts its own hidden engine, but it no longer listens on the command port.
  It did, so a Patina window opened afterward moved to the next port (7861, the Blender add-on's), and Blender
  talked to the hidden engine: Blender got its startup sphere's UVs for the user's object;
- when a Patina opens on that port later, the MCP tools switch to it (live mode);
- `bridge_export` for an object this Patina has no link to is refused, instead of exporting another object;
- (1.2.7) SIGTERM ends `--mcp` while its client keeps stdin open, with its own engine and in live mode. It waited
  for input forever, and only SIGKILL ended it.

    PATINA_BIN=build/patina python3 tests/api/test_mcp_port.py
"""
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402

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


def listening(port):
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_mcp_port_test_")

    def env(name):
        home = os.path.join(tmp, name)
        os.makedirs(home, exist_ok=True)
        return dict(os.environ, HOME=home, XDG_DATA_HOME=os.path.join(home, "data"),
                    XDG_CONFIG_HOME=os.path.join(home, "config"), XDG_CACHE_HOME=os.path.join(home, "cache"))

    port = free_port()
    mcp = subprocess.Popen([exe, "--mcp", "--port", str(port)], env=env("mcp"), stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    window = None
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l) for l in mcp.stdout], daemon=True).start()
    nid = [0]

    def rpc(method, params=None, timeout=120):
        nid[0] += 1
        mcp.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid[0], "method": method, "params": params or {}}) + "\n")
        mcp.stdin.flush()
        while True:
            msg = json.loads(lines.get(timeout=timeout))
            if msg.get("id") == nid[0]:
                return msg

    def call(name, args=None):
        r = rpc("tools/call", {"name": name, "arguments": args or {}})["result"]
        return "".join(c.get("text", "") for c in r["content"] if c["type"] == "text")

    def faces():
        return json.loads(call("list_objects"))["objects"][0]["faces"]

    try:
        rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test"}})
        mcp.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        hidden = faces()
        check(hidden > 6, "--mcp with no window: its own engine answers (startup sphere, %d faces)" % hidden)
        check(not listening(port), "and it leaves the command port %d free" % port)

        # a Patina opens on the port: the tools drive it
        window = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env("window"),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        c.request("new_project", {"primitive": "cube", "resolution": 512})
        live = faces()
        for _ in range(10):  # (it looks again every 2 s)
            if live == 6:
                break
            time.sleep(0.5)
            live = faces()
        check(live == 6, "a Patina opened on that port later: the MCP tools drive it (%d faces, its cube)" % live)

        # Blender asks for an object this Patina doesn't have: refused, not the cube's textures and UVs
        try:
            c.request("bridge_export", {"object": "Mai_Body_UE5"})
            check(False, "bridge_export for an object it has no link to is refused")
        except protocol.RemoteError as e:
            check("no object linked to 'Mai_Body_UE5'" in str(e), "bridge_export for an object it has no link to is refused (%s)" % e)

        # (1.2.7) the stop signal, in live mode (this window) and with its own engine (a port nothing listens on)
        if os.name == "posix":
            for what, p in (("live mode", port), ("its own engine", free_port())):
                m = subprocess.Popen([exe, "--mcp", "--port", str(p)], env=env("term"), stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(3)
                m.terminate()
                try:
                    code = m.wait(timeout=10)
                    check(True, "SIGTERM ends --mcp with stdin still open, %s (exit %d)" % (what, code))
                except subprocess.TimeoutExpired:
                    m.kill()
                    m.wait()
                    check(False, "SIGTERM ends --mcp with stdin still open, %s" % what)
                m.stdin.close()
    finally:
        for p in (mcp, window):
            if p is None:
                continue
            if p is mcp:
                try:
                    p.stdin.close()
                except OSError:
                    pass
            p.terminate()
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("mcp port: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
