# SPDX-License-Identifier: GPL-3.0-or-later
"""Start-up tracer: runs Ren'Py's real renpy.py under a profile hook.

Used as the launcher's "renpy.py" (`--renpy-base <dir containing this file>`) when the
payload dies before Ren'Py writes its first log line. Every *first* import, Python call
into Ren'Py's tree and C call is printed as one `PROBE ...` line straight to fd 1, followed
by a short pause so the line has left the console before the next step runs. The last
lines received therefore name the step that was running when the process (or console) died.

Environment:
  KSRE_PROBE_RENPY_BASE  real Ren'Py base (default: ../renpy next to this directory)
  KSRE_PROBE_PAUSE       seconds to pause after each line (default 0.004)
  KSRE_PROBE_ALL         1 = trace every Python call, not only files under the Ren'Py base
"""

import os
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
base = os.environ.get("KSRE_PROBE_RENPY_BASE") or os.path.join(os.path.dirname(here), "renpy")
pause = float(os.environ.get("KSRE_PROBE_PAUSE", "0.004"))
trace_all = os.environ.get("KSRE_PROBE_ALL") == "1"
t0 = time.monotonic()
seen = set()
n = [0]


def emit(kind, what):
    n[0] += 1
    os.write(1, ("PROBE %6d %8.3f %s %s\n" % (n[0], time.monotonic() - t0, kind, what)).encode("utf-8", "replace"))
    if pause:
        time.sleep(pause)


class ImportTracer:
    """meta_path entry that only reports imports, then defers to the real finders."""

    def find_spec(self, name, path=None, target=None):
        emit("import", name)
        return None


def hook(frame, event, arg):
    if event == "call":
        code = frame.f_code
        fn = code.co_filename
        if not trace_all and not fn.startswith(base):
            return
        key = (fn, code.co_firstlineno)
        if key in seen:
            return
        seen.add(key)
        emit("call", "%s:%d %s" % (fn[len(base) + 1:] if fn.startswith(base) else fn, code.co_firstlineno, code.co_name))
    elif event == "c_call":
        key = id(arg)
        if key in seen:
            return
        seen.add(key)
        mod = getattr(arg, "__module__", None)
        if mod is None:
            owner = getattr(arg, "__self__", None)
            mod = type(owner).__name__ if owner is not None else "?"
        emit("ccall", "%s.%s" % (mod, getattr(arg, "__qualname__", getattr(arg, "__name__", "?"))))


emit("start", "base=%s pause=%s argv=%r" % (base, pause, sys.argv))
real = os.path.join(base, "renpy.py")
with open(real, "r", encoding="utf-8") as f:
    src = f.read()
code = compile(src, real, "exec")
globals()["__file__"] = real
sys.argv[0] = real
# This file is called renpy.py too: drop its directory so "import renpy" finds the package.
sys.path[:] = [p for p in sys.path if os.path.abspath(p or ".") != here and not p.startswith(here + os.sep)]
sys.path += [os.path.join(base, "lib", "python"), base]
sys.meta_path.insert(0, ImportTracer())

import threading  # noqa: E402

threading.setprofile(hook)
sys.setprofile(hook)
try:
    exec(code, globals())  # defines renpy.py's functions and runs main() (__name__ is "__main__")
finally:
    sys.setprofile(None)
    emit("end", "probe finished")
