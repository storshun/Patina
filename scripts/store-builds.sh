#!/usr/bin/env bash
# Puts this version's release files in the builds folder and moves earlier versions' files into its "Old Builds".
#   scripts/store-builds.sh [dist-dir] [builds-dir] [--web path/to/patina-<ver>-web-library.7z] [--release]
# Copies patina-<ver>-linux-x86_64.tar.gz, patina-<ver>-windows-x64.zip and the Blender add-on as
# patina_bridge-<add-on ver>.zip. Any other version's patina-* files and any other add-on zip move to "Old Builds"
# (web libraries only when a new one comes with --web). Folders (extracted builds) and other files stay put.
# (1.0) --release also keeps this version in the builds folder's "releases": the Linux and Windows packages, the
# source, the add-on and their checksums, which no later version moves away.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB=""
RELEASE=0
ARGS=()
while (($#)); do
  if [[ "$1" == --web ]]; then WEB="$2"; shift 2
  elif [[ "$1" == --release ]]; then RELEASE=1; shift
  else ARGS+=("$1"); shift; fi
done
DIST="${ARGS[0]:-$ROOT/dist}"
DEST="${ARGS[1]:-$HOME/Downloads/Patina Builds}"
OLD="$DEST/Old Builds"
VERSION="$(sed -n 's/^project(Patina VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
ADDON="$(sed -n 's/^version = "\([0-9.]*\)".*/\1/p' "$ROOT/blender/patina_bridge/blender_manifest.toml")"

NAMES=("patina-$VERSION-linux-x86_64.tar.gz" "patina-$VERSION-windows-x64.zip" "patina_bridge-$ADDON.zip")
SRCS=("$DIST/${NAMES[0]}" "$DIST/${NAMES[1]}" "$ROOT/blender/dist/patina_bridge.zip")
if [[ -n "$WEB" ]]; then NAMES+=("patina-$VERSION-web-library.7z"); SRCS+=("$WEB"); fi
for s in "${SRCS[@]}"; do [[ -f "$s" ]] || { echo "missing: $s" >&2; exit 1; }; done

mkdir -p "$OLD"
shopt -s nullglob
for f in "$DEST"/patina-* "$DEST"/patina_bridge-*; do
  n="$(basename "$f")"
  [[ -f "$f" ]] || continue
  case "$n" in
    "patina-$VERSION-"* | "patina_bridge-$ADDON.zip") continue ;;
    patina-*-web-library.7z) [[ -n "$WEB" ]] || continue ;;
  esac
  if [[ -e "$OLD/$n" ]]; then echo "left (already in Old Builds): $n"; continue; fi
  mv -- "$f" "$OLD/"
  echo "moved to Old Builds: $n"
done

for i in "${!NAMES[@]}"; do
  if cmp -s "${SRCS[$i]}" "$DEST/${NAMES[$i]}"; then echo "already there: ${NAMES[$i]}"; continue; fi
  cp -- "${SRCS[$i]}" "$DEST/${NAMES[$i]}"
  echo "stored: ${NAMES[$i]}"
done

if ((RELEASE)); then
  REL="$DEST/releases"
  mkdir -p "$REL"
  SRC="$DIST/patina-$VERSION-src.tar.gz"
  [[ -f "$SRC" ]] || { echo "missing: $SRC" >&2; exit 1; }
  RNAMES=("${NAMES[@]}" "patina-$VERSION-src.tar.gz")
  RSRCS=("${SRCS[@]}" "$SRC")
  for i in "${!RNAMES[@]}"; do
    if cmp -s "${RSRCS[$i]}" "$REL/${RNAMES[$i]}"; then echo "already in releases: ${RNAMES[$i]}"; continue; fi
    cp -- "${RSRCS[$i]}" "$REL/${RNAMES[$i]}"
    echo "release: ${RNAMES[$i]}"
  done
  (cd "$REL" && sha256sum "${RNAMES[@]}" > "patina-$VERSION-SHA256SUMS.txt")
  echo "release: patina-$VERSION-SHA256SUMS.txt"
fi
