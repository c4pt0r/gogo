#!/bin/sh
# make test-examples: every example program builds, and the ones with an
# expected output run and print it.
#   tools/examples.sh BUILD_DIR    (BUILD_DIR holds gogo, gogovm and gogovm_mcu)
#
# Every examples/*/ and platform/esp32s3/examples/*/ directory with .gg
# files is built with `gogo build` (board-only examples too: building
# needs no board). One with an expected.out is then run with `gogo run`
# on the host VM, with an empty package search path, and its stdout must
# equal expected.out.
#
# An example of several packages keeps its program in app/ (the build
# root, so its own imports start with app/), with app/expected.out; each
# .gg file beside app/ is then a script of its own, built and, with a
# <name>.expected.out beside it, run. A file `gogopath` names the
# example's package search path, relative to the example (e.g. "gopath":
# GOGOPATH=<example>/gopath), for its builds and runs.
#
# The files next to an example, as in tests/golden_test.go:
#   input         fed to stdin
#   config        the config store ($GOGO_CONFIG; else a file that does not exist)
#   fsroot        run with --fs-root on a fresh temp dir (a line "sd" also creates sd/)
#   mods/<name>/  each built into <fs root>/flash/<name>.ggb (implies an fs root)
#   expected.err  each non-empty line must appear in stderr
#   mcu           also run on gogovm_mcu (the MCU VM configuration)
# Exit 1 if any example fails.
set -u
[ $# -eq 1 ] || { echo "usage: tools/examples.sh BUILD_DIR" >&2; exit 2; }
build=$(cd "$1" && pwd) || exit 2
cd "$(dirname "$0")/.." || exit 2
gogo="$build/gogo"
[ -x "$gogo" ] || { echo "examples: $gogo missing" >&2; exit 2; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
built=0; ran=0; fail=0

# run DIR PROG WANT VM: run PROG (DIR, DIR/app or a script) of the example
# in DIR on VM and compare its stdout with WANT; 0 if it matches
run() {
    d=$1; prog=$2; want=$3; vm=$4; name=$(basename "$d")
    out="$tmp/$name.out"; err="$tmp/$name.err"
    args=""
    if [ -f "$d/fsroot" ] || [ -d "$d/mods" ]; then
        root="$tmp/$name.fs"; rm -rf "$root"; mkdir -p "$root"
        [ -f "$d/fsroot" ] && grep -qw sd "$d/fsroot" && mkdir -p "$root/sd"
        if [ -d "$d/mods" ]; then
            mkdir -p "$root/flash"
            for m in "$d"/mods/*/; do
                GOGOPATH=$gp "$gogo" build "$m" -o "$root/flash/$(basename "$m").ggb" || return 1
            done
        fi
        args="--fs-root $root"
    fi
    cfg="$tmp/no-config"
    [ -f "$d/config" ] && cfg="$PWD/$d/config"
    stdin=/dev/null
    [ -f "$d/input" ] && stdin="$d/input"
    # shellcheck disable=SC2086 # $args is empty or "--fs-root DIR"
    if ! GOGO_VM="$vm" GOGO_CONFIG="$cfg" GOGOPATH=$gp "$gogo" run $args "$prog" <"$stdin" >"$out" 2>"$err"; then
        echo "FAIL $prog ($(basename "$vm")): exit status"; cat "$err"; return 1
    fi
    if ! cmp -s "$out" "$want"; then
        echo "FAIL $prog ($(basename "$vm")): stdout differs from $want"
        diff "$want" "$out" | head -20; return 1
    fi
    if [ -f "$d/expected.err" ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            grep -qF -- "$line" "$err" || { echo "FAIL $prog ($(basename "$vm")): stderr lacks: $line"; return 1; }
        done < "$d/expected.err"
    fi
    return 0
}

# check DIR PROG WANT: build PROG; if WANT exists, run it on gogovm (and
# gogovm_mcu when DIR has mcu)
check() {
    d=$1; prog=$2; want=$3
    if ! GOGOPATH=$gp "$gogo" build "$prog" -o "$tmp/image.ggb" >"$tmp/build.log" 2>&1; then
        echo "FAIL $prog: does not build"; cat "$tmp/build.log"; fail=$((fail + 1)); return
    fi
    built=$((built + 1))
    [ -f "$want" ] || return
    run "$d" "$prog" "$want" "$build/gogovm" || { fail=$((fail + 1)); return; }
    if [ -f "$d/mcu" ]; then
        run "$d" "$prog" "$want" "$build/gogovm_mcu" || { fail=$((fail + 1)); return; }
    fi
    ran=$((ran + 1))
}

for d in examples/*/ platform/esp32s3/examples/*/; do
    d=${d%/}
    gp=''
    [ -f "$d/gogopath" ] && gp="$PWD/$d/$(cat "$d/gogopath")"
    if ls "$d"/app/*.gg >/dev/null 2>&1; then
        check "$d" "$d/app" "$d/app/expected.out"
        for f in "$d"/*.gg; do
            [ -f "$f" ] && check "$d" "$f" "${f%.gg}.expected.out"
        done
    elif ls "$d"/*.gg >/dev/null 2>&1; then
        check "$d" "$d" "$d/expected.out"
    fi
done
echo "examples: $built built, $ran run, $fail failed"
[ "$fail" -eq 0 ]
