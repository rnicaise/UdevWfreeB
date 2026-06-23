# BLE product connectivity notes

## Current experiment: BLE advertising scan

The `initiator_gena_ble_adv_debug` preset enables a low-risk BLE scan experiment without SoftDevice. The initiator keeps the normal UWB ranging loop and broadcasts one non-connectable advertising packet about every 100 ms.

Advertising payload:

- Device name: `UWB`
- Manufacturer data company ID: `0xFFFF` for development
- Data byte 0-1: smoothed distance in centimeters, `int16_t`, little-endian
- Data byte 2-3: ranging sample counter, `uint16_t`, little-endian

Build:

```sh
cmake --preset initiator_gena_ble_adv_debug
cmake --build --preset initiator_gena_ble_adv_debug
```

DFU-compatible build for a board that already has the Secure DFU bootloader:

```sh
cmake --preset initiator_gena_ble_adv_dfu_debug
cmake --build --preset initiator_gena_ble_adv_dfu_debug
```

Test with nRF Connect:

1. Flash `build/initiator_gena_ble_adv_debug/uwb_initiator_gena.hex` directly, or package `build/initiator_gena_ble_adv_dfu_debug/uwb_initiator_gena.hex` for UART DFU.
2. Open nRF Connect mobile and scan.
3. Look for the `UWB` advertiser.
4. Inspect manufacturer data and decode the first two bytes as signed centimeters.

This is intentionally not connectable. It proves phone visibility and RF coexistence before moving the firmware onto a SoftDevice memory layout.

## Android app scan mode

The Android receiver app has a `BLE` control next to the USB control. It scans for the `UWB` advertiser and decodes manufacturer data `0xFFFF` directly into the normal distance pipeline.

Decoded packet fields:

- byte 0-1: signed distance in centimeters, little-endian
- byte 2-3: unsigned sample counter, little-endian
- scan RSSI is shown as the radio RSSI field

With `BLE_ADV` firmware this mode is monitor-only because the advertiser is non-connectable. The Android app is also prepared for the GATT firmware path: if the `UWB` peripheral is connectable and exposes Nordic UART Service, the app switches to `BLE GATT` and sends the same command strings used over USB.

BLE command service expected by the Android app:

- Service UUID: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- Phone-to-device write characteristic: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- Device-to-phone notify characteristic: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`
- Commands: `PYRO,FIRE`, `PYRO,FIRE_NOW`, `CFG,GET_ROLE`, plus future config commands

FIRE and arming controls are enabled only for USB or `BLE GATT`. Plain `BLE` advertising remains read-only.

## Firmware prototype: Nordic UART Service

The `initiator_gena_ble_gatt_debug` preset builds a first connectable BLE command prototype on nRF52833 with SoftDevice S113 and Nordic UART Service.

Build:

```sh
cmake --preset initiator_gena_ble_gatt_debug
cmake --build --preset initiator_gena_ble_gatt_debug
```

Runtime behavior:

- Device name: `UWB`
- SoftDevice: S113, app flash start `0x0001C000`, app RAM start `0x20002608`
- Service UUID: Nordic UART Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX/write accepts newline-terminated command strings such as `CFG,GET_ROLE`, `PYRO,FIRE`, and `PYRO,FIRE_NOW`
- TX/notify mirrors the existing UART line stream, including role/status responses and CSV distance rows

This is not yet a UART Secure DFU package path. First hardware validation should use J-Link/SWD to program S113 plus the app, or a SoftDevice-aware bootloader/settings flow. The existing non-SoftDevice UART DFU and raw BLE advertising builds remain separate.

## Product direction: connectable GATT

The clean product path should use Nordic SoftDevice S113 or S132 and expose a custom `UWB Ranging` GATT service. The first useful service can publish a notification characteristic at 10-20 Hz with:

- timestamp in milliseconds
- raw distance in millimeters
- filtered or smoothed distance in millimeters
- validity flag
- quality score or NLOS score
- sample counter

Expected implications:

- application flash no longer starts at `0x00001000`; it must start after the SoftDevice
- DFU packages must include the SoftDevice-aware application layout
- UART DFU bootloader must be rebuilt or configured for the selected SoftDevice
- UWB ranging should continue at the internal rate, with BLE notifications decimated
