# SPDX-License-Identifier: GPL-3.0-or-later
# Experiment 03b: import sweep + curated CPython test-suite subset, run by the 03 payload:
#   upload to /data/ksre-ps5/03/tests.py and write /data/ksre-ps5/03/args.txt with
#   "--script /data/ksre-ps5/03/tests.py [MODULE ...]"   (default: DEFAULT_MODULES)
# The tests come from lib/python312-tests.zip (ports/cpython/build.sh ps5).
# Output: "03T ..." lines; per-module "03T MODULE", final "03T SUMMARY".
import faulthandler
import io
import os
import shutil
import sys
import time
import unittest

T_START = time.monotonic()
if os.environ.get("KSRE_PS5_T0"):
    print("03T first-line t=%.3fs after payload start" % (
        time.clock_gettime(time.CLOCK_MONOTONIC) - float(os.environ["KSRE_PS5_T0"])))
ON_PS5 = sys.platform.startswith("freebsd")
HOME = os.path.dirname(os.path.dirname(sys.path[0]))  # .../python (sys.path[0] = lib/python312.zip)
TEST_ZIP = os.path.join(HOME, "lib", "python312-tests.zip")
WORK = "/data/ksre-ps5/03/work" if ON_PS5 else os.path.abspath("build/03-tests-work")
MODULE_TIMEOUT = 300  # faulthandler watchdog per module: dump tracebacks and _exit(1)

DEFAULT_MODULES = [
    "test.test_struct", "test.test_json", "test.test_zlib", "test.test_time",
    "test.test_float", "test.test_math", "test.test_threading", "test.test_os",
    "test.test_asyncio.test_futures", "test.test_asyncio.test_tasks",
    "test.test_asyncio.test_locks", "test.test_asyncio.test_queues",
    "test.test_asyncio.test_runners", "test.test_asyncio.test_timeouts",
    "test.test_asyncio.test_taskgroups", "test.test_asyncio.test_base_events",
]

# Imports Ren'Py 8.5 / KSRE rely on (directly or via the stdlib); all must work.
TARGET_IMPORTS = """
abc argparse array ast asyncio atexit audioop base64 binascii bisect bz2 calendar cmath
codecs collections collections.abc colorsys concurrent.futures contextlib contextvars copy
copyreg csv dataclasses datetime decimal difflib dis email encodings.utf_8 encodings.idna
enum errno faulthandler fnmatch fractions functools gc gettext glob gzip hashlib heapq hmac
html html.parser http.client importlib importlib.machinery importlib.resources importlib.util
inspect io itertools json keyword linecache locale logging lzma marshal math mmap
operator os pathlib pickle pkgutil platform plistlib posixpath pprint queue random re
resource secrets select selectors shlex shutil signal socket socketserver stat statistics
string struct subprocess sys sysconfig tarfile tempfile textwrap threading time timeit
token tokenize traceback types typing unicodedata urllib.parse urllib.request uuid warnings
wave weakref xml.etree.ElementTree xml.dom.minidom zipfile zipimport zlib zoneinfo
""".split()
# Known missing in this build (ports/cpython/README.md): reported, not counted as failures.
EXPECTED_MISSING = {"ctypes", "ssl", "sqlite3", "grp", "curses", "readline", "dbm",
                    "_ctypes", "_ssl", "_hashlib", "_sqlite3", "_curses", "_dbm", "_gdbm",
                    "multiprocessing", "_multiprocessing", "tkinter", "turtle", "idlelib",
                    "_tkinter", "_posixshmem", "_uuid", "_crypt", "crypt", "nis", "spwd",
                    "_curses_panel", "ossaudiodev", "msvcrt", "winreg", "winsound", "_winapi",
                    "_overlapped", "nt", "_scproxy", "pty", "tty", "_testcapi"}
SKIP_IMPORT = {"antigravity", "this", "__hello__", "__phello__", "idlelib", "tkinter",
               "turtle", "turtledemo", "test", "lib2to3", "ensurepip", "venv", "pydoc_data"}


def no_process_spawning():
    """fork/vfork/exec are statically linked SDK code of unknown behaviour (03a) and
    exec-family syscalls may raise SIGSYS: make every spawn path fail with ENOSYS."""
    import errno
    import _posixsubprocess

    def enosys(*a, **k):
        raise OSError(errno.ENOSYS, "process spawning disabled on the PS5 (03b)")
    _posixsubprocess.fork_exec = enosys
    for name in list(vars(os)):
        if name in ("fork", "forkpty", "system", "posix_spawn", "posix_spawnp") or \
                name.startswith(("exec", "spawn")):
            delattr(os, name)
    import subprocess
    subprocess._USE_POSIX_SPAWN = False
    subprocess._USE_VFORK = False


