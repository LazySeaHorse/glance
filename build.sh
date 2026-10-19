#!/usr/bin/env bash
# Cross-compiles Glance.exe for 64-bit Windows using mingw-w64.
#   sudo apt install mingw-w64
#   ./build.sh            -> build/Glance.exe
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CXX=x86_64-w64-mingw32-g++-posix
WINDRES=x86_64-w64-mingw32-windres

$WINDRES -I res res/glance.rc -O coff -o build/glance.res.o

$CXX -std=c++17 -O2 -municode -mwindows -static -s \
  -Wall -Wno-unknown-pragmas -Wno-missing-braces \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
  src/main.cpp src/loader.cpp build/glance.res.o \
  -o build/Glance.exe \
  -ld2d1 -ld3d11 -ldxgi -ldwrite -lwindowscodecs -lole32 -loleaut32 -luuid \
  -lshlwapi -lshell32 -lcomdlg32 -luser32 -lgdi32 -ladvapi32

ls -la build/Glance.exe
