#!/bin/sh
# Code size of the VM core (and of the other runtime libraries) as built into
# the ESP32-S3 firmware.
#
# Method ("linked", the default): compile runtime/vm/*.c (except check32.c)
# with the firmware's flags (FW_FLAGS / FW_DEFS below), link the objects with
# relaxation and --gc-sections (xtensa-esp32s3-elf-gcc -nostdlib; unresolved
# libc symbols are ignored) and sum .text + .rodata + .literal of the result.
# The relaxed link turns each -mlongcalls call (l32r + callx8 + a 4-byte
# literal) back into a 3-byte call8, as the firmware link does, so sharing a
# helper across files costs what it costs on the board. Checked against the
# StickS3 IDF build's gogo.map: within about 1% (see docs/design.md, 5.10).
#
# gc-sections roots: the public API of the library, derived, not listed:
# every global symbol the objects define that code outside the library names:
# platform/, runtime/* (other libraries, runtime/embed = libgogo) and embed/,
# tests and tools excluded. A name used only inside a static inline function
# or function-like macro of the library's own headers counts when code outside
# names that inline or macro (e.g. vm_write -> vm_out). So test-only exports,
# which nothing outside calls, drop out, as --gc-sections drops them in the
# firmware. Functions the host build reaches but the board does not (libgogo's
# API) stay: the measure is the library's API, not one board's use of it.
#
#   tools/vmsize.sh            print the per-file table (each object's bytes in the
#                              linked result, from the linker map; the rows add up to
#                              the total), the total and the line count of the same
#                              files; exit 1 above the limit
#   tools/vmsize.sh --record   also append "date,commit,bytes,lines,method" to
#                              docs/vmsize-history.csv (lines: of those .c files, a
#                              trend only, no limit; method: linked)
#   tools/vmsize.sh --objects  the old measure, for comparison: the sum of the object
#                              files' sections, -Os -mlongcalls, no firmware flags, no
#                              link (rows of docs/vmsize-history.csv with method
#                              "objects"); combines with --no-float
#   tools/vmsize.sh --no-float the VM core built without floating point (-DGOGO_FLOAT=0,
#                              runtime/vm/config.h): the size of firmware without the
#                              "float" requirement (never recorded; the record and the
#                              limit are for the default build, floats on)
#   tools/vmsize.sh --roots    print the gc-sections roots of the VM core and exit
#   tools/vmsize.sh --check-flags BUILD
#                              compare FW_FLAGS with the code-generation flags the
#                              IDF build in BUILD (its compile_commands.json) gives
#                              runtime/vm/interp.c; exit 1 if they differ
# The other libraries, measured the same linked way (roots: what code outside
# that directory names); their fixed state (.bss + .data) is summed over the
# objects before the link, as before, so a gate on it never depends on gc:
#   tools/vmsize.sh --floatlib the float library (runtime/floatlib/*.c, not VM core): its
#                              code size, and fixed state (limit 0)
#   tools/vmsize.sh --net      the net stack instead (runtime/net/*.c, not VM core): code
#                              size and its fixed state (sizeof(NetState), the
#                              board's storage, plus any .bss + .data); exit 1 if
#                              the fixed state exceeds 4096 bytes
#   tools/vmsize.sh --vfs      the vfs core instead (runtime/vfs/*.c): code size and fixed
#                              state (.bss + .data); exit 1 above 3072 bytes
#   tools/vmsize.sh --screen   the "screen" port instead (runtime/screen/*.c, not VM core):
#                              code size and fixed state (.bss + .data, limit 0: its
#                              grid is allocated per open port, charged to the owner)
#   tools/vmsize.sh --dist     the "node" port instead (runtime/dist/*.c, not VM core): code
#                              size and fixed state (.bss + .data, limit 0: its state
#                              is allocated at the first node link)
#   tools/vmsize.sh --tls      the "tls" port (runtime/net/tls/tls.c) and the mbedTLS it links
#                              ($MBEDTLS_DIR, runtime/net/tls/config): linked with
#                              --gc-sections from net_tls_driver, so only what the
#                              port reaches counts (not VM core; informational). It
#                              keeps its own flags: the firmware links IDF's mbedTLS
#                              (hardware crypto, another config), not this build.
# Skips (exit 0) when the xtensa toolchain is not installed.
set -e
LIMIT=65536
cd "$(dirname "$0")/.."

