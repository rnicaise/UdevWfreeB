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
message("Accel target: ${UWB_ACCEL_TARGET}")

# Validate role selection.
if(NOT DEFINED UWB_ROLE)
    message(FATAL_ERROR "UWB_ROLE is not defined. Use a preset or -DUWB_ROLE=initiator|responder")
endif()
if(NOT UWB_ROLE STREQUAL "initiator" AND NOT UWB_ROLE STREQUAL "responder")
    message(FATAL_ERROR "UWB_ROLE must be 'initiator' or 'responder' (received: ${UWB_ROLE})")
endif()

if(NOT DEFINED UWB_PURE)
    set(UWB_PURE OFF CACHE BOOL "Pure UWB build: no BLE, no IMU, no pyro")
endif()

if(NOT DEFINED UWB_UART_1M)
    set(UWB_UART_1M ON CACHE BOOL "Pure UWB UART at 1 Mbps (OFF = 460800, for J-Link VCOM)")
endif()

if(NOT DEFINED UWB_ACCEL_TARGET)
    set(UWB_ACCEL_TARGET "legacy" CACHE STRING "Accelerometer hardware target: legacy|gena|auto")
endif()
if(NOT UWB_ACCEL_TARGET STREQUAL "legacy" AND NOT UWB_ACCEL_TARGET STREQUAL "gena" AND NOT UWB_ACCEL_TARGET STREQUAL "auto")
    message(FATAL_ERROR "UWB_ACCEL_TARGET must be 'legacy', 'gena', or 'auto' (received: ${UWB_ACCEL_TARGET})")
endif()

# nRF52833 memory layout for DWM3001CDK.
# Override before include(config.cmake), which defines nRF52840 defaults.
set(FLASH_SIZE 0x00080000 CACHE STRING "Flash size" FORCE)   # 512KB
set(RAM_SIZE   0x00020000 CACHE STRING "Ram size" FORCE)      # 128KB

option(UWB_DFU_APP_LAYOUT "Build application for Nordic Secure DFU bootloader layout" OFF)
option(UWB_BLE_ADV_ENABLED "Broadcast initiator distance in BLE advertising packets without SoftDevice" OFF)
option(UWB_BLE_GATT_ENABLED "Expose initiator commands and telemetry over Nordic UART Service BLE GATT" OFF)
option(UWB_BLE_GATT_STANDALONE "Diagnostic: build only a minimal S113/NUS app without UWB, SPI, or UART" OFF)
option(UWB_UART_RX_DISABLED "Diagnostic: do not start UART RX or poll UART commands" OFF)
option(UWB_UART_TX_DISABLED "Diagnostic: drop UART TX log output" OFF)
option(UWB_SOFTDEVICE_VTOR_DIRECT "Diagnostic: point VTOR directly at the SoftDevice vector table after BLE init" OFF)
option(UWB_BLE_GATT_PAUSE_AFTER_READY "Diagnostic: pause after BLE_GATT,READY before ranging loop" OFF)
set(UWB_BLE_GATT_STANDALONE_STAGE 0 CACHE STRING "Diagnostic standalone init stage: 0=BLE only, 1=gpio_init, 2=SPI init, 3=DW IRQ init, 4=DW probe, 5=DW initialise, 6=DW configure, 7=DW ranging settings, 8=accel init, 9=RTC2 start")
set(UWB_DFU_APP_FLASH_ADDR 0x00001000 CACHE STRING "Application flash start when Nordic MBR/bootloader is installed")
set(UWB_DFU_BOOTLOADER_START 0x00078000 CACHE STRING "Reserved bootloader start address for nRF52833 Secure DFU")
set(UWB_BLE_GATT_APP_FLASH_ADDR 0x0001C000 CACHE STRING "Application flash start for nRF52833 S113 SoftDevice")
set(UWB_BLE_GATT_SETTINGS_FLASH_ADDR 0x00077000 CACHE STRING "Reserved flash page for BLE GATT persistent settings")
# Event log ("black box") region: 6 slots x 20 KB, right below the settings
# page. The app flash region ends here so the linker can never overlap it.
set(UWB_BLE_GATT_EVENTLOG_FLASH_ADDR 0x00059000 CACHE STRING "Reserved flash region for UWB event log capture slots")
set(UWB_BLE_GATT_APP_RAM_ADDR 0x20002608 CACHE STRING "Application RAM start for nRF52833 S113 SoftDevice")
set(UWB_NRF5_SDK_PATH "${CMAKE_SOURCE_DIR}/nRF5_SDK_17.1.0_ddde560" CACHE PATH "Path to local Nordic nRF5 SDK")

