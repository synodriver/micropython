set(IDF_TARGET esp32c2)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.riscv
    boards/sdkconfig.ble
    boards/sdkconfig.ble_max_c2
    boards/sdkconfig.c2
    # C2 has unusably low free RAM without these optimisations
    boards/sdkconfig.free_ram
)
