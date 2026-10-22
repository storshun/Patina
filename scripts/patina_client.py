#!/usr/bin/env python3
"""Tiny client for Patina's JSON-lines command server (used by the doc/web build scripts).

    python3 scripts/patina_client.py [--port 7860] <command> '<json args>'
"""
import json
import socket
import sys


class Client:
    def __init__(self, port=7860, host="127.0.0.1", timeout=600):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.f = self.sock.makefile("rwb")
        self.n = 0

    def __call__(self, cmd, **args):
        self.n += 1
        self.f.write((json.dumps({"id": self.n, "cmd": cmd, "args": args}) + "\n").encode())
        self.f.flush()
        r = json.loads(self.f.readline())
        if "error" in r:
            raise RuntimeError(f"{cmd}: {r['error']}")
        return r.get("result")


if __name__ == "__main__":
    a = sys.argv[1:]
    port = 7860
    if a[:1] == ["--port"]:
        port = int(a[1])
        a = a[2:]
    c = Client(port)
    print(json.dumps(c(a[0], **(json.loads(a[1]) if len(a) > 1 else {})), indent=1)[:4000])
