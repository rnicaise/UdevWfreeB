#
# CMakeLists.txt - UWB Ranging Minimal (SS-TWR)
#
# This project integrates with the Qorvo SDK build system.
# It reuses the UWB driver, the nRF BSP, and SDK UWB configs,
# but replaces the SDK examples with this project's code.
#
# Target : DWM3001CDK (nRF52833 + DW3000)
#
# HOW IT WORKS:
#
#   The Qorvo SDK has this build structure:
#
#     Drivers/CMakeLists.txt              <- top-level project
#       |-- cmake/config.cmake            <- DW3000/DW3720 driver variables
#       |-- cmake/arm-none-eabi-gcc.cmake <- ARM cross-compile toolchain
#       |-- API/Shared/dwt_uwb_driver     <- UWB driver library
#       └── API/Build_Platforms/nRF52840-DK/
#           |-- CMakeLists.txt            <- platform sources (SPI, GPIO, etc.)
#           |-- Source/main.c             <- SDK example entrypoint
#           |-- Source/platform/          <- nRF52840 HAL (SPI, sleep, mutex...)
#           |-- sdk/                      <- Nordic SDK drivers
#           |-- Setup/                    <- linker script template
#           |-- Startup/                  <- ARM startup code
#           └── lib/                      <- RTT, chelpers, qio, qosal
#
#   This project does not modify SDK files. It adds project sources and
#   supplies its own application entrypoint.
#
# USAGE:
#
#   # From this repository root:
#   cmake --preset=initiator_debug
#   cmake --build --preset initiator_debug
#
#   # Or for the responder:
#   cmake --preset=responder_debug
#   cmake --build --preset responder_debug
#

cmake_minimum_required(VERSION 3.20)

