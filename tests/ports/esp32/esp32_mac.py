# Exercise the SDK MAC table and read-only eFuse APIs, without starting drivers.
try:
    import esp32
    from esp32 import read_mac, iface_mac_addr_set, derive_local_mac
    from esp32 import mac_addr_len_get, efuse_mac_get_default, efuse_mac_get_custom
    from machine import unique_id
except ImportError:
    print("SKIP")
    raise SystemExit


def expect_error(error_type, function, *args):
    try:
        function(*args)
    except error_type:
        return
    raise AssertionError("expected exception")


mac_types = (
    (esp32.ESP_MAC_WIFI_STA, 6),
    (esp32.ESP_MAC_WIFI_SOFTAP, 6),
    (esp32.ESP_MAC_BT, 6),
    (esp32.ESP_MAC_ETH, 6),
    (esp32.ESP_MAC_IEEE802154, 8),
    (esp32.ESP_MAC_BASE, 6),
    (esp32.ESP_MAC_EFUSE_FACTORY, 6),
    (esp32.ESP_MAC_EFUSE_EXT, 2),
)
for mac_type, length in mac_types + ((esp32.ESP_MAC_EFUSE_CUSTOM, 6),):
    assert mac_addr_len_get(mac_type) in (0, length)
for mac_type in (esp32.ESP_MAC_BASE, esp32.ESP_MAC_EFUSE_FACTORY, esp32.ESP_MAC_EFUSE_CUSTOM):
    assert mac_addr_len_get(mac_type) == 6
assert (mac_addr_len_get(esp32.ESP_MAC_IEEE802154) == 8) == (
    mac_addr_len_get(esp32.ESP_MAC_EFUSE_EXT) == 2
)
print("MAC length query OK")

addresses = {}
for mac_type, length in mac_types:
    try:
        address = read_mac(mac_type)
    except OSError as error:
        # Known types can be absent on this chip; other SDK errors are failures.
        assert error.errno == 95  # EOPNOTSUPP
        assert mac_addr_len_get(mac_type) == 0
        expect_error(OSError, iface_mac_addr_set, bytes(length), mac_type)
    else:
        assert isinstance(address, bytes) and len(address) == length
        assert mac_addr_len_get(mac_type) == length
        assert read_mac(mac_type) == address
        addresses[mac_type] = address
assert esp32.ESP_MAC_BASE in addresses
assert esp32.ESP_MAC_EFUSE_FACTORY in addresses
# A custom eFuse MAC may be absent or fail its SDK version/CRC validation.
try:
    custom = read_mac(esp32.ESP_MAC_EFUSE_CUSTOM)
except OSError:
    pass
else:
    assert isinstance(custom, bytes) and len(custom) == 6
    addresses[esp32.ESP_MAC_EFUSE_CUSTOM] = custom
print("read MAC types OK")


def mac48(raw):
    return raw if len(raw) == 6 else raw[:3] + raw[5:]


efuse_length = 8 if mac_addr_len_get(esp32.ESP_MAC_IEEE802154) else 6
factory_raw = efuse_mac_get_default()
assert isinstance(factory_raw, bytes) and len(factory_raw) == efuse_length
assert mac48(factory_raw) == addresses[esp32.ESP_MAC_EFUSE_FACTORY]
assert efuse_mac_get_default() == factory_raw
custom_raw = None
try:
    custom_raw = efuse_mac_get_custom()
except OSError:
    assert esp32.ESP_MAC_EFUSE_CUSTOM not in addresses
else:
    assert isinstance(custom_raw, bytes) and len(custom_raw) == efuse_length
    assert mac48(custom_raw) == addresses[esp32.ESP_MAC_EFUSE_CUSTOM]
    assert efuse_mac_get_custom() == custom_raw
print("direct eFuse reads OK")

chip_id = unique_id()
assert isinstance(chip_id, bytes) and len(chip_id) == 6
assert chip_id == factory_raw[:6]  # Preserve the port's existing identifier.
for _ in range(4):
    assert unique_id() == chip_id
print("unique ID OK")

source = bytearray(b"\x00\x11\x22\x33\x44\x55")
assert derive_local_mac(source) == b"\x02\x11\x22\x33\x44\x55"
assert source == b"\x00\x11\x22\x33\x44\x55"
assert derive_local_mac(memoryview(b"\x02\x11\x22\x33\x44\x55")) == b"\x06\x11\x22\x33\x44\x55"
assert derive_local_mac(b"\x01\x11\x22\x33\x44\x55") == b"\x03\x11\x22\x33\x44\x55"
print("local derivation OK")