def import_sweep():
    import importlib
    # Import asyncio before the sweep reaches the builtin _asyncio: importing _asyncio
    # first creates a second module instance during asyncio's own import, and
    # asyncio.current_task() then returns None inside tasks (upstream 3.12 behaviour,
    # reproduced on the host build too; 03b).
    import asyncio  # noqa: F401
    import pkgutil
    bad, missing, ok = [], [], 0
    names = set(TARGET_IMPORTS) | set(sys.builtin_module_names)
    names |= {m.name for m in pkgutil.iter_modules([sys.path[0]])}
    for name in sorted(names - SKIP_IMPORT):
        try:
            importlib.import_module(name)
            ok += 1
        except Exception as e:  # noqa: BLE001
            entry = "%s(%s: %s)" % (name, type(e).__name__, str(e)[:60])
            (missing if name.split(".")[0] in EXPECTED_MISSING else bad).append(entry)
    target_bad = [b for b in bad if b.split("(")[0] in TARGET_IMPORTS]
    print("03T IMPORTS ok=%d failed=%d expected-missing=%d target-failed=%d"
          % (ok, len(bad), len(missing), len(target_bad)))
    for b in bad:
        print("03T IMPORT-FAIL %s" % b)
    print("03T IMPORT-EXPECTED-MISSING %s" % " ".join(m.split("(")[0] for m in missing))
    return bad


def prepare_env():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, "tmp"), exist_ok=True)
    os.chdir(WORK)  # os_helper.TESTFN is relative to the cwd ("/" on the console)
    os.environ["TMPDIR"] = os.path.join(WORK, "tmp")
    os.environ.setdefault("HOME", WORK)
    import tempfile
    tempfile.tempdir = os.environ["TMPDIR"]
    # Unpack the tests: unittest discovery (test_json, test_asyncio) and the data-file
    # lookups (math_testcases.txt, certdata/) need real directories, not a zip.
    import zipfile
    with zipfile.ZipFile(TEST_ZIP) as zf:
        zf.extractall(os.path.join(WORK, "lib"))
    sys.path.insert(0, os.path.join(WORK, "lib"))
    from test import support
    support.use_resources = []          # no network/cpu/largefile resource tests
    support.has_subprocess_support = False
    support.has_fork_support = False
    support.verbose = 0


def run_module(name, totals, failed_ids):
    import importlib
    t0 = time.monotonic()
    faulthandler.dump_traceback_later(MODULE_TIMEOUT, exit=True)
    try:
        mod = importlib.import_module(name)
        suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
        # Quiet runner: only failure tracebacks and the module line reach stdout (the
        # elfldr socket); a verbose stream made the console log ~5000 lines.
        res = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
        for t, tb in res.failures + res.errors:
            print("03T TRACEBACK %s\n%s" % (t.id(), tb.rstrip()))
    except Exception as e:  # noqa: BLE001
        print("03T MODULE %s ERROR %s: %s" % (name, type(e).__name__, e))
        totals["errors"] += 1
        failed_ids.append(name)
        return
    finally:
        faulthandler.cancel_dump_traceback_later()
    counts = (res.testsRun, len(res.failures), len(res.errors), len(res.skipped))
    for k, v in zip(("run", "failures", "errors", "skipped"), counts):
        totals[k] += v
    failed_ids.extend(t.id() for t, _ in res.failures + res.errors)
    print("03T MODULE %s run=%d fail=%d err=%d skip=%d time=%.1fs"
          % ((name,) + counts + (time.monotonic() - t0,)))


def main():
    print("03T python %s platform=%s" % (sys.version.split()[0], sys.platform))
    if ON_PS5:
        no_process_spawning()
    bad_imports = import_sweep()
    prepare_env()
    modules = sys.argv[1:] or DEFAULT_MODULES
    totals = dict(run=0, failures=0, errors=0, skipped=0)
    failed_ids = []
    for name in modules:
        run_module(name, totals, failed_ids)
    import resource
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    rss_mib = rss / (1024.0 * 1024 if sys.platform == "darwin" else 1024.0)
    summary = ("03T SUMMARY modules=%d run=%d fail=%d err=%d skip=%d imports-failed=%d "
               "time=%.1fs ru_maxrss=%.1fMiB" % (
                   len(modules), totals["run"], totals["failures"], totals["errors"],
                   totals["skipped"], len(bad_imports), time.monotonic() - T_START, rss_mib))
    for t in failed_ids:
        print("03T FAILED %s" % t)
    print(summary)
    if ON_PS5:
        try:
            with open("/data/ksre-ps5/03/last-tests.log", "w", encoding="utf-8") as f:
                f.write("\n".join(["FAILED " + t for t in failed_ids] + [summary, ""]))
        except OSError as e:
            print("03T note: cannot write last-tests.log: %s" % e)
    os.chdir("/" if ON_PS5 else os.path.dirname(WORK))
    return 0 if not (failed_ids or bad_imports) else 1


raise SystemExit(main())
