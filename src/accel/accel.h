/*
 * accel.h - LIS2DH12 accelerometer driver (minimal, blocking)
 *
 * The DWM3001C module integrates a LIS2DH12 on an internal I2C bus.
 * This driver uses nRF52833 TWIM0 directly (no nrf_twi SDK dependency).
 */

#ifndef ACCEL_H
#define ACCEL_H

#include <stdint.h>
#include <stdbool.h>

/* Accelerometer data (raw, +/-2g, 10-bit left-justified -> mg) */
typedef struct {
    int16_t x;  /* mg */
    int16_t y;  /* mg */
    int16_t z;  /* mg */
} accel_data_t;

/* Initialize TWIM and configure LIS2DH12. Returns true if WHO_AM_I is valid. */
bool accel_init(void);

/* Read XYZ using a single multi-byte transaction. */
bool accel_read(accel_data_t *data);

#endif /* ACCEL_H */
