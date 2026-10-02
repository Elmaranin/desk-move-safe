cmake_minimum_required(VERSION 3.13)

# RP2350 / Pico 2 (Arm Cortex-M33 secure build). The production board is an
# RP2350A Supermini — same die, different break-out.
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Pico platform")

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 13_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ========================================================================================

if (NOT FREERTOS_KERNEL_PATH AND NOT DEFINED ENV{FREERTOS_KERNEL_PATH})
    set(FREERTOS_KERNEL_PATH "${USERHOME}/work/diy/rp2350/FreeRTOS-Kernel")
endif ()

include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)

project(desk-move-safe C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(${PROJECT_NAME}
    src/main.c
    src/bus.c
    src/wire.c
    src/flap.c
    src/flap_task.c
    src/console.c
    src/desk_frame.c
    src/settings.c
    src/nvs.c
    src/stepper.c
    src/tmc2209.c
    src/mode.c
    src/mt6835.c
    src/encoder.c
    src/limits.c
    src/params.c
    src/trace.c
    src/led.c
)

# The driver's UART. Both hardware UARTs belong to the desk bus, so the third
# serial port is built in PIO. See src/tmc_uart.pio.
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/tmc_uart.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/ws2812.pio)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include    # board_config.h
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}            # FreeRTOSConfig.h
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    hardware_uart       # the two taps and the one transmitter
    hardware_flash      # nvs: presets and tuning, last flash sector
    hardware_pio        # the flap driver's UART; the desk has both UART blocks
    hardware_timer      # the alarm that clocks out the STEP pulses
    hardware_spi        # the MT6835 magnetic encoder
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# Compiles out every flash read and write — for bisecting, not for use.
if (DEFINED SETTINGS_ENABLE_FLASH)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SETTINGS_ENABLE_FLASH=${SETTINGS_ENABLE_FLASH})
endif ()

# stdio over USB CDC: the console, and what lets flash.sh reset into BOOTSEL
# without the button. UART off — both UARTs are the desk bus.
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    # Send console output regardless of DTR, so terminals that do not raise it
    # still see a live console.
    PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1
)

pico_add_extra_outputs(${PROJECT_NAME})
