#!/bin/sh
# Usage: CC=/path/to/i686-w64-mingw32-gcc sh build.sh
cd "$(dirname "$0")" || exit 1
"${CC:-i686-w64-mingw32-gcc}" -shared -O2 -s -Wall -Wno-unused-function -Wno-dll-attribute-on-redeclaration \
    ddraw.c -o ../../game/ddraw.dll -lgdi32 -luser32 -Wl,--kill-at
