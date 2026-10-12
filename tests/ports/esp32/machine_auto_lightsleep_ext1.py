# For builds with CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y and PM
# callbacks enabled. Default builds SKIP. All three selected pins must be free;
# keep both RTC inputs low, preferably with external pulldowns. Software IRQs
# test notification delivery; this is not a physical wakeup/power measurement.
import errno
import esp32
import machine
import micropython
import os
import time
from machine import Pin

chip = os.uname().machine.upper().replace("-", "")
pins = None
for name, numbers in (
    ("ESP32H2", (8, 9, 15)),
    ("ESP32C6", (2, 3, 10)),
    ("ESP32C5", (1, 2, 8)),
    ("ESP32P4", (1, 2, 17)),
):
    if name in chip:
        pins = numbers
        break
if pins is None:
    print("SKIP")
    raise SystemExit

hz = machine.freq()
machine.freq(hz)
esp32.wake_on_ext1(pins=[])
esp32.wake_on_gpio(pins=[])
p, q, non_rtc = [Pin(n, Pin.IN, Pin.PULL_DOWN) for n in pins]
hits = []


def handler(pin):
    hits.append(pin)


def rejected(call):
    try:
        call()
    except OSError:
        return
    raise AssertionError("incompatible IRQ accepted")


try:
    non_rtc.irq(handler=handler, trigger=Pin.IRQ_RISING)
    try:
        machine.freq(hz, hz, True)
    except OSError as exc:
        if exc.errno != errno.EOPNOTSUPP:
            raise
    else:
        print("SKIP")  # Digital GPIO wakeup is still available in this build.
        raise SystemExit
    assert non_rtc.irq().trigger() == Pin.IRQ_RISING
    non_rtc.irq(handler=None)

    irq = p.irq(handler=handler, trigger=Pin.IRQ_RISING)
    machine.freq(hz, hz, True)  # Valid RTC input must work in this build.
    rejected(lambda: non_rtc.irq(handler=handler, trigger=Pin.IRQ_RISING))
    assert non_rtc.irq().trigger() == 0
    assert irq.trigger() == Pin.IRQ_RISING
    print("EXT1 fallback and unsupported input rollback OK")

    esp32.wake_on_ext1(pins=(q,), level=esp32.WAKEUP_ANY_HIGH)
    for _ in range(3):
        time.sleep_ms(150)
        assert hits == []
        rejected(lambda: q.irq(handler=handler, trigger=Pin.IRQ_RISING))
        assert q.irq().trigger() == 0
        p.irq(handler=None)
        irq = p.irq(handler=handler, trigger=Pin.IRQ_RISING)
    esp32.wake_on_ext1(pins=[])
    print("independent explicit EXT1 configuration retained OK")

    q_irq = q.irq(handler=handler, trigger=Pin.IRQ_RISING)

    def notify(_):
        irq()
        q_irq()
        q_irq()

    micropython.schedule(notify, None)
    time.sleep_ms(150)
    assert hits.count(p) == 1 and hits.count(q) == 1 and len(hits) == 2
    hits.clear()
    time.sleep_ms(250)
    assert hits == []
    assert irq.trigger() == q_irq.trigger() == Pin.IRQ_RISING
    print("multiple EXT1 IRQ inputs and no stale notification OK")
finally:
    p.irq(handler=None)
    q.irq(handler=None)
    non_rtc.irq(handler=None)
    machine.freq(hz)
    esp32.wake_on_ext1(pins=[])
    esp32.wake_on_gpio(pins=[])
