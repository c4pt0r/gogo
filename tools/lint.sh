#!/bin/sh
# Lint (make lint):
#   gofmt     every .go file, and every .gg file (gogo source is Go syntax,
#             but gofmt only reads .go: each is copied as .go into a temp
#             dir and checked there); the files listed must be formatted
#   go vet    ./...
#   tools/*.sh checked by shellcheck, when it is installed (skipped otherwise)
# The files are the ones git knows (tracked or untracked, not ignored), so
# build output and nested worktrees are left out; outside a git checkout,
# every file under the repo except build/ and dot directories. So untracked
# work in progress is checked too, and every .gg file must parse: a future
# test of a syntax error would need an exclusion here. shellcheck covers
# only tools/*.sh.
# Exit 1 on any finding.
set -u
cd "$(dirname "$0")/.." || exit 2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0

files() { # files PATTERN: repo files matching *.PATTERN, one per line
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files --cached --others --exclude-standard "*.$1"
    else
        find . -path ./build -prune -o -name '.?*' -prune -o -name "*.$1" -print | sed 's|^\./||'
    fi
}

bad=$(files go | xargs gofmt -l) || status=1   # a file gofmt cannot parse: its error is printed
if [ -n "$bad" ]; then
    echo "lint: gofmt wants to reformat (gofmt -w <file>):"; echo "$bad" | sed 's/^/  /'
    status=1
fi

files gg > "$tmp/gg.list"
while IFS= read -r f; do
    [ -f "$f" ] || continue
    mkdir -p "$tmp/gg/$(dirname "$f")"
    # A script's "#!" first line is not Go: checked as a comment line.
    sed '1s|^#!|// #!|' "$f" > "$tmp/gg/${f%.gg}.go"
done < "$tmp/gg.list"
# Errors (a file gofmt cannot parse) name the .gg file, not the copy.
bad=$(cd "$tmp/gg" && gofmt -l . 2>"$tmp/gg.err") || status=1
sed 's|^\./||; s|\.go:|.gg:|' "$tmp/gg.err" >&2
bad=$(echo "$bad" | sed 's|^\./||; s/\.go$/.gg/; /^$/d')
if [ -n "$bad" ]; then
    echo "lint: gofmt wants to reformat these .gg files (gofmt reads .go: format a .go copy):"
    echo "$bad" | sed 's/^/  /'
    status=1
fi
echo "lint: gofmt checked $(files go | wc -l | tr -d ' ') .go and $(wc -l < "$tmp/gg.list" | tr -d ' ') .gg files"

go vet ./... || status=1

if command -v shellcheck >/dev/null 2>&1; then
    shellcheck tools/*.sh || status=1
    echo "lint: shellcheck checked tools/*.sh"
else
    echo "lint: shellcheck skipped (not installed)"
fi
exit $status
