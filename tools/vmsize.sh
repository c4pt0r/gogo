#!/bin/sh
# VM core size: compiles vm/*.c (except check32.c) for the ESP32-S3 with -Os
# and sums .text/.rodata/.literal — the same measure as the M3 budget
# (platform/esp32s3/vmsize.sh, which reads the IDF build instead).
#   tools/vmsize.sh            print the per-file table and the total; exit 1 above the limit
#   tools/vmsize.sh --record   also append "date,commit,bytes" to docs/vmsize-history.csv
#   tools/vmsize.sh --net      the net stack instead (net/*.c, not VM core): code
#                              size and its fixed state (.bss + .data); exit 1 if
#                              the fixed state exceeds 4096 bytes
# Skips (exit 0) when the xtensa toolchain is not installed.
set -e
LIMIT=65536
cd "$(dirname "$0")/.."
GCC=$(command -v xtensa-esp32s3-elf-gcc 2>/dev/null || ls "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | head -1 || true)
if [ -z "$GCC" ]; then echo "vmsize: skipped (xtensa-esp32s3-elf-gcc not found)"; exit 0; fi
SIZE=${GCC%gcc}size
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
if [ "$1" = "--net" ]; then
    total=0; fixed=0; lines=""
    for f in net/*.c; do
        o="$OUT/$(basename "$f" .c).o"
        "$GCC" -std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
            -DGOGO_ARENA_BYTES=163840u -Ivm -Inet -Ivm/test/stub -c "$f" -o "$o"
        n=$("$SIZE" -A "$o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
        b=$("$SIZE" -A "$o" | awk '/^\.(bss|data)/ {s += $2} END {print s + 0}')
        total=$((total + n)); fixed=$((fixed + b))
        lines="$lines$(printf '%7d  %s' "$n" "$f")
"
    done
    printf '%s' "$lines" | sort -rn
    printf '%7d  net code total (text+rodata+literal; not VM core)\n' "$total"
    printf '%7d  net fixed state (bss+data, limit 4096)\n' "$fixed"
    if [ "$fixed" -gt 4096 ]; then echo "netsize: fixed state exceeds 4096 bytes"; exit 1; fi
    exit 0
fi
total=0
lines=""
for f in vm/*.c; do
    [ "$f" = vm/check32.c ] && continue
    o="$OUT/$(basename "$f" .c).o"
    "$GCC" -std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
        -DGOGO_ARENA_BYTES=163840u -Ivm -Ivm/test/stub -c "$f" -o "$o"
    n=$("$SIZE" -A "$o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
    total=$((total + n))
    lines="$lines$(printf '%7d  %s' "$n" "$f")
"
done
printf '%s' "$lines" | sort -rn
printf '%7d  total (limit %d, %d%%)\n' "$total" "$LIMIT" $((total * 100 / LIMIT))
if [ "$1" = "--record" ]; then
    rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    [ -f docs/vmsize-history.csv ] || echo "date,commit,bytes" > docs/vmsize-history.csv
    echo "$(date +%Y-%m-%d),$rev,$total" >> docs/vmsize-history.csv
fi
if [ "$total" -gt "$LIMIT" ]; then echo "vmsize: VM core exceeds $LIMIT bytes"; exit 1; fi