# Qorvo SDK path. The SDK is vendored under vendor/sdk.
# All SDK dependencies are included in this repository.
set(QORVO_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/sdk" CACHE PATH "Path to vendored SDK")

# ARM toolchain from the SDK.
set(CMAKE_TOOLCHAIN_FILE "${QORVO_SDK_PATH}/cmake/arm-none-eabi-gcc.cmake" CACHE STRING "Toolchain")

# Project.
project(uwb_ranging VERSION 1.0.0 LANGUAGES C ASM)

message("=== UWB Ranging Minimal ===")
message("SDK path: ${QORVO_SDK_PATH}")
message("Build type: ${CMAKE_BUILD_TYPE}")
message("Role: ${UWB_ROLE}")

# Validate role selection.
if(NOT DEFINED UWB_ROLE)
    message(FATAL_ERROR "UWB_ROLE is not defined. Use a preset or -DUWB_ROLE=initiator|responder")
endif()
if(NOT UWB_ROLE STREQUAL "initiator" AND NOT UWB_ROLE STREQUAL "responder")
    message(FATAL_ERROR "UWB_ROLE must be 'initiator' or 'responder' (received: ${UWB_ROLE})")
endif()

# nRF52833 memory layout for DWM3001CDK.
# Override before include(config.cmake), which defines nRF52840 defaults.
set(FLASH_SIZE 0x00080000 CACHE STRING "Flash size" FORCE)   # 512KB
set(RAM_SIZE   0x00020000 CACHE STRING "Ram size" FORCE)      # 128KB

# Include SDK CMake helpers.
include("${QORVO_SDK_PATH}/cmake/config.cmake")
include("${QORVO_SDK_PATH}/cmake/util.cmake")

# SDK paths.
set(UWB_DRIVER_PATH "${QORVO_SDK_PATH}/API/Shared/dwt_uwb_driver")
set(PLATFORM_PATH "${QORVO_SDK_PATH}/API/Build_Platforms/nRF52840-DK")
set(EXAMPLES_PATH "${QORVO_SDK_PATH}/API/Src")

# Variable expected by internal SDK CMakeLists files.
set(PLATFORM_SPECIFIC_FILES_PATH "${PLATFORM_PATH}" CACHE STRING "" FORCE)

# Linker script generated from the SDK template with nRF52833 sizes.
set(LINKER_SCRIPT_IN "${PLATFORM_PATH}/Setup/nrf52840_linker.ld.in")
set(LINKER_SCRIPT_OUT "${CMAKE_BINARY_DIR}/nrf52833_linker.ld")
configure_file(${LINKER_SCRIPT_IN} ${LINKER_SCRIPT_OUT} @ONLY)

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -T ${LINKER_SCRIPT_OUT} -nostartfiles")

# Build the UWB driver.
# The SDK has a GCC 15 -Wmaybe-uninitialized false positive; keep the
# suppression scoped to the vendored driver targets.
set(DWT_DW3000 ${USE_DRV_DW3000})
set(DWT_DW3720 ${USE_DRV_DW3720})
add_subdirectory(${UWB_DRIVER_PATH} ${CMAKE_BINARY_DIR}/uwb_driver)

# Suppress the SDK false positive in code controlled by the vendor.
if(TARGET dw3000_uwb_driver)
    target_compile_options(dw3000_uwb_driver PRIVATE -Wno-maybe-uninitialized)
endif()
if(TARGET dw3720_uwb_driver)
    target_compile_options(dw3720_uwb_driver PRIVATE -Wno-maybe-uninitialized)
endif()

# Build platform libraries.
add_subdirectory(${PLATFORM_PATH}/lib/chelpers ${CMAKE_BINARY_DIR}/chelpers)
add_subdirectory(${PLATFORM_PATH}/lib/rtt ${CMAKE_BINARY_DIR}/rtt)
add_subdirectory(${PLATFORM_PATH}/lib/qio ${CMAKE_BINARY_DIR}/qio)
add_subdirectory(${PLATFORM_PATH}/lib/qosal ${CMAKE_BINARY_DIR}/qosal)

# nRF52833 startup; the SDK nRF52840 startup is not suitable for this board.
set(STARTUP_PATH "${PLATFORM_PATH}/Startup")
add_library(nrf52833_startup STATIC EXCLUDE_FROM_ALL
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk/ses_startup_nrf52833.s
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk/ses_startup_nrf_common.s
    ${STARTUP_PATH}/thumb_crt0.s
)
set_source_files_properties(
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk/ses_startup_nrf52833.s
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk/ses_startup_nrf_common.s
    PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp"
)
set_source_files_properties(
    ${STARTUP_PATH}/thumb_crt0.s
    PROPERTIES COMPILE_FLAGS "-x assembler-with-cpp -DINITIALIZE_STACK=1"
)

# Output names.
set(TARGET_ELF uwb_${UWB_ROLE}.elf)
set(TARGET_HEX uwb_${UWB_ROLE}.hex)

# Sources.
# Sources platform — custom DWM3001CDK (single DW3000, no SPI2/IRQ2)
set(PLATFORM_SOURCES
    ${PLATFORM_PATH}/Source/platform/deca_mutex.c
    ${PLATFORM_PATH}/Source/platform/deca_probe_interface.c
    ${PLATFORM_PATH}/Source/platform/deca_sleep.c
    ${CMAKE_SOURCE_DIR}/src/platform/deca_spi_dwm3001cdk.c
    ${CMAKE_SOURCE_DIR}/src/platform/port_dwm3001cdk.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk/system_nrf52833.c
    ${PLATFORM_PATH}/sdk/components/boards/boards.c
    ${PLATFORM_PATH}/sdk/integration/nrfx/legacy/nrf_drv_clock.c
    ${PLATFORM_PATH}/sdk/integration/nrfx/legacy/nrf_drv_spi.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_clock.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_gpiote.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_power.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/prs/nrfx_prs.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_spi.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_spim.c
    ${PLATFORM_PATH}/sdk/integration/nrfx/legacy/nrf_drv_power.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error_handler_gcc.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error_weak.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_util_platform.c
    ${PLATFORM_PATH}/sdk/components/libraries/atomic/nrf_atomic.c
    ${PLATFORM_PATH}/sdk/components/libraries/queue/nrf_queue.c
    ${PLATFORM_PATH}/sdk/components/libraries/balloc/nrf_balloc.c
    ${PLATFORM_PATH}/sdk/components/libraries/log/src/nrf_log_frontend.c
    ${PLATFORM_PATH}/sdk/components/libraries/memobj/nrf_memobj.c
    ${PLATFORM_PATH}/sdk/components/libraries/strerror/nrf_strerror.c
    ${PLATFORM_PATH}/sdk/components/drivers_nrf/usbd/nrf_drv_usbd.c
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/app_usbd.c
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/app_usbd_core.c
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/app_usbd_serial_num.c
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/app_usbd_string_desc.c
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/class/cdc/acm/app_usbd_cdc_acm.c
)

# Required Qorvo SDK sources.
set(SDK_SOURCES
    ${EXAMPLES_PATH}/config_options.c
    ${EXAMPLES_PATH}/examples/shared_data/shared_functions.c
)

# Project sources.
set(PROJECT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/ranging.c
    ${CMAKE_SOURCE_DIR}/src/common/radio_quality.c
    ${CMAKE_SOURCE_DIR}/src/common/uwb_profiles.c
)

# Select initiator or responder application source.
if(UWB_ROLE STREQUAL "initiator")
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/initiator/main_initiator.c
        ${CMAKE_SOURCE_DIR}/src/accel/accel.c
        ${CMAKE_SOURCE_DIR}/src/uart/uart_log.c
    )
