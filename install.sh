#!/bin/sh
# Installs the gogo command (and gogovm, when the release has one for this
# computer) from a GitHub release:
#
#   curl -fsSL https://raw.githubusercontent.com/c4pt0r/gogo/main/install.sh | sh
#
# It picks the file for this OS (macOS, Linux) and CPU (amd64, arm64),
# checks it against the release's SHA256SUMS and installs it in
# /usr/local/bin, or ~/.local/bin when /usr/local/bin is not writable.
#
#   GOGO_VERSION=v0.1.0       a release tag (default: the latest release)
#   GOGO_INSTALL_DIR=~/bin    where to install
#
# Windows: download gogo-windows-amd64.exe or gogo-windows-arm64.exe from
# the release page instead.
set -eu

repo=c4pt0r/gogo

say() { printf 'gogo install: %s\n' "$*"; }
die() { say "$*" >&2; exit 1; }

case "$(uname -s)" in
Darwin) os=darwin ;;
Linux) os=linux ;;
*) die "unsupported OS $(uname -s): download a file from https://github.com/$repo/releases" ;;
esac

case "$(uname -m)" in
x86_64 | amd64) arch=amd64 ;;
arm64 | aarch64) arch=arm64 ;;
*) die "unsupported CPU $(uname -m): download a file from https://github.com/$repo/releases" ;;
esac

# Rosetta: an arm64 Mac running this shell as x86_64 still gets the arm64 file.
if [ "$os" = darwin ] && [ "$arch" = amd64 ] &&
	[ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
	arch=arm64
fi

if [ -n "${GOGO_VERSION:-}" ]; then
	base="https://github.com/$repo/releases/download/$GOGO_VERSION"
else
	base="https://github.com/$repo/releases/latest/download"
fi

if command -v curl >/dev/null 2>&1; then
	fetch() { curl -fsSL -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch() { wget -q -O "$2" "$1"; }
else
	die "needs curl or wget"
fi

if command -v sha256sum >/dev/null 2>&1; then
	sha() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	die "needs sha256sum or shasum to check the download"
fi

if [ -n "${GOGO_INSTALL_DIR:-}" ]; then
	dir=$GOGO_INSTALL_DIR
elif [ -w /usr/local/bin ]; then
	dir=/usr/local/bin
else
	dir="$HOME/.local/bin"
fi
mkdir -p "$dir"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "cannot download $base/SHA256SUMS"

# install NAME FILE: download FILE, check it, install it as NAME.
# Returns 1 if the release has no FILE.
install_one() {
	want=$(awk -v f="$2" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
	[ -n "$want" ] || return 1
	fetch "$base/$2" "$tmp/$2" || die "cannot download $base/$2"
	got=$(sha "$tmp/$2")
	[ "$got" = "$want" ] || die "$2: checksum mismatch (got $got, want $want)"
	chmod +x "$tmp/$2"
	mv "$tmp/$2" "$dir/$1"
	say "installed $dir/$1 ($2)"
}

install_one gogo "gogo-$os-$arch" || die "the release has no gogo-$os-$arch"
install_one gogovm "gogovm-$os-$arch" || say "the release has no gogovm-$os-$arch (gogo run needs one: build it with make vm)"

case ":$PATH:" in
*":$dir:"*) ;;
*) say "add $dir to your PATH, e.g. export PATH=\"$dir:\$PATH\"" ;;
esac
"$dir/gogo" version
