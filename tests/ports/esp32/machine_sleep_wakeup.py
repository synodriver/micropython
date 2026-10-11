# Wakeup configuration regression, not a physical wakeup/current measurement.
# Requires the selected pin to be unused by board peripherals. No output is driven.
import esp32
import machine
import os
from machine import Pin

chip = os.uname().machine.upper().replace("-", "")
if "ESP32H2" in chip:
    pin_number = 8  # Rejected by the old, incorrect classic-ESP32 RTC pin table.
elif "ESP32C5" in chip or "ESP32P4" in chip:
    pin_number = 1
elif "ESP32C2" in chip or "ESP32C3" in chip:
    pin_number = 3
elif "ESP32C6" in chip:
    pin_number = 2
else:
    pin_number = 4

p = Pin(pin_number, Pin.IN)
hz = machine.freq()
machine.freq(hz, hz, False)
esp32.wake_on_gpio(pins=[])
if hasattr(esp32, "wake_on_ext0"):
    esp32.wake_on_ext0(pin=None)
if hasattr(esp32, "wake_on_ext1"):
    esp32.wake_on_ext1(pins=[])


def rejected(call, exception):
    try:
        call()
    except exception:
        return
    raise AssertionError("configuration unexpectedly accepted")


try:
    rejected(lambda: p.irq(trigger=Pin.IRQ_RISING, wake=machine.SLEEP), ValueError)
    if hasattr(esp32, "wake_on_ext0"):
        p.irq(trigger=Pin.WAKE_HIGH, wake=machine.SLEEP)
        rejected(lambda: p.irq(trigger=Pin.WAKE_HIGH, wake=0), ValueError)
        p.irq(handler=None)
    else:
        rejected(lambda: p.irq(trigger=Pin.WAKE_HIGH, wake=machine.SLEEP), ValueError)
    print("Pin.irq validation OK")

    if hasattr(esp32, "wake_on_ext1"):
        # Configuration before enabling PM, and updates while PM is enabled.
        esp32.wake_on_ext1(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        machine.freq(hz, hz, True)
        # RTC and digital wakeup on the same pad must fail without committing
        # the GPIO config. This does not depend on which level the pin reads.
        rejected(lambda: esp32.wake_on_gpio(pins=(p,)), OSError)
        esp32.wake_on_ext1(level=esp32.WAKEUP_ALL_LOW)
        esp32.wake_on_ext1(pins=[])
        # Configuring after enabling PM must work too.
        esp32.wake_on_ext1(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        esp32.wake_on_ext1(pins=[])
    else:
        esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        machine.freq(hz, hz, True)
        esp32.wake_on_gpio(level=esp32.WAKEUP_ALL_LOW)
        esp32.wake_on_gpio(pins=[])
        esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        esp32.wake_on_gpio(pins=[])
    print("automatic wakeup updates OK")

    if hasattr(esp32, "wake_on_ext0"):
        p.irq(trigger=Pin.WAKE_HIGH, wake=machine.DEEPSLEEP)
        # A deep-sleep-only EXT0 must not conflict with automatic GPIO wakeup.
        esp32.wake_on_gpio(pins=(p,))
        rejected(lambda: p.irq(trigger=Pin.WAKE_HIGH, wake=machine.SLEEP), OSError)
        esp32.wake_on_gpio(level=esp32.WAKEUP_ANY_HIGH)
        esp32.wake_on_gpio(pins=[])
        p.irq(handler=None)
    print("sleep mode selection OK")

    machine.freq(hz, hz, False)
    rejected(lambda: machine.lightsleep(-1), ValueError)
    machine.lightsleep(1)
    print("manual sleep OK")
finally:
    machine.freq(hz, hz, False)
    esp32.wake_on_gpio(pins=[])
    if hasattr(esp32, "wake_on_ext0"):
        esp32.wake_on_ext0(pin=None)
    if hasattr(esp32, "wake_on_ext1"):
        esp32.wake_on_ext1(pins=[])
    p.irq(handler=None)
