#!/bin/sh
# Checks that libgogo.a's global symbols are exactly the API functions
# that gogo.h declares (GOGO_API ... gogo_name(...)): no internal function
# leaks out, and every declared function is there.
#   tools/libsyms.sh build/libgogo.a runtime/include/gogo.h
set -e
LC_ALL=C; export LC_ALL
[ $# -eq 2 ] || { echo "usage: $0 libgogo.a gogo.h" >&2; exit 2; }
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
sed -n 's/^GOGO_API .*[^a-z_]\(gogo_[a-z_]*\)(.*/\1/p' "$2" | sort -u > "$t/want"
[ -s "$t/want" ] || { echo "libsyms: no GOGO_API functions in $2" >&2; exit 2; }
# Defined globals ("addr T name"); Mach-O names carry a leading underscore.
macho=0
[ "$(uname -s)" = Darwin ] && macho=1
nm -g "$1" | awk -v macho=$macho 'NF == 3 && $2 ~ /^[A-TV-Z]$/ { s = $3; if (macho) sub(/^_/, "", s); print s }' | sort -u > "$t/got"
if ! cmp -s "$t/want" "$t/got"; then
    echo "libsyms: $1's global symbols differ from $2's API (< missing, > extra):"
    diff "$t/want" "$t/got" | grep '^[<>]' | sed 's/^/  /'
    exit 1
fi
echo "libsyms: ok ($(wc -l < "$t/got" | tr -d ' ') global symbols, exactly gogo.h's API)"
