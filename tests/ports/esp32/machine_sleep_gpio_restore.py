# ESP32-S3 GPIO4 sleep-state regression; requires an unused pin.
# Only sleep input/output/selection bits change; the pin stays an active input.
import esp32
import machine
import os
from errno import EOPNOTSUPP
from machine import Pin

if "ESP32S3" not in os.uname().machine.upper().replace("-", ""):
    print("SKIP")
    raise SystemExit

hz = machine.freq()
try:
    machine.freq(hz, hz, True)
except TypeError:
    print("SKIP")
    raise SystemExit
except ValueError as error:
    if str(error) != "automatic light sleep requires PM and tickless idle":
        raise
    print("SKIP")
    raise SystemExit
machine.freq(hz, hz, False)

p = Pin(4, Pin.IN)
# IDF's S3 GPIO_PIN_MUX_REG[4]: DR_REG_IO_MUX_BASE (0x60009000) + 0x14.
iomux = 0x60009014
sleep_mask = 0x13  # SLP_IE | SLP_SEL | SLP_OE.
original = machine.mem32[iomux] & sleep_mask


def sleep_bits(value):
    machine.mem32[iomux] = (machine.mem32[iomux] & ~sleep_mask) | value


try:
    esp32.wake_on_gpio(pins=[])
    esp32.wake_on_ext0(pin=None)
    esp32.wake_on_ext1(pins=[])
    sleep_bits(0)
    machine.freq(hz, hz, True)
    try:
        esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
    except OSError as error:
        # A custom build can power down the GPIO peripheral domain.
        if error.errno != EOPNOTSUPP:
            raise
        print("SKIP")
        raise SystemExit
    esp32.wake_on_gpio(pins=[])
    assert machine.mem32[iomux] & sleep_mask == 0
    print("cancel restores sleep state")

    esp32.wake_on_gpio(pins=(p,), level=esp32.WAKEUP_ANY_HIGH)
    machine.freq(hz, hz, False)
    assert machine.mem32[iomux] & sleep_mask == 0
    print("disable restores sleep state")

    machine.lightsleep(1)
    assert machine.mem32[iomux] & sleep_mask == 0
    print("manual sleep restores sleep state")
finally:
    machine.freq(hz, hz, False)
    esp32.wake_on_gpio(pins=[])
    sleep_bits(original)
