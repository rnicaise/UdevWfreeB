#!/usr/bin/env zsh
set -euo pipefail

# Build Nordic Secure DFU **BLE** bootloader for DWM3001 (nRF52833 + S113).
# Based on SDK example pca10100_s113_ble. Output: build/dfu_bootloader_ble_dwm3001/armgcc/_build/nrf52833_xxaa_s113.hex
# Flash layout: bootloader 0x78000 (24 KB), MBR params 0x7E000, BL settings 0x7F000 — matches app layout.

sdk_root="${NRF5_SDK_ROOT:-${1:-nRF5_SDK_17.1.0_ddde560}}"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk_root="$(cd "$repo_root/$sdk_root" 2>/dev/null || cd "$sdk_root"; pwd)"
work_dir="$repo_root/build/dfu_bootloader_ble_dwm3001"
source_dir="$sdk_root/examples/dfu/secure_bootloader/pca10100_s113_ble"
armgcc_dir="$work_dir/armgcc"
key_file="${DFU_PRIVATE_KEY:-$repo_root/secrets/dfu_private.pem}"

gnu_root="$(dirname "$(command -v arm-none-eabi-gcc)")/"

if [[ ! -d "$source_dir" ]]; then
  print "Missing Nordic BLE bootloader example: $source_dir" >&2
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

# Patch BLE transport: keep the SAME GAP address as the application so the
# web page can reconnect to the bootloader without a second Bluetooth chooser.
cp "$sdk_root/components/libraries/bootloader/ble_dfu/nrf_dfu_ble.c" "$work_dir/nrf_dfu_ble.c"
python3 - <<'PY' "$work_dir/nrf_dfu_ble.c"
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = "    /* Increase the BLE address by one when advertising openly. */\n    addr.addr[0] += 1;\n"
assert old in text
path.write_text(text.replace(old, "    /* Keep the application's BLE address so web clients can silently reconnect. */\n"))
PY

python3 - <<'PY' "$armgcc_dir/Makefile" "$sdk_root" "$gnu_root"
from pathlib import Path
import sys

makefile = Path(sys.argv[1])
sdk_root = sys.argv[2]
gnu_root = sys.argv[3]
text = makefile.read_text()
text = text.replace('PROJECT_NAME     := secure_bootloader_ble_s113_pca10100', 'PROJECT_NAME     := secure_bootloader_ble_s113_dwm3001')
text = text.replace('SDK_ROOT := ../../../../..', f'SDK_ROOT := {sdk_root}')
text = text.replace('PROJ_DIR := ../..', 'PROJ_DIR := $(SDK_ROOT)/examples/dfu/secure_bootloader')
text = text.replace('$(PROJ_DIR)/../dfu_public_key.c', f'{makefile.parent.parent}/dfu_public_key.c')
text = text.replace('$(SDK_ROOT)/components/libraries/bootloader/ble_dfu/nrf_dfu_ble.c', f'{makefile.parent.parent}/nrf_dfu_ble.c')
text = text.replace('CFLAGS += -DBOARD_PCA10100', 'CFLAGS += -DBOARD_CUSTOM')
text = text.replace('ASMFLAGS += -DBOARD_PCA10100', 'ASMFLAGS += -DBOARD_CUSTOM')
text = text.replace('CFLAGS += -Wall -Werror', 'CFLAGS += -Wall -Werror -Wno-array-bounds -Wno-error=array-bounds')
text = text.replace('GNU_INSTALL_ROOT ?= /usr/local/gcc-arm-none-eabi-9-2020-q2-update/bin/', f'GNU_INSTALL_ROOT ?= {gnu_root}')
# config dir: use our patched copy instead of the SDK one
text = text.replace('$(PROJ_DIR)/pca10100_s113_ble/config', f'{makefile.parent.parent}/config')
makefile.write_text(text)
PY

python3 - <<'PY' "$work_dir/config/sdk_config.h"
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
replacements = {
    # No usable button on DWM3001 in the field: GPREGRET (BOOT,DFU) only
    '#define NRF_BL_DFU_ENTER_METHOD_BUTTON 1': '#define NRF_BL_DFU_ENTER_METHOD_BUTTON 0',
    # Give the web page time to reconnect and stream the update
    '#define NRF_BL_DFU_INACTIVITY_TIMEOUT_MS 120000': '#define NRF_BL_DFU_INACTIVITY_TIMEOUT_MS 120000',
    '#define NRF_DFU_BLE_ADV_NAME "DfuTarg"': '#define NRF_DFU_BLE_ADV_NAME "UWBR_DFU"',
    # Same GAP address as the app: expose Service Changed so hosts drop their GATT cache
    '#define NRF_SDH_BLE_SERVICE_CHANGED 0': '#define NRF_SDH_BLE_SERVICE_CHANGED 1',
}
for old, new in replacements.items():
    assert old in text, f'missing: {old}'
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
EOF

print "Building BLE secure bootloader"
make -C "$armgcc_dir" -j8 GNU_INSTALL_ROOT="$gnu_root"

print "Done: $armgcc_dir/_build/nrf52833_xxaa_s113.hex"