# Include SDK CMake helpers.
include("${QORVO_SDK_PATH}/cmake/config.cmake")
include("${QORVO_SDK_PATH}/cmake/util.cmake")

if(UWB_DFU_APP_LAYOUT)
    math(EXPR UWB_DFU_APP_FLASH_SIZE "${UWB_DFU_BOOTLOADER_START} - ${UWB_DFU_APP_FLASH_ADDR}" OUTPUT_FORMAT HEXADECIMAL)
    set(FLASH_ADDR ${UWB_DFU_APP_FLASH_ADDR} CACHE STRING "Flash start address" FORCE)
    set(FLASH_SIZE ${UWB_DFU_APP_FLASH_SIZE} CACHE STRING "Flash size" FORCE)
    message("DFU app layout: FLASH_ADDR=${FLASH_ADDR}, FLASH_SIZE=${FLASH_SIZE}, BOOTLOADER_START=${UWB_DFU_BOOTLOADER_START}")
endif()

if(UWB_BLE_ADV_ENABLED AND UWB_BLE_GATT_ENABLED)
    message(FATAL_ERROR "UWB_BLE_ADV_ENABLED and UWB_BLE_GATT_ENABLED are mutually exclusive")
endif()

if(UWB_BLE_GATT_STANDALONE AND NOT UWB_BLE_GATT_ENABLED)
    message(FATAL_ERROR "UWB_BLE_GATT_STANDALONE requires UWB_BLE_GATT_ENABLED")
endif()

if(UWB_BLE_GATT_ENABLED)
    if(NOT UWB_ROLE STREQUAL "initiator")
        message(FATAL_ERROR "UWB_BLE_GATT_ENABLED is currently supported only for initiator builds")
    endif()
    if(NOT EXISTS "${UWB_NRF5_SDK_PATH}/components/softdevice/s113/hex/s113_nrf52_7.2.0_softdevice.hex")
        message(FATAL_ERROR "UWB_BLE_GATT_ENABLED requires nRF5 SDK 17.1.0 at UWB_NRF5_SDK_PATH=${UWB_NRF5_SDK_PATH}")
    endif()
    math(EXPR UWB_BLE_GATT_APP_FLASH_SIZE "${UWB_BLE_GATT_EVENTLOG_FLASH_ADDR} - ${UWB_BLE_GATT_APP_FLASH_ADDR}" OUTPUT_FORMAT HEXADECIMAL)
    set(FLASH_ADDR ${UWB_BLE_GATT_APP_FLASH_ADDR} CACHE STRING "Flash start address" FORCE)
    set(FLASH_SIZE ${UWB_BLE_GATT_APP_FLASH_SIZE} CACHE STRING "Flash size" FORCE)
    set(RAM_ADDR ${UWB_BLE_GATT_APP_RAM_ADDR} CACHE STRING "Ram start address" FORCE)
    math(EXPR UWB_BLE_GATT_APP_RAM_SIZE "0x20020000 - ${UWB_BLE_GATT_APP_RAM_ADDR}" OUTPUT_FORMAT HEXADECIMAL)
    set(RAM_SIZE ${UWB_BLE_GATT_APP_RAM_SIZE} CACHE STRING "Ram size" FORCE)
    message("BLE GATT S113 layout: FLASH_ADDR=${FLASH_ADDR}, FLASH_SIZE=${FLASH_SIZE}, SETTINGS_ADDR=${UWB_BLE_GATT_SETTINGS_FLASH_ADDR}, RAM_ADDR=${RAM_ADDR}, RAM_SIZE=${RAM_SIZE}")
endif()

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
if(UWB_PURE)
    set(TARGET_ELF uwb_${UWB_ROLE}_pure.elf)
    set(TARGET_HEX uwb_${UWB_ROLE}_pure.hex)
    set(TARGET_BASENAME uwb_${UWB_ROLE}_pure)
else()
    set(TARGET_ELF uwb_${UWB_ROLE}_${UWB_ACCEL_TARGET}.elf)
    set(TARGET_HEX uwb_${UWB_ROLE}_${UWB_ACCEL_TARGET}.hex)
    set(TARGET_BASENAME uwb_${UWB_ROLE}_${UWB_ACCEL_TARGET})
