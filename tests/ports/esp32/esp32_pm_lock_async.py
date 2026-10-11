from errno import EOPNOTSUPP

try:
    import asyncio
    import esp32

    lock = esp32.PMLock(esp32.PM_NO_LIGHT_SLEEP, name="pmtest_async")
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit
except OSError as error:
    if error.errno != EOPNOTSUPP:
        raise
    print("SKIP")
    raise SystemExit


def active():
    for line in esp32.pm_dump_locks().splitlines():
        fields = line.split()
        if fields and fields[0] == "pmtest_async":
            return int(fields[3])
    return None


async def main():
    ready = asyncio.Event()
    wait = asyncio.Event()

    async def worker():
        with lock:
            ready.set()
            await wait.wait()

    task = asyncio.create_task(worker())
    await ready.wait()
    assert active() == 1
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    assert active() == 0
    print("cancellation releases lock OK")


try:
    asyncio.run(main())
finally:
    while active():
        lock.release()
    lock.deinit()
    asyncio.new_event_loop()
assert active() is None
print("async cleanup OK")