# The firmware's code-generation flags for runtime/vm/*.c, in one place: the
# StickS3 IDF build's compile_commands.json and toolchain/cflags (ESP-IDF 6.1,
# esp-15.2.0), less warnings, debug info, include paths and path maps.
# `tools/vmsize.sh --check-flags <idf-build-dir>` checks them against a build.
FW_FLAGS="-std=c99 -Os -mlongcalls -mdisable-hardware-atomics -ffunction-sections -fdata-sections
          -freorder-blocks -mno-target-align -fstrict-volatile-bitfields -fno-jump-tables
          -fno-tree-switch-conversion -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-bzero
          -fzero-init-padding-bits=all -fno-malloc-dce"
# The ESP values of the runtime/vm/config.h knobs (platform/esp32s3/main/CMakeLists.txt).
FW_DEFS="-DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_MAX_PORTS=16u -DGOGO_EVQ_CAP=64u
         -DGOGO_ARENA_BYTES=163840u"
# The old measure (--objects), unchanged.
OBJ_FLAGS="-std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u"

MODE=vm; OBJECTS=0; FLOAT=1; RECORD=0; BUILD=""
while [ $# -gt 0 ]; do
    case $1 in
        --net|--vfs|--screen|--floatlib|--tls|--dist|--roots) MODE=${1#--} ;;
        --check-flags) MODE=check-flags; BUILD=${2:?--check-flags needs the IDF build directory}; shift ;;
        --objects) OBJECTS=1 ;;
        --no-float) FLOAT=0 ;;
        --record) RECORD=1 ;;
        *) echo "vmsize: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
if [ "$RECORD" = 1 ] && { [ "$OBJECTS" = 1 ] || [ "$FLOAT" = 0 ] || [ "$MODE" != vm ]; }; then
    echo "vmsize: --record is for the default measure only" >&2; exit 2
fi

if [ "$MODE" = check-flags ]; then
    cc="$BUILD/compile_commands.json"
    [ -f "$cc" ] || { echo "vmsize: no $cc (run idf.py build first)" >&2; exit 2; }
    cmd=$(grep '"command":.*runtime/vm/interp\.c' "$cc" | head -1)
    [ -n "$cmd" ] || { echo "vmsize: runtime/vm/interp.c not in $cc" >&2; exit 2; }
    # @"<file>" response files (toolchain/cflags) hold more flags.
    rsp=$(printf '%s\n' "$cmd" | tr ' ' '\n' | sed -n 's/^@\\*"*\([^"\\]*\)\\*"*$/\1/p')
    # Code generation only: -O, -std (the last one wins), -f and -m, less
    # diagnostics and path maps.
    have=$( { printf '%s\n' "$cmd" | tr ' ' '\n'; for r in $rsp; do cat "$r"; done; } |
        grep -E '^-(O|std=|f|m)' | grep -Ev '^-f(diagnostics-color|macro-prefix-map|file-prefix-map|debug-prefix-map)' |
        awk '/^-std=/ {std = $0; next} {print} END {if (std != "") print std}' | LC_ALL=C sort -u)
    # shellcheck disable=SC2086 # one flag a line
    want=$(printf '%s\n' $FW_FLAGS | LC_ALL=C sort -u)
    if [ "$have" = "$want" ]; then echo "vmsize: FW_FLAGS match $BUILD"; exit 0; fi
    echo "vmsize: FW_FLAGS differ from $BUILD (< only in FW_FLAGS, > only in the IDF build):"
    printf '%s\n' "$want" > "${TMPDIR:-/tmp}/vmsize-want.$$"
    printf '%s\n' "$have" | diff "${TMPDIR:-/tmp}/vmsize-want.$$" - | grep '^[<>]' || true
    rm -f "${TMPDIR:-/tmp}/vmsize-want.$$"
    exit 1