elseif(UWB_ROLE STREQUAL "responder")
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/responder/main_responder.c
        ${CMAKE_SOURCE_DIR}/src/accel/accel.c
        ${CMAKE_SOURCE_DIR}/src/uart/uart_log.c
    )
endif()

# Custom application entrypoint.
list(APPEND PROJECT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.c
)

# Include directories.
set(INCLUDE_DIRS
    # Project code
    ${CMAKE_SOURCE_DIR}/src/board
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/accel
    ${CMAKE_SOURCE_DIR}/src/uart

    # SDK platform
    ${PLATFORM_PATH}/Source/config
    ${PLATFORM_PATH}/Source/platform
    ${PLATFORM_PATH}/sdk/components/drivers_nrf/nrf_soc_nosd
    ${PLATFORM_PATH}/sdk/components/boards
    ${PLATFORM_PATH}/sdk/components/toolchain/cmsis/include
    ${PLATFORM_PATH}/sdk/components/libraries/balloc
    ${PLATFORM_PATH}/sdk/components/libraries/queue
    ${PLATFORM_PATH}/sdk/components/libraries/ringbuf
    ${PLATFORM_PATH}/sdk/components/libraries/log
    ${PLATFORM_PATH}/sdk/components/libraries/log/src
    ${PLATFORM_PATH}/sdk/components/libraries/memobj
    ${PLATFORM_PATH}/sdk/components/libraries/util
    ${PLATFORM_PATH}/sdk/components/libraries/atomic
    ${PLATFORM_PATH}/sdk/components/libraries/delay
    ${PLATFORM_PATH}/sdk/components/libraries/experimental_section_vars
    ${PLATFORM_PATH}/sdk/components/libraries/strerror
    ${PLATFORM_PATH}/sdk/components/libraries/usbd
    ${PLATFORM_PATH}/sdk/components/drivers_nrf/power
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/config
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/class/cdc
    ${PLATFORM_PATH}/sdk/components/libraries/usbd/class/cdc/acm
    ${PLATFORM_PATH}/sdk/components/drivers_nrf/usbd
    ${PLATFORM_PATH}/sdk/modules/nrfx
    ${PLATFORM_PATH}/sdk/modules/nrfx/hal
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/include
    ${PLATFORM_PATH}/sdk/integration/nrfx
    ${PLATFORM_PATH}/sdk/integration/nrfx/legacy
    ${PLATFORM_PATH}/sdk/external/fprintf
    ${PLATFORM_PATH}/sdk/modules/nrfx/mdk

    # SDK examples headers for config_options.h, shared_defines.h, etc.
    ${EXAMPLES_PATH}/
    ${EXAMPLES_PATH}/examples/shared_data
    ${EXAMPLES_PATH}/examples/examples_info
)

# Create executable.
add_executable(${TARGET_ELF}
    ${PLATFORM_SOURCES}
    ${SDK_SOURCES}
    ${PROJECT_SOURCES}
)

target_include_directories(${TARGET_ELF} PRIVATE ${INCLUDE_DIRS})

target_link_libraries(${TARGET_ELF} PRIVATE
    uwb_driver
    nrf52833_startup
    c
    chelpers
    rtt_io
    m
)

target_compile_definitions(${TARGET_ELF} PRIVATE
    BOARD_CUSTOM
    CONFIG_GPIO_AS_PINRESET
    CONFIG_NFCT_PINS_AS_GPIOS
    FLOAT_ABI_HARD
    INITIALIZE_USER_SECTIONS
    NO_VTOR_CONFIG
    NRF52833_XXAA
    USE_DRV_DW3000
    USE_DRV_DW3720
    NB_DW_DRIVERS=2
    NRF_LOG_BACKEND_RTT_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    NRF_LOG_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    USE_USB_CDC=1
    # UWB config: option 35 = channel 5, PRF 64M, preamble 128, 6.8 Mbps, STS 64.
    CMAKE_UWB_CONFIG_OPTION=35
    # Role selection for main.c.
    $<$<STREQUAL:${UWB_ROLE},initiator>:UWB_ROLE_INITIATOR>
    $<$<STREQUAL:${UWB_ROLE},responder>:UWB_ROLE_RESPONDER>
)

target_link_options(${TARGET_ELF} PRIVATE
    -Wl,-Map=$<TARGET_FILE_DIR:${TARGET_ELF}>/uwb_${UWB_ROLE}.map
)

set_target_properties(${TARGET_ELF} PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT_OUT}
)

# Generate flashable hex.
add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET_ELF}> ${TARGET_HEX}
    COMMENT "Generating ${TARGET_HEX}"
)
