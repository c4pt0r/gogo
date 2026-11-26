#!/bin/sh
# make test-examples: every example program builds, and the ones with an
# expected output run and print it.
#   tools/examples.sh BUILD_DIR    (BUILD_DIR holds gogo, gogovm and gogovm_mcu)
#
# Every examples/*/ and platform/esp32s3/examples/*/ directory with .gg
# files is built with `gogo build` (board-only examples too: building
# needs no board). One with an expected.out is then run with `gogo run`
# on the host VM, with an empty package search path, and its stdout must
# equal expected.out. The files next to it, as in tests/golden_test.go:
#   input         fed to stdin
#   config        the config store ($GOGO_CONFIG; else a file that does not exist)
#   fsroot        run with --fs-root on a fresh temp dir (a line "sd" also creates sd/)
#   mods/<name>/  each built into <fs root>/flash/<name>.gmod (implies an fs root)
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

# run DIR VM: run the example in DIR on VM and compare; 0 if it matches
run() {
    d=$1; vm=$2; name=$(basename "$d")
    out="$tmp/$name.out"; err="$tmp/$name.err"
    args=""
    if [ -f "$d/fsroot" ] || [ -d "$d/mods" ]; then
        root="$tmp/$name.fs"; rm -rf "$root"; mkdir -p "$root"
        [ -f "$d/fsroot" ] && grep -qw sd "$d/fsroot" && mkdir -p "$root/sd"
        if [ -d "$d/mods" ]; then
            mkdir -p "$root/flash"
            for m in "$d"/mods/*/; do
                GOGOPATH='' "$gogo" build "$m" -o "$root/flash/$(basename "$m").gmod" || return 1
            done
        fi
        args="--fs-root $root"
    fi
    cfg="$tmp/no-config"
    [ -f "$d/config" ] && cfg="$PWD/$d/config"
    stdin=/dev/null
    [ -f "$d/input" ] && stdin="$d/input"
    # shellcheck disable=SC2086 # $args is empty or "--fs-root DIR"
    if ! GOGO_VM="$vm" GOGO_CONFIG="$cfg" GOGOPATH='' "$gogo" run $args "$d" <"$stdin" >"$out" 2>"$err"; then
        echo "FAIL $d ($(basename "$vm")): exit status"; cat "$err"; return 1
    fi
    if ! cmp -s "$out" "$d/expected.out"; then
        echo "FAIL $d ($(basename "$vm")): stdout differs from expected.out"
        diff "$d/expected.out" "$out" | head -20; return 1
    fi
    if [ -f "$d/expected.err" ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            grep -qF -- "$line" "$err" || { echo "FAIL $d ($(basename "$vm")): stderr lacks: $line"; return 1; }
        done < "$d/expected.err"
    fi
    return 0
}

for d in examples/*/ platform/esp32s3/examples/*/; do
    d=${d%/}
    ls "$d"/*.gg >/dev/null 2>&1 || continue
    if ! GOGOPATH='' "$gogo" build "$d" -o "$tmp/image.ggb" >"$tmp/build.log" 2>&1; then
        echo "FAIL $d: does not build"; cat "$tmp/build.log"; fail=$((fail + 1)); continue
    fi
    built=$((built + 1))
    [ -f "$d/expected.out" ] || continue
    run "$d" "$build/gogovm" || { fail=$((fail + 1)); continue; }
    if [ -f "$d/mcu" ]; then
        run "$d" "$build/gogovm_mcu" || { fail=$((fail + 1)); continue; }
    fi
    ran=$((ran + 1))
done
echo "examples: $built built, $ran run, $fail failed"
[ "$fail" -eq 0 ]