fi

# shellcheck disable=SC2012 # the first match of a glob
GCC=$(command -v xtensa-esp32s3-elf-gcc 2>/dev/null || ls "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | head -1 || true)
if [ -z "$GCC" ]; then echo "vmsize: skipped (xtensa-esp32s3-elf-gcc not found)"; exit 0; fi
SIZE=${GCC%gcc}size
NM=${GCC%gcc}nm
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# sections FILE...: .text + .rodata + .literal (with -ffunction-sections also
# .text.*, ...) of the files; fixed FILE...: their .bss + .data.
sections() { "$SIZE" -A "$@" | awk '$1 ~ /^\.(text|rodata|literal)(\.|$)/ {s += $2} END {print s + 0}'; }
fixed_of() { "$SIZE" -A "$@" | awk '$1 ~ /^\.(bss|data)(\.|$)/ {s += $2} END {print s + 0}'; }

# roots DIR OBJDIR: the gc-sections roots of the library in DIR (header
# comment): the global symbols defined in OBJDIR/*.o that the consumers name.
roots() {
    # The consumers: C sources and headers outside DIR's own files.
    find platform runtime embed \( -name test -o -name tools -o -name managed_components -o -name components \) -prune \
        -o \( -name '*.c' -o -name '*.h' \) -print | while read -r f; do
        case $f in "$1"/*/*) echo "$f" ;; "$1"/*) ;; *) echo "$f" ;; esac
    done > "$2/consumers"
    xargs cat < "$2/consumers" | tr -cs 'A-Za-z0-9_' '\n' | LC_ALL=C sort -u > "$2/names"
    # "name word": word is named in the body of DIR's static inline function
    # or function-like macro "name".
    cat "$1"/*.h | awk '
        function emit(s,   n, i, w) {
            n = split(s, w, /[^A-Za-z0-9_]+/)
            for (i = 1; i <= n; i++) if (w[i] ~ /^[A-Za-z_]/ && w[i] != name) print name, w[i]
        }
        function braces(s,   t, o) { t = s; o = gsub(/\{/, "", t); t = s; return o - gsub(/\}/, "", t) }
        mac { emit($0); mac = /\\$/; next }
        depth > 0 { emit($0); depth += braces($0); next }
        pend { emit($0); if (index($0, "{")) { pend = 0; depth = braces($0) } ; next }
        /^#define [A-Za-z_][A-Za-z0-9_]*\(/ {
            name = $2; sub(/\(.*/, "", name); emit(substr($0, length($1 $2) + 2)); mac = /\\$/; next
        }
        /static inline/ {
            s = $0; sub(/\(.*/, "", s); n = split(s, w, /[^A-Za-z0-9_]+/); name = w[n]
            emit(substr($0, index($0, "("))); depth = braces($0); pend = !index($0, "{"); next
        }' > "$2/inline"
    "$NM" -g --defined-only "$2"/*.o | awk 'NF == 3 {print $3}' | LC_ALL=C sort -u > "$2/exports"
    # Names reached through inlines and macros, to a fixed point; then the
    # exports among the names.
    awk 'FILENAME == ARGV[1] {body[$1] = body[$1] " " $2; next}
         FILENAME == ARGV[2] {named[$1] = 1; next}
         {def[$1] = 1}
         END {
             do {
                 more = 0
                 for (n in body) if (named[n] && !done[n]) {
                     done[n] = 1; k = split(body[n], w, " ")
                     for (i = 1; i <= k; i++) if (!named[w[i]]) { named[w[i]] = 1; more = 1 }
                 }
             } while (more)
             for (s in def) if (named[s]) print s
         }' "$2/inline" "$2/names" "$2/exports" | LC_ALL=C sort
}

# linked NAME DIR CFLAGS SRC...: compile SRC with the firmware flags into
# $OUT/NAME, link from the roots, print the per-file table; sets code (the
# linked .text + .rodata + .literal) and fixed (.bss + .data of the objects).
linked() {
    name=$1; dir=$2; cflags=$3; shift 3
    d=$OUT/$name; mkdir -p "$d"
    for f in "$@"; do
        # shellcheck disable=SC2086
        "$GCC" $FW_FLAGS $FW_DEFS $cflags -c "$f" -o "$d/$(basename "$f" .c).o"
    done
    roots "$dir" "$d" > "$d/roots"
    # shellcheck disable=SC2046
    "$GCC" -nostdlib -Wl,--gc-sections -Wl,-e,0 -Wl,--unresolved-symbols=ignore-all \
        $(sed 's/^/-Wl,-u,/' "$d/roots") -Wl,-Map,"$d/map" -o "$d/linked.elf" "$d"/*.o
    code=$(sections "$d/linked.elf")
    fixed=$(fixed_of "$d"/*.o)
    # Each input section's linked size, by object; merged string constants
    # (one pool for all objects, which the map gives to the first) and
    # alignment padding on rows of their own.
    awk -v dir="$dir" '
        function hex(h,   v, i) {
            h = tolower(h); sub(/^0x/, "", h); v = 0
            for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
            return v
        }
        function add(sec, s, f,   n, p) {
            if (sec ~ /\.str[0-9]|\.cst[0-9]/) f = "(merged string constants)"
            else { n = split(f, p, "/"); f = p[n]; sub(/\.o$/, ".c", f); f = dir "/" f }
            by[f] += hex(s)
        }
        /^[^ ]/ { out = ($1 ~ /^\.(text|rodata|literal)$/); sec = ""; next }
        !out { next }
        /^ \*fill\*/ { by["(alignment padding)"] += hex($3); next }
        /^ \.[^ ]+$/ { sec = $1; next }
        $1 ~ /^\./ && NF >= 4 && $2 ~ /^0x/ { add($1, $3, $4); sec = ""; next }
        sec != "" && $1 ~ /^0x/ && NF >= 3 { add(sec, $2, $3); sec = ""; next }
        { sec = "" }
        END { for (f in by) if (by[f]) printf "%7d  %s\n", by[f], f }' "$d/map" | sort -rn > "$d/table"
    cat "$d/table"
    rows=$(awk '{s += $1} END {print s + 0}' "$d/table")
    if [ "$code" -eq 0 ] || [ "$rows" -ne "$code" ]; then
        echo "vmsize: the map rows add up to $rows, the linked sections to $code" >&2; exit 1
    fi
}

