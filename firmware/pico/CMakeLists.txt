cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake OPTIONAL)

project(pico_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(pico_firmware main.c)
target_link_libraries(pico_firmware pico_stdlib)
pico_add_extra_outputs(pico_firmware)