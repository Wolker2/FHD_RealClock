#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
dest="${1:-FHDRealClock.dll}"
"${CC:-i686-w64-mingw32-gcc}" -std=c11 -O2 -Wall -Wextra -Werror \
    -ffreestanding -fno-stack-protector -shared -nostdlib \
    -Wl,--entry,_DllMain@12,--no-insert-timestamp -s \
    fhd_clock.c clock_geometry.c clock_settings.c fhd_clock.def -lkernel32 -luser32 -o "$dest"
