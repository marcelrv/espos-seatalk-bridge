#!/usr/bin/env python3
"""Telnet console for the seatalk-bridge bench app: send keys, capture output.

Default host is espos-f768.local (override with --host). Examples:

    python tools/stconsole.py                       # show output for 15 s
    python tools/stconsole.py -k "k" -s 20          # run the self-test, read 20 s
    python tools/stconsole.py -k "v" -s 10          # toggle the simulated boat
    python tools/stconsole.py -k "d" -s 2           # datagram table
    python tools/stconsole.py -k "t" -s 2           # one statistics block
    python tools/stconsole.py -k "8" -s 5           # heading via 89 <-> 9C
    python tools/stconsole.py --host 192.168.1.50 -k "pzk" -s 20

Keys are sent one by one with a short delay so the firmware sees each one.
"""

import argparse
import socket
import sys
import time

DEFAULT_HOST = "espos-f768.local"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"host (default {DEFAULT_HOST})")
    ap.add_argument("--port", type=int, default=23)
    ap.add_argument("-k", "--keys", default="", help="keys to send after connecting")
    ap.add_argument("-s", "--seconds", type=float, default=15.0, help="how long to capture output")
    ap.add_argument("-d", "--key-delay", type=float, default=0.3, help="delay between keys (s)")
    args = ap.parse_args()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=10)
    except OSError as e:
        print(f"stconsole: cannot connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        return 2
    sock.settimeout(0.2)

    out = sys.stdout
    end = time.time() + args.seconds
    keys = list(args.keys)
    next_key = time.time() + 0.8   # let the banner arrive first

    def pump():
        try:
            data = sock.recv(4096)
        except socket.timeout:
            return True
        except OSError:
            return False
        if not data:
            return False
        out.write(data.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", ""))
        out.flush()
        return True

    while time.time() < end:
        if keys and time.time() >= next_key:
            sock.sendall(keys.pop(0).encode("ascii"))
            next_key = time.time() + args.key_delay
        if not pump():
            print("\nstconsole: connection closed", file=sys.stderr)
            break
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
