#!/bin/sh
# Runs the examples/embed-c host programs (built by `make example-embed`
# into BUILD/embed-c) on example images and compares their output:
#   main IMAGE                    examples/many_idle against its expected.out
#   main --flash --allow-load     examples/hotload (modules in /flash, code.Load allowed)
#   main --flash                  the same with code.Load refused (hotload-refused.out)
#   main on examples/netdemo      refused: it needs the net capability
#   main --two A B                httpparse and many_idle stepped by one loop
#   post greeter.gogo             greeter/embed.out
#   port hostecho.gogo            examples/hostecho/embed.out
#   docs/book/18-embedding.md     every c (go) block is cut verbatim from
#                                 examples/embed-c/*.c (the Go programs it runs)
# Usage: examples/embed-c/test.sh BUILD   (from the repository root; needs BUILD/gogo)
set -u
B=${1:-build}
E=$B/embed-c
T=$E/run
rm -rf "$T"; mkdir -p "$T/img" "$T/flash"
fail=0
check() { # name want-file got-file
    if cmp -s "$2" "$3"; then echo "ok   $1"
    else echo "FAIL $1"; diff "$2" "$3" | head -20; fail=1; fi
}
for p in many_idle httpparse hotload hostecho netdemo embed-c/greeter; do
    "$B/gogo" build "examples/$p" -o "$T/img/$(basename "$p").gogo" || { echo "FAIL build $p"; exit 1; }
done
for m in worker_v1 worker_v2; do
    "$B/gogo" build "examples/hotload/mods/$m" -o "$T/flash/$m.gmod" || { echo "FAIL build $m"; exit 1; }
done

"$E/main" "$T/img/many_idle.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL main many_idle: exit status"; fail=1; }
check "main many_idle" examples/many_idle/expected.out "$T/out"

"$E/main" --flash "$T/flash" --allow-load "$T/img/hotload.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL main hotload: exit status"; fail=1; }
check "main --flash --allow-load hotload" examples/hotload/expected.out "$T/out"

"$E/main" --flash "$T/flash" "$T/img/hotload.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL main hotload refused: exit status"; fail=1; }
check "main --flash hotload (code.Load refused)" examples/embed-c/hotload-refused.out "$T/out"

if "$E/main" "$T/img/netdemo.gogo" > "$T/out" 2> "$T/err"; then echo "FAIL main netdemo: ran without the net capability"; fail=1
elif grep -q "image needs net" "$T/err" && grep -q "error -4" "$T/err"; then echo "ok   main netdemo (refused: needs net)"
else echo "FAIL main netdemo"; cat "$T/err"; fail=1; fi

"$E/main" --two "$T/img/httpparse.gogo" "$T/img/many_idle.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL main --two: exit status"; fail=1; }
sed -n 's/^a| //p' "$T/out" > "$T/a"
sed -n 's/^b| //p' "$T/out" > "$T/b"
check "main --two: httpparse" examples/httpparse/expected.out "$T/a"
check "main --two: many_idle" examples/many_idle/expected.out "$T/b"

"$E/post" "$T/img/greeter.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL post: exit status"; fail=1; }
check "post greeter" examples/embed-c/greeter/embed.out "$T/out"

"$E/port" "$T/img/hostecho.gogo" > "$T/out" 2> "$T/err" || { echo "FAIL port: exit status"; fail=1; }
check "port hostecho" examples/hostecho/embed.out "$T/out"
grep -qx "4 commands" "$T/err" || { echo "FAIL port: commands"; cat "$T/err"; fail=1; }

# The book chapter's snippets: each block must appear, as it is, in a source.
snip() { # lang sources...
    lang=$1; shift
    awk -v lang="$lang" '
        FNR == 1 { file++ }
        file < nsrc + 1 && FILENAME != book { src = src $0 "\n"; next }
        FILENAME == book && $0 == "```" lang { inb = 1; blk = ""; start = FNR; next }
        FILENAME == book && inb && $0 == "```" { inb = 0; n++
            if (index(src, blk) == 0) { print "FAIL book snippet at line " start " (" lang "): not in the example sources"; bad = 1 }
            next }
        FILENAME == book && inb { blk = blk $0 "\n" }
        END { if (bad) exit 1; if (n == 0) { print "FAIL book: no " lang " snippets"; exit 1 }; print "ok   book " lang " snippets (" n ")" }
    ' nsrc=$# book=docs/book/18-embedding.md "$@" docs/book/18-embedding.md || fail=1
}
snip c examples/embed-c/main.c examples/embed-c/post.c examples/embed-c/port.c
snip go examples/embed-c/greeter/main.go examples/hostecho/main.go

[ $fail = 0 ] && echo "example-embed: ok"
exit $fail
