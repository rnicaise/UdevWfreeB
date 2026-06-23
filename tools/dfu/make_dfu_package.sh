#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -lt 3 ]]; then
  print "Usage: $0 <application.hex> <signing-key.pem> <output.zip> [app-version]" >&2
  print "Example: $0 build/initiator_gena_dfu_debug/uwb_initiator_gena.hex secrets/dfu_private.pem out/initiator.zip 1" >&2
  exit 2
fi

app_hex="$1"
key_file="$2"
out_zip="$3"
app_version="${4:-1}"
hw_version="${DFU_HW_VERSION:-52}"
sd_req="${DFU_SD_REQ:-0x00}"
validation="${DFU_APP_BOOT_VALIDATION:-VALIDATE_GENERATED_CRC}"

nrfutil nrf5sdk-tools pkg generate \
  --application "$app_hex" \
  --application-version "$app_version" \
  --hw-version "$hw_version" \
  --sd-req "$sd_req" \
  --app-boot-validation "$validation" \
  --key-file "$key_file" \
  "$out_zip"

print "DFU package written: $out_zip"
