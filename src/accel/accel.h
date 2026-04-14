/*
 * accel.h — LIS2DH12 accelerometer driver (minimal, blocking)
 *
 * Le DWM3001C module intègre un LIS2DH12 connecté en I2C interne.
 * Ce driver utilise le TWIM0 du nRF52833 directement (pas de SDK nrf_twi).
 */

#ifndef ACCEL_H
#define ACCEL_H

#include <stdint.h>
#include <stdbool.h>

/* Données accéléromètre (raw, ±2g, 10 bits left-justified → mg) */
typedef struct {
    int16_t x;  /* mg */
    int16_t y;  /* mg */
    int16_t z;  /* mg */
} accel_data_t;

/* Init le TWIM et configure le LIS2DH12. Retourne true si WHO_AM_I OK. */
bool accel_init(void);

/* Lit XYZ en une seule lecture multi-byte. */
bool accel_read(accel_data_t *data);

#endif /* ACCEL_H */
