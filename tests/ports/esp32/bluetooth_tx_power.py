# Validate the enhanced TX power binding without starting the BLE controller.
try:
    import bluetooth
    import errno

    ble = bluetooth.BLE()
    set_power = ble.gap_set_tx_power
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit

ble.active(False)

for args in (
    (-1, 0, 9),
    (5, 0, 9),
    (65536, 0, 9),
    (0, -1, 9),
    (0, 65536, 9),
    (0, 0, -1),
    (0, 0, 16),
    (0, 0, 256),
    (0, 1, 9),
    (2, 1, 9),
    (3, 1, 9),
):
    try:
        set_power(*args)
        assert False
    except ValueError:
        pass
print("range validation OK")

for args in ((None, 0, 9), (0, None, 9), (0, 0, "9")):
    try:
        set_power(*args)
        assert False
    except TypeError:
        pass
print("type validation OK")

for index in range(3):
    args = [0, 0, 9]
    args[index] = 1 << 64
    try:
        set_power(*args)
        assert False
    except OverflowError:
        pass
print("integer overflow OK")

# C6 starts at level 3. Other targets expose level 0 as well.
if not hasattr(bluetooth, "TX_POWER_N24"):
    for level in (0, 1, 2):
        try:
            set_power(0, 0, level)
            assert False
        except ValueError:
            pass
print("target power levels OK")

for args in ((0, 0, 9), (1, 65535, 9), (4, 65535, 9)):
    try:
        set_power(*args)
        assert False
    except OSError as error:
        assert error.args[0] == errno.ENODEV
try:
    set_power(power_type=0, handle=0, power_level=9)
    assert False
except OSError as error:
    assert error.args[0] == errno.ENODEV
print("inactive controller OK")
