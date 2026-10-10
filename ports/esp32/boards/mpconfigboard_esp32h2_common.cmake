set(IDF_TARGET esp32h2)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.riscv
    boards/sdkconfig.h2
    boards/sdkconfig.ble
)

# H2's connection limit increased from 35 to 70 in IDF 5.5.3.
# Board defaults are loaded before project.cmake defines the IDF version.
include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}" VERSION_LESS "5.5.3")
    list(APPEND SDKCONFIG_DEFAULTS boards/sdkconfig.ble_max_h2_legacy)
else()
    list(APPEND SDKCONFIG_DEFAULTS boards/sdkconfig.ble_max_c6)
endif()
