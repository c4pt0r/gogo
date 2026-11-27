#!/bin/sh
# Global-state guard (embed mode): no mutable globals in runtime/.
# Compiles runtime/{vm,vfs,stream,input,net,console,screen,embed,gpio,configsvc,prov,board}/*.c (not check32.c,
# plus the vfs/fat and vfs/lfs backends when their headers are there,
# not test/) at -O0 with -fno-common, in three configurations (host default,
# MCU flags, -DGOGO_EMBED), and lists every symbol that lives in a mutable data
# section: .bss*, .data* (not .data.rel.ro*), .sbss*, .sdata*, .tbss, .tdata,
# COMMON (ELF) or __DATA,__bss/__data/__common/__thread_* (Mach-O). That covers
# file-scope variables, weak data and function-local statics; const data is not
# listed. Function-local statics are named func.var (clang, Mach-O); gcc ELF
# only emits var.N or var$N, shown as ?.var, which matches any func.var of that file.
# Fails on a symbol (in any configuration) missing from the allow-list, and on
# allow-list lines that match nothing (so each embed task deletes its own lines),
# except lines whose comment starts with "permanent:" (state that must stay
# process-global, with the reason after the tag): those are never reported stale.
# Also fails on any weak symbol, defined or undefined (nm W/V/w/v on ELF,
# "weak" on Mach-O): a hook a host may omit is a NULL field in gogo_host or
# VmBoard, never a weak default. Weak symbols have no allow-list; --print
# lists them as "<file> weak:<symbol>".
#   tools/globals.sh [--allow file] [--print] [files...]
#   tools/globals.sh [--print] --objects file.a|file.o...
#     --print   list the symbols found (no allow-list check); files default to runtime/
#     --objects scan built objects and archives (each member) as they are, with
#               no compile and no allow-list: any mutable or weak symbol fails
#               (make lib checks build/libgogo.a this way; members are listed
#               as archive(member)).
# Environment (make globals passes the VM build's values):
#   GLOBALS_CC, GLOBALS_CFLAGS, GLOBALS_MCU_FLAGS, GLOBALS_NM (GNU/LLVM nm for ELF),
#   GLOBALS_TLS_CFLAGS: the "tls" port's flags (mbedTLS headers, shell-quoted). runtime/net/tls/*.c
#   is scanned only when set (the build found mbedTLS); otherwise its allow-list
#   entries are not reported as stale.
set -e
LC_ALL=C; export LC_ALL
cd "$(dirname "$0")/.."
ALLOW=tools/globals.allow; PRINT=0; TLS_FILES=""; OBJECTS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --allow) ALLOW=$2; shift 2 ;;
        --print) PRINT=1; shift ;;
        --objects) OBJECTS=1; shift ;;
        *) break ;;
    esac
done
CC=${GLOBALS_CC:-${CC:-cc}}
CFLAGS=${GLOBALS_CFLAGS:--std=c99}
MCU=${GLOBALS_MCU_FLAGS:--DGOGO_DEFAULT_MAX_WORDS=4096u -DGOGO_TABLE_CAP=1024u -DGOGO_ARENA_BYTES=163840u}
INC="-Iruntime/vm -Iruntime/net -Iruntime/vfs -Iruntime/input -Iruntime/stream -Iruntime/console -Iruntime/screen -Iruntime/embed -Iruntime/gpio -Iruntime/configsvc -Iruntime/prov -Iruntime/board -Iruntime/dist -Iplatform/unix"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

if [ "$OBJECTS" = 1 ]; then FILES="$*"; ALLOW=/dev/null
    [ -n "$FILES" ] || { echo "globals: --objects needs files" >&2; exit 2; }
