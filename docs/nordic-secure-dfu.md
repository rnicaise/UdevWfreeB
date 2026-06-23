# Nordic Secure DFU over UART

This project can move to Nordic Secure DFU so field firmware updates no longer require J-Link after the initial bootloader installation.

## Current Status

Prepared in this repo:

- `nrfutil nrf5sdk-tools` is the expected package/upload tool.
- The local nRF5 SDK extraction is expected at `nRF5_SDK_17.1.0_ddde560/` unless `NRF5_SDK_ROOT` is set.
- DFU app build presets are available:
  - `initiator_gena_dfu_debug`
  - `responder_gena_dfu_debug`
- A DWM3001/nRF52833 Secure Serial UART bootloader build helper is available.
- The firmware accepts `BOOT,DFU` over the application UART, sets Nordic GPREGRET `0xB1`, and resets into the bootloader.
- DFU helper scripts are available under `tools/dfu/`.

Still required before first use:

- Flash MBR + bootloader + bootloader settings + first DFU-linked app once via J-Link/SWD.

After that initial SWD install, application updates can be sent through UART.

## Validation Status

Validated on 2026-06-23:

- Initiator GenA first install over J-Link/SWD on probe `802009545`.
- First-flash image: `build/dfu_packages/initiator_gena_first_flash.hex`.
- Application boot after first install: `ROLE,INITIATOR` and CSV output confirmed at `460800` baud.
- Application-to-bootloader handoff: `BOOT,DFU` returns `ACK,BOOT_DFU`.
- UART DFU update at bootloader baud `115200` completed successfully with signed initiator packages.
- Application boot after UART DFU: `ROLE,INITIATOR` and CSV output confirmed.

Long-term TODO:

- Integrate the same external UART access on the responder hardware before validating responder UART DFU.
- Required responder UART signals are board TX `P0.19`, board RX `P0.15`, and common GND; CTS/RTS are not used.
- Responder DFU artifacts are already generated, but responder field updates cannot be tested until that UART is physically available.

## Memory Layout

The normal firmware is linked at `0x00000000` and is not bootloader-compatible.

DFU app presets enable `UWB_DFU_APP_LAYOUT=ON`, which currently uses:

```text
MBR:                  0x00000000..0x00000FFF
Application start:    0x00001000
Reserved BL start:    0x00078000
MBR params page:      0x0007E000
Settings page:        normally last flash page, 0x0007F000
```

`UWB_DFU_BOOTLOADER_START` is configurable from CMake if the final bootloader build needs a different start address.

## Build a DFU-Linked Application

```sh
cmake --preset=initiator_gena_dfu_debug
cmake --build --preset initiator_gena_dfu_debug

cmake --preset=responder_gena_dfu_debug
cmake --build --preset responder_gena_dfu_debug
```

Example output files:

```text
build/initiator_gena_dfu_debug/uwb_initiator_gena.hex
build/responder_gena_dfu_debug/uwb_responder_gena.hex
```

## Generate Signing Key

Keep the private key out of Git.

```sh
tools/dfu/generate_dfu_key.sh secrets/dfu_private.pem
```

`secrets/` is ignored by Git. The bootloader build helper also generates `secrets/dfu_private.pem` automatically if it is missing, then compiles the derived public key into the bootloader.

## Build the Secure UART Bootloader

```sh
tools/dfu/build_secure_bootloader_dwm3001.sh
```

Default DWM3001 UART pins:

- TX: P0.19
- RX: P0.15
- HWFC: disabled
- DFU baud: Nordic default `115200`

Output:

```text
build/dfu_bootloader_dwm3001/armgcc/_build/nrf52833_xxaa_mbr.hex
```

## Generate an Application DFU Package

```sh
tools/dfu/make_dfu_package.sh \
  build/initiator_gena_dfu_debug/uwb_initiator_gena.hex \
  secrets/dfu_private.pem \
  build/dfu_packages/initiator_gena_v1.zip \
  1
```

Defaults:

- `DFU_HW_VERSION=52`
- `DFU_SD_REQ=0x00` for no SoftDevice
- `DFU_APP_BOOT_VALIDATION=VALIDATE_GENERATED_CRC`

## Upload over UART

The upload wrapper sends the application bootloader command automatically by default:

```sh
tools/dfu/serial_dfu_upload.sh build/dfu_packages/initiator_gena_v1.zip /dev/cu.usbserial-0001 115200
```

It talks to the running app at `DFU_APP_BAUD=460800`, sends `BOOT,DFU` until `ACK,BOOT_DFU`, then starts Nordic serial DFU at the bootloader baud. Set `DFU_SEND_BOOT_CMD=0` if the target is already in bootloader mode.

Manual bootloader entry command:

```text
BOOT,DFU
```

The app replies `ACK,BOOT_DFU`, flushes UART TX, sets GPREGRET to `0xB1`, and resets. The bootloader then waits for serial DFU.

If the bootloader is configured for the current project baud rate, use `460800` instead.

The upload wrapper uses `DFU_CONNECT_DELAY=0` by default. In testing, the default Nordic delay could miss the short post-reset serial bootloader ping window on this UART adapter.

## First-Time J-Link Install

The first install still requires SWD/J-Link because there is no bootloader on the boards yet.

Typical one-time image set:

```text
MBR + secure bootloader + bootloader settings + uwb_*_dfu_debug.hex
```

Generate the DFU package, settings, and merged first-flash image after the app hex exists:

```sh
tools/dfu/make_first_flash_hex.sh \
  initiator_gena \
  build/initiator_gena_dfu_debug/uwb_initiator_gena.hex \
  build/dfu_packages \
  1
```

Current generated examples:

```text
build/dfu_packages/initiator_gena_first_flash.hex
build/dfu_packages/responder_gena_first_flash.hex
build/dfu_packages/initiator_gena_v1.zip
build/dfu_packages/responder_gena_v1.zip
```

Flash one role once with J-Link/SWD:

```sh
nrfjprog -f NRF52 --program build/dfu_packages/initiator_gena_first_flash.hex --sectorerase --verify --reset
```

The settings generator uses `--no-backup` because `0x0007E000` is reserved for the bootloader MBR params page in this layout.

## Notes

- nRF52833 has no native USB device like nRF52840, so UART serial DFU is the practical transport here.
- Do not flash a DFU-linked app without MBR/bootloader installed; it starts at `0x1000`, not `0x0`.
- Do not use the normal app hex as a DFU package input once the bootloader is installed; use the `*_dfu_debug` app hex.
- Keep `secrets/dfu_private.pem`; replacing it requires reflashing a bootloader built with the new public key.
