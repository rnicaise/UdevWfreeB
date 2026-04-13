/*
 * ble_adv.h — BLE Advertising via RADIO peripheral (no SoftDevice)
 *
 * Diffuse la distance UWB dans des paquets BLE advertising
 * (ADV_NONCONN_IND) lisibles par n'importe quel scanner BLE
 * (ex: nRF Connect sur smartphone).
 *
 * Format Manufacturer Specific Data (Company ID 0xFFFF = test) :
 *   Byte 0-1 : distance en centimètres (int16_t, little-endian)
 *   Byte 2-3 : compteur de mesures (uint16_t, little-endian)
 */

#ifndef BLE_ADV_H
#define BLE_ADV_H

#include <stdint.h>

/* Initialise le RADIO pour BLE advertising */
void ble_adv_init(void);

/* Met à jour la distance à diffuser (appelé après chaque ranging) */
void ble_adv_update(float distance_m, uint32_t count);

/* Envoie un paquet advertising sur les 3 canaux BLE (37, 38, 39) */
void ble_adv_send(void);

#endif /* BLE_ADV_H */
