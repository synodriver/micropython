find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ESP32_BLE5_DIR ${MICROPY_PORT_DIR}/ble5)
set(ESP32_BLE5_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ble5)
set(ESP32_BLE5_INPUTS
    ${MICROPY_DIR}/extmod/modbluetooth.c
    ${MICROPY_DIR}/extmod/nimble/modbluetooth_nimble.c
    ${ESP32_BLE5_DIR}/make_bindings.py
    ${ESP32_BLE5_DIR}/bluetooth_ble5.h
    ${ESP32_BLE5_DIR}/bluetooth_ble5_bindings.c
    ${ESP32_BLE5_DIR}/bluetooth_ble5_nimble.c
    ${ESP32_BLE5_DIR}/bluetooth_ble5_events.c
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ESP32_BLE5_INPUTS})
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${ESP32_BLE5_DIR}/make_bindings.py
        --micropython ${MICROPY_DIR} --output ${ESP32_BLE5_OUTPUT}
    RESULT_VARIABLE ESP32_BLE5_RESULT
)
if(NOT ESP32_BLE5_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate ESP32 BLE 5 bindings")
endif()
list(REMOVE_ITEM MICROPY_SOURCE_EXTMOD
    ${MICROPY_DIR}/extmod/modbluetooth.c
    ${MICROPY_DIR}/extmod/nimble/modbluetooth_nimble.c
)
list(APPEND MICROPY_SOURCE_EXTMOD
    ${ESP32_BLE5_OUTPUT}/modbluetooth_esp32.c
    ${ESP32_BLE5_OUTPUT}/modbluetooth_nimble_esp32.c
)
list(APPEND MICROPY_INC_CORE ${ESP32_BLE5_DIR})
