#!/bin/sh
# Runs every book example on the host VM and compares its output with the
# files next to it:
#
#   expected.out   stdout, byte for byte
#   expected.err   stderr, byte for byte (missing: stderr must be empty)
#   input          fed to stdin (gpio / kbd / power script)
#   fsroot         present: run with --fs-root on a fresh temp directory
#                  (a line "sd" also creates the /sd directory)
#   mods/<name>/   present: each is built as a module into <fs root>/flash/<name>.gmod
#                  (implies an fs root, like fsroot)
#   config         used as the config store ($GOGO_CONFIG)
#   compile-fails  present: the program must NOT compile; the compiler's
#                  message is compared with expected.err
#
# Usage: docs/book/examples/run.sh [name...]   (from anywhere in the repo)
# Needs build/gogo and build/gogovm (make vm; go build -o build/gogo ./cmd/gogo).
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
gogo="$repo/build/gogo"
[ -x "$gogo" ] || { echo "build/gogo missing: go build -o build/gogo ./cmd/gogo" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
if [ $# -gt 0 ]; then names="$*"; else names=$(cd "$here" && ls -d */ | tr -d /); fi
for name in $names; do
    d="$here/$name"
    [ -f "$d/main.go" ] || continue
    out="$tmp/$name.out"; err="$tmp/$name.err"
    if [ -f "$d/compile-fails" ]; then
        if (cd "$d" && "$gogo" build . -o "$tmp/$name.gogo") >"$out" 2>"$err"; then
            echo "FAIL $name: compiled, but should not"; fail=$((fail+1)); continue
        fi
        : >"$out"
    else
        args=""
        if [ -f "$d/fsroot" ] || [ -d "$d/mods" ]; then
            root="$tmp/$name.fs"; mkdir -p "$root"
            [ -f "$d/fsroot" ] && grep -qw sd "$d/fsroot" && mkdir -p "$root/sd"
            if [ -d "$d/mods" ]; then
                mkdir -p "$root/flash"
                for m in "$d"/mods/*/; do
                    mn=$(basename "$m")
                    (cd "$m" && "$gogo" build . -o "$root/flash/$mn.gmod") || { echo "FAIL $name: module $mn"; fail=$((fail+1)); continue 2; }
                done
            fi
            args="--fs-root $root"
        fi
        cfg="$tmp/no-config"
        [ -f "$d/config" ] && cfg="$d/config"
        stdin=/dev/null
        [ -f "$d/input" ] && stdin="$d/input"
        (cd "$d" && GOGO_CONFIG="$cfg" "$gogo" run $args . <"$stdin" >"$out" 2>"$err")
        code=$?
        if [ $code -ne 0 ]; then
            echo "FAIL $name: exit status $code"; cat "$err"; fail=$((fail+1)); continue
        fi
    fi
    if [ "${RECORD:-}" = 1 ]; then   # maintainers: RECORD=1 run.sh name rewrites the expected files
        cp "$out" "$d/expected.out"
        if [ -s "$err" ]; then cp "$err" "$d/expected.err"; else rm -f "$d/expected.err"; fi
        [ -s "$d/expected.out" ] || rm -f "$d/expected.out"
    fi
    ok=1
    cmp -s "$out" "$d/expected.out" 2>/dev/null || { [ ! -f "$d/expected.out" ] && [ ! -s "$out" ]; } || ok=0
    if [ -f "$d/expected.err" ]; then cmp -s "$err" "$d/expected.err" || ok=0
    elif [ -s "$err" ]; then ok=0; fi
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "ok   $name"
    else
        fail=$((fail+1)); echo "FAIL $name"
        echo "--- stdout"; cat "$out"; echo "--- stderr"; cat "$err"
    fi
done
echo "$pass passed, $fail failed"
[ $fail = 0 ]
