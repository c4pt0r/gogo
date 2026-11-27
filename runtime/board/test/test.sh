#!/bin/sh
# Board ports (runtime/board/board_ports.c) on a gogovm built with the
# test drivers in ports/ (`make test-board-ports`):
#   - registration in name order; both of two ports of one name, a board driver's name
#     and a name too long refused, each with a boot line (--provides, stderr);
#   - a driver's refusal code reaching port.Open;
#   - events posted from another thread (gogo_poster_port_post), all in order.
# Usage: runtime/board/test/test.sh BUILD   (from the repository root)
set -u
B=${1:-build}
D=runtime/board/test
VM=$B/board-ports/gogovm
T=$B/board-ports/run
rm -rf "$T"; mkdir -p "$T"
fail=0

printf 'test.beta\ntest.burst\ntest.refuser\ntest.ticker\ntest.zeta\n' > "$T/provides.want"
"$VM" --provides | tail -5 > "$T/provides"
if cmp -s "$T/provides.want" "$T/provides"; then echo "ok   provides: board ports in name order, refused ones left out"
else echo "FAIL provides"; diff "$T/provides.want" "$T/provides"; fail=1; fi

"$B/gogo" build "$D/prog" -o "$T/prog.ggb" || { echo "FAIL build $D/prog"; exit 1; }
GOGO_CONFIG=$T/no-config "$VM" "$T/prog.ggb" > "$T/out" 2> "$T/err"
rc=$?
if [ $rc = 0 ] && cmp -s "$D/prog/want.out" "$T/out"; then echo "ok   program (board ports, refusal codes, posts from a thread)"
else echo "FAIL program: exit $rc"; diff "$D/prog/want.out" "$T/out"; cat "$T/err"; fail=1; fi

# A port closed while its thread waits for room (GOGO_EFULL): the
# driver's close stops the thread before it joins it, so the VM ends.
"$B/gogo" build "$D/abandon" -o "$T/abandon.ggb" || { echo "FAIL build $D/abandon"; exit 1; }
GOGO_CONFIG=$T/no-config "$VM" "$T/abandon.ggb" > "$T/abandon.out" 2> /dev/null &
pid=$!
n=0
while kill -0 $pid 2>/dev/null && [ $n -lt 100 ]; do sleep 0.1; n=$((n + 1)); done
if kill -0 $pid 2>/dev/null; then
    kill -9 $pid; echo "FAIL a ticker closed with a backlog: the VM hangs"; fail=1
elif wait $pid && grep -qx "open true" "$T/abandon.out"; then echo "ok   a ticker closed with a backlog ends"
else echo "FAIL abandon: exit status or output"; cat "$T/abandon.out"; fail=1; fi

# The board's post byte budget (ports.post_bytes, here 2K): a post past it
# is GOGO_EFULL (-4), and a thread that waits for room loses nothing.
"$B/gogo" build "$D/burst" -o "$T/burst.ggb" || { echo "FAIL build $D/burst"; exit 1; }
printf 'ports.post_bytes=2K\n' > "$T/budget-config"
printf '1000 bytes: 0\n4000 bytes: -4\nticker events: 2000\n' > "$T/burst.want"
GOGO_CONFIG=$T/budget-config "$VM" "$T/burst.ggb" > "$T/burst.out" 2> "$T/burst.err"
if cmp -s "$T/burst.want" "$T/burst.out"; then echo "ok   the post byte budget (ports.post_bytes)"
else echo "FAIL post budget"; diff "$T/burst.want" "$T/burst.out"; cat "$T/burst.err"; fail=1; fi

# A board port the build should have but does not register is said at boot.
GOGO_CONFIG=$T/no-config "$VM" --expect-ports test.beta,test.missing "$T/abandon.ggb" > /dev/null 2> "$T/expect.err"
if grep -qx 'gogo: board port "test.missing" expected but not registered: is its component linked whole (WHOLE_ARCHIVE), and its GOGO_BOARD_PORT named as its gogo-driver.json says?' "$T/expect.err" &&
   ! grep -q '"test.beta" expected' "$T/expect.err"; then echo "ok   an expected board port that is missing is said at boot"
else echo "FAIL --expect-ports"; cat "$T/expect.err"; fail=1; fi

cat > "$T/err.want" <<'END'
gogo: board port "gpio" not registered: a board port's name needs a vendor prefix and a '.' ("acme.sensor")
gogo: board port "test.a_name_longer_than_31_bytes" not registered: the name is too long (31 bytes at most)
gogo: board port "test.alpha" not registered: two board ports have this name (none of them is registered)
gogo: board port "test.alpha" not registered: two board ports have this name (none of them is registered)
gogo: board port "test.bad name" not registered: a bad name (letters, digits, '_', '-', '.') or entry (its size or driver)
END
if cmp -s "$T/err.want" "$T/err"; then echo "ok   boot lines for the refused ports"
else echo "FAIL boot lines"; diff "$T/err.want" "$T/err"; fail=1; fi

[ $fail = 0 ] && echo "test-board-ports: ok"
exit $fail
