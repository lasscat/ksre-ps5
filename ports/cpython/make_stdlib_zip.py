#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack the pure-Python stdlib of the PS5 CPython build into python312.zip.

Usage: make_stdlib_zip.py LIBDIR OUT.zip [--target-zip PATH] [--include PREFIX ...]

LIBDIR is the installed lib/python3.12 of the target build (it carries the target's
_sysconfigdata). Every .py file is stored with an *unchecked hash-based* .pyc next to it
(PEP 552), because zipimport has no __pycache__ support and cannot write bytecode; the
.py stays for tracebacks. Entries are sorted and time-stamped 1980-01-01, so the same
input gives the same zip. Must run with the matching host Python (same magic number).

With --include only files under the given relative prefixes are packed (no exclude
list, no _sysconfigdata check): used for the small test-suite zip (03b), built from
the source tree's Lib/ because the install has no test package.
"""
import argparse
import os
import py_compile
import sys
import tempfile
import zipfile

# Top-level names left out: tests, GUI/IDE, installers and tools for other platforms.
EXCLUDE_TOP = {
    "test", "idlelib", "tkinter", "turtledemo", "turtle.py", "ensurepip", "lib2to3",
    "__phello__", "site-packages", "lib-dynload", "venv", "pydoc_data",
}
EXCLUDE_PREFIX = ("config-",)
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def wanted(rel, include=None):
    parts = rel.split("/")
    if include is not None:  # data files only when named exactly
        return rel in include or (rel.endswith(".py") and "__pycache__" not in parts and any(
            rel.startswith(p.rstrip("/") + "/") for p in include))
    if parts[0] in EXCLUDE_TOP or parts[0].startswith(EXCLUDE_PREFIX):
        return False
    if "__pycache__" in parts:
        return False
    return rel.endswith(".py")


def add(zf, arcname, data):
    info = zipfile.ZipInfo(arcname, FIXED_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libdir")
    ap.add_argument("out")
    ap.add_argument("--target-zip", default="/data/ksre-ps5/python/lib/python312.zip",
                    help="path of the zip on the console (recorded in code objects)")
    ap.add_argument("--include", action="append",
                    help="pack only these relative paths/prefixes (repeatable)")
    args = ap.parse_args()

    if sys.version_info[:2] != (3, 12):
        sys.exit("run this with the host Python 3.12 (bytecode must match the target)")

    files = []
    for root, dirs, names in os.walk(args.libdir):
        dirs.sort()
        for name in names:
            rel = os.path.relpath(os.path.join(root, name), args.libdir).replace(os.sep, "/")
            if wanted(rel, args.include):
                files.append(rel)
    files.sort()
    if args.include is None and not any(f.startswith("_sysconfigdata_") for f in files):
        sys.exit("no _sysconfigdata_*.py in %s (not a target install?)" % args.libdir)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp_out = args.out + ".part"
    with tempfile.TemporaryDirectory() as tmp, \
            zipfile.ZipFile(tmp_out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        cfile = os.path.join(tmp, "x.pyc")
        for rel in files:
            src = os.path.join(args.libdir, rel)
            with open(src, "rb") as f:
                add(zf, rel, f.read())
            if not rel.endswith(".py"):
                continue
            py_compile.compile(
                src, cfile=cfile, dfile=args.target_zip + "/" + rel, doraise=True,
                invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH)
            with open(cfile, "rb") as f:
                add(zf, rel + "c", f.read())
    os.replace(tmp_out, args.out)
    print("stdlib zip: %s (%d modules, %d bytes)" % (args.out, len(files), os.path.getsize(args.out)))


if __name__ == "__main__":
    main()