endif()

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
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/prs/nrfx_prs.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_spi.c
    ${PLATFORM_PATH}/sdk/modules/nrfx/drivers/src/nrfx_spim.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error_handler_gcc.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_error_weak.c
    ${PLATFORM_PATH}/sdk/components/libraries/util/app_util_platform.c
    ${PLATFORM_PATH}/sdk/components/libraries/atomic/nrf_atomic.c
    ${PLATFORM_PATH}/sdk/components/libraries/balloc/nrf_balloc.c
    ${PLATFORM_PATH}/sdk/components/libraries/log/src/nrf_log_frontend.c
    ${PLATFORM_PATH}/sdk/components/libraries/memobj/nrf_memobj.c
    ${PLATFORM_PATH}/sdk/components/libraries/strerror/nrf_strerror.c
)

# Required Qorvo SDK sources.
set(SDK_SOURCES
    ${EXAMPLES_PATH}/config_options.c
    ${EXAMPLES_PATH}/examples/shared_data/shared_functions.c
)

# Project sources.
if(UWB_PURE)
    # Pure UWB: no IMU, no pyro, no radio-quality diagnostics, no profiles.
    set(PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/common/ranging.c
        ${CMAKE_SOURCE_DIR}/src/platform/nrf_hfxo.c
        ${CMAKE_SOURCE_DIR}/src/uart/uart_log.c
    )
    if(UWB_ROLE STREQUAL "initiator")
        list(APPEND PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/initiator/main_initiator_pure.c)
    else()
        list(APPEND PROJECT_SOURCES ${CMAKE_SOURCE_DIR}/src/responder/main_responder_pure.c)
    endif()
else()
set(PROJECT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/ranging.c
    ${CMAKE_SOURCE_DIR}/src/common/radio_quality.c
    ${CMAKE_SOURCE_DIR}/src/common/uwb_profiles.c
    ${CMAKE_SOURCE_DIR}/src/board/pyro_buzzer.c
)

# Select initiator or responder application source.
if(UWB_BLE_GATT_STANDALONE)
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ble/ble_gatt_standalone_main.c
        ${CMAKE_SOURCE_DIR}/src/accel/accel.c
    )
elseif(UWB_ROLE STREQUAL "initiator")
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
endif()

if(UWB_BLE_ADV_ENABLED)
    if(NOT UWB_ROLE STREQUAL "initiator")
        message(FATAL_ERROR "UWB_BLE_ADV_ENABLED is currently supported only for initiator builds")
    endif()
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ble/ble_adv.c
    )
endif()

if(UWB_BLE_GATT_ENABLED)
    set(NRF5_SDK_PATH "${UWB_NRF5_SDK_PATH}")
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ble/ble_nus_bridge.c
        ${CMAKE_SOURCE_DIR}/src/ble/softdevice_svc_forwarder.S
        ${CMAKE_SOURCE_DIR}/src/common/uwb_persistent_settings.c
        ${CMAKE_SOURCE_DIR}/src/common/uwb_event_log.c
        ${NRF5_SDK_PATH}/components/ble/ble_services/ble_nus/ble_nus.c
        ${NRF5_SDK_PATH}/components/ble/ble_link_ctx_manager/ble_link_ctx_manager.c
        ${NRF5_SDK_PATH}/components/ble/common/ble_advdata.c
        ${NRF5_SDK_PATH}/components/ble/common/ble_conn_state.c
        ${NRF5_SDK_PATH}/components/ble/common/ble_srv_common.c
        ${NRF5_SDK_PATH}/components/ble/nrf_ble_gatt/nrf_ble_gatt.c
        ${NRF5_SDK_PATH}/components/softdevice/common/nrf_sdh.c
        ${NRF5_SDK_PATH}/components/softdevice/common/nrf_sdh_ble.c
        ${NRF5_SDK_PATH}/components/softdevice/common/nrf_sdh_soc.c
        ${NRF5_SDK_PATH}/components/libraries/atomic_flags/nrf_atflags.c
        ${NRF5_SDK_PATH}/components/libraries/experimental_section_vars/nrf_section_iter.c
        ${NRF5_SDK_PATH}/modules/nrfx/soc/nrfx_atomic.c
    )
endif()

# Custom application entrypoint.
if(NOT UWB_BLE_GATT_STANDALONE)
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/main.c
    )
endif()

