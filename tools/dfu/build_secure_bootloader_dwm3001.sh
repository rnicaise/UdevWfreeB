#!/usr/bin/env zsh
set -euo pipefail

sdk_root="${NRF5_SDK_ROOT:-${1:-nRF5_SDK_17.1.0_ddde560}}"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk_root="$(cd "$repo_root/$sdk_root" 2>/dev/null || cd "$sdk_root"; pwd)"
work_dir="$repo_root/build/dfu_bootloader_dwm3001"
source_dir="$sdk_root/examples/dfu/secure_bootloader/pca10040_uart"
armgcc_dir="$work_dir/armgcc"
key_file="${DFU_PRIVATE_KEY:-$repo_root/secrets/dfu_private.pem}"

gnu_root="$(dirname "$(command -v arm-none-eabi-gcc)")/"

if [[ ! -d "$source_dir" ]]; then
  print "Missing Nordic UART bootloader example: $source_dir" >&2
  exit 1
fi

if [[ ! -f "$sdk_root/external/micro-ecc/micro-ecc/uECC.c" ]]; then
  print "micro-ecc source missing; running Nordic fetch/build helper"
  (cd "$sdk_root/external/micro-ecc" && tr -d '\r' < build_all.sh | GNU_INSTALL_ROOT="$gnu_root" bash)
fi

if [[ ! -f "$sdk_root/external/micro-ecc/nrf52hf_armgcc/armgcc/micro_ecc_lib_nrf52.a" ]]; then
  print "Building micro-ecc hard-float library"
  make -C "$sdk_root/external/micro-ecc/nrf52hf_armgcc/armgcc" GNU_INSTALL_ROOT="$gnu_root"
fi

rm -rf "$work_dir"
mkdir -p "$work_dir"
mkdir -p "$(dirname "$key_file")"

if [[ ! -f "$key_file" ]]; then
  print "Generating local DFU signing key: $key_file"
  nrfutil nrf5sdk-tools keys generate "$key_file" >/dev/null
fi

nrfutil nrf5sdk-tools keys display --key pk --format code "$key_file" > "$work_dir/dfu_public_key.c"

cp -R "$source_dir/config" "$work_dir/config"
mkdir -p "$armgcc_dir"
cp "$source_dir/armgcc/Makefile" "$armgcc_dir/Makefile"
cp "$source_dir/armgcc/secure_bootloader_gcc_nrf52.ld" "$armgcc_dir/secure_bootloader_gcc_nrf52.ld"

python3 - <<'PY' "$armgcc_dir/Makefile" "$sdk_root" "$gnu_root"
from pathlib import Path
import sys

makefile = Path(sys.argv[1])
sdk_root = sys.argv[2]
gnu_root = sys.argv[3]
text = makefile.read_text()
text = text.replace('PROJECT_NAME     := secure_bootloader_uart_mbr_pca10040', 'PROJECT_NAME     := secure_bootloader_uart_mbr_dwm3001_nrf52833')
text = text.replace('TARGETS          := nrf52832_xxaa_mbr', 'TARGETS          := nrf52833_xxaa_mbr')
text = text.replace('SDK_ROOT := ../../../../..', f'SDK_ROOT := {sdk_root}')
text = text.replace('PROJ_DIR := ../..', 'PROJ_DIR := $(SDK_ROOT)/examples/dfu/secure_bootloader')
text = text.replace('$(OUTPUT_DIRECTORY)/nrf52832_xxaa_mbr.out:', '$(OUTPUT_DIRECTORY)/nrf52833_xxaa_mbr.out:')
text = text.replace('$(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52.S', '$(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52833.S')
text = text.replace('$(SDK_ROOT)/modules/nrfx/mdk/system_nrf52.c', '$(SDK_ROOT)/modules/nrfx/mdk/system_nrf52833.c')
text = text.replace('$(PROJ_DIR)/../dfu_public_key.c', f'{makefile.parent.parent}/dfu_public_key.c')
text = text.replace('CFLAGS += -DBOARD_PCA10040', 'CFLAGS += -DBOARD_CUSTOM')
text = text.replace('CFLAGS += -DNRF52832_XXAA\n', 'CFLAGS += -DNRF52833_XXAA\n')
text = text.replace('CFLAGS += -DNRF52_PAN_74\n', '')
text = text.replace('CFLAGS += -Wall -Werror', 'CFLAGS += -Wall -Werror -Wno-array-bounds -Wno-error=array-bounds')
text = text.replace('ASMFLAGS += -DNRF52832_XXAA\n', 'ASMFLAGS += -DNRF52833_XXAA\n')
text = text.replace('ASMFLAGS += -DNRF52_PAN_74\n', '')
text = text.replace('ASMFLAGS += -DBOARD_PCA10040', 'ASMFLAGS += -DBOARD_CUSTOM')
text = text.replace('nrf52832_xxaa_mbr: CFLAGS += -D__HEAP_SIZE=0', 'nrf52833_xxaa_mbr: CFLAGS += -D__HEAP_SIZE=0')
text = text.replace('nrf52832_xxaa_mbr: ASMFLAGS += -D__HEAP_SIZE=0', 'nrf52833_xxaa_mbr: ASMFLAGS += -D__HEAP_SIZE=0')
text = text.replace('default: nrf52832_xxaa_mbr', 'default: nrf52833_xxaa_mbr')
text = text.replace('@echo\t\tnrf52832_xxaa_mbr', '@echo\t\tnrf52833_xxaa_mbr')
text = text.replace('$(OUTPUT_DIRECTORY)/nrf52832_xxaa_mbr.hex', '$(OUTPUT_DIRECTORY)/nrf52833_xxaa_mbr.hex')
text = text.replace('GNU_INSTALL_ROOT ?= /usr/local/gcc-arm-none-eabi-9-2020-q2-update/bin/', f'GNU_INSTALL_ROOT ?= {gnu_root}')
makefile.write_text(text)
PY

