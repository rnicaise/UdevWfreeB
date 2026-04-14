/*
 * accel.c — LIS2DH12 driver via TWIM0 (minimal, blocking, no SDK TWI)
 *
 * Utilise directement les registres TWIM0 du nRF52833.
 * Pas de dépendance à nrf_twi_sensor / nrf_drv_twi.
 *
 * Pins I2C internes du module DWM3001C :
 *   SDA = P0.16   SCL = P0.13   (vérifiable via WHO_AM_I au boot)
 *
 * NOTE: TWIM0 et SPIM0/SPIS0/TWIS0 partagent le même bloc périphérique
 * (0x40003000). On désactive SPIM0 avant d'activer TWIM0.
 */

#include "accel.h"
#include <nrf.h>
#include <nrf_gpio.h>
#include <string.h>

/* ── DWM3001C module I2C pins (from datasheet Rev F, Table 2) ── */
#define ACC_SDA_PIN     NRF_GPIO_PIN_MAP(0, 24)   /* I2C0_SDA, pin 14 */
#define ACC_SCL_PIN     NRF_GPIO_PIN_MAP(1,  4)   /* I2C0_SCL, pin 15 */

/* ── LIS2DH12 I2C address ── */
/* SA0=HIGH → 0x19, SA0=LOW → 0x18. We try both. */
#define LIS2DH12_ADDR_HIGH  0x19
#define LIS2DH12_ADDR_LOW   0x18

/* ── LIS2DH12 registers ── */
#define LIS2DH12_WHO_AM_I       0x0F
#define LIS2DH12_WHO_AM_I_VAL   0x33
#define LIS2DH12_CTRL_REG1      0x20
#define LIS2DH12_CTRL_REG4      0x23
#define LIS2DH12_OUT_X_L        0x28

/* ── TWIM0 instance ── */
#define TWI  NRF_TWIM0

/* ── Timeout for TWI operations (~10ms at 64MHz) ── */
#define TWI_TIMEOUT  640000

/* ── Internal helpers ── */

static void twim_init(void)
{
    /* Disable SPIM0 which shares the same peripheral block as TWIM0 */
    NRF_SPIM0->ENABLE = 0;
    NRF_SPI0->ENABLE  = 0;

    /* Configure GPIO for I2C */
    nrf_gpio_cfg(ACC_SCL_PIN,
        NRF_GPIO_PIN_DIR_INPUT,
        NRF_GPIO_PIN_INPUT_CONNECT,
        NRF_GPIO_PIN_PULLUP,
        NRF_GPIO_PIN_S0D1,   /* Standard 0, Disconnect 1 (open-drain) */
        NRF_GPIO_PIN_NOSENSE);

    nrf_gpio_cfg(ACC_SDA_PIN,
        NRF_GPIO_PIN_DIR_INPUT,
        NRF_GPIO_PIN_INPUT_CONNECT,
        NRF_GPIO_PIN_PULLUP,
        NRF_GPIO_PIN_S0D1,
        NRF_GPIO_PIN_NOSENSE);

    /* Configure TWIM0 */
    TWI->PSEL.SCL = ACC_SCL_PIN;
    TWI->PSEL.SDA = ACC_SDA_PIN;
    TWI->ADDRESS  = LIS2DH12_ADDR_HIGH;  /* will try LOW if WHO_AM_I fails */
    TWI->FREQUENCY = TWIM_FREQUENCY_FREQUENCY_K100;  /* 100 kHz (conservative) */

    /* Enable */
    TWI->ENABLE = TWIM_ENABLE_ENABLE_Enabled;
}

static bool twi_write(const uint8_t *data, uint8_t len)
{
    volatile uint32_t timeout = TWI_TIMEOUT;

    TWI->TXD.PTR    = (uint32_t)data;
    TWI->TXD.MAXCNT = len;

    TWI->EVENTS_STOPPED = 0;
    TWI->EVENTS_ERROR   = 0;

    /* TX only, then STOP */
    TWI->SHORTS = TWIM_SHORTS_LASTTX_STOP_Msk;
    TWI->TASKS_STARTTX = 1;

    while (!TWI->EVENTS_STOPPED && !TWI->EVENTS_ERROR && --timeout) { }

    if (!timeout || TWI->EVENTS_ERROR) {
        TWI->EVENTS_ERROR = 0;
        TWI->TASKS_STOP = 1;
        timeout = TWI_TIMEOUT;
        while (!TWI->EVENTS_STOPPED && --timeout) { }
        TWI->EVENTS_STOPPED = 0;
        return false;
    }

    TWI->EVENTS_STOPPED = 0;
    return true;
}

