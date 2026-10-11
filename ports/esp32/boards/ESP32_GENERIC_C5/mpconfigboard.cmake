include(boards/mpconfigboard_esp32c5_common.cmake)

list(APPEND SDKCONFIG_DEFAULTS
    boards/sdkconfig.flash_qio_80m
    boards/sdkconfig.csi
    boards/ESP32_GENERIC_C5/sdkconfig.partition_ble5
)
