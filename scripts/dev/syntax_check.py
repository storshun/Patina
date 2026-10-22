#!/usr/bin/env python3
"""Compiles source files with -fsyntax-only, with the flags in build/compile_commands.json (no object files, no
ninja: safe to run while a build runs).  syntax_check.py src/ui/Panels.cpp [more files...]"""
import json
import os
import shlex
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
db = json.load(open(os.path.join(root, os.environ.get("PATINA_BUILD", "build"), "compile_commands.json")))
bad = 0
for f in sys.argv[1:]:
    path = os.path.abspath(f)
    e = next((x for x in db if os.path.abspath(os.path.join(x["directory"], x["file"])) == path), None)
    if not e:
        print("not in compile_commands.json:", f)
        bad += 1
        continue
    args = shlex.split(e["command"]) if "command" in e else e["arguments"]
    out, k = [], 0
    while k < len(args):
        if args[k] == "-o":
            k += 2
            continue
        if args[k] in ("-MD", "-MMD"):
            k += 1
            continue
        if args[k] in ("-MF", "-MT", "-MQ"):
            k += 2
            continue
        out.append(args[k])
        k += 1
    extra = shlex.split(os.environ.get("PATINA_EXTRA_FLAGS", ""))
    r = subprocess.run(out + ["-fsyntax-only"] + extra, cwd=e["directory"])
    print(("ok  " if r.returncode == 0 else "FAIL") + " " + f)
    bad += r.returncode != 0
sys.exit(1 if bad else 0)
