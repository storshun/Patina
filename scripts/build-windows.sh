#!/usr/bin/env bash
# Cross-compiles Patina for Windows x64 on Linux and packages the zip.
#
#   LLVM_MINGW=/opt/llvm-mingw scripts/build-windows.sh [out-dir]
#
# Needs an llvm-mingw toolchain (https://github.com/mstorsjo/llvm-mingw - unpack
# a release, or build it with its build-all.sh; with a system clang/lld use
# "--host-clang"). SDL3 is built from source for Windows the first time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
: "${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw toolchain root}"
WORK="${WORK:-$ROOT/build-win}"
SDL_SRC="${SDL_SRC:-}"
SDL_PREFIX="$WORK/sdl3"
VERSION="$(sed -n 's/^project(Patina VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
TC="$ROOT/cmake/toolchain-mingw-llvm.cmake"

mkdir -p "$WORK"
if [[ ! -f "$SDL_PREFIX/lib/libSDL3.a" ]]; then
  if [[ -z "$SDL_SRC" ]]; then
    SDL_SRC="$WORK/SDL"
    [[ -d "$SDL_SRC" ]] || git clone --depth 1 --branch release-3.2.24 https://github.com/libsdl-org/SDL.git "$SDL_SRC"
  fi
  echo "==> SDL3 for Windows"
  cmake -S "$SDL_SRC" -B "$WORK/sdl-build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DLLVM_MINGW="$LLVM_MINGW" \
    -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" >/dev/null
  cmake --build "$WORK/sdl-build" --target install >/dev/null
fi

echo "==> Patina"
cmake -S "$ROOT" -B "$WORK/patina" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DLLVM_MINGW="$LLVM_MINGW" \
  -DCMAKE_BUILD_TYPE=Release -DPATINA_BUILD_TESTS=OFF -DPATINA_FETCH_SDL3=OFF -DCMAKE_PREFIX_PATH="$SDL_PREFIX" >/dev/null
cmake --build "$WORK/patina"

echo "==> Package"
NAME="patina-$VERSION-windows-x64"
STAGE="$OUT/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cmake --install "$WORK/patina" --prefix "$STAGE" >/dev/null
"$LLVM_MINGW/bin/llvm-strip" "$STAGE/patina.exe" "$STAGE/patina-cli.exe"
python3 "$ROOT/scripts/shrink_images.py" "$STAGE/docs/manual/img"  # (1.0: 256-color PNGs, a third the size)
[[ -d "$ROOT/web/dist" ]] || python3 "$ROOT/scripts/build_web.py"
rm -rf "$STAGE/web" && cp -r "$ROOT/web/dist" "$STAGE/web"
bash "$ROOT/blender/make_zip.sh" >/dev/null
cp "$ROOT/blender/dist/patina_bridge.zip" "$STAGE/"
cp "$ROOT/packaging/windows/install.ps1" "$ROOT/packaging/windows/INSTALL-WINDOWS.txt" "$STAGE/"
cp "$ROOT/CHANGELOG.md" "$STAGE/CHANGELOG.md"  # (1.2: what changed, for the people who use it)
mkdir -p "$STAGE/LICENSES"
cp "$ROOT/web/vendor/three/LICENSE" "$STAGE/LICENSES/three.js.txt"
[[ -f "$ROOT/third_party/LICENSES.md" ]] && cp "$ROOT/third_party/LICENSES.md" "$STAGE/LICENSES/"
cp "$ROOT/third_party/httplib/LICENSE" "$STAGE/LICENSES/cpp-httplib.txt"
cp "$ROOT/third_party/tinyexr/LICENSE" "$STAGE/LICENSES/tinyexr.txt"
cp "$ROOT/third_party/tinyexr/LICENSE.miniz" "$STAGE/LICENSES/miniz.txt"
cp "$ROOT/third_party/ufbx/LICENSE" "$STAGE/LICENSES/ufbx.txt"
cp "$ROOT/third_party/llama.cpp/LICENSE" "$STAGE/LICENSES/llama.cpp.txt"
cp "$ROOT/third_party/quadriflow/LICENSE.txt" "$STAGE/LICENSES/quadriflow.txt"
cp "$ROOT/third_party/lemon/LICENSE" "$STAGE/LICENSES/lemon.txt"
cp "$ROOT/third_party/libsatsuma/LICENSE" "$STAGE/LICENSES/libsatsuma.txt"
cp "$ROOT/third_party/libtimekeeper/LICENSE" "$STAGE/LICENSES/libtimekeeper.txt"
cp "$ROOT/third_party/meshoptimizer/LICENSE.md" "$STAGE/LICENSES/meshoptimizer.txt"
cp "$ROOT/third_party/xatlas/LICENSE" "$STAGE/LICENSES/xatlas.txt"
cp "$ROOT/third_party/eigen/COPYING.MPL2" "$STAGE/LICENSES/eigen-MPL2.txt"
sed -i "s/@VERSION@/$VERSION/g" "$STAGE/INSTALL-WINDOWS.txt"
# Windows line endings for the text files people open in Notepad
for f in "$STAGE/INSTALL-WINDOWS.txt" "$STAGE/install.ps1"; do sed -i 's/$/\r/' "$f"; done
( cd "$OUT" && rm -f "$NAME.zip" && python3 -c "
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + '.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, files in os.walk(name):
        for f in sorted(files):
            p = os.path.join(d, f)
            z.write(p, p)
" "$NAME" )
ls -la "$OUT/$NAME.zip"
