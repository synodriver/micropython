# PM lock ownership and lifecycle; run on an ESP32 firmware with PM enabled.
import gc
from errno import EBUSY, EOPNOTSUPP

try:
    import esp32

    PMLock = esp32.PMLock
    kind = esp32.PM_NO_LIGHT_SLEEP
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit


def active(name):
    for line in esp32.pm_dump_locks().splitlines():
        fields = line.split()
        if fields and fields[0] == name:
            return int(fields[3])
    return None


def raises(exception, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except exception as error:
        return error
    raise AssertionError("expected exception")


try:
    lock = PMLock(kind, name="pmtest")
except OSError as error:
    if error.errno != EOPNOTSUPP:
        raise
    print("SKIP")
    raise SystemExit

try:
    assert active("pmtest") == 0
    raises(RuntimeError, lock.release)
    assert lock.acquire() is None
    lock.acquire()
    assert active("pmtest") == 2
    assert raises(OSError, lock.deinit).errno == EBUSY
    assert active("pmtest") == 2
    lock.release()
    assert active("pmtest") == 1
    lock.release()
    print("recursive count and busy deinit OK")

    try:
        with lock as entered:
            assert entered is lock
            with lock:
                assert active("pmtest") == 2
                raise ValueError("body")
    except ValueError as error:
        assert str(error) == "body"
    else:
        raise AssertionError("exception swallowed")
    assert active("pmtest") == 0
    with lock:
        assert active("pmtest") == 1
    print("nested with, exception and reuse OK")

    other = PMLock(kind, name="pmtest_other")
    try:
        with lock, other:
            assert active("pmtest") == active("pmtest_other") == 1
        assert active("pmtest") == active("pmtest_other") == 0
    finally:
        other.deinit()
    print("independent locks OK")
finally:
    # Also clean up if an assertion failed after acquiring the lock.
    while active("pmtest"):
        lock.release()
    lock.deinit()

lock.deinit()
assert active("pmtest") is None
raises(ValueError, lock.acquire)
raises(ValueError, lock.release)
raises(ValueError, lock.__enter__)
print("deinit idempotence and invalid use OK")

for invalid in (-1, 3, 1000):
    raises(ValueError, PMLock, invalid)
raises(OverflowError, PMLock, 1 << 80)
raises(TypeError, PMLock, "bad")
raises(TypeError, PMLock, kind, "positional name")
raises(TypeError, PMLock, kind, name=b"bytes")
raises(ValueError, PMLock, kind, name="nul\0name")
for lock_kind in (esp32.PM_CPU_FREQ_MAX, esp32.PM_APB_FREQ_MAX, kind):
    lock = PMLock(lock_kind)
    with lock:
        pass
    lock.deinit()
print("types and arguments OK")

# A dynamically allocated name must remain valid after its original str dies.
lock = PMLock(kind, name="".join(["pmtest", "_name"]))
gc.collect()
assert active("pmtest_name") == 0
lock.deinit()
print("name lifetime OK")

for suffix in ("\u00e9", "\u4e2d", "\U0001f600"):
    # 14 ASCII bytes leave room for only part of the final UTF-8 character.
    lock = PMLock(kind, name="pmtest_unicode" + suffix)
    try:
        assert active("pmtest_unicode") == 0
        esp32.pm_dump_locks().encode("utf-8").decode("utf-8")
    finally:
        lock.deinit()
print("UTF-8 diagnostic name truncation OK")


def abandon():
    unused = PMLock(kind, name="pmtest_gc")
    unused.acquire()
    unused.acquire()
    assert active("pmtest_gc") == 2
    # Exercise collection of cycles as well as unreferenced lock objects.
    cycle = [unused]
    cycle.append(cycle)


abandon()
gc.collect()
gc.collect()
assert active("pmtest_gc") is None
print("GC releases all holds and deletes lock OK")
