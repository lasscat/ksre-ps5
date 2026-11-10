#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run an ELF on the console as *homebrew* (a foreground app) and stream its output.

A payload sent straight to ps5-payload-elfldr (tools/deploy.sh, port 9021) runs
as a background process. Payloads that draw to the TV must be started by the
websrv homebrew launcher instead, which runs them as a foreground application.
This tool:

  1. uploads the ELF over FTP to /data/homebrew/<name>/eboot.elf (ftpsrv, default port 2121),
  2. asks websrv (default port 8080) to run it: GET /hbldr?path=...&pipe=1,
  3. prints the streamed stdout/stderr until the payload exits (or --timeout).

Requires ps5-payload-websrv and ftpsrv to be running on the console.
Standard library only.
"""

import argparse
import ftplib
import http.client
import sys
import time
import urllib.parse


def upload(host, port, name, payload):
    with open(payload, "rb") as f, ftplib.FTP() as ftp:
        ftp.connect(host, port, timeout=15)
        ftp.login()  # ftpsrv accepts anonymous
        for d in ("/data", "/data/homebrew", "/data/homebrew/" + name):
            try:
                ftp.mkd(d)
            except ftplib.error_perm:
                pass  # exists
        ftp.voidcmd("TYPE I")
        ftp.storbinary("STOR /data/homebrew/%s/eboot.elf" % name, f)
    return "/data/homebrew/%s/eboot.elf" % name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("payload")
    ap.add_argument("--name", required=True, help="directory name under /data/homebrew")
    ap.add_argument("--ftp-port", type=int, default=2121)
    ap.add_argument("--web-port", type=int, default=8080)
    ap.add_argument("--args", default="", help="argument string passed to the ELF")
    ap.add_argument("--log", help="also append received output to this file")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--no-upload", action="store_true", help="launch the ELF already on the console")
    a = ap.parse_args()

    path = "/data/homebrew/%s/eboot.elf" % a.name
    if not a.no_upload:
        print("ps5_hbrun: uploading %s -> %s" % (a.payload, path), file=sys.stderr)
        upload(a.host, a.ftp_port, a.name, a.payload)

    q = {"path": path, "pipe": "1", "cwd": "/data/homebrew/" + a.name}
    if a.args:
        q["args"] = a.args
    url = "/hbldr?" + urllib.parse.urlencode(q)
    print("ps5_hbrun: launching via websrv (port %d); streaming output" % a.web_port, file=sys.stderr)
    conn = http.client.HTTPConnection(a.host, a.web_port, timeout=15)
    conn.request("GET", url)
    resp = conn.getresponse()
    if resp.status != 200:
        print("ps5_hbrun: websrv answered HTTP %d (launch failed)" % resp.status, file=sys.stderr)
        return 2
    logf = open(a.log, "ab") if a.log else None
    deadline = time.time() + a.timeout
    conn.sock.settimeout(5)
    while time.time() < deadline:
        try:
            chunk = resp.read1(4096)
        except OSError:  # read timeout: keep waiting until the deadline
            continue
        if not chunk:
            break
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
        if logf:
            logf.write(chunk)
            logf.flush()
    if time.time() >= deadline:
        print("ps5_hbrun: timeout reached", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
