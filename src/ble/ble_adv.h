/*
 * ble_adv.h - BLE advertising via RADIO peripheral (no SoftDevice)
 *
 * Broadcasts UWB distance in BLE advertising packets
 * (ADV_NONCONN_IND), readable by any BLE scanner
 * (e.g. nRF Connect on smartphone).
 *
 * Manufacturer Specific Data format (Company ID 0xFFFF = test):
 *   Byte 0-1 : distance in centimeters (int16_t, little-endian)
 *   Byte 2-3 : measurement counter (uint16_t, little-endian)
 */

#ifndef BLE_ADV_H
#define BLE_ADV_H

#include <stdint.h>

/* Initialize RADIO for BLE advertising */
void ble_adv_init(void);

/* Update distance to broadcast (called after each ranging cycle) */
void ble_adv_update(float distance_m, uint32_t count);

/* Send one advertising packet on BLE channels 37, 38, and 39 */
void ble_adv_send(void);

#endif /* BLE_ADV_H */
