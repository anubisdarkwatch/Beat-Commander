#!/usr/bin/env bash
# Builds and runs a host screen preview. Usage: tools/build_preview.sh <M5GFX dir> <out dir> [preview_dial.cpp | preview_stick.cpp]
set -e
GFX="$1/src"; OUT="$2"; HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/obj"
SRCS=$(find "$GFX" -maxdepth 1 -name "*.cpp"; find "$GFX/lgfx/v1" -maxdepth 1 -name '*.cpp'; find "$GFX/lgfx/v1/misc" "$GFX/lgfx/v1/panel" "$GFX/lgfx/v1/platforms/sdl" -name '*.cpp'; find "$GFX/lgfx/utility" "$GFX/lgfx/Fonts" -name "*.c")
for s in $SRCS; do o="$OUT/obj/$(echo "$s" | md5sum | cut -c1-12).o"; [ -f "$o" ] || g++ -x c++ -std=c++17 -O1 -w -I"$GFX" $(sdl2-config --cflags) -c "$s" -o "$o"; done
g++ -std=c++17 -O1 -w -I"$GFX" $(sdl2-config --cflags) "$HERE/${3:-preview_dial.cpp}" "$OUT"/obj/*.o $(sdl2-config --libs) -lpthread -o "$OUT/preview"
cd "$OUT" && ./preview
