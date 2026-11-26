#!/bin/sh
# Builds BUILD/libgogoc.a (make libgogoc): the gogo compiler as a C
# library, embed/gogoc/gogoc.h. `go build -buildmode=c-archive` makes an
# archive of the compiler and the Go runtime; its objects are then
# prelinked into one relocatable object (ld -r) whose only globals are
# the gogoc_* functions, as libgogo.a's are the gogo_* ones (an export
# list on Darwin, objcopy on ELF; LIB_LD and LIB_OBJCOPY name other
# tools), so the Go runtime's own C symbols (crosscall2, x_cgo_init,
# fatalf, ...) never clash with a host's. tools/libsyms.sh then checks
# the globals against embed/gogoc/libgogoc.syms and gogoc.h.
#   tools/libgogoc.sh BUILD      (from the repository root)
set -eu
b=${1:?usage: tools/libgogoc.sh BUILD}
d=$b/gogoc
ld=${LIB_LD:-ld}
objcopy=${LIB_OBJCOPY:-objcopy}
mkdir -p "$d"
go build -buildmode=c-archive -o "$d/go.a" ./embed/gogoc
rm -rf "$d/obj"
mkdir "$d/obj"
(cd "$d/obj" && ar x ../go.a && rm -f __.SYMDEF*)
if [ "$(uname -s)" = Darwin ]; then
    echo '_gogoc_*' > "$d/exports.txt"
    "$ld" -r -exported_symbols_list "$d/exports.txt" -o "$d/libgogoc.o" "$d"/obj/*.o
else
    "$ld" -r -o "$d/libgogoc.o" "$d"/obj/*.o
    "$objcopy" -w --keep-global-symbol='gogoc_*' "$d/libgogoc.o"
fi
rm -f "$b/libgogoc.a"
ar rcs "$b/libgogoc.a" "$d/libgogoc.o"
LIBSYMS_API=GOGOC_API LIBSYMS_PREFIX=gogoc_ tools/libsyms.sh "$b/libgogoc.a" embed/gogoc/libgogoc.syms embed/gogoc/gogoc.h ||
    { rm -f "$b/libgogoc.a"; exit 1; }
