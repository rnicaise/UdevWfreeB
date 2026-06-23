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
