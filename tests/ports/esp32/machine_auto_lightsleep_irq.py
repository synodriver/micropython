# Run on a board with the selected GPIO unused and held low (internal pulldown).
# Exercises registration/delivery, not physical wakeup or power consumption.
import asyncio
import esp32
import machine
import micropython
import os
import time
from machine import Pin

chip = os.uname().machine.upper().replace("-", "")
pin_number = (
    8
    if "ESP32H2" in chip
    else 1
    if "ESP32C5" in chip or "ESP32P4" in chip
    else 3
    if "ESP32C2" in chip or "ESP32C3" in chip
    else 2
    if "ESP32C6" in chip
    else 4
)
p = Pin(pin_number, Pin.IN, Pin.PULL_DOWN)
hz = machine.freq()
machine.freq(hz)
esp32.wake_on_gpio(pins=[])
if hasattr(esp32, "wake_on_ext0"):
    esp32.wake_on_ext0(pin=None)
if hasattr(esp32, "wake_on_ext1"):
    esp32.wake_on_ext1(pins=[])

hits = []


def handler(pin):
    hits.append(pin)


def noop(_):
    pass


def rejected(call):
    try:
        call()
    except OSError:
        return
    raise AssertionError("conflicting wakeup source accepted")


try:
    irq = p.irq(handler=handler, trigger=Pin.IRQ_RISING)
    machine.freq(hz, hz, True)  # Register before enabling automatic sleep.
    assert irq.trigger() == Pin.IRQ_RISING

    def saturate(_):
        # Scheduler callbacks cannot run recursively here. A static node must
        # still deliver the IRQ even when the ordinary Python queue is full.
        for unused in range(32):
            try:
                micropython.schedule(noop, None)
            except RuntimeError:
                break
        irq()
        irq()  # Coalesces with the pending notification for this pin.

    micropython.schedule(saturate, None)
    time.sleep_ms(150)
    assert hits == [p]
    print("full queue and coalescing OK")

    def cancel(_):
        irq()
        p.irq(handler=None)

    hits.clear()
    micropython.schedule(cancel, None)
    time.sleep_ms(30)
    assert hits == []
    print("pending cancellation OK")

    # Configure in the reverse order: PM first, then IRQ registration.
    irq = p.irq(handler=handler, trigger=Pin.IRQ_RISING)
    rejected(lambda: esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH))
    assert irq.trigger() == Pin.IRQ_RISING
    irq()
    time.sleep_ms(30)
    assert hits == [p]
    p.irq(handler=None)
    wake_source = getattr(esp32, "wake_on_ext1", esp32.wake_on_gpio)
    wake_source(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
    rejected(lambda: p.irq(handler=handler, trigger=Pin.IRQ_RISING))
    assert p.irq().trigger() == 0
    wake_source(pins=[])
    print("conflict rollback in both orders OK")

    if hasattr(esp32, "wake_on_ext1"):
        p.irq(handler=handler, trigger=Pin.IRQ_RISING)
        rejected(lambda: esp32.wake_on_ext1(pins=(p,), level=esp32.WAKEUP_ANY_HIGH))
        p.irq(handler=None)
        esp32.wake_on_ext1(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        rejected(lambda: p.irq(handler=handler, trigger=Pin.IRQ_RISING))
        assert p.irq().trigger() == 0
        esp32.wake_on_ext1(pins=[])
    print("EXT1 conflict rollback OK")

    irq = p.irq(handler=handler, trigger=Pin.IRQ_HIGH_LEVEL)
    time.sleep_ms(30)
    assert irq.trigger() == Pin.IRQ_HIGH_LEVEL
    assert irq.trigger(Pin.IRQ_RISING) == Pin.IRQ_HIGH_LEVEL
    assert irq.trigger() == Pin.IRQ_RISING
    print("logical trigger getter and setter OK")

    flag = asyncio.ThreadSafeFlag()
    p.irq(handler=lambda pin: flag.set(), trigger=Pin.IRQ_RISING)

    async def notify():
        await asyncio.sleep_ms(20)
        irq()

    async def run():
        task = asyncio.create_task(notify())
        await asyncio.wait_for_ms(flag.wait(), 500)
        await task

    asyncio.run(run())
    print("asyncio notification OK")

    # No new electrical input: timer wakeups must not replay old notifications.
    p.irq(handler=handler, trigger=Pin.IRQ_RISING)
    hits.clear()
    time.sleep_ms(250)
    assert hits == []
    machine.freq(hz)

    def twice(_):
        irq()
        irq()

    micropython.schedule(twice, None)
    time.sleep_ms(30)
    assert hits == [p, p]
    print("original single-argument and IRQ scheduling OK")

    # A conflict registered while automatic sleep is off must also be rejected
    # when enabling PM, without disabling the existing ordinary IRQ.
    machine.freq(hz)
    esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
    rejected(lambda: machine.freq(hz, hz, True))
    assert p.irq().trigger() == Pin.IRQ_RISING
    print("enable validation OK")

    esp32.wake_on_gpio(pins=[])
    p.irq(handler=None)
    for _ in range(3):
        wake_source(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
        machine.freq(hz, hz, True)
        rejected(lambda: p.irq(handler=handler, trigger=Pin.IRQ_RISING))
        # Remove the explicit owner, then acquire the same input for IRQ use.
        wake_source(pins=[])
        irq = p.irq(handler=handler, trigger=Pin.IRQ_RISING)
        hits.clear()
        time.sleep_ms(150)
        assert hits == []
        irq()
        time.sleep_ms(30)
        assert hits == [p]
        assert irq.trigger() == Pin.IRQ_RISING
        wake_pins = machine.wake_pins()
        assert isinstance(wake_pins, tuple)
        assert all(isinstance(pin, int) for pin in wake_pins)
        assert tuple(sorted(set(wake_pins))) == wake_pins
        p.irq(handler=None)
        machine.freq(hz)
    print("repeated wake resource handover and query compatibility OK")
finally:
    p.irq(handler=None)
    machine.freq(hz)
    esp32.wake_on_gpio(pins=[])
    if hasattr(esp32, "wake_on_ext1"):
        esp32.wake_on_ext1(pins=[])
