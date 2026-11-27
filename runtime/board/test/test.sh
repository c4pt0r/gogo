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

printf 'beta\nrefuser\nticker\nzeta\n' > "$T/provides.want"
"$VM" --provides | tail -4 > "$T/provides"
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

cat > "$T/err.want" <<'END'
gogo: board port "a_name_longer_than_thirty_one_bytes" not registered: the name is too long
gogo: board port "alpha" not registered: two board ports have this name (none of them is registered)
gogo: board port "alpha" not registered: two board ports have this name (none of them is registered)
gogo: board port "gpio" not registered: the name is taken (by a driver of the runtime or the board)
END
if cmp -s "$T/err.want" "$T/err"; then echo "ok   boot lines for the refused ports"
else echo "FAIL boot lines"; diff "$T/err.want" "$T/err"; fail=1; fi

[ $fail = 0 ] && echo "test-board-ports: ok"
exit $fail