# Include directories.
set(INCLUDE_DIRS
    # Project code
    ${CMAKE_SOURCE_DIR}/src/board
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/accel
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/uart
    ${CMAKE_SOURCE_DIR}/src/ble

    # SDK platform
    ${PLATFORM_PATH}/Source/config
    ${PLATFORM_PATH}/Source/platform
    ${PLATFORM_PATH}/sdk/components/drivers_nrf/nrf_soc_nosd
    ${PLATFORM_PATH}/sdk/components/boards
    ${PLATFORM_PATH}/sdk/components/toolchain/cmsis/include
    ${PLATFORM_PATH}/sdk/components/libraries/balloc
    ${PLATFORM_PATH}/sdk/components/libraries/ringbuf
    ${PLATFORM_PATH}/sdk/components/libraries/log
    ${PLATFORM_PATH}/sdk/components/libraries/log/src
    ${PLATFORM_PATH}/sdk/components/libraries/memobj
    ${PLATFORM_PATH}/sdk/components/libraries/util
    ${PLATFORM_PATH}/sdk/components/libraries/atomic
    ${PLATFORM_PATH}/sdk/components/libraries/delay
    ${PLATFORM_PATH}/sdk/components/libraries/experimental_section_vars
    ${PLATFORM_PATH}/sdk/components/libraries/strerror
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

if(UWB_BLE_GATT_ENABLED)
    list(APPEND INCLUDE_DIRS
        ${UWB_NRF5_SDK_PATH}/components
        ${UWB_NRF5_SDK_PATH}/components/ble/common
        ${UWB_NRF5_SDK_PATH}/components/ble/ble_link_ctx_manager
        ${UWB_NRF5_SDK_PATH}/components/ble/ble_services/ble_nus
        ${UWB_NRF5_SDK_PATH}/components/ble/nrf_ble_gatt
        ${UWB_NRF5_SDK_PATH}/components/softdevice/common
        ${UWB_NRF5_SDK_PATH}/components/softdevice/s113/headers
        ${UWB_NRF5_SDK_PATH}/components/softdevice/s113/headers/nrf52
        ${UWB_NRF5_SDK_PATH}/components/libraries/experimental_section_vars
        ${UWB_NRF5_SDK_PATH}/components/libraries/atomic_flags
        ${UWB_NRF5_SDK_PATH}/components/libraries/svc
        ${UWB_NRF5_SDK_PATH}/components/libraries/fds
        ${UWB_NRF5_SDK_PATH}/components/libraries/fstorage
        ${UWB_NRF5_SDK_PATH}/components/libraries/timer
        ${UWB_NRF5_SDK_PATH}/components/libraries/util
        ${UWB_NRF5_SDK_PATH}/components/libraries/strerror
        ${UWB_NRF5_SDK_PATH}/external/fprintf
        ${UWB_NRF5_SDK_PATH}/modules/nrfx
        ${UWB_NRF5_SDK_PATH}/modules/nrfx/hal
        ${UWB_NRF5_SDK_PATH}/modules/nrfx/mdk
        ${UWB_NRF5_SDK_PATH}/modules/nrfx/soc
        ${UWB_NRF5_SDK_PATH}/integration/nrfx
    )
endif()

# Create executable.
add_executable(${TARGET_ELF}
    ${PLATFORM_SOURCES}
    ${SDK_SOURCES}
    ${PROJECT_SOURCES}
)

target_include_directories(${TARGET_ELF} PRIVATE ${INCLUDE_DIRS})

if(UWB_BLE_GATT_ENABLED)
    target_include_directories(${TARGET_ELF} BEFORE PRIVATE
        ${UWB_NRF5_SDK_PATH}/components/softdevice/s113/headers
        ${UWB_NRF5_SDK_PATH}/components/softdevice/s113/headers/nrf52
    )
endif()

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
    FLOAT_ABI_HARD
    INITIALIZE_USER_SECTIONS
    $<$<NOT:$<BOOL:${UWB_BLE_GATT_ENABLED}>>:NO_VTOR_CONFIG>
    NRF52833_XXAA
    # nRF52840-only SPIM3 erratum workaround writes to 0x40000E00 (POWER
    # space); with the SoftDevice enabled the MWU traps that write and the
    # firmware hangs. Not applicable to nRF52833 — force it off.
    NRFX_SPIM3_NRF52840_ANOMALY_198_WORKAROUND_ENABLED=0
    USE_DRV_DW3000
    USE_DRV_DW3720
    NB_DW_DRIVERS=2
    NRF_LOG_BACKEND_RTT_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    NRF_LOG_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    # UWB config: option 35 = channel 5, PRF 64M, preamble 128, 6.8 Mbps, STS 64.
    CMAKE_UWB_CONFIG_OPTION=35
    # Role selection for main.c.
    $<$<STREQUAL:${UWB_ROLE},initiator>:UWB_ROLE_INITIATOR>
    $<$<STREQUAL:${UWB_ROLE},responder>:UWB_ROLE_RESPONDER>
    # Pure UWB build: no BLE, no IMU, HFXO forced, UART at 1 Mbps (or 460800 if UWB_UART_1M=OFF).
    $<$<BOOL:${UWB_PURE}>:UWB_PURE>
    $<$<AND:$<BOOL:${UWB_PURE}>,$<BOOL:${UWB_UART_1M}>>:UWB_UART_BAUD_1M>
    # Accelerometer hardware generation selection.
    $<$<STREQUAL:${UWB_ACCEL_TARGET},legacy>:UWB_ACCEL_TARGET_LEGACY>
    $<$<STREQUAL:${UWB_ACCEL_TARGET},gena>:UWB_ACCEL_TARGET_GENA>
    $<$<STREQUAL:${UWB_ACCEL_TARGET},auto>:UWB_ACCEL_TARGET_AUTO>
    $<$<BOOL:${UWB_BLE_ADV_ENABLED}>:UWB_BLE_ADV_ENABLED>
    $<$<BOOL:${UWB_BLE_GATT_ENABLED}>:UWB_BLE_GATT_ENABLED>
    $<$<BOOL:${UWB_BLE_GATT_STANDALONE}>:UWB_BLE_GATT_STANDALONE>
    $<$<BOOL:${UWB_UART_RX_DISABLED}>:UWB_UART_RX_DISABLED>
    $<$<BOOL:${UWB_UART_TX_DISABLED}>:UWB_UART_TX_DISABLED>
    $<$<BOOL:${UWB_SOFTDEVICE_VTOR_DIRECT}>:UWB_SOFTDEVICE_VTOR_DIRECT>
    $<$<BOOL:${UWB_BLE_GATT_PAUSE_AFTER_READY}>:UWB_BLE_GATT_PAUSE_AFTER_READY>
    UWB_BLE_GATT_STANDALONE_STAGE=${UWB_BLE_GATT_STANDALONE_STAGE}
)