elif [ $# -gt 0 ]; then FILES="$*"; else
    FILES=""
    for d in vm vfs stream input net console screen embed gpio configsvc prov board dist; do
        for f in runtime/"$d"/*.c; do
            [ -f "$f" ] || continue
            [ "$f" = runtime/vm/check32.c ] && continue
            FILES="$FILES $f"
        done
    done
    if [ -n "$GLOBALS_TLS_CFLAGS" ]; then
        for f in runtime/net/tls/*.c; do TLS_FILES="$TLS_FILES $f"; done
    fi
fi
# The file-system backends need their library's header: FatFs is vendored
# (with the test's sdkconfig stub); LittleFS only after a firmware build
# fetched it (GLOBALS_LFS_DIR overrides), else lfs_be.c is not scanned.
BE_FILES=""
if [ $# -eq 0 ] && [ "$OBJECTS" = 0 ]; then
    BE_FILES="runtime/vfs/fat/fat_be.c"
    LFS_DIR=${GLOBALS_LFS_DIR:-platform/esp32s3/managed_components/joltwallet__littlefs/src/littlefs}
    [ -f "$LFS_DIR/lfs.h" ] && BE_FILES="$BE_FILES runtime/vfs/lfs/lfs_be.c"
fi
BE_INC="-Iruntime/vfs/fat -Iruntime/vfs/fat/test/stub -Iplatform/esp32s3/components/fatfs/src -Iruntime/vfs/lfs -I${LFS_DIR:-.} -DGOGO_FS_SLOT_BYTES=192u"

# GNU/LLVM nm (needs --format=sysv) for ELF objects.
elf_nm() {
    if [ -n "$GLOBALS_NM" ]; then echo "$GLOBALS_NM"; return; fi
    for n in llvm-nm gnu-nm gnm nm; do
        c=$(command -v $n 2>/dev/null) || continue
        if "$c" --format=sysv /dev/null 2>&1 | grep -qi 'invalid\|unknown\|usage'; then :; else echo "$c"; return; fi
    done
    for c in "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-nm; do
        [ -x "$c" ] && { echo "$c"; return; }
    done
    echo "globals: no GNU/LLVM nm for ELF objects (set GLOBALS_NM)" >&2; exit 2
}

found="$OUT/found"; : > "$found"
weak="$OUT/weak"; : > "$weak"
scan() { # file flags...
    f=$1; shift
    # shellcheck disable=SC2086
    $CC $CFLAGS "$@" -O0 -fno-common $INC -c "$f" -o "$OUT/o.o"
    scan_obj "$OUT/o.o" "$f"
}
scan_obj() { # object label
    o=$1; label=$2
    if [ "$(od -An -tx1 -N4 "$o" | tr -d ' ')" = 7f454c46 ]; then
        "$(elf_nm)" "$o" | awk -v f="$label" '$(NF-1) ~ /^[WVwv]$/ { print f, "weak:" $NF }' >> "$weak"
        "$(elf_nm)" --format=sysv "$o" | awk -F'|' -v f="$label" '
            { n = $1; gsub(/ /, "", n); sec = $7; gsub(/ /, "", sec) }
            sec ~ /^(\.bss|\.data|\.sbss|\.sdata|\.tbss|\.tdata|\*COM\*)/ && sec !~ /^\.data\.rel\.ro/ && n !~ /^\.L/ {
                if (n ~ /[.$][0-9]+$/) { sub(/[.$][0-9]+$/, "", n); n = "?." n }
                print f, n }' >> "$found"
    else
        # Mach-O: classify by section (nm letters are unreliable: a zero-fill
        # global can show as S or __common). ltmpN / l_* are assembler temporaries;
        # a thread-local's storage is named var$tlv$init (listed as var).
        nm -m "$o" | awk -v f="$label" '/ weak / { s = $NF; sub(/^_/, "", s); print f, "weak:" s }' >> "$weak"
        nm -m "$o" | awk -v f="$label" '
            /^[0-9a-f]+ \(__(DATA|DATA_DIRTY),__(bss|data|common|thread_bss|thread_data)\)/ && $NF !~ /^(ltmp|l_|lCPI|lJTI)/ {
                s = $NF; sub(/^_/, "", s); sub(/\$tlv\$init$/, "", s); print f, s }' >> "$found"
    fi
}
if [ "$OBJECTS" = 1 ]; then
    for f in $FILES; do
        case "$f" in
        *.a)
            x="$OUT/ar"; rm -rf "$x"; mkdir "$x"
            a=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
            (cd "$x" && ar x "$a")
            for m in $(ar t "$f"); do
                case "$m" in *.o) scan_obj "$x/$m" "$(basename "$f")($m)" ;; esac
            done ;;
        *) scan_obj "$f" "$f" ;;
        esac
    done
    FILES=""
fi
for f in $FILES; do
    scan "$f"
    # shellcheck disable=SC2086 # $MCU is a list of flags
    scan "$f" $MCU
    scan "$f" -DGOGO_EMBED
done
for f in $BE_FILES; do
    # shellcheck disable=SC2086
    scan "$f" $BE_INC
done
for f in $TLS_FILES; do
    # eval: the flags carry shell quoting (-DMBEDTLS_CONFIG_FILE='"..."').
    eval "scan \"\$f\" $GLOBALS_TLS_CFLAGS"
    eval "scan \"\$f\" $GLOBALS_TLS_CFLAGS \$MCU"
    eval "scan \"\$f\" $GLOBALS_TLS_CFLAGS -DGOGO_EMBED"
done
sort -u "$found" -o "$found"
sort -u "$weak" -o "$weak"
if [ "$PRINT" = 1 ]; then cat "$found" "$weak"; exit 0; fi

sed -e 's/#.*//' "$ALLOW" | awk -v tls="$TLS_FILES" 'NF >= 2 && (tls != "" || $1 !~ /^runtime\/net\/tls\//) { print $1, $2 }' | sort -u > "$OUT/allow"
awk '/#[ \t]*permanent:/ { sub(/#.*/, ""); if (NF >= 2) print $1, $2 }' "$ALLOW" | sort -u > "$OUT/perm"
# Match: exact, or found "?.var" against any allowed "func.var" of the same file.
awk -v new="$OUT/new" -v stale="$OUT/stale" '
    FILENAME == permf { keep[$1 " " $2] = 1; next }
    FILENAME == allowf { allow[$1 " " $2] = 1; next }
    { ok = ($1 " " $2) in allow
      if (ok) used[$1 " " $2] = 1
      else if ($2 ~ /^\?\./) {
          v = substr($2, 3)
          for (a in allow) { split(a, q, " "); if (q[1] == $1 && q[2] ~ ("(^|\\.)" v "$")) { ok = 1; used[a] = 1 } }
      }
      if (!ok) print $1, $2 > new }
    END { for (a in allow) if (!(a in used) && !(a in keep)) print a > stale }
' permf="$OUT/perm" allowf="$OUT/allow" "$OUT/perm" "$OUT/allow" "$found"
touch "$OUT/new" "$OUT/stale"
bad=0
if [ -s "$weak" ]; then
    echo "globals: weak symbols in runtime/ (make the hook a NULL-able gogo_host / VmBoard field):"; sed 's/^/  /' "$weak"; bad=1
fi
if [ -s "$OUT/new" ]; then
    echo "globals: mutable global state not on the allow-list:"; sort "$OUT/new" | sed 's/^/  /'; bad=1
fi
if [ -s "$OUT/stale" ]; then
    echo "globals: allow-list entries that no longer exist (delete them from $ALLOW):"; sort "$OUT/stale" | sed 's/^/  /'; bad=1
fi
[ "$bad" -eq 0 ] || exit 1
echo "globals: ok ($(wc -l < "$OUT/allow" | tr -d ' ') allowed symbols, no new mutable globals, no weak symbols)"
