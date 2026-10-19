#!/usr/bin/env bash
# Cross-compiles Glance.exe for 64-bit Windows using mingw-w64.
#   sudo apt install mingw-w64
#   ./build.sh                          -> build/Glance.exe (version from ./VERSION)
#   GLANCE_VERSION=1.2.3 ./build.sh     -> stamp a specific version
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CXX=x86_64-w64-mingw32-g++-posix
WINDRES=x86_64-w64-mingw32-windres

VERSION="${GLANCE_VERSION:-$(tr -d ' \r\n' < VERSION)}"
# Numeric part for the Windows version resource: first three numbers, e.g. "1.2.3-dev+abc" -> 1,2,3
IFS=. read -r V1 V2 V3 <<< "$(grep -oE '^[0-9]+(\.[0-9]+){0,2}' <<< "$VERSION" || echo 0)"
V1=${V1:-0}; V2=${V2:-0}; V3=${V3:-0}
cat > build/version.h <<H
#define GV_MAJ $V1
#define GV_MIN $V2
#define GV_PAT $V3
#define GV_STR "$VERSION"
#define GV_WSTR L"$VERSION"
H

$WINDRES -I res -I build res/glance.rc -O coff -o build/glance.res.o

# --no-insert-timestamp: identical sources give a byte-identical exe.
$CXX -std=c++17 -Os -municode -mwindows -static -s -Wl,--no-insert-timestamp \
  -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -Ibuild \
  src/main.cpp src/loader.cpp src/fileops.cpp build/glance.res.o \
  -o build/Glance.exe \
  -ld2d1 -ld3d11 -ldxgi -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid \
  -lshlwapi -lshell32 -lcomdlg32 -luser32 -lgdi32 -ladvapi32

echo "Glance $VERSION"
ls -la build/Glance.exe
