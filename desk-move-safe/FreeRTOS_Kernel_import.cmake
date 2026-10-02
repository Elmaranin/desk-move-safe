# Locates the FreeRTOS kernel and selects the correct RP2040/RP2350 port.
#
# Set FREERTOS_KERNEL_PATH (env var or -D) to a checkout of
# https://github.com/FreeRTOS/FreeRTOS-Kernel  that is new enough to include
# the RP2350 ports. See README.md.

if (DEFINED ENV{FREERTOS_KERNEL_PATH} AND (NOT FREERTOS_KERNEL_PATH))
    set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
    message("Using FREERTOS_KERNEL_PATH from environment ('${FREERTOS_KERNEL_PATH}')")
endif ()

if (NOT FREERTOS_KERNEL_PATH)
    message(FATAL_ERROR "FREERTOS_KERNEL_PATH is not set. Point it at a FreeRTOS-Kernel checkout (see README.md).")
endif ()

get_filename_component(FREERTOS_KERNEL_PATH "${FREERTOS_KERNEL_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if (NOT EXISTS ${FREERTOS_KERNEL_PATH})
    message(FATAL_ERROR "FreeRTOS kernel directory '${FREERTOS_KERNEL_PATH}' not found")
endif ()

# Choose the port directory for the active platform.
if (PICO_PLATFORM MATCHES "rp2350-riscv")
    set(_FR_PORT "RP2350_RISC-V")
elseif (PICO_PLATFORM MATCHES "rp2350")
    set(_FR_PORT "RP2350_ARM_NTZ")
else ()
    set(_FR_PORT "RP2040")
endif ()

# Different kernel versions place the pico ports in one of these two trees.
set(_FR_CANDIDATES
    "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/${_FR_PORT}"
    "${FREERTOS_KERNEL_PATH}/portable/ThirdParty/Community-Supported-Ports/GCC/${_FR_PORT}"
)

set(_FR_PORT_DIR "")
foreach (_cand ${_FR_CANDIDATES})
    if (EXISTS "${_cand}/library.cmake")
        set(_FR_PORT_DIR "${_cand}")
        break ()
    endif ()
endforeach ()

if (NOT _FR_PORT_DIR)
    message(FATAL_ERROR
        "Could not find FreeRTOS port '${_FR_PORT}' under ${FREERTOS_KERNEL_PATH}.\n"
        "Your kernel checkout may predate RP2350 support, or use a different layout.\n"
        "Replace this file with the FreeRTOS_Kernel_import.cmake shipped in raspberrypi/pico-examples.")
endif ()

message("FreeRTOS: using port at ${_FR_PORT_DIR}")
include(${_FR_PORT_DIR}/library.cmake)
