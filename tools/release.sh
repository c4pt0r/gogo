#!/bin/sh
# Builds the release assets that `gogo setup` downloads (asset names:
# cmd/gogo/setup_release.go). The GitHub release workflow
# (.github/workflows/release.yml) runs the same steps.
#
#   tools/release.sh [version]          everything, into build/release/<version>/
#   tools/release.sh firmware BOARD DIR  one board's firmware (sticks3, cardputer)
#   tools/release.sh cli VERSION DIR     the gogo command for every OS and CPU
#   tools/release.sh vm DIR [OS/ARCH...] the host VM, gogovm, for each target
#                                        (default: every one in VM_TARGETS)
#   tools/release.sh notices DIR         the license files (below)
#   tools/release.sh sums DIR            DIR/SHA256SUMS over the other files
#
# License files shipped with every release: LICENSE (gogo, BSD-3-Clause),
# THIRD_PARTY_NOTICES.md, font-licenses.zip (the console font's OFL texts,
# runtime/console/font/OFL.txt and LICENSES/), and LICENSE-APACHE-2.0.txt,
# the Apache-2.0 text of ESP-IDF, mbedTLS and the Wi-Fi libraries in the
# firmware (copied from the ESP-IDF tree by the firmware step).
#
# version defaults to `git describe --tags --always --dirty`. The firmware
# needs ESP-IDF (v6.1.1): idf.py on PATH, else $IDF_PATH/export.sh or
# ~/esp/esp-idf/export.sh is sourced. The ESP-IDF build directories go
# under $GOGO_RELEASE_WORK (default build/release-work), each started
# afresh. The gogo command is cross-compiled with GOOS/GOARCH (no cgo).
#
# gogovm (C, `make vm`) is built for macOS and Linux on amd64 and arm64 as
# gogovm-<os>-<arch>; `gogo run` finds it next to the gogo command. The
# compiler for a target is $CC_<os>_<arch> if set (e.g.
# CC_linux_arm64=aarch64-linux-gnu-gcc), else on macOS `clang -arch` for
# both darwin targets, else on Linux `cc` for the host's own CPU. The
# Linux VMs are linked statically. The release VMs are built without the
# tls port (it needs ESP-IDF's mbedTLS source): build gogovm from source
# for HTTPS on the host.
set -eu
cd "$(dirname "$0")/.."
repo=$(pwd)

BOARDS="sticks3 cardputer"
TARGETS="darwin/amd64 darwin/arm64 linux/amd64 linux/arm64 windows/amd64 windows/arm64"
VM_TARGETS="darwin/amd64 darwin/arm64 linux/amd64 linux/arm64"

die() { echo "release.sh: $*" >&2; exit 1; }

idf_env() {
    command -v idf.py >/dev/null 2>&1 && return 0
    for d in "${IDF_PATH:-}" "$HOME/esp/esp-idf"; do
        if [ -n "$d" ] && [ -f "$d/export.sh" ]; then
            # shellcheck disable=SC1091
            . "$d/export.sh" >/dev/null
            return 0
        fi
    done
    die "ESP-IDF not found (idf.py, \$IDF_PATH or ~/esp/esp-idf)"
}

# firmware BOARD DIR: build the board's default firmware, copy the files
# its flasher_args.json names as DIR/BOARD-<base name>.
firmware() {
    board=$1 out=$2
    work=${GOGO_RELEASE_WORK:-$repo/build/release-work}/$board
    case $board in
    sticks3) set -- ;;
    cardputer) set -- -DGOGO_BOARD=cardputer "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.cardputer" ;;
    *) die "unknown board $board (boards: $BOARDS)" ;;
    esac
    idf_env
    rm -rf "$work"
    mkdir -p "$work" "$out"
    idf.py -C platform/esp32s3 -B "$work" "-DSDKCONFIG=$work/sdkconfig" "$@" build
    fa=$work/flasher_args.json
    # The files flasher_args.json names, one per line.
    files=$(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1]))["flash_files"].values()))' "$fa")
    [ -n "$files" ] || die "$fa names no files"
    cp "$fa" "$out/$board-flasher_args.json"
    [ -n "${IDF_PATH:-}" ] || die "IDF_PATH is not set after sourcing ESP-IDF's export.sh (needed for its LICENSE)"
    cp "$IDF_PATH/LICENSE" "$out/LICENSE-APACHE-2.0.txt"
    grep -q "Apache License" "$out/LICENSE-APACHE-2.0.txt" || die "$IDF_PATH/LICENSE is not the Apache License"
    for f in $files; do
        cp "$work/$f" "$out/$board-$(basename "$f")"
    done
    echo "firmware $board: $(echo $files | tr '\n' ' ')"
}

