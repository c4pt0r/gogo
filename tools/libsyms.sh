#!/bin/sh
# Checks libgogo.a's external symbols: gogo_* is the public API (gogo.h);
# every other symbol the archive defines must start with one of the
# internal prefixes runtime/include/README.md reserves (a static archive
# cannot hide them). Fails on a defined symbol with any other prefix, and on
# a gogo_* symbol outside runtime/embed.
#   tools/libsyms.sh build/libgogo.a
set -e
LC_ALL=C; export LC_ALL
[ $# -eq 1 ] || { echo "usage: $0 libgogo.a" >&2; exit 2; }
# Keep in sync with runtime/include/README.md.
RESERVED="vm_ va_ actor_ bytes_ const_ embed_ evq_ fs_dir_ heap_ image_ inbox_ interp_ module_ msg_ names_ out_ port_ runq_ sig_ signal_ stream_ strs_ timer_ vfs_"
# "member.o: " headers (GNU and LLVM nm) or "archive(member.o):" (macOS nm),
# then "addr T name"; Mach-O names carry a leading underscore.
nm -g "$1" | awk -v reserved="$RESERVED" '
    BEGIN { n = split(reserved, r, " ") }
    /:$/ { m = $0; sub(/:$/, "", m); sub(/^.*\(/, "", m); sub(/\)$/, "", m); next }
    NF == 3 && $2 ~ /^[TDBSCRV]$/ {
        s = $3; sub(/^_/, "", s)
        total++
        if (s ~ /^gogo_/) { pub++; if (m !~ /^embed_/) { print "libsyms: " s " (public) defined in " m " outside runtime/embed"; bad = 1 }; next }
        ok = 0
        for (i = 1; i <= n; i++) if (index(s, r[i]) == 1) { ok = 1; break }
        if (!ok) { print "libsyms: " s " in " m ": not gogo_* and no reserved prefix"; bad = 1 }
    }
    END { if (bad) exit 1; printf "libsyms: ok (%d public gogo_* symbols, %d internal under reserved prefixes)\n", pub, total - pub }'
