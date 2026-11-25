#!/bin/sh
# VM code size gate: sums the .text, .rodata and .literal sections (with
# -ffunction-sections these are .text.*, .rodata.*, .literal.*) of the
# runtime/vm/*.c objects in the ESP-IDF build and fails above the 64 KB budget.
#
#   platform/esp32s3/vmsize.sh [build-dir]    (default: platform/esp32s3/build)
#
# Needs xtensa-esp32s3-elf-size: in PATH after `. ~/esp/esp-idf/export.sh`,
# else taken from ~/.espressif, or set SIZE=/path/to/size.
set -eu
LIMIT=65536
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/build}

size=${SIZE:-}
if [ -z "$size" ]; then
    if command -v xtensa-esp32s3-elf-size >/dev/null 2>&1; then
        size=xtensa-esp32s3-elf-size
    else
        for s in "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-size; do
            [ -x "$s" ] && size=$s
        done
    fi
fi
[ -n "$size" ] || { echo "vmsize: xtensa-esp32s3-elf-size not found (source export.sh or set SIZE)" >&2; exit 2; }

objs=$(find "$build/esp-idf/main" -path '*/runtime/vm/*.c.obj' ! -name 'check32.c.obj' 2>/dev/null | sort)
[ -n "$objs" ] || { echo "vmsize: no runtime/vm/*.c objects under $build (run idf.py build first)" >&2; exit 2; }

# shellcheck disable=SC2086
total=$("$size" -A $objs | awk '
    $1 ~ /^\.(text|rodata|literal)(\.|$)/ { sum += $2 }
    END { print sum + 0 }')
n=$(echo "$objs" | wc -l | tr -d ' ')
echo "vm code: $total bytes (.text+.rodata+.literal of $n runtime/vm/*.c objects, limit $LIMIT)"
[ "$total" -le "$LIMIT" ] || { echo "vmsize: over the $LIMIT-byte budget" >&2; exit 1; }
