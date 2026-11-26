#!/bin/sh
# Checks libgogo.a's exported symbols against the reviewed list
# (runtime/include/libgogo.syms, one name per line, sorted): the
# archive's global symbols must be exactly that list (no internal
# function leaks out, every API function is there), and so must the
# functions the headers declare (GOGO_API ... gogo_name(...)), so that an
# API change is always a visible change to the list.
#   tools/libsyms.sh build/libgogo.a runtime/include/libgogo.syms runtime/include/gogo.h ...
#   tools/libsyms.sh --subset build/libgogo-debug.a runtime/include/libgogo.syms
#       the debug archive (not prelinked: its internal functions are
#       global too) must define every listed function
set -e
LC_ALL=C; export LC_ALL
subset=0
if [ "${1:-}" = --subset ]; then subset=1; shift; fi
[ $# -ge 3 ] || { [ $subset = 1 ] && [ $# -eq 2 ]; } || {
    echo "usage: $0 libgogo.a libgogo.syms header... | --subset libgogo-debug.a libgogo.syms" >&2; exit 2; }
lib=$1 list=$2
shift 2
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
grep -v '^#' "$list" | sed '/^$/d' > "$t/list"
sort -u "$t/list" > "$t/want"
cmp -s "$t/list" "$t/want" || { echo "libsyms: $list is not sorted, or has a name twice" >&2; exit 1; }
macho=0
[ "$(uname -s)" = Darwin ] && macho=1
if [ $subset = 1 ]; then
    nm -g "$lib" | awk -v macho=$macho 'NF == 3 && $2 == "T" { s = $3; if (macho) sub(/^_/, "", s); print s }' | sort -u > "$t/got"
    missing=$(comm -23 "$t/want" "$t/got")
    [ -z "$missing" ] || { echo "libsyms: $lib lacks listed functions:"; echo "$missing" | sed 's/^/  /'; exit 1; }
    echo "libsyms: ok ($(wc -l < "$t/want" | tr -d ' ') listed functions in $lib)"
    exit 0
fi
sed -n 's/^GOGO_API .*[^a-z_]\(gogo_[a-z_0-9]*\)(.*/\1/p' "$@" | sort -u > "$t/decl"
[ -s "$t/decl" ] || { echo "libsyms: no GOGO_API functions in $*" >&2; exit 2; }
# Defined globals ("addr T name"); Mach-O names carry a leading underscore.
nm -g "$lib" | awk -v macho=$macho 'NF == 3 && $2 ~ /^[A-TV-Z]$/ { s = $3; if (macho) sub(/^_/, "", s); print s }' | sort -u > "$t/got"
bad=0
if ! cmp -s "$t/want" "$t/decl"; then
    echo "libsyms: the headers' GOGO_API functions differ from $list (< only in the list, > only declared):"
    diff "$t/want" "$t/decl" | grep '^[<>]' | sed 's/^/  /'
    bad=1
fi
if ! cmp -s "$t/want" "$t/got"; then
    echo "libsyms: $lib's global symbols differ from $list (< missing, > extra):"
    diff "$t/want" "$t/got" | grep '^[<>]' | sed 's/^/  /'
    bad=1
fi
[ $bad = 0 ] || exit 1
echo "libsyms: ok ($(wc -l < "$t/got" | tr -d ' ') global symbols, exactly $list)"
