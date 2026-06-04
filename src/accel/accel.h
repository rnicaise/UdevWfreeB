/*
 * accel.h - external BMI323 accelerometer interface (SPI)
 *
 * The firmware exposes a tiny backend-agnostic API:
 *   accel_init() and accel_read().
 *
 * This implementation is wired to an external BMI323 in SPI mode
 * and returns acceleration values in mg.
 */

#ifndef ACCEL_H
#define ACCEL_H

#include <stdint.h>
#include <stdbool.h>

/* Accelerometer data in milligravity (mg). */
typedef struct {
    int16_t x;  /* mg */
    int16_t y;  /* mg */
    int16_t z;  /* mg */
} accel_data_t;

typedef struct {
    uint16_t chip_id_reg;
    uint16_t err_reg;
    uint8_t read_addr_flag;
    uint8_t read_data_lsb_idx;
    uint8_t spi_cpol;
    uint8_t spi_cpha;
    uint8_t probe_rx[5];
    bool bitbang;
    bool probe_ok;
} accel_diag_t;

/* Initialize the external accelerometer backend. */
bool accel_init(void);

/* Read XYZ acceleration in mg. */
bool accel_read(accel_data_t *data);

/* Read latest backend diagnostics (for boot-time debug logs). */
bool accel_get_diag(accel_diag_t *diag);

#endif /* ACCEL_H */
