#!/bin/sh
# Board ports (runtime/board/board_ports.c) on a gogovm built with the
# test drivers in ports/ (`make test-board-ports`):
#   - registration in name order, a later duplicate, a board driver's name
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

printf 'alpha\nrefuser\nticker\nzeta\n' > "$T/provides.want"
"$VM" --provides | tail -4 > "$T/provides"
if cmp -s "$T/provides.want" "$T/provides"; then echo "ok   provides: board ports in name order, refused ones left out"
else echo "FAIL provides"; diff "$T/provides.want" "$T/provides"; fail=1; fi

"$B/gogo" build "$D/prog" -o "$T/prog.ggb" || { echo "FAIL build $D/prog"; exit 1; }
GOGO_CONFIG=$T/no-config "$VM" "$T/prog.ggb" > "$T/out" 2> "$T/err"
rc=$?
if [ $rc = 0 ] && cmp -s "$D/prog/want.out" "$T/out"; then echo "ok   program (first alpha, refusal codes, posts from a thread)"
else echo "FAIL program: exit $rc"; diff "$D/prog/want.out" "$T/out"; cat "$T/err"; fail=1; fi

cat > "$T/err.want" <<'END'
gogo: board port "a_name_longer_than_thirty_one_bytes" not registered: the name is too long
gogo: board port "alpha" not registered: the name is taken (by a driver of the runtime or the board, or a board port before it)
gogo: board port "gpio" not registered: the name is taken (by a driver of the runtime or the board, or a board port before it)
END
if cmp -s "$T/err.want" "$T/err"; then echo "ok   boot lines for the refused ports"
else echo "FAIL boot lines"; diff "$T/err.want" "$T/err"; fail=1; fi

[ $fail = 0 ] && echo "test-board-ports: ok"
exit $fail