for length in (0, 2, 5, 7, 8, 9):
    expect_error(ValueError, derive_local_mac, bytes(length))
expect_error(TypeError, derive_local_mac, None)
for mac_type, address in addresses.items():
    expect_error(ValueError, iface_mac_addr_set, address[:-1], mac_type)
    expect_error(ValueError, iface_mac_addr_set, address + b"\x00", mac_type)
expect_error(TypeError, iface_mac_addr_set, None, esp32.ESP_MAC_ETH)
print("buffer validation OK")

for invalid_type in (-1, 9, 256, 1 << 30):
    expect_error(ValueError, mac_addr_len_get, invalid_type)
    expect_error(ValueError, read_mac, invalid_type)
    expect_error(ValueError, iface_mac_addr_set, bytes(6), invalid_type)
# These exceed the machine integer range and fail before enum validation.
for invalid_type in (1 << 100, -(1 << 100)):
    expect_error(OverflowError, mac_addr_len_get, invalid_type)
    expect_error(OverflowError, read_mac, invalid_type)
    expect_error(OverflowError, iface_mac_addr_set, bytes(6), invalid_type)
expect_error(TypeError, read_mac, None)
expect_error(TypeError, mac_addr_len_get, None)
expect_error(TypeError, iface_mac_addr_set, bytes(6), None)
expect_error(TypeError, efuse_mac_get_default, 0)
expect_error(TypeError, efuse_mac_get_custom, 0)
print("type validation OK")

base = addresses[esp32.ESP_MAC_BASE]
expect_error(OSError, iface_mac_addr_set, b"\x01" + base[1:], esp32.ESP_MAC_BASE)
assert read_mac(esp32.ESP_MAC_BASE) == base
for mac_type in (esp32.ESP_MAC_EFUSE_FACTORY, esp32.ESP_MAC_EFUSE_CUSTOM):
    expect_error(OSError, iface_mac_addr_set, bytes(6), mac_type)
assert read_mac(esp32.ESP_MAC_EFUSE_FACTORY) == addresses[esp32.ESP_MAC_EFUSE_FACTORY]
print("SDK validation OK")

new_base = bytearray(base)
new_base[0] ^= 4
try:
    assert iface_mac_addr_set(bytes(new_base), esp32.ESP_MAC_BASE) is None
    assert read_mac(esp32.ESP_MAC_BASE) == bytes(new_base)
    assert efuse_mac_get_default() == factory_raw
    assert unique_id() == chip_id
    if custom_raw is not None:
        assert efuse_mac_get_custom() == custom_raw
    # Reading an interface above cached it. Updating BASE must not silently
    # replace those addresses, or the factory eFuse address.
    for mac_type, address in addresses.items():
        if mac_type != esp32.ESP_MAC_BASE:
            assert read_mac(mac_type) == address
finally:
    iface_mac_addr_set(base, esp32.ESP_MAC_BASE)
assert read_mac(esp32.ESP_MAC_BASE) == base
print("base MAC cache OK")

# Cover six-, eight- and two-byte setters where supported. Restore the SDK
# table after each test; an existing driver's active MAC is not changed here.
for mac_type in (esp32.ESP_MAC_ETH, esp32.ESP_MAC_IEEE802154, esp32.ESP_MAC_EFUSE_EXT):
    if mac_type not in addresses:
        continue
    original = addresses[mac_type]
    changed = bytearray(original)
    changed[0] ^= 4
    try:
        assert iface_mac_addr_set(memoryview(changed), mac_type) is None
        assert read_mac(mac_type) == bytes(changed)
        assert mac_addr_len_get(mac_type) == len(original)
        # Even overriding the cached extension must not affect a direct read.
        assert efuse_mac_get_default() == factory_raw
        assert unique_id() == chip_id
        if custom_raw is not None:
            assert efuse_mac_get_custom() == custom_raw
        changed[-1] ^= 1
        assert read_mac(mac_type) != bytes(changed)  # SDK copies the input.
    finally:
        iface_mac_addr_set(original, mac_type)
    assert read_mac(mac_type) == original
assert read_mac(esp32.ESP_MAC_BASE) == base
assert read_mac(esp32.ESP_MAC_EFUSE_FACTORY) == addresses[esp32.ESP_MAC_EFUSE_FACTORY]
print("MAC table override OK")
