# API/notification regression test. Actual sleep current needs hardware measurement.
import machine
import micropython
import time

original_freq = machine.freq()
try:
    machine.freq(original_freq, True)
except TypeError:
    # Firmware predating the optional argument.
    print("SKIP")
    raise SystemExit
except ValueError as error:
    # A custom firmware may intentionally disable PM or tickless idle.
    if str(error) != "automatic light sleep requires PM and tickless idle":
        raise
    print("SKIP")
    raise SystemExit

try:
    assert machine.freq() == original_freq
    print("optional argument OK")

    for sleep in (machine.lightsleep, machine.deepsleep):
        try:
            sleep(1)
            assert False
        except ValueError:
            pass
    print("manual sleep guard OK")

    callbacks = []
    micropython.schedule(lambda value: callbacks.append(value), 42)
    time.sleep_ms(150)
    assert callbacks == [42]
    print("scheduled callback OK")

    start = time.ticks_ms()
    time.sleep_ms(150)
    assert time.ticks_diff(time.ticks_ms(), start) >= 150
    print("delay deadline OK")

    # The old one-argument setter switches back to the original no-sleep mode.
    machine.freq(original_freq)
    machine.lightsleep(1)
    print("one argument compatibility OK")

    machine.freq(original_freq, True)
    machine.freq(original_freq, False)
    machine.lightsleep(1)
    print("explicit disable OK")
finally:
    machine.freq(original_freq, False)
