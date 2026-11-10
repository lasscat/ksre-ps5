# SPDX-License-Identifier: GPL-3.0-or-later
# Experiment 03 test script, embedded into the payload by the Makefile (hello_py.h).
# One line per check: "03 PASS|FAIL <name> <detail>", then "03 SUMMARY pass=N fail=M".
import sys
import time

T0 = time.monotonic()
ON_PS5 = sys.platform.startswith("freebsd")
results = []


def check(name, fn):
    try:
        detail = fn()
    except BaseException as e:  # report everything, keep going
        import traceback
        print("03 FAIL %s %s: %s" % (name, type(e).__name__, e))
        traceback.print_exc()
        results.append((name, False))
    else:
        print("03 PASS %s %s" % (name, "" if detail is None else detail))
        results.append((name, True))


def t_encodings():
    import codecs
    import encodings  # noqa: F401
    s = "café € \U0001f600 日本"
    assert s.encode("utf-8").decode("utf-8") == s
    assert len("\U0001f600") == 1  # non-BMP char is one code point even with 16-bit wchar_t
    return "fs=%s stdout=%s cp932=%s" % (
        sys.getfilesystemencoding(), sys.stdout.encoding,
        codecs.lookup("cp932").name)


def t_zlib():
    import zlib
    data = bytes(range(256)) * 4096
    packed = zlib.compress(data, 6)
    assert zlib.decompress(packed) == data
    assert zlib.crc32(b"123456789") == 0xCBF43926
    return "1 MiB -> %d bytes, zlib %s" % (len(packed), zlib.ZLIB_RUNTIME_VERSION)


def t_json():
    import json
    obj = {"a": [1, 2.5, None, True], "b": "é\U0001f600", "c": {"d": -7}}
    assert json.loads(json.dumps(obj)) == obj
    import _json  # noqa: F401  (C accelerator is built in)
    return "round-trip ok"


def t_struct():
    import _struct
    import struct
    raw = struct.pack("<IdQh", 0xDEADBEEF, 1.5, 2**63 + 1, -2)
    assert struct.unpack("<IdQh", raw) == (0xDEADBEEF, 1.5, 2**63 + 1, -2)
    assert _struct.calcsize("<IdQh") == 22
    return "calcsize(P)=%d" % struct.calcsize("P")


def subnormal_ok():
    # True when subnormals survive (MXCSR FTZ/DAZ cleared in this thread).
    # FTZ flushes subnormal results (strtod, multiply); DAZ treats subnormal inputs as 0.
    tiny = float.fromhex("0x1p-1074")
    return tiny > 0.0 and tiny * 1.0 > 0.0 and float("4.9e-324") > 0.0


def t_math():
    import cmath
    import math
    assert math.isclose(math.sqrt(2.0) ** 2, 2.0)
    assert math.gamma(5) == 24.0 and math.factorial(20) == 2432902008176640000
    assert math.fsum([0.1] * 10) == 1.0
    assert cmath.sqrt(-1) == 1j
    assert math.isnan(float("nan")) and math.isinf(math.inf)
    assert subnormal_ok(), "subnormals flushed to zero (FTZ/DAZ still set)"
    return "subnormals ok, float_info.min=%r" % sys.float_info.min


def t_select():
    import select
    r, w, x = select.select([], [], [], 0)
    kinds = [k for k in ("kqueue", "epoll", "poll") if hasattr(select, k)]
    if ON_PS5:
        kq = select.kqueue()
        kq.close()
    return "available: %s" % ",".join(kinds)


def t_socket():
    import _socket  # noqa: F401
    import socket
    a, b = socket.socketpair()
    try:
        a.sendall(b"ping")
        assert b.recv(4) == b"ping"
    finally:
        a.close()
        b.close()
    return "socketpair ok, has_ipv6=%s" % socket.has_ipv6


def t_threading():
    import threading
    out = {}
    meet = threading.Barrier(4, timeout=10)  # all four alive at once => distinct idents

    def work(i):
        meet.wait()
        out[i] = (sum(range(100000 * (i + 1))), threading.get_ident(), subnormal_ok())

    threads = [threading.Thread(target=work, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(10)
    assert len(out) == 4, out
    assert len({v[1] for v in out.values()}) == 4
    # Reported, not required: whether new threads start with FTZ/DAZ cleared (R2b, 03b).
    return "4 threads ok, subnormals in threads=%s" % all(v[2] for v in out.values())


def t_asyncio():
    import asyncio

    async def step(i):
        await asyncio.sleep(0.01 * i)
        return i * i

    async def main():
        loop = asyncio.get_running_loop()
        vals = await asyncio.gather(*(step(i) for i in range(4)))
        return vals, type(loop._selector).__name__

    vals, sel = asyncio.run(main())
    assert vals == [0, 1, 4, 9], vals
    return "gather ok, selector=%s" % sel


def t_urandom():
    import os
    b = os.urandom(32)
    assert len(b) == 32 and b != os.urandom(32)
    return "32 bytes"


print("03 sys.version %s" % sys.version.replace("\n", " "))
print("03 sys.platform=%s implementation=%s maxunicode=%d utf8_mode=%d isolated=%d"
      % (sys.platform, sys.implementation.cache_tag, sys.maxunicode,
         sys.flags.utf8_mode, sys.flags.isolated))
print("03 sys.path=%r" % (sys.path,))
print("03 builtin modules=%d" % len(sys.builtin_module_names))

for _name, _fn in [("encodings", t_encodings), ("zlib", t_zlib), ("json", t_json),
                   ("_struct", t_struct), ("math", t_math), ("select", t_select),
                   ("_socket", t_socket), ("threading", t_threading),
                   ("asyncio", t_asyncio), ("os.urandom", t_urandom)]:
    check(_name, _fn)

failed = [n for n, ok in results if not ok]
summary = "03 SUMMARY pass=%d fail=%d time=%.2fs%s" % (
    len(results) - len(failed), len(failed), time.monotonic() - T0,
    (" failed=" + ",".join(failed)) if failed else "")
print(summary)

# On the console also leave a log on /data (read it over FTP if stdout did not stream).
if ON_PS5:
    try:
        import os
        os.makedirs("/data/ksre-ps5/03", exist_ok=True)
        with open("/data/ksre-ps5/03/last-run.log", "w", encoding="utf-8") as f:
            f.write(sys.version + "\n")
            for n, ok in results:
                f.write("%s %s\n" % ("PASS" if ok else "FAIL", n))
            f.write(summary + "\n")
    except OSError as e:
        print("03 note: could not write /data/ksre-ps5/03/last-run.log: %s" % e)

raise SystemExit(1 if failed else 0)