# cli VERSION DIR
cli() {
    v=$1 out=$2
    mkdir -p "$out"
    for t in $TARGETS; do
        os=${t%/*} arch=${t#*/} ext=
        [ "$os" = windows ] && ext=.exe
        CGO_ENABLED=0 GOOS=$os GOARCH=$arch go build -trimpath -ldflags "-s -w -X main.version=$v" \
            -o "$out/gogo-$os-$arch$ext" ./cmd/gogo
        echo "cli gogo-$os-$arch$ext"
    done
}

# vm_cc OS ARCH: the C compiler command for a gogovm target, or nothing.
vm_cc() {
    os=$1 arch=$2
    eval "cc=\${CC_${os}_${arch}:-}"
    if [ -n "$cc" ]; then echo "$cc"; return; fi
    host_arch=$(uname -m)
    case $host_arch in x86_64|amd64) host_arch=amd64 ;; aarch64|arm64) host_arch=arm64 ;; esac
    case $os/$(uname -s) in
    darwin/Darwin)
        a=$arch; [ "$a" = amd64 ] && a=x86_64
        echo "clang -arch $a -mmacosx-version-min=11.0" ;;
    linux/Linux)
        [ "$arch" = "$host_arch" ] && echo "${CC:-cc}" ;;
    esac
}

# vm DIR [OS/ARCH...]
vm() {
    out=$1; shift
    # shellcheck disable=SC2086 # one target per word
    [ $# -gt 0 ] || set -- $VM_TARGETS
    mkdir -p "$out"
    for t in "$@"; do
        os=${t%/*} arch=${t#*/}
        case " $VM_TARGETS " in *" $t "*) ;; *) die "no gogovm for $t (targets: $VM_TARGETS)" ;; esac
        cc=$(vm_cc "$os" "$arch")
        [ -n "$cc" ] || die "cannot build gogovm-$os-$arch on $(uname -s) $(uname -m): set CC_${os}_${arch} to a C compiler for it"
        flags=-g0
        [ "$os" = linux ] && flags="-g0 -static"
        work=${GOGO_RELEASE_WORK:-$repo/build/release-work}/vm-$os-$arch
        rm -rf "$work"
        # MBEDTLS_DIR points nowhere: no tls port (above).
        make -s BUILD="$work" CC="$cc" EXTRA_CFLAGS="$flags" MBEDTLS_DIR=/nonexistent "$work/gogovm"
        cp "$work/gogovm" "$out/gogovm-$os-$arch"
        echo "vm gogovm-$os-$arch"
    done
}

# notices DIR
notices() {
    mkdir -p "$1"
    out=$(cd "$1" && pwd)
    cp LICENSE THIRD_PARTY_NOTICES.md "$out/"
    rm -f "$out/font-licenses.zip"
    (cd runtime/console/font && zip -qX -r "$out/font-licenses.zip" OFL.txt LICENSES)
    echo "notices LICENSE THIRD_PARTY_NOTICES.md font-licenses.zip"
}

# sums DIR
sums() {
    out=$1
    if command -v sha256sum >/dev/null 2>&1; then sha="sha256sum"; else sha="shasum -a 256"; fi
    (cd "$out" && rm -f SHA256SUMS && list=$(ls | LC_ALL=C sort) && sums=$(echo "$list" | xargs $sha) &&
        printf '%s\n' "$sums" > SHA256SUMS)
    echo "sums $out/SHA256SUMS"
}

case ${1:-} in
firmware) [ $# -eq 3 ] || die "usage: tools/release.sh firmware BOARD DIR"; firmware "$2" "$3" ;;
cli) [ $# -eq 3 ] || die "usage: tools/release.sh cli VERSION DIR"; cli "$2" "$3" ;;
vm) [ $# -ge 2 ] || die "usage: tools/release.sh vm DIR [OS/ARCH...]"; shift; vm "$@" ;;
notices) [ $# -eq 2 ] || die "usage: tools/release.sh notices DIR"; notices "$2" ;;
sums) [ $# -eq 2 ] || die "usage: tools/release.sh sums DIR"; sums "$2" ;;
-*) die "usage: tools/release.sh [version] | firmware BOARD DIR | cli VERSION DIR | vm DIR [OS/ARCH...] | notices DIR | sums DIR" ;;
*)
    v=${1:-$(git describe --tags --always --dirty)}
    case $v in
    ""|.*|*/*|*[!A-Za-z0-9._-]*) die "bad version \"$v\" (letters, digits, . _ -; not starting with .)" ;;
    esac
    out=$repo/build/release/$v
    rm -rf "$out"
    mkdir -p "$out"
    for b in $BOARDS; do firmware "$b" "$out"; done
    cli "$v" "$out"
    vm "$out"
    notices "$out"
    sums "$out"
    echo "release $v: $out"
    ls -l "$out"
    ;;
esac
