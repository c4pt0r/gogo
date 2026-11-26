#!/bin/sh
# Lint (make lint):
#   gofmt     every .go file, and every .gg file (gogo source is Go syntax,
#             but gofmt only reads .go: each is copied as .go into a temp
#             dir and checked there); the files listed must be formatted
#   go vet    ./...
#   tools/*.sh checked by shellcheck, when it is installed (skipped otherwise)
# The files are the ones git knows (tracked or untracked, not ignored), so
# build output and nested worktrees are left out; outside a git checkout,
# every file under the repo except build/ and dot directories.
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

bad=$(files go | xargs gofmt -l)
if [ -n "$bad" ]; then
    echo "lint: gofmt wants to reformat (gofmt -w <file>):"; echo "$bad" | sed 's/^/  /'
    status=1
fi

files gg > "$tmp/gg.list"
while IFS= read -r f; do
    [ -f "$f" ] || continue
    mkdir -p "$tmp/gg/$(dirname "$f")"
    cp "$f" "$tmp/gg/${f%.gg}.go"
done < "$tmp/gg.list"
bad=$(cd "$tmp/gg" && gofmt -l . | sed 's/\.go$/.gg/')
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
