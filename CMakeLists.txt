#
# CMakeLists.txt — UWB Ranging Minimal (DS-TWR)
#
# Ce projet s'intègre dans le build system du SDK Qorvo.
# Il réutilise le driver UWB, le BSP nRF, et les configs UWB
# du SDK, mais remplace les exemples par notre propre code.
#
# Target : DWM3001CDK (nRF52833 + DW3000)
#
# COMMENT ÇA MARCHE :
#
#   Le SDK Qorvo a cette structure de build :
#
#     Drivers/CMakeLists.txt          ← Top-level, crée le projet
#       ├── cmake/config.cmake        ← Variables (driver DW3000/DW3720)
#       ├── cmake/arm-none-eabi-gcc.cmake ← Toolchain ARM cross-compile
#       ├── API/Shared/dwt_uwb_driver ← Librairie driver UWB (compilée en .a)
#       └── API/Build_Platforms/nRF52840-DK/
#           ├── CMakeLists.txt        ← Sources platform (SPI, GPIO, etc.)
#           ├── Source/main.c         ← Point d'entrée (appelle example_pointer)
#           ├── Source/platform/      ← HAL nRF52840 (SPI, sleep, mutex...)
#           ├── sdk/                  ← Nordic SDK (drivers nRF52840)
#           ├── Setup/               ← Linker script
#           ├── Startup/             ← Code démarrage ARM (reset handler)
#           └── lib/                 ← RTT, chelpers, qio, qosal
#
#   Notre projet ne modifie RIEN dans le SDK. On ajoute juste nos .c
#   comme sources supplémentaires et on remplace le example_selection.h
#   par notre propre sélection.
#
# UTILISATION :
#
#   # Depuis ce dossier (uwb-ranging/)
#   cmake --preset=initiator_debug
#   cmake --build --preset initiator_debug
#
#   # Ou pour le responder :
#   cmake --preset=responder_debug
#   cmake --build --preset responder_debug
#

cmake_minimum_required(VERSION 3.20)

# ── Chemin vers le SDK Qorvo ──
# ── SDK vendorisé dans vendor/sdk/ ──
# Toutes les dépendances SDK (driver UWB, Nordic nRF5 SDK, HAL platform)
# sont incluses dans le repo. Pas besoin de télécharger le SDK séparément.
set(QORVO_SDK_PATH "${CMAKE_SOURCE_DIR}/vendor/sdk" CACHE PATH "Path to vendored SDK")

# ── Toolchain ARM (depuis le SDK) ──
set(CMAKE_TOOLCHAIN_FILE "${QORVO_SDK_PATH}/cmake/arm-none-eabi-gcc.cmake" CACHE STRING "Toolchain")

# ── Projet ──
project(uwb_ranging VERSION 1.0.0 LANGUAGES C ASM)

message("=== UWB Ranging Minimal ===")
message("SDK path: ${QORVO_SDK_PATH}")
message("Build type: ${CMAKE_BUILD_TYPE}")
message("Role: ${UWB_ROLE}")

# ── Validation du rôle (initiator ou responder) ──
if(NOT DEFINED UWB_ROLE)
    message(FATAL_ERROR "UWB_ROLE non défini. Utilise un preset ou -DUWB_ROLE=initiator|responder")
endif()
if(NOT UWB_ROLE STREQUAL "initiator" AND NOT UWB_ROLE STREQUAL "responder")
    message(FATAL_ERROR "UWB_ROLE doit être 'initiator' ou 'responder' (reçu: ${UWB_ROLE})")
endif()

# ── Memory layout nRF52833 (DWM3001CDK) ──
# Override AVANT include(config.cmake) qui définit les défauts nRF52840
set(FLASH_SIZE 0x00080000 CACHE STRING "Flash size" FORCE)   # 512KB
set(RAM_SIZE   0x00020000 CACHE STRING "Ram size" FORCE)      # 128KB

# ── Inclure la config du SDK ──
include("${QORVO_SDK_PATH}/cmake/config.cmake")
include("${QORVO_SDK_PATH}/cmake/util.cmake")

# ── Paths SDK ──
set(UWB_DRIVER_PATH "${QORVO_SDK_PATH}/API/Shared/dwt_uwb_driver")
set(PLATFORM_PATH "${QORVO_SDK_PATH}/API/Build_Platforms/nRF52840-DK")
set(EXAMPLES_PATH "${QORVO_SDK_PATH}/API/Src")

# Variable attendue par les CMakeLists.txt internes du SDK
set(PLATFORM_SPECIFIC_FILES_PATH "${PLATFORM_PATH}" CACHE STRING "" FORCE)

