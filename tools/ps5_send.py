#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Send a payload ELF to ps5-payload-elfldr and stream its output.

Equivalent to the SDK's `prospero-deploy` (socat) transport, using only the
Python standard library: connect, send the file, half-close the write side,
then print everything the payload writes to stdout/stderr (elfldr connects the
payload's stdio to this socket) until the payload exits, an end-marker line
(--until, default any "KSRE_PS5_<name>_END" line) arrives, --idle-timeout passes
without output, or --timeout expires.
"""

import argparse
import re
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
    ap.add_argument("--idle-timeout", type=float, default=0,
                    help="stop after this many seconds without output (0 = off)")
    ap.add_argument("--until", default=r"KSRE_PS5_[A-Za-z0-9_]*_END",
                    help="regex of the line that ends the run ('' = off)")
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
    start = last_data = time.monotonic()
    until = re.compile(args.until.encode()) if args.until else None
    pending = b""
    try:
        sock.settimeout(None)
        sock.sendall(data)
        sock.shutdown(socket.SHUT_WR)
        print(f"ps5_send: sent {len(data)} bytes; streaming output (Ctrl-C to stop)",
              file=sys.stderr)
        sock.settimeout(1.0)
        while True:
            now = time.monotonic()
            if args.timeout and now - start > args.timeout:
                print("ps5_send: timeout reached", file=sys.stderr)
                break
            if args.idle_timeout and now - last_data > args.idle_timeout:
                print("ps5_send: no output for %.0f s, stopping" % args.idle_timeout,
                      file=sys.stderr)
                break
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            last_data = time.monotonic()
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if log:
                log.write(chunk)
                log.flush()
            if until:
                lines = (pending + chunk).split(b"\n")
                pending = lines.pop()[-4096:]
                if any(until.search(line) for line in lines):
                    print("ps5_send: end marker seen after %.1f s" % (time.monotonic() - start),
                          file=sys.stderr)
                    break
    except KeyboardInterrupt:
        print("\nps5_send: interrupted", file=sys.stderr)
    finally:
        sock.close()
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
