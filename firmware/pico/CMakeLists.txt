# CMake del nodo Raspberry Pi Pico W (firmware/pico)

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico_firmware C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Raíz del repo, para usar las carpetas compartidas common/ y firmware/core/
set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_executable(pico_firmware
    main.c
    link.c                                    # serial USB: tramas y búfer de salida
    ${REPO_ROOT}/firmware/core/rts_engine.c   # calendarizador local (compartido con ESP32)
    ${REPO_ROOT}/common/src/protocol.c        # armado y validación de tramas
    ${REPO_ROOT}/common/src/crc16.c           # CRC-16
    ${REPO_ROOT}/firmware/core/ut.c           # unidad_trabajo() del profesor (sin modificar)
)

pico_set_program_name(pico_firmware "pico_firmware")
pico_set_program_version(pico_firmware "0.1")

# Serial por USB (/dev/ttyACM0), no por UART
pico_enable_stdio_uart(pico_firmware 0)
pico_enable_stdio_usb(pico_firmware 1)

target_include_directories(pico_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${REPO_ROOT}/common/include
    ${REPO_ROOT}/firmware/core
)

target_compile_options(pico_firmware PRIVATE -Wall -Wextra)

target_link_libraries(pico_firmware
    pico_stdlib     # gpio, timers, stdio, time_us_64()
    hardware_sync   # __wfi()
)

pico_add_extra_outputs(pico_firmware)