if(UWB_BLE_GATT_ENABLED)
    target_compile_definitions(${TARGET_ELF} PRIVATE
        APP_TIMER_V2
        APP_TIMER_V2_RTC1_ENABLED
        BLE_ADVDATA_ENABLED=1
        BLE_CONN_STATE_ENABLED=1
        BLE_NUS_ENABLED=1
        BLE_NUS_BLE_OBSERVER_PRIO=2
        NRF_BLE_GATT_ENABLED=1
        NRF_BLE_GATT_MTU_EXCHANGE_INITIATION_ENABLED=1
        NRF_SDH_ENABLED=1
        NRF_SDH_BLE_ENABLED=1
        NRF_SDH_SOC_ENABLED=1
        UWB_SETTINGS_FLASH_ADDR=${UWB_BLE_GATT_SETTINGS_FLASH_ADDR}
        UWB_EVENTLOG_FLASH_ADDR=${UWB_BLE_GATT_EVENTLOG_FLASH_ADDR}
        NRF_SDH_CLOCK_LF_SRC=0
        NRF_SDH_CLOCK_LF_RC_CTIV=16
        NRF_SDH_CLOCK_LF_RC_TEMP_CTIV=2
        NRF_SDH_CLOCK_LF_ACCURACY=1
        NRF_SDH_BLE_CENTRAL_LINK_COUNT=0
        NRF_SDH_BLE_GAP_DATA_LENGTH=27
        NRF_SDH_BLE_GAP_EVENT_LENGTH=6
        NRF_SDH_BLE_GATT_MAX_MTU_SIZE=23
        NRF_SDH_BLE_GATTS_ATTR_TAB_SIZE=1408
        NRF_SDH_BLE_OBSERVER_PRIO_LEVELS=4
        NRF_SDH_BLE_PERIPHERAL_LINK_COUNT=1
        NRF_SDH_BLE_SERVICE_CHANGED=0
        NRF_SDH_BLE_TOTAL_LINK_COUNT=1
        NRF_SDH_BLE_VS_UUID_COUNT=1
        NRF_SD_BLE_API_VERSION=7
        S113
        SOFTDEVICE_PRESENT
    )
endif()

target_link_options(${TARGET_ELF} PRIVATE
    -Wl,-Map=$<TARGET_FILE_DIR:${TARGET_ELF}>/${TARGET_BASENAME}.map
)

set_target_properties(${TARGET_ELF} PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT_OUT}
)

# Generate flashable hex.
add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET_ELF}> ${TARGET_HEX}
    COMMENT "Generating ${TARGET_HEX}"
)
