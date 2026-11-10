#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an ELF on the console as *homebrew* (a foreground app) and stream its output.

A payload sent straight to ps5-payload-elfldr (tools/deploy.sh, port 9021) runs
as a background process. Payloads that draw to the TV must be started by the
websrv homebrew launcher instead, which runs them as a foreground application.
This tool:

  1. uploads the ELF over FTP to /data/homebrew/<name>/eboot.elf (ftpsrv, default port 2121),
     plus any --extra files (e.g. Mesa .so libraries) next to it,
  2. asks websrv (default port 8080) to run it: GET /hbldr?path=...&pipe=1,
  3. prints the streamed stdout/stderr and stops as soon as the payload prints its end
     marker (--until, default any "KSRE_PS5_<name>_END" line), or when the connection
     closes, or when no output arrives for --idle-timeout seconds, or at --timeout.

The payload's working directory is /data/homebrew/<name>; the SDK's run-time
linker searches that directory for shared libraries a payload dlopen()s, so
--extra files placed there are found without touching /user/homebrew.

Requires ps5-payload-websrv and ftpsrv to be running on the console.
Standard library only.
"""

import argparse
import ftplib
import os
import re
import select
import socket
import sys
import time
import urllib.parse


def remote_size(ftp, path):
    """Size of a remote file, or None if it does not exist / SIZE is unsupported."""
    try:
        return ftp.size(path)
    except (ftplib.error_perm, ftplib.error_reply):
        return None


def upload(host, port, name, payload, extras=(), force=False):
    """Upload the ELF (always) and extra files (skipped when the size matches).

    extras: iterable of "LOCAL[:REMOTE_NAME]" strings; REMOTE_NAME defaults to
    the local file name. Symlinks are followed, so a library symlink such as
    libEGL.so.1 uploads the real file under the soname the loader looks for.
    """
    base = "/data/homebrew/" + name
    with ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=15)
        ftp.login()  # ftpsrv accepts anonymous
        for d in ("/data", "/data/homebrew", base):
            try:
                ftp.mkd(d)
            except ftplib.error_perm:
                pass  # exists
        ftp.voidcmd("TYPE I")
        with open(payload, "rb") as f:
            ftp.storbinary("STOR %s/eboot.elf" % base, f)
        for spec in extras:
            local, _, remote = spec.partition(":")
            remote = remote or os.path.basename(local)
            size = os.path.getsize(local)  # follows symlinks
            dest = "%s/%s" % (base, remote)
            if not force and remote_size(ftp, dest) == size:
                print("ps5_hbrun: %s already on the console (%d bytes), skipped" % (remote, size), file=sys.stderr)
                continue
            print("ps5_hbrun: uploading %s -> %s (%d bytes)" % (local, dest, size), file=sys.stderr)
            with open(local, "rb") as f:
                ftp.storbinary("STOR " + dest, f, blocksize=65536)
    return base + "/eboot.elf"



class _Chunked:
    """Incremental HTTP/1.1 chunked-transfer decoder (feed bytes, get payload bytes)."""

    def __init__(self):
        self.buf = b""
        self.left = 0  # payload bytes still expected in the current chunk
        self.done = False

    def feed(self, data):
        self.buf += data
        out = b""
        while not self.done:
            if self.left > 0:
                take = self.buf[: self.left]
                out += take
                self.buf = self.buf[len(take):]
                self.left -= len(take)
                if self.left > 0:
                    break
                continue
            if self.buf.startswith(b"\r\n"):  # CRLF that ends a chunk
                self.buf = self.buf[2:]
                continue
            nl = self.buf.find(b"\r\n")
            if nl < 0:
                break
            size = int(self.buf[:nl].split(b";")[0].strip() or b"0", 16)
            self.buf = self.buf[nl + 2:]
            if size == 0:
                self.done = True
            self.left = size
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("payload")
    ap.add_argument("--name", required=True, help="directory name under /data/homebrew")
    ap.add_argument("--ftp-port", type=int, default=2121)
    ap.add_argument("--web-port", type=int, default=8080)
    ap.add_argument("--args", default="", help="argument string passed to the ELF")
    ap.add_argument("--env", default="", help='space-separated VAR=value list for the payload, e.g. "MESA_DEBUG=1 EGL_LOG_LEVEL=debug"')
    ap.add_argument("--extra", action="append", default=[], metavar="LOCAL[:REMOTE]",
                    help="also upload this file next to eboot.elf (repeatable); skipped if the size already matches")
    ap.add_argument("--reupload", action="store_true", help="upload --extra files even if the size matches")
    ap.add_argument("--log", help="also append received output to this file")
    ap.add_argument("--timeout", type=float, default=120, help="hard limit in seconds")
    ap.add_argument("--idle-timeout", type=float, default=90, help="stop if the payload prints nothing for this long")
    ap.add_argument("--until", default=r"KSRE_PS5_[A-Za-z0-9_]*_END", help="regex of the line that marks the end of the run")
    ap.add_argument("--no-upload", action="store_true", help="launch the ELF already on the console")
    a = ap.parse_args()

    path = "/data/homebrew/%s/eboot.elf" % a.name
    if not a.no_upload:
        print("ps5_hbrun: uploading %s -> %s" % (a.payload, path), file=sys.stderr)
        upload(a.host, a.ftp_port, a.name, a.payload, a.extra, a.reupload)

    q = {"path": path, "pipe": "1", "cwd": "/data/homebrew/" + a.name}
    if a.args:
        q["args"] = a.args
    if a.env:
        q["env"] = a.env
    url = "/hbldr?" + urllib.parse.urlencode(q)
    print("ps5_hbrun: launching via websrv (port %d); streaming output" % a.web_port, file=sys.stderr)
    # A raw socket polled with select(): http.client's buffered reader cannot be used with a socket
    # timeout, because after the first timeout every later read raises "cannot read from timed out
    # object" and a quiet stretch (e.g. a long benchmark) then looks like a dead payload.
    sock = socket.create_connection((a.host, a.web_port), timeout=15)
    sock.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % (url, a.host)).encode())
    sock.settimeout(None)

    def recv(wait):
        """Bytes, b"" on EOF, or None if nothing arrived within `wait` seconds."""
        r, _, _ = select.select([sock], [], [], wait)
        return sock.recv(65536) if r else None

    head = b""
    start = time.time()
    while b"\r\n\r\n" not in head:
        if time.time() - start > 15:
            print("ps5_hbrun: no HTTP response headers from websrv", file=sys.stderr)
            return 2
        d = recv(1.0)
        if d == b"":
            print("ps5_hbrun: websrv closed the connection before answering", file=sys.stderr)
            return 2
        if d:
            head += d
    head, body = head.split(b"\r\n\r\n", 1)
    status_line = head.split(b"\r\n", 1)[0].decode("latin-1")
    parts = status_line.split()
    if len(parts) < 2 or parts[1] != "200":
        print("ps5_hbrun: websrv answered '%s' (launch failed)" % status_line, file=sys.stderr)
        return 2
    chunked = b"transfer-encoding: chunked" in head.lower()
    dec = _Chunked() if chunked else None

    logf = open(a.log, "ab") if a.log else None
    end_re = re.compile(a.until)
    deadline = start + a.timeout
    last_data = time.time()
    text = b""
    reason = None
    seen_end = False
    end_wait = 0.0

    def emit(raw):
        sys.stdout.write(raw.decode("utf-8", "replace"))
        sys.stdout.flush()
        if logf:
            logf.write(raw)
            logf.flush()

    d = body
    while True:
        if d:
            raw = dec.feed(d) if dec else d
            if raw:
                last_data = time.time()
                emit(raw)
                text += raw
                lines = text.split(b"\n")
                text = lines.pop()
                if not seen_end and any(end_re.search(l.decode("utf-8", "replace")) for l in lines):
                    seen_end = True
                    end_wait = time.time() + 2  # a moment for trailing lines, never wait for EOF
            if dec and dec.done:
                reason = "stream ended (payload exited)"
                break
        now = time.time()
        if seen_end and now >= end_wait:
            reason = "end marker seen"
            break
        if now >= deadline:
            reason = "hard timeout (%.0f s)" % a.timeout
            break
        if now - last_data >= a.idle_timeout:
            reason = "no output for %.0f s (payload hung or exited without an end marker)" % a.idle_timeout
            break
        d = recv(0.5)
        if d == b"":
            reason = "connection closed (payload exited)"
            break
    print("ps5_hbrun: stopped after %.0f s: %s" % (time.time() - start, reason), file=sys.stderr)
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