python3 - <<'PY' "$work_dir/config/sdk_config.h"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
replacements = {
    '#define NRF_BL_DFU_ENTER_METHOD_BUTTON 1': '#define NRF_BL_DFU_ENTER_METHOD_BUTTON 0',
    '#define NRF_BL_DFU_ENTER_METHOD_BUTTON_PIN 16': '#define NRF_BL_DFU_ENTER_METHOD_BUTTON_PIN 16',
    '#define NRF_BL_DFU_INACTIVITY_TIMEOUT_MS 120000': '#define NRF_BL_DFU_INACTIVITY_TIMEOUT_MS 30000',
    '#define NRF_DFU_SERIAL_UART_USES_HWFC 1': '#define NRF_DFU_SERIAL_UART_USES_HWFC 0',
    '#define UART_DEFAULT_CONFIG_HWFC 0': '#define UART_DEFAULT_CONFIG_HWFC 0',
    '#define NRFX_UART_DEFAULT_CONFIG_HWFC 0': '#define NRFX_UART_DEFAULT_CONFIG_HWFC 0',
    '#define NRFX_UARTE_DEFAULT_CONFIG_HWFC 0': '#define NRFX_UARTE_DEFAULT_CONFIG_HWFC 0',
}
for old, new in replacements.items():
    text = text.replace(old, new)
path.write_text(text)
PY

cat > "$work_dir/config/custom_board.h" <<'EOF'
#pragma once

#include "nrf_gpio.h"

#define LEDS_NUMBER 1
#define LED_1 14
#define LED_START LED_1
#define LED_STOP LED_1
#define LEDS_LIST { LED_1 }
#define LEDS_ACTIVE_STATE 0
#define LEDS_INV_MASK (1UL << LED_1)
#define BSP_LED_0 LED_1

#define BUTTONS_NUMBER 0
#define BUTTONS_LIST { }
#define BUTTONS_ACTIVE_STATE 0
#define BUTTON_PULL NRF_GPIO_PIN_PULLUP

#define RX_PIN_NUMBER 15
#define TX_PIN_NUMBER 19
#define CTS_PIN_NUMBER 0xFFFFFFFF
#define RTS_PIN_NUMBER 0xFFFFFFFF
#define HWFC false
EOF

print "Building DWM3001 nRF52833 UART Secure DFU bootloader in $work_dir"
make -C "$armgcc_dir" GNU_INSTALL_ROOT="$gnu_root" "$@"
print "Bootloader hex: $armgcc_dir/_build/nrf52833_xxaa_mbr.hex"
print "MBR hex:        $sdk_root/components/softdevice/mbr/hex/mbr_nrf52_2.4.1_mbr.hex"
