#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Send a payload ELF to ps5-payload-elfldr and stream its output.

Equivalent to the SDK's `prospero-deploy` (socat) transport, using only the
Python standard library: connect, send the file, half-close the write side,
then print everything the payload writes to stdout/stderr (elfldr connects the
payload's stdio to this socket) until the payload exits or --timeout expires.
"""

import argparse
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("payload")
    ap.add_argument("--log", help="also append received output to this file")
    ap.add_argument("--timeout", type=float, default=0,
                    help="stop after this many seconds (0 = until the payload exits)")
    ap.add_argument("--connect-timeout", type=float, default=10)
    args = ap.parse_args()

    with open(args.payload, "rb") as f:
        data = f.read()

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.connect_timeout)
    except OSError as e:
        print(f"ps5_send: cannot connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        print("ps5_send: is ps5-payload-elfldr running on the console?", file=sys.stderr)
        return 3

    log = open(args.log, "ab") if args.log else None
    start = time.monotonic()
    try:
        sock.settimeout(None)
        sock.sendall(data)
        sock.shutdown(socket.SHUT_WR)
        print(f"ps5_send: sent {len(data)} bytes; streaming output (Ctrl-C to stop)",
              file=sys.stderr)
        sock.settimeout(1.0)
        while True:
            if args.timeout and time.monotonic() - start > args.timeout:
                print("ps5_send: timeout reached", file=sys.stderr)
                break
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if log:
                log.write(chunk)
                log.flush()
    except KeyboardInterrupt:
        print("\nps5_send: interrupted", file=sys.stderr)
    finally:
        sock.close()
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