# Linker script (template nRF, configuré avec les tailles nRF52833)
set(LINKER_SCRIPT_IN "${PLATFORM_PATH}/Setup/nrf52840_linker.ld.in")
set(LINKER_SCRIPT_OUT "${CMAKE_BINARY_DIR}/nrf52833_linker.ld")
configure_file(${LINKER_SCRIPT_IN} ${LINKER_SCRIPT_OUT} @ONLY)

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -T ${LINKER_SCRIPT_OUT} -nostartfiles")

# ── Compiler le driver UWB ──
# Note: Le SDK a un faux positif -Wmaybe-uninitialized avec GCC 15
# On ne peut pas modifier le code SDK, on supprime juste ce warning pour le driver
set(DWT_DW3000 ${USE_DRV_DW3000})
set(DWT_DW3720 ${USE_DRV_DW3720})
add_subdirectory(${UWB_DRIVER_PATH} ${CMAKE_BINARY_DIR}/uwb_driver)

# Supprimer le warning faux positif dans le code SDK qu'on ne contrôle pas
if(TARGET dw3000_uwb_driver)
    target_compile_options(dw3000_uwb_driver PRIVATE -Wno-maybe-uninitialized)
endif()
if(TARGET dw3720_uwb_driver)
    target_compile_options(dw3720_uwb_driver PRIVATE -Wno-maybe-uninitialized)
endif()

# ── Compiler les libs platform ──
add_subdirectory(${PLATFORM_PATH}/lib/chelpers ${CMAKE_BINARY_DIR}/chelpers)
add_subdirectory(${PLATFORM_PATH}/lib/rtt ${CMAKE_BINARY_DIR}/rtt)
add_subdirectory(${PLATFORM_PATH}/lib/qio ${CMAKE_BINARY_DIR}/qio)
add_subdirectory(${PLATFORM_PATH}/lib/qosal ${CMAKE_BINARY_DIR}/qosal)

# ── Startup nRF52833 (on ne peut pas utiliser nrf52840_startup du SDK) ──
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

# ── Nom de l'exécutable ──
set(TARGET_ELF uwb_${UWB_ROLE}.elf)
set(TARGET_HEX uwb_${UWB_ROLE}.hex)

# ── Sources ──
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

# Sources du SDK Qorvo nécessaires (config UWB, shared functions)
set(SDK_SOURCES
    ${EXAMPLES_PATH}/config_options.c
    ${EXAMPLES_PATH}/examples/shared_data/shared_functions.c
)

# Notre code
set(PROJECT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/ranging.c
)

# Sélection initiator ou responder
if(UWB_ROLE STREQUAL "initiator")
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/initiator/main_initiator.c
    )
elseif(UWB_ROLE STREQUAL "responder")
    list(APPEND PROJECT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/responder/main_responder.c
        ${CMAKE_SOURCE_DIR}/src/ble/ble_adv.c
    )
endif()

# main.c custom (point d'entrée)
list(APPEND PROJECT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.c
)

# ── Includes ──
set(INCLUDE_DIRS
    # Notre code
    ${CMAKE_SOURCE_DIR}/src/board
    ${CMAKE_SOURCE_DIR}/src/common

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

    # SDK exemples (pour config_options.h, shared_defines.h, etc.)
    ${EXAMPLES_PATH}/
    ${EXAMPLES_PATH}/examples/shared_data
    ${EXAMPLES_PATH}/examples/examples_info
)

# ── Créer l'exécutable ──
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
    FLOAT_ABI_HARD
    INITIALIZE_USER_SECTIONS
    NO_VTOR_CONFIG
    NRF52833_XXAA
    USE_DRV_DW3000
    USE_DRV_DW3720
    NB_DW_DRIVERS=2
    NRF_LOG_BACKEND_RTT_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    NRF_LOG_ENABLED=$<IF:$<CONFIG:Debug>,1,0>
    # Config UWB : Option 35 = Channel 5, PRF 64M, Preamble 128, 6.8Mbps, STS 64
    CMAKE_UWB_CONFIG_OPTION=35
    # Sélection du rôle pour main.c
    $<$<STREQUAL:${UWB_ROLE},initiator>:UWB_ROLE_INITIATOR>
    $<$<STREQUAL:${UWB_ROLE},responder>:UWB_ROLE_RESPONDER>
)

target_link_options(${TARGET_ELF} PRIVATE
    -Wl,-Map=$<TARGET_FILE_DIR:${TARGET_ELF}>/uwb_${UWB_ROLE}.map
)

set_target_properties(${TARGET_ELF} PROPERTIES
    LINK_DEPENDS ${LINKER_SCRIPT_OUT}
)

# ── Générer le .hex (flashable) ──
add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${TARGET_ELF}> ${TARGET_HEX}
    COMMENT "Generating ${TARGET_HEX}"
)
