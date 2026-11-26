#!/bin/sh
# VM core size: compiles runtime/vm/*.c (except check32.c) for the ESP32-S3 with -Os
# and sums .text/.rodata/.literal — the same measure as the M3 budget
# (platform/esp32s3/vmsize.sh, which reads the IDF build instead).
#   tools/vmsize.sh            print the per-file table and the total; exit 1 above the limit
#   tools/vmsize.sh --record   also append "date,commit,bytes" to docs/vmsize-history.csv
#   tools/vmsize.sh --net      the net stack instead (runtime/net/*.c, not VM core): code
#                              size and its fixed state (sizeof(NetState), the
#                              board's storage, plus any .bss + .data); exit 1 if
#                              the fixed state exceeds 4096 bytes
#   tools/vmsize.sh --vfs      the vfs core instead (runtime/vfs/*.c): code size and fixed
#                              state (.bss + .data); exit 1 above 3072 bytes
#   tools/vmsize.sh --screen   the "screen" port instead (runtime/screen/*.c, not VM core):
#                              code size and fixed state (.bss + .data, limit 0: its
#                              grid is allocated per open port, charged to the owner)
#   tools/vmsize.sh --tls      the "tls" port (runtime/net/tls/tls.c) and the mbedTLS it links
#                              ($MBEDTLS_DIR, runtime/net/tls/config): linked with
#                              --gc-sections from net_tls_driver, so only what the
#                              port reaches counts (not VM core; informational)
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
    for f in runtime/net/*.c; do
        o="$OUT/$(basename "$f" .c).o"
        "$GCC" -std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
            -DGOGO_ARENA_BYTES=163840u -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub -c "$f" -o "$o"
        n=$("$SIZE" -A "$o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
        b=$("$SIZE" -A "$o" | awk '/^\.(bss|data)/ {s += $2} END {print s + 0}')
        total=$((total + n)); fixed=$((fixed + b))
        lines="$lines$(printf '%7d  %s' "$n" "$f")
"
    done
    # The stack's state is a NetState the board supplies: measured as the
    # .bss of one object of that type, built for the target.
    printf '#include "net_int.h"\nNetState net_size_probe;\n' > "$OUT/probe.c"
    "$GCC" -std=c99 -Os -mlongcalls -fno-common -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
        -DGOGO_ARENA_BYTES=163840u -Iruntime/vm -Iruntime/net -Iruntime/stream -Iruntime/vm/test/stub -c "$OUT/probe.c" -o "$OUT/probe.o"
    state=$("$SIZE" -A "$OUT/probe.o" | awk '/^\.bss/ {s += $2} END {print s + 0}')
    printf '%s' "$lines" | sort -rn
    printf '%7d  net code total (text+rodata+literal; not VM core)\n' "$total"
    printf '%7d  net fixed state: sizeof(NetState) %d (board storage) + bss/data %d (limit 4096)\n' \
        "$((state + fixed))" "$state" "$fixed"
    fixed=$((state + fixed))
    if [ "$fixed" -gt 4096 ]; then echo "netsize: fixed state exceeds 4096 bytes"; exit 1; fi
    exit 0
fi
if [ "$1" = "--tls" ]; then
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
    port=$("$SIZE" -A "$OUT/tls.o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
    code=$("$SIZE" -A "$OUT/tls.elf" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
    fixed=$("$SIZE" -A "$OUT/tls.elf" | awk '/^\.(bss|data)/ {s += $2} END {print s + 0}')
    printf '%7d  runtime/net/tls/tls.c (the port, before gc)\n' "$port"
    printf '%7d  tls code total: the port + mbedTLS as linked (text+rodata+literal; not VM core)\n' "$code"
    printf '%7d  tls fixed state (bss+data)\n' "$fixed"
    exit 0
fi
if [ "$1" = "--screen" ]; then
    total=0; fixed=0; lines=""
    for f in runtime/screen/*.c; do
        o="$OUT/$(basename "$f" .c).o"
        "$GCC" -std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u \
            -Iruntime/screen -Iruntime/console -Iruntime/stream -Iruntime/vm -Iruntime/vm/test/stub -c "$f" -o "$o"
        n=$("$SIZE" -A "$o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
        b=$("$SIZE" -A "$o" | awk '/^\.(bss|data)/ {s += $2} END {print s + 0}')
        total=$((total + n)); fixed=$((fixed + b))
        lines="$lines$(printf '%7d  %s' "$n" "$f")
"
    done
    printf '%s' "$lines" | sort -rn
    printf '%7d  screen code total (text+rodata+literal; not VM core)\n' "$total"
    printf '%7d  screen fixed state (bss+data; the grid, 4 bytes a cell, is per open port)\n' "$fixed"
    if [ "$fixed" -gt 0 ]; then echo "screensize: runtime/screen has fixed state"; exit 1; fi
    exit 0
fi
if [ "$1" = "--vfs" ]; then
    total=0; fixed=0; lines=""
    for f in runtime/vfs/*.c; do
        o="$OUT/$(basename "$f" .c).o"
        "$GCC" -std=c99 -Os -mlongcalls -Iruntime/vfs -Iruntime/vm -Iruntime/vm/test/stub -c "$f" -o "$o"
        n=$("$SIZE" -A "$o" | awk '/^\.(text|rodata|literal)/ {s += $2} END {print s + 0}')
        b=$("$SIZE" -A "$o" | awk '/^\.(bss|data)/ {s += $2} END {print s + 0}')
        total=$((total + n)); fixed=$((fixed + b))
        lines="$lines$(printf '%7d  %s' "$n" "$f")
"
    done
    printf '%s' "$lines" | sort -rn
    # VfsState (one instance's mounts and slots) lives in board storage, not
    # in runtime/vfs: count it as fixed state.
    printf '#include "vfs_int.h"\nVfsState vfs_state_probe;\n' > "$OUT/probe.c"
    "$GCC" -std=c99 -Os -Iruntime/vfs -c "$OUT/probe.c" -o "$OUT/probe.o"
    st=$("$SIZE" -A "$OUT/probe.o" | awk '/^\.(bss|data|common)/ {s += $2} END {print s + 0}')
    [ "$st" -gt 0 ] || st=$("$SIZE" "$OUT/probe.o" | awk 'NR == 2 {print $3}')
    fixed=$((fixed + st))
    printf '%7d  vfs code total (text+rodata+literal; not VM core)\n' "$total"
    printf '%7d  VfsState (board storage, per instance)\n' "$st"
    printf '%7d  vfs fixed state (bss+data + VfsState, storage limit 3072)\n' "$fixed"
    if [ "$fixed" -gt 3072 ]; then echo "vfssize: fixed state exceeds 3072 bytes"; exit 1; fi
    exit 0
fi
total=0
lines=""
for f in runtime/vm/*.c; do
    [ "$f" = runtime/vm/check32.c ] && continue
    o="$OUT/$(basename "$f" .c).o"
    "$GCC" -std=c99 -Os -mlongcalls -DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u \
        -DGOGO_ARENA_BYTES=163840u -Iruntime/vm -Iruntime/vm/test/stub -c "$f" -o "$o"
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
