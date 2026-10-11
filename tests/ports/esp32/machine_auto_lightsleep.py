# API/notification regression test. Actual sleep current needs hardware measurement.
import machine
import micropython
import os
import time

original_freq = machine.freq()
try:
    machine.freq(original_freq, original_freq, True)
except TypeError:
    # Firmware predating the frequency-range interface.
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
    print("three-argument range API OK")

    try:
        machine.freq(original_freq, True)
    except TypeError:
        pass
    else:
        raise AssertionError("two-argument form should be rejected")
    print("two-argument form rejected OK")

    for args in (
        (original_freq + 1, original_freq + 1, False),
        (original_freq + 1000000, original_freq, False),
    ):
        try:
            machine.freq(*args)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid frequency range should be rejected")
    print("frequency range validation OK")

    chip = os.uname().machine.upper().replace("-", "")
    min_supported_freq = (
        16000000 if "ESP32H2" in chip else 80000000 if "ESP32C2" in chip else 20000000
    )
    machine.freq(min_supported_freq, original_freq, False)
    assert min_supported_freq <= machine.freq() <= original_freq
    machine.freq(original_freq)
    print("variable frequency range OK")

    # The range and one-argument calls above disabled automatic sleep.
    machine.freq(original_freq, original_freq, True)
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

    machine.freq(original_freq, original_freq, True)
    machine.freq(original_freq, original_freq, False)
    machine.lightsleep(1)
    print("explicit disable OK")
finally:
    machine.freq(original_freq, original_freq, False)
