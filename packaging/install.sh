#!/usr/bin/env bash
# Installs the prebuilt Patina into a prefix (default ~/.local).
#   ./install.sh                 # -> ~/.local/bin/patina
#   PREFIX=/opt/patina ./install.sh
#   ./install.sh --uninstall
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

sizes=(16 24 32 48 64 128 256 512)
files=(bin/patina share/applications/patina.desktop share/icons/hicolor/scalable/apps/patina.svg)
for s in "${sizes[@]}"; do files+=("share/icons/hicolor/${s}x${s}/apps/patina.png"); done
dirs=(share/patina/materials share/patina/blender share/patina/web share/patina/samples share/doc/patina)

if [[ "${1:-}" == "--uninstall" ]]; then
  for f in "${files[@]}"; do rm -fv "$PREFIX/$f"; done
  for d in "${dirs[@]}"; do if [[ -d "$PREFIX/$d" ]]; then rm -rf "$PREFIX/$d"; echo "removed $PREFIX/$d"; fi; done
  rmdir "$PREFIX/share/patina" 2>/dev/null || true
  command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
  echo "Patina removed from $PREFIX (your settings in ~/.config/patina and libraries in ~/.local/share/patina/user are kept)."
  exit 0
fi

echo "==> Installing Patina to $PREFIX"
mkdir -p "$PREFIX/bin" "$PREFIX/share/applications" "$PREFIX/share/doc"
install -m 755 "$here/bin/patina" "$PREFIX/bin/patina"
# replace only the shipped data; ~/.local/share/patina/user (your materials and brushes) is left alone
mkdir -p "$PREFIX/share/patina"
for d in materials blender web samples; do
  rm -rf "$PREFIX/share/patina/$d"
  [[ -d "$here/share/patina/$d" ]] && cp -r "$here/share/patina/$d" "$PREFIX/share/patina/$d"
done
rm -rf "$PREFIX/share/doc/patina"
cp -r "$here/share/doc/patina" "$PREFIX/share/doc/patina"
# the application icon (the scalable one of versions before 0.7.3 goes: it would win over these)
rm -f "$PREFIX/share/icons/hicolor/scalable/apps/patina.svg"
for s in "${sizes[@]}"; do
  mkdir -p "$PREFIX/share/icons/hicolor/${s}x${s}/apps"
  install -m 644 "$here/share/icons/hicolor/${s}x${s}/apps/patina.png" "$PREFIX/share/icons/hicolor/${s}x${s}/apps/patina.png"
done
# absolute Exec path: desktop sessions do not always have ~/.local/bin on PATH
sed "s|^Exec=patina|Exec=$PREFIX/bin/patina|" "$here/share/applications/patina.desktop" \
  > "$PREFIX/share/applications/patina.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q "$PREFIX/share/icons/hicolor" 2>/dev/null || true

# runtime checks (Fedora package names)
missing=()
LDC="$(command -v ldconfig || true)"
[[ -z "$LDC" && -x /sbin/ldconfig ]] && LDC=/sbin/ldconfig
[[ -z "$LDC" && -x /usr/sbin/ldconfig ]] && LDC=/usr/sbin/ldconfig
if [[ -n "$LDC" ]]; then
  libs="$("$LDC" -p 2>/dev/null || true)"
  grep -q 'libEGL.so.1'  <<<"$libs" || missing+=(libglvnd-egl)
  grep -q 'libGL.so.1'   <<<"$libs" || missing+=(libglvnd-glx)
  grep -q 'libX11.so.6'  <<<"$libs" || missing+=(libX11)
  grep -q 'libXi.so.6'   <<<"$libs" || missing+=(libXi)
  grep -q 'libSDL3.so.0' <<<"$libs" || missing+=(SDL3)
fi
command -v zenity >/dev/null || command -v kdialog >/dev/null || missing+=(zenity)
if ((${#missing[@]})); then
  echo
  echo "!! Missing runtime packages: ${missing[*]}"
  echo "   sudo dnf install ${missing[*]} mesa-dri-drivers"
fi
# a system older than the one this was built on: say so now, not when the menu entry silently fails
if ! out="$("$PREFIX/bin/patina" --version 2>&1)" && grep -q 'GLIBC' <<<"$out"; then
  echo
  echo "!! This build needs a newer C library (glibc) than this system has:"
  grep -o 'GLIBC_[0-9.]*' <<<"$out" | sort -u | sed 's/^/     /'
  echo "   Build Patina from the source download instead (see INSTALL.txt)."
fi

cat <<EOF

Done.
  Run:            $PREFIX/bin/patina            (or "Patina" in your application menu)
  Manual:         $PREFIX/share/doc/patina/manual/index.html   (Help > Manual in the app)
  Patina Web:     $PREFIX/share/patina/web/index.html          (browser painter, works offline)
  Blender add-on: $here/patina_bridge.zip
                  Blender 4.2+: Edit > Preferences > Get Extensions > (v) > Install from Disk...
                  Blender 3.6-4.1: Edit > Preferences > Add-ons > Install...
  Claude app:     $PREFIX/bin/patina --setup-claude   (or Blender Link panel > Connect to Claude app)
  Claude Code:    claude mcp add patina -- $PREFIX/bin/patina --mcp
  Uninstall:      $here/install.sh --uninstall
EOF
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "  Note: $PREFIX/bin is not on your PATH." ;; esac
