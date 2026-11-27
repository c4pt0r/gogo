#!/usr/bin/env python3
"""ble_central.py: a computer as the phone of a board's "ble" port.

Checks a board that runs examples/ble-echo (gogo build examples/ble-echo
-o echo.ggb && gogo flash echo.ggb) over the real radio, with the computer
as the central: scan, connect, read, write, subscribe, notify, a throughput
figure and disconnect. Needs Python 3.9+ and bleak (pip install bleak); on
macOS the terminal needs the Bluetooth permission (System Settings, Privacy
& Security, Bluetooth), or the scan fails or finds nothing.

    python3 tools/ble_central.py [--name gogo-echo] [--count 200] [--size N] [--timeout 10]

Prints one line per check and exits 0 if every check passed. The
program's own lines (gogo monitor) show its side: connected, subscribed,
echo, flood ..., disconnected.
"""
import argparse
import asyncio
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("ble_central.py needs bleak: pip install bleak")

SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
COUNTER = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # Read | Notify
COMMAND = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Write | WriteNoResp
REPLY = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"    # Notify

failed = 0


def check(ok, what):
    global failed
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failed += 1


async def main(args):
    t0 = time.monotonic()
    dev = await BleakScanner.find_device_by_name(args.name, timeout=args.timeout)
    check(dev is not None, "scan: found %s in %.1f s%s" % (
        args.name, time.monotonic() - t0, " (%s)" % dev.address if dev else ""))
    if dev is None:
        print("     (nothing found: is the program running, and does the terminal have the Bluetooth permission?)")
        return
    t0 = time.monotonic()
    async with BleakClient(dev, timeout=args.timeout) as c:
        check(c.is_connected, "connect in %.1f s, mtu %d" % (time.monotonic() - t0, c.mtu_size))
        svc = c.services.get_service(SVC)
        check(svc is not None, "service %s with %d characteristics" % (SVC, len(svc.characteristics) if svc else 0))

        v = await c.read_gatt_char(COUNTER)
        check(v.isdigit(), "read counter: %r" % bytes(v))
        n0 = int(v)

        replies, counters = asyncio.Queue(), asyncio.Queue()
        await c.start_notify(REPLY, lambda _, d: replies.put_nowait(bytes(d)))
        await c.start_notify(COUNTER, lambda _, d: counters.put_nowait(bytes(d)))
        check(True, "subscribe reply and counter")

        await c.write_gatt_char(COMMAND, b"hello gogo", response=True)
        got = await asyncio.wait_for(replies.get(), args.timeout)
        check(got == b"hello gogo", "write (with response) and notify back: %r" % got)
        cnt = await asyncio.wait_for(counters.get(), args.timeout)
        check(cnt == str(n0 + 1).encode(), "counter notified: %r" % cnt)
        v = await c.read_gatt_char(COUNTER)
        check(v == str(n0 + 1).encode(), "read counter again: %r" % bytes(v))

        await c.write_gatt_char(COMMAND, b"no response", response=False)
        got = await asyncio.wait_for(replies.get(), args.timeout)
        check(got == b"no response", "write without response: %r" % got)
        await asyncio.wait_for(counters.get(), args.timeout)

        # throughput: the board sends count notifications of MTU - 3 bytes
        # (or --size bytes)
        size, n, t0 = 0, 0, time.monotonic()
        cmd = b"flood %d" % args.count + (b" %d" % args.size if args.size else b"")
        await c.write_gatt_char(COMMAND, cmd, response=True)
        await asyncio.wait_for(counters.get(), args.timeout)
        first = None
        while n < args.count:
            d = await asyncio.wait_for(replies.get(), args.timeout)
            first = first or time.monotonic()
            n += 1
            size += len(d)
        dt = time.monotonic() - (first or t0)
        check(n == args.count, "throughput: %d notifications of %d bytes, %d bytes in %.2f s: %.1f KB/s" % (
            n, size // max(n, 1), size, dt, size / dt / 1024 if dt > 0 else 0))

        await c.stop_notify(REPLY)
        await c.stop_notify(COUNTER)
    check(not c.is_connected, "disconnect")
    # the board advertises again
    dev = await BleakScanner.find_device_by_name(args.name, timeout=args.timeout)
    check(dev is not None, "advertising again after the disconnect")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", default="gogo-echo", help="the advertised name (default gogo-echo)")
    ap.add_argument("--count", type=int, default=200, help="notifications for the throughput check")
    ap.add_argument("--size", type=int, default=0, help="bytes per notification (default MTU - 3)")
    ap.add_argument("--timeout", type=float, default=10.0, help="seconds per step")
    a = ap.parse_args()
    try:
        asyncio.run(main(a))
    except Exception as e:  # bleak raises its own errors for permissions, timeouts
        check(False, "%s: %s" % (type(e).__name__, e))
    sys.exit(1 if failed else 0)
