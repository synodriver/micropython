set(IDF_TARGET esp32c5)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.riscv
    boards/sdkconfig.ble
    boards/sdkconfig.ble_max_c6
    boards/sdkconfig.240mhz
    boards/sdkconfig.spiram_quad
)