# objects CFLAGS SRC...: the old measure: per-file section sums, no link; sets
# code and fixed.
objects() {
    cflags=$1; shift
    code=0; fixed=0; lines=""
    for f in "$@"; do
        o="$OUT/$(basename "$f" .c).o"
        # shellcheck disable=SC2086
        "$GCC" $OBJ_FLAGS $cflags -c "$f" -o "$o"
        n=$(sections "$o")
        code=$((code + n)); fixed=$((fixed + $(fixed_of "$o")))
        lines="$lines$(printf '%7d  %s' "$n" "$f")
"
    done
    printf '%s' "$lines" | sort -rn
}

# lib NAME DIR CFLAGS SRC...: a library other than the VM core, by the chosen method.
lib() {
    if [ "$OBJECTS" = 1 ]; then
        name=$1; dir=$2; shift 2; objects "$@"; how="objects, not linked"
    else
        linked "$@"; how="linked"
    fi
}

if [ "$MODE" = net ]; then
    lib net runtime/net "-Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub" runtime/net/*.c
    # The stack's state is a NetState the board supplies: measured as the
    # .bss of one object of that type, built for the target.
    printf '#include "net_int.h"\nNetState net_size_probe;\n' > "$OUT/probe.c"
    "$GCC" -std=c99 -Os -mlongcalls -fno-common -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
        -DGOGO_ARENA_BYTES=163840u -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub -c "$OUT/probe.c" -o "$OUT/probe.o"
    state=$("$SIZE" -A "$OUT/probe.o" | awk '/^\.bss/ {s += $2} END {print s + 0}')
    printf '%7d  net code total (text+rodata+literal, %s; not VM core)\n' "$code" "$how"
    printf '%7d  net fixed state: sizeof(NetState) %d (board storage) + bss/data %d (limit 4096)\n' \
        "$((state + fixed))" "$state" "$fixed"
    fixed=$((state + fixed))
    if [ "$fixed" -gt 4096 ]; then echo "netsize: fixed state exceeds 4096 bytes"; exit 1; fi
    exit 0
fi
if [ "$MODE" = tls ]; then
    M=${MBEDTLS_DIR:-$HOME/esp/esp-idf/components/mbedtls/mbedtls}
    T=$M/tf-psa-crypto
    if [ ! -f "$M/library/ssl_tls.c" ]; then echo "tlssize: skipped (no mbedTLS source at $M)"; exit 0; fi
    INC="-Iruntime/net/tls/config -Iruntime/net/tls/host -I$M/include -I$T/include -I$T/drivers/builtin/include -I$M/library -I$T/core
         -I$T/drivers/builtin/src -I$T/dispatch -I$T/utilities -I$T/platform -I$T/extras"
    FLAGS="-std=c99 -Os -mlongcalls -ffunction-sections -fdata-sections -w"
    i=0
    for f in "$M"/library/*.c "$T"/core/*.c "$T"/drivers/builtin/src/*.c "$T"/extras/*.c "$T"/platform/*.c "$T"/utilities/*.c; do
        i=$((i + 1))
        # shellcheck disable=SC2086
        "$GCC" $FLAGS $INC -DMBEDTLS_CONFIG_FILE='"gogo_mbedtls_config.h"' \
            -DTF_PSA_CRYPTO_CONFIG_FILE='"gogo_crypto_config.h"' -c "$f" -o "$OUT/mb_$i.o"
    done
    # shellcheck disable=SC2086
    # The port itself with warnings on (32-bit target), unlike mbedTLS.
    "$GCC" ${FLAGS%-w} -Wall -Wextra -Werror $INC -DMBEDTLS_CONFIG_FILE='"gogo_mbedtls_config.h"' -DTF_PSA_CRYPTO_CONFIG_FILE='"gogo_crypto_config.h"' \
        -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u -Iruntime/net/tls -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub \
        -c runtime/net/tls/tls.c -o "$OUT/tls.o"
    "$GCC" -nostdlib -Wl,--gc-sections -Wl,-u,net_tls_driver -Wl,-e,0 -Wl,--unresolved-symbols=ignore-all \
        -o "$OUT/tls.elf" "$OUT/tls.o" "$OUT"/mb_*.o
    port=$(sections "$OUT/tls.o")
    code=$(sections "$OUT/tls.elf")
    fixed=$(fixed_of "$OUT/tls.elf")
    printf '%7d  runtime/net/tls/tls.c (the port, before gc)\n' "$port"
    printf '%7d  tls code total: the port + mbedTLS as linked (text+rodata+literal; not VM core)\n' "$code"
    printf '%7d  tls fixed state (bss+data)\n' "$fixed"
    exit 0
fi
if [ "$MODE" = screen ]; then
    lib screen runtime/screen "-Iruntime/screen -Iruntime/console -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub" runtime/screen/*.c
    printf '%7d  screen code total (text+rodata+literal, %s; not VM core)\n' "$code" "$how"
    printf '%7d  screen fixed state (bss+data; the grid, 4 bytes a cell, is per open port)\n' "$fixed"
    if [ "$fixed" -gt 0 ]; then echo "screensize: runtime/screen has fixed state"; exit 1; fi
    exit 0
fi
if [ "$MODE" = dist ]; then
    lib dist runtime/dist "-Iruntime/dist -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub" runtime/dist/*.c
    printf '%7d  dist code total (text+rodata+literal, %s; not VM core)\n' "$code" "$how"
    printf '%7d  dist fixed state (bss+data; its tables are allocated at the first node link)\n' "$fixed"
    if [ "$fixed" -gt 0 ]; then echo "distsize: runtime/dist has fixed state"; exit 1; fi
    exit 0
fi
if [ "$MODE" = floatlib ]; then
    lib floatlib runtime/floatlib "-Iruntime/vm -Iruntime/vm/test/stub" runtime/floatlib/*.c
    printf '%7d  floatlib code total (text+rodata+literal, %s; not VM core)\n' "$code" "$how"
    printf '%7d  floatlib fixed state (bss+data)\n' "$fixed"
    if [ "$fixed" -gt 0 ]; then echo "floatlibsize: runtime/floatlib has fixed state"; exit 1; fi
    exit 0
fi
if [ "$MODE" = vfs ]; then
    lib vfs runtime/vfs "-Iruntime/vfs -Iruntime/vm -Iruntime/vm/test/stub" runtime/vfs/*.c
    # VfsState (one instance's mounts and slots) lives in board storage, not
    # in runtime/vfs: count it as fixed state.
    printf '#include "vfs_int.h"\nVfsState vfs_state_probe;\n' > "$OUT/probe.c"
    "$GCC" -std=c99 -Os -Iruntime/vfs -c "$OUT/probe.c" -o "$OUT/probe.o"
    st=$("$SIZE" -A "$OUT/probe.o" | awk '/^\.(bss|data|common)/ {s += $2} END {print s + 0}')
    [ "$st" -gt 0 ] || st=$("$SIZE" "$OUT/probe.o" | awk 'NR == 2 {print $3}')
    fixed=$((fixed + st))
    printf '%7d  vfs code total (text+rodata+literal, %s; not VM core)\n' "$code" "$how"
    printf '%7d  VfsState (board storage, per instance)\n' "$st"
    printf '%7d  vfs fixed state (bss+data + VfsState, storage limit 3072)\n' "$fixed"
    if [ "$fixed" -gt 3072 ]; then echo "vfssize: fixed state exceeds 3072 bytes"; exit 1; fi
    exit 0
fi

# The VM core.
VMSRCS=""
for f in runtime/vm/*.c; do [ "$f" = runtime/vm/check32.c ] || VMSRCS="$VMSRCS $f"; done
VMFLAGS="-DGOGO_FLOAT=$FLOAT -Iruntime/vm -Iruntime/vm/test/stub"
if [ "$MODE" = roots ]; then
    # shellcheck disable=SC2086
    linked vm runtime/vm "$VMFLAGS" $VMSRCS > /dev/null
    cat "$OUT/vm/roots"
    exit 0
fi
if [ "$OBJECTS" = 1 ]; then
    # shellcheck disable=SC2086
    objects "$VMFLAGS" $VMSRCS
    how="object sections summed, not linked: the old measure"
else
    # shellcheck disable=SC2086
    linked vm runtime/vm "$VMFLAGS" $VMSRCS
    how="linked, firmware flags, $(wc -l < "$OUT/vm/roots" | tr -d ' ') API roots"
fi
total=$code
[ "$FLOAT" = 0 ] && echo "(built with -DGOGO_FLOAT=0: no floating point)"
printf '%7d  total (%s; limit %d, %d%%)\n' "$total" "$how" "$LIMIT" $((total * 100 / LIMIT))
# Lines of C of the same files (runtime/vm/*.c without check32.c; tests are elsewhere).
# shellcheck disable=SC2086
loc=$(cat $VMSRCS | wc -l | tr -d ' ')
printf '%7d  lines of C in these files\n' "$loc"
if [ "$RECORD" = 1 ]; then
    rev=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
    [ -f docs/vmsize-history.csv ] || echo "date,commit,bytes,lines,method" > docs/vmsize-history.csv
    echo "$(date +%Y-%m-%d),$rev,$total,$loc,linked" >> docs/vmsize-history.csv
fi
if [ "$total" -gt "$LIMIT" ]; then echo "vmsize: VM core exceeds $LIMIT bytes"; exit 1; fi
