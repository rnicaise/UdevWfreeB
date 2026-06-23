#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -lt 1 ]]; then
  print "Usage: $0 <private-key.pem>" >&2
  print "Example: $0 secrets/dfu_private.pem" >&2
  exit 2
fi

key_file="$1"
mkdir -p "$(dirname "$key_file")"

if [[ -e "$key_file" ]]; then
  print "Refusing to overwrite existing key: $key_file" >&2
  exit 1
fi

nrfutil nrf5sdk-tools keys generate "$key_file"
nrfutil nrf5sdk-tools keys display --key pk --format code "$key_file"
