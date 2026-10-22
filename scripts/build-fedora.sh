#!/usr/bin/env bash
# Build, test and install Patina on Fedora.
#   ./scripts/build-fedora.sh            # deps + build + tests + install to ~/.local
#   ./scripts/build-fedora.sh --no-deps  # skip dnf
#   PREFIX=/usr/local ./scripts/build-fedora.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
PREFIX="${PREFIX:-$HOME/.local}"
JOBS="${JOBS:-$(nproc)}"

if [[ "${1:-}" != "--no-deps" ]]; then
  echo "==> Installing build dependencies (sudo dnf)"
  PKGS=(gcc-c++ cmake ninja-build git pkgconf-pkg-config
        mesa-libGL-devel mesa-libEGL-devel libglvnd-devel
        zenity)
  # SDL3 is packaged from Fedora 41 on; older releases build it from source
  if dnf list --available SDL3-devel >/dev/null 2>&1 || rpm -q SDL3-devel >/dev/null 2>&1; then
    PKGS+=(SDL3-devel)
  else
    echo "    SDL3-devel not available - SDL 3 will be built from source (needs the X11/Wayland headers below)"
    PKGS+=(libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel
           libXinerama-devel libXScrnSaver-devel wayland-devel wayland-protocols-devel libxkbcommon-devel
           libdecor-devel)
  fi
  sudo dnf install -y "${PKGS[@]}"
fi

echo "==> Configuring ($BUILD)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"

echo "==> Building"
cmake --build "$BUILD" -j "$JOBS"

echo "==> Tests"
( cd "$BUILD" && ctest --output-on-failure ) || {
  echo "!! Some tests failed. CPU tests (core, uv) must pass; the gpu test needs a working EGL driver."
}

echo "==> Installing to $PREFIX"
cmake --install "$BUILD"

echo "==> Packaging the Blender add-on"
bash "$ROOT/blender/make_zip.sh" >/dev/null && echo "    $ROOT/blender/dist/patina_bridge.zip"

cat <<EOF

Done.
  Run:            $PREFIX/bin/patina            (make sure $PREFIX/bin is on PATH)
  Blender add-on: Edit > Preferences > Add-ons > (v) > Install from Disk... > blender/dist/patina_bridge.zip
  Claude Code:    claude mcp add patina -- $PREFIX/bin/patina --mcp
EOF
