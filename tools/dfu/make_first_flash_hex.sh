#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -lt 3 ]]; then
  print "Usage: $0 <role> <application.hex> <output-dir> [app-version]" >&2
  print "Example: $0 initiator_gena build/initiator_gena_dfu_debug/uwb_initiator_gena.hex build/dfu_packages 1" >&2
  exit 2
fi

role="$1"
app_hex="$2"
out_dir="$3"
app_version="${4:-1}"

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
sdk_root="${NRF5_SDK_ROOT:-$repo_root/nRF5_SDK_17.1.0_ddde560}"
key_file="${DFU_PRIVATE_KEY:-$repo_root/secrets/dfu_private.pem}"
mbr_hex="$sdk_root/components/softdevice/mbr/hex/mbr_nrf52_2.4.1_mbr.hex"
bootloader_hex="$repo_root/build/dfu_bootloader_dwm3001/armgcc/_build/nrf52833_xxaa_mbr.hex"
settings_hex="$out_dir/${role}_bootloader_settings.hex"
first_flash_hex="$out_dir/${role}_first_flash.hex"
package_zip="$out_dir/${role}_v${app_version}.zip"

mkdir -p "$out_dir"

if [[ ! -f "$bootloader_hex" ]]; then
  "$repo_root/tools/dfu/build_secure_bootloader_dwm3001.sh" "$sdk_root"
fi

"$repo_root/tools/dfu/make_dfu_package.sh" "$app_hex" "$key_file" "$package_zip" "$app_version"

nrfutil nrf5sdk-tools settings generate \
  --family NRF52 \
  --application "$app_hex" \
  --application-version "$app_version" \
  --bootloader-version 1 \
  --bl-settings-version 2 \
  --app-boot-validation VALIDATE_GENERATED_CRC \
  --no-backup \
  "$settings_hex"

mergehex -m "$mbr_hex" "$bootloader_hex" "$settings_hex" "$app_hex" -o "$first_flash_hex"

print "DFU package:      $package_zip"
print "Settings hex:     $settings_hex"
print "First-flash hex:  $first_flash_hex"