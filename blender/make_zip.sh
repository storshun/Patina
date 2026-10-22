#!/usr/bin/env bash
# Package the add-on as dist/patina_bridge.zip.
#
# The zip contains a single top-level folder "patina_bridge/" holding
# __init__.py and blender_manifest.toml, which both installers accept:
#   * Blender 4.2+  Edit > Preferences > Get Extensions > (v) > Install from Disk...
#   * Blender 3.6 - 4.1  Edit > Preferences > Add-ons > Install...
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/patina_bridge"
dist="$here/dist"
out="$dist/patina_bridge.zip"

[ -f "$src/__init__.py" ] || { echo "error: $src/__init__.py not found" >&2; exit 1; }
mkdir -p "$dist"
rm -f "$out"

# Byte-compile as a syntax check (output discarded).
if command -v python3 >/dev/null 2>&1; then
  python3 - "$src" <<'PY'
import pathlib, sys
for p in sorted(pathlib.Path(sys.argv[1]).glob("*.py")):
    compile(p.read_text(encoding="utf-8"), str(p), "exec")
PY
fi

if command -v zip >/dev/null 2>&1; then
  (cd "$here" && zip -qr -X "$out" patina_bridge -x '*/__pycache__/*' '*.pyc' '*/.DS_Store')
else
  python3 - "$here" "$out" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, dirs, files in os.walk(os.path.join(root, "patina_bridge")):
        dirs[:] = sorted(x for x in dirs if x != "__pycache__")
        for f in sorted(files):
            if f.endswith(".pyc") or f == ".DS_Store":
                continue
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
PY
fi

echo "wrote $out"
