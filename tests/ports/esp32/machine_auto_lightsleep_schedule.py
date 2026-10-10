# A callback queued while poll sleeps must be handled before poll checks again.
# For the latency regression, run without active USB or socket event callbacks.
import io
import machine
import micropython
import select
import time

try:
    import _thread

    machine.freq(machine.freq(), True)
except (ImportError, TypeError):
    print("SKIP")
    raise SystemExit
except ValueError as error:
    if str(error) != "automatic light sleep requires PM and tickless idle":
        raise
    print("SKIP")
    raise SystemExit


class Flag(io.IOBase):
    ready = False
    callback_time = 0

    def ioctl(self, request, flags):
        if request == 3:
            return flags if self.ready else 0
        return -1

    def set(self, unused):
        self.callback_time = time.ticks_ms()
        self.ready = True


flag = Flag()
poll = select.poll()
poll.register(flag, select.POLLIN)
finished = _thread.allocate_lock()
finished.acquire()


def notify():
    try:
        time.sleep_ms(30)
        micropython.schedule(flag.set, None)
    finally:
        finished.release()


started = False
try:
    _thread.start_new_thread(notify, ())
    started = True
    assert poll.poll(1000)
    # The old implementation waits a second 100 ms after the callback runs.
    assert time.ticks_diff(time.ticks_ms(), flag.callback_time) < 80
    print("callback during poll OK")
finally:
    if started:
        finished.acquire()
    machine.freq(machine.freq(), False)
