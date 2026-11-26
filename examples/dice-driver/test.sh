#!/bin/sh
# Runs examples/dice-driver (book chapter 22) and checks the chapter
# against the code it shows (`make example-driver`):
#   dice-board dice.ggb           the program on the test board, which has
#                                 the "dice" driver: stdout must equal driver.out
#   gogo run (gogovm)             refused: the image requires "dice" (exit 3)
#   docs/book/22-extending-with-c.md
#                                 every c block is cut verbatim from a source
#                                 file in the tree, every go block from
#                                 main.gg or lib/, and the text block after
#                                 "Its output" is driver.out
# Usage: examples/dice-driver/test.sh BUILD   (from the repository root)
set -u
B=${1:-build}
D=examples/dice-driver
BOOK=docs/book/22-extending-with-c.md
T=$B/dice-driver/run
rm -rf "$T"; mkdir -p "$T"
fail=0

"$B/gogo" build "$D" -o "$T/dice.ggb" || { echo "FAIL build $D"; exit 1; }

if "$B/dice-driver/dice-board" "$T/dice.ggb" > "$T/out" 2> "$T/err"; then
    if cmp -s "$D/driver.out" "$T/out"; then echo "ok   dice-board dice.ggb"
    else echo "FAIL dice-board dice.ggb"; diff "$D/driver.out" "$T/out" | head -20; fail=1; fi
    [ -s "$T/err" ] && { echo "FAIL dice-board: stderr"; cat "$T/err"; fail=1; }
else echo "FAIL dice-board: exit status"; cat "$T/err"; fail=1; fi

GOGO_VM="$B/gogovm" "$B/gogo" run "$D" > "$T/out" 2> "$T/err"
rc=$?
if [ $rc = 3 ] && grep -q "requires dice" "$T/err"; then echo "ok   gogovm refuses it (requires dice)"
else echo "FAIL gogovm: exit $rc"; cat "$T/err"; fail=1; fi

# snip LANG SOURCES...: each ```LANG block of the chapter must appear, as
# it is, in one of the sources.
snip() {
    lang=$1; shift
    awk -v lang="$lang" -v book="$BOOK" '
        FILENAME != book { src = src $0 "\n"; next }
        $0 == "```" lang { inb = 1; blk = ""; start = FNR; next }
        inb && $0 == "```" { inb = 0; n++
            if (index(src, blk) == 0) { print "FAIL book snippet at line " start " (" lang "): not in the sources"; bad = 1 }
            next }
        inb { blk = blk $0 "\n" }
        END { if (bad) exit 1; if (n == 0) { print "FAIL book: no " lang " snippets"; exit 1 }
              print "ok   book " lang " snippets (" n ")" }
    ' "$@" "$BOOK" || fail=1
}
snip c $D/dice.c $D/board.c runtime/vm/port.h runtime/gpio/gpio.h runtime/gpio/gpio.c \
    platform/unix/gpio_unix.c platform/esp32s3/main/gpio_esp.c platform/esp32s3/main/platform_esp.c \
    runtime/input/power.c runtime/input/input.h runtime/stream/stream.h runtime/net/tcp.c
snip go $D/main.gg lib/port/port.gg lib/gpio/gpio.gg

# The output block: the ```text block right after the line "Its output:".
awk '/^Its output:$/ { want = 1; next }
     want == 1 && $0 == "```text" { want = 2; next }
     want == 2 && $0 == "```" { exit }
     want == 2 { print }' "$BOOK" > "$T/book.out"
if cmp -s "$D/driver.out" "$T/book.out"; then echo "ok   book output block is driver.out"
else echo "FAIL book output block"; diff "$D/driver.out" "$T/book.out" | head -20; fail=1; fi

[ $fail = 0 ] && echo "example-driver: ok"
exit $fail
