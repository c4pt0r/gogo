#!/bin/sh
# VM code size gate on a real ESP-IDF build: sums the linked sizes of the
# .text, .rodata and .literal input sections (.text.*, ... with
# -ffunction-sections) of the runtime/vm/*.c objects in the build's linker map
# (gogo.map), so the bytes are the ones the firmware carries: after
# relaxation (a long call is 3 bytes, its literal gone) and --gc-sections
# (functions the board never calls dropped). Fails above the 64 KB budget.
# The map gives the merged string pool (.rodata.str1.1) to whichever object
# comes first, usually not a VM one, so this reads about 0.4 KB under
# tools/vmsize.sh, which counts the VM's strings; the rest agrees within a
# few bytes.
#
#   platform/esp32s3/vmsize.sh [build-dir]    (default: platform/esp32s3/build)
set -eu
LIMIT=65536
here=$(cd "$(dirname "$0")" && pwd)
build=${1:-$here/build}
map=$build/gogo.map
[ -f "$map" ] || { echo "vmsize: no $map (run idf.py build first)" >&2; exit 2; }

# The archive members of libmain.a that are runtime/vm/*.c (no two sources
# of the component share a basename).
names=$(for f in "$here"/../../runtime/vm/*.c; do
    b=$(basename "$f"); [ "$b" = check32.c ] || printf '%s ' "$b"
done)
out=$(awk -v names="$names" '
    function hex(h,   v, i) {
        h = tolower(h); sub(/^0x/, "", h); v = 0
        for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
        return v
    }
    function add(a, s, f,   m) {
        if (hex(a) == 0 || !match(f, /libmain\.a\([^)]*\.c\.obj\)$/)) return
        m = substr(f, RSTART + 10, RLENGTH - 11); sub(/\.obj$/, "", m)
        if (m in vm) { sum += hex(s); seen[m] = 1 }
    }
    BEGIN { n = split(names, w, " "); for (i = 1; i <= n; i++) vm[w[i]] = 1 }
    /^Linker script and memory map/ { on = 1; next }
    !on { next }
    /^ \.(text|literal|rodata)[^ ]*$/ { sec = 1; next }
    $1 ~ /^\.(text|literal|rodata)/ && NF >= 4 && $2 ~ /^0x/ { add($2, $3, $4); sec = 0; next }
    sec && $1 ~ /^0x/ && NF >= 3 { add($1, $2, $3); sec = 0; next }
    { sec = 0 }
    END { k = 0; for (m in seen) k++; print sum + 0, k }' "$map")
total=${out% *}; n=${out#* }
[ "$n" -gt 0 ] || { echo "vmsize: no runtime/vm objects in $map" >&2; exit 2; }
echo "vm code: $total bytes (linked .text+.rodata+.literal of $n runtime/vm/*.c objects in gogo.map, limit $LIMIT)"
[ "$total" -le "$LIMIT" ] || { echo "vmsize: over the $LIMIT-byte budget" >&2; exit 1; }