static bool twi_write_then_read(const uint8_t *tx, uint8_t tx_len,
                                 uint8_t *rx, uint8_t rx_len)
{
    volatile uint32_t timeout = TWI_TIMEOUT;

    TWI->TXD.PTR    = (uint32_t)tx;
    TWI->TXD.MAXCNT = tx_len;
    TWI->RXD.PTR    = (uint32_t)rx;
    TWI->RXD.MAXCNT = rx_len;

    TWI->EVENTS_STOPPED = 0;
    TWI->EVENTS_ERROR   = 0;

    /* TX then RX then STOP */
    TWI->SHORTS = TWIM_SHORTS_LASTTX_STARTRX_Msk | TWIM_SHORTS_LASTRX_STOP_Msk;
    TWI->TASKS_STARTTX = 1;

    while (!TWI->EVENTS_STOPPED && !TWI->EVENTS_ERROR && --timeout) { }

    if (!timeout || TWI->EVENTS_ERROR) {
        TWI->EVENTS_ERROR = 0;
        TWI->TASKS_STOP = 1;
        timeout = TWI_TIMEOUT;
        while (!TWI->EVENTS_STOPPED && --timeout) { }
        TWI->EVENTS_STOPPED = 0;
        return false;
    }

    TWI->EVENTS_STOPPED = 0;
    return true;
}

static bool lis2dh12_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return twi_write(buf, 2);
}

static bool lis2dh12_read_reg(uint8_t reg, uint8_t *val)
{
    return twi_write_then_read(&reg, 1, val, 1);
}

/* ── Public API ── */

bool accel_init(void)
{
    twim_init();

    /* Try address 0x19 first, then 0x18 */
    uint8_t who = 0;
    if (!lis2dh12_read_reg(LIS2DH12_WHO_AM_I, &who) || who != LIS2DH12_WHO_AM_I_VAL) {
        /* Try alternate address */
        TWI->ENABLE = 0;
        TWI->ADDRESS = LIS2DH12_ADDR_LOW;
        TWI->ENABLE = TWIM_ENABLE_ENABLE_Enabled;
        who = 0;
        if (!lis2dh12_read_reg(LIS2DH12_WHO_AM_I, &who) || who != LIS2DH12_WHO_AM_I_VAL) {
            return false;
        }
    }

    /* CTRL_REG1: 100 Hz, normal mode, XYZ enabled */
    lis2dh12_write_reg(LIS2DH12_CTRL_REG1, 0x57);  /* ODR=100Hz, LP=0, XYZ=en */

    /* CTRL_REG4: ±2g, high-resolution */
    lis2dh12_write_reg(LIS2DH12_CTRL_REG4, 0x08);  /* BDU=0, FS=±2g, HR=1 */

    return true;
}

bool accel_read(accel_data_t *data)
{
    uint8_t raw[6];
    /* Auto-increment read: set MSB of register address */
    uint8_t reg = LIS2DH12_OUT_X_L | 0x80;

    if (!twi_write_then_read(&reg, 1, raw, 6)) {
        return false;
    }

    /* Raw values are 12-bit left-justified in 16-bit (high-resolution mode)
     * Sensitivity at ±2g HR = 1 mg/digit (after >>4)
     */
    int16_t raw_x = (int16_t)(raw[1] << 8 | raw[0]) >> 4;
    int16_t raw_y = (int16_t)(raw[3] << 8 | raw[2]) >> 4;
    int16_t raw_z = (int16_t)(raw[5] << 8 | raw[4]) >> 4;

    data->x = raw_x;  /* Already in mg at ±2g HR */
    data->y = raw_y;
    data->z = raw_z;

    return true;
}
