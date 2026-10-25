#!/usr/bin/env bash
# Builds the binary release tarball from an already configured+built build dir.
#   scripts/package-release.sh [build-dir] [out-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build-release}"
OUT="${2:-$ROOT/dist}"
VERSION="$(sed -n 's/^project(Patina VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
NAME="patina-$VERSION-linux-x86_64"
STAGE="$OUT/$NAME"

rm -rf "$STAGE"
mkdir -p "$STAGE"
# Patina Web with its material library baked again by this build, in a home of its own (1.2.0 went out with
# the library of 0.26, which still had the British names: only a missing bundle used to rebuild it)
WEBHOME="$(mktemp -d)"
HOME="$WEBHOME" XDG_DATA_HOME="$WEBHOME/data" XDG_CONFIG_HOME="$WEBHOME/config" XDG_CACHE_HOME="$WEBHOME/cache" \
  EGL_LOG_LEVEL=fatal python3 "$ROOT/scripts/build_web.py" --patina "$BUILD/patina"
rm -rf "$WEBHOME"
cmake --install "$BUILD" --prefix "$STAGE" >/dev/null
strip "$STAGE/bin/patina"
python3 "$ROOT/scripts/shrink_images.py" "$STAGE/share/doc/patina/manual/img"  # (1.0: 256-color PNGs, a third the size)
bash "$ROOT/blender/make_zip.sh" >/dev/null
cp "$ROOT/blender/dist/patina_bridge.zip" "$STAGE/"
install -m 755 "$ROOT/packaging/install.sh" "$STAGE/install.sh"
# what the binary needs: the newest glibc symbol version it uses, and the system it was built on
GLIBC="$(objdump -T "$STAGE/bin/patina" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -V | tail -1)"
BUILT_ON="$(. /etc/os-release && echo "$NAME $VERSION_ID")"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@GLIBC@/$GLIBC/g" -e "s/@BUILT_ON@/$BUILT_ON/g" \
  "$ROOT/packaging/INSTALL.txt" > "$STAGE/INSTALL.txt"
cp "$ROOT/README.md" "$STAGE/README.md"
cp "$ROOT/CHANGELOG.md" "$STAGE/CHANGELOG.md"  # (1.2: what changed, for the people who use it)
mkdir -p "$STAGE/share/doc/patina"
cp "$ROOT/blender/README.md" "$STAGE/share/doc/patina/BLENDER_ADDON.md"
cp "$ROOT/docs/PLAN.md" "$ROOT/docs/workflow.png" "$ROOT/third_party/LICENSES.md" "$STAGE/share/doc/patina/"
cp "$ROOT/third_party/httplib/LICENSE" "$STAGE/share/doc/patina/LICENSE.cpp-httplib"
cp "$ROOT/third_party/quadriflow/LICENSE.txt" "$STAGE/share/doc/patina/LICENSE.quadriflow"
cp "$ROOT/third_party/lemon/LICENSE" "$STAGE/share/doc/patina/LICENSE.lemon"
cp "$ROOT/third_party/libsatsuma/LICENSE" "$STAGE/share/doc/patina/LICENSE.libsatsuma"
cp "$ROOT/third_party/libtimekeeper/LICENSE" "$STAGE/share/doc/patina/LICENSE.libtimekeeper"
cp "$ROOT/third_party/meshoptimizer/LICENSE.md" "$STAGE/share/doc/patina/LICENSE.meshoptimizer"
cp "$ROOT/third_party/xatlas/LICENSE" "$STAGE/share/doc/patina/LICENSE.xatlas"
cp "$ROOT/third_party/eigen/COPYING.MPL2" "$STAGE/share/doc/patina/LICENSE.eigen-MPL2"
cp "$ROOT/third_party/tinyexr/LICENSE" "$STAGE/share/doc/patina/LICENSE.tinyexr"
cp "$ROOT/third_party/tinyexr/LICENSE.miniz" "$STAGE/share/doc/patina/LICENSE.miniz"
cp "$ROOT/third_party/ufbx/LICENSE" "$STAGE/share/doc/patina/LICENSE.ufbx"
cp "$ROOT/third_party/llama.cpp/LICENSE" "$STAGE/share/doc/patina/LICENSE.llama.cpp"

# (1.2.10) install.sh copies every shipped data folder: a new one must be added to its list
for d in "$STAGE"/share/patina/*/; do
  d="$(basename "$d")"
  grep -q "^data=(.*\b$d\b" "$STAGE/install.sh" || { echo "install.sh does not install share/patina/$d" >&2; exit 1; }
done

tar -C "$OUT" -czf "$OUT/$NAME.tar.gz" "$NAME"
git -C "$ROOT" archive --format=tar.gz --prefix="patina-$VERSION/" -o "$OUT/patina-$VERSION-src.tar.gz" HEAD
( cd "$OUT" && sha256sum "$NAME.tar.gz" "patina-$VERSION-src.tar.gz" > SHA256SUMS )
ls -la "$OUT"
