#!/usr/bin/env python3
"""Checks the printf formats of translated text: compiles the files with -fsyntax-only and T_/P_/L_ as the plain
English literals (PATINA_FORMAT_CHECK in src/ui/Tr.h), so -Wformat sees every format with its arguments, and
-Wformat-security a translated text used as a format without any.  format_check.py [files...] (default: src/ui)"""
import glob
import os
import subprocess
import sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
files = sys.argv[1:] or sorted(glob.glob(os.path.join(root, "src", "ui", "*.cpp")))
env = dict(os.environ, PATINA_EXTRA_FLAGS="-DPATINA_FORMAT_CHECK -Wformat -Wformat-security -Werror=format -Werror=format-security")
sys.exit(subprocess.call([sys.executable, os.path.join(root, "scripts", "dev", "syntax_check.py")] + files, env=env))
