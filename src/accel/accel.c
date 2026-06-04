/*
 * accel.c - BMI323 accelerometer backend over SPI (blocking)
 *
 * The public API stays unchanged: accel_init() / accel_read().
 */

#include "accel.h"

#include <nrf.h>
#include <nrf_gpio.h>
#include <nrf_delay.h>

/* BMI323 wiring on DWM3001C (SPI 4-wire). */
#define BMI323_SCLK_PIN NRF_GPIO_PIN_MAP(0, 17)
#define BMI323_MOSI_PIN NRF_GPIO_PIN_MAP(0, 20)
#define BMI323_MISO_PIN NRF_GPIO_PIN_MAP(0, 21)
#define BMI323_CS_PIN   NRF_GPIO_PIN_MAP(0, 11)
#define BMI323_INT1_PIN NRF_GPIO_PIN_MAP(1, 8)
#define BMI323_INT2_PIN NRF_GPIO_PIN_MAP(0, 6)

#define BMI323_SPI NRF_SPIM2
#define BMI323_SPI_TIMEOUT 640000u

/* BMI323 register map (8-bit register addresses, 16-bit register values). */
#define BMI323_REG_CHIP_ID    0x00u
#define BMI323_REG_ERR_REG    0x01u
#define BMI323_REG_ACC_DATA_X 0x03u
#define BMI323_REG_ACC_CONF   0x20u

#define BMI323_CHIP_ID_EXPECTED 0x43u

/* Accel-only: normal mode, 8g, 50 Hz. */
#define BMI323_ACC_CONF_VALUE 0x4027u

/* ERR_REG bit0 = fatal error. */
#define BMI323_ERR_FATAL_MASK 0x0001u

/* SPI read decoding discovered at runtime from CHIP_ID transaction. */
static uint8_t g_read_addr_flag = 0x80u;
static uint8_t g_read_data_lsb_idx = 2u;
static uint8_t g_spi_cpol = SPIM_CONFIG_CPOL_ActiveHigh;
static uint8_t g_spi_cpha = SPIM_CONFIG_CPHA_Leading;
static uint8_t g_probe_rx[5] = { 0 };
static uint16_t g_chip_id_reg = 0u;
static uint16_t g_err_reg = 0u;
static bool g_probe_ok = false;
static bool g_use_bitbang = false;

static void bmi323_spi_set_mode(uint32_t cpol, uint32_t cpha)
{
    g_spi_cpol = (uint8_t)cpol;
    g_spi_cpha = (uint8_t)cpha;
    BMI323_SPI->CONFIG =
        (SPIM_CONFIG_ORDER_MsbFirst << SPIM_CONFIG_ORDER_Pos) |
        (cpol << SPIM_CONFIG_CPOL_Pos) |
        (cpha << SPIM_CONFIG_CPHA_Pos);
}

static void bmi323_spi_select_interface(void)
{
    nrf_gpio_pin_clear(BMI323_CS_PIN);
    nrf_delay_us(5u);
    nrf_gpio_pin_set(BMI323_CS_PIN);
    nrf_delay_us(250u);
}

static void bmi323_bitbang_init(void)
{
    NRF_SPIM2->ENABLE = 0;
    NRF_SPI2->ENABLE = 0;

    nrf_gpio_cfg_output(BMI323_CS_PIN);
    nrf_gpio_cfg_output(BMI323_SCLK_PIN);
    nrf_gpio_cfg_output(BMI323_MOSI_PIN);
    nrf_gpio_cfg_input(BMI323_MISO_PIN, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BMI323_INT1_PIN, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BMI323_INT2_PIN, NRF_GPIO_PIN_NOPULL);

    nrf_gpio_pin_clear(BMI323_SCLK_PIN);
    nrf_gpio_pin_set(BMI323_MOSI_PIN);
    nrf_gpio_pin_set(BMI323_CS_PIN);

    g_use_bitbang = true;
    g_spi_cpol = 0u;
    g_spi_cpha = 0u;

    bmi323_spi_select_interface();
}

static uint8_t bmi323_bitbang_byte(uint8_t tx)
{
    uint8_t rx = 0u;

    for (uint8_t bit = 0u; bit < 8u; bit++)
    {
        if ((tx & 0x80u) != 0u)
        {
            nrf_gpio_pin_set(BMI323_MOSI_PIN);
        }
        else
        {
            nrf_gpio_pin_clear(BMI323_MOSI_PIN);
        }

        nrf_delay_us(2u);
        nrf_gpio_pin_set(BMI323_SCLK_PIN);
        nrf_delay_us(2u);

        rx <<= 1;
        if (nrf_gpio_pin_read(BMI323_MISO_PIN) != 0u)
        {
            rx |= 1u;
        }

        nrf_gpio_pin_clear(BMI323_SCLK_PIN);
        nrf_delay_us(2u);
        tx <<= 1;
    }

    return rx;
}

static bool bmi323_bitbang_xfer(const uint8_t *tx, uint8_t *rx, uint8_t len)
{
    static uint8_t rx_sink[16];

    if ((tx == NULL) || (len == 0u) || (len > 16u))
    {
        return false;
    }

    if (rx == NULL)
    {
        rx = rx_sink;
    }

    for (uint8_t i = 0u; i < len; i++)
    {
        rx[i] = bmi323_bitbang_byte(tx[i]);
    }

    return true;
}

static void bmi323_spi_init(void)
{
    NRF_SPIM2->ENABLE = 0;
    NRF_SPI2->ENABLE = 0;
    g_use_bitbang = false;

    nrf_gpio_cfg_output(BMI323_CS_PIN);
    nrf_gpio_pin_set(BMI323_CS_PIN);

    nrf_gpio_cfg_input(BMI323_INT1_PIN, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BMI323_INT2_PIN, NRF_GPIO_PIN_NOPULL);

    BMI323_SPI->PSEL.SCK = BMI323_SCLK_PIN;
    BMI323_SPI->PSEL.MOSI = BMI323_MOSI_PIN;
    BMI323_SPI->PSEL.MISO = BMI323_MISO_PIN;

    BMI323_SPI->FREQUENCY = SPIM_FREQUENCY_FREQUENCY_M1;
    bmi323_spi_set_mode(SPIM_CONFIG_CPOL_ActiveHigh, SPIM_CONFIG_CPHA_Leading);
    BMI323_SPI->ORC = 0xFFu;
    BMI323_SPI->ENABLE = SPIM_ENABLE_ENABLE_Enabled;

    bmi323_spi_select_interface();
}

static bool bmi323_spi_xfer(const uint8_t *tx, uint8_t *rx, uint8_t len)
{
    static uint8_t rx_sink[16];
    volatile uint32_t timeout = BMI323_SPI_TIMEOUT;

    if (g_use_bitbang)
    {
        return bmi323_bitbang_xfer(tx, rx, len);
    }

    if ((len == 0u) || (len > 16u))
    {
        return false;
    }

    if (rx == NULL)
    {
        rx = rx_sink;
    }

    BMI323_SPI->TXD.PTR = (uint32_t)tx;
    BMI323_SPI->TXD.MAXCNT = len;
    BMI323_SPI->RXD.PTR = (uint32_t)rx;
    BMI323_SPI->RXD.MAXCNT = len;
    BMI323_SPI->EVENTS_END = 0;
    BMI323_SPI->EVENTS_STOPPED = 0;

    BMI323_SPI->TASKS_START = 1;
    while ((BMI323_SPI->EVENTS_END == 0u) && (--timeout > 0u)) { }

    BMI323_SPI->TASKS_STOP = 1;
    timeout = BMI323_SPI_TIMEOUT;
    while ((BMI323_SPI->EVENTS_STOPPED == 0u) && (--timeout > 0u)) { }
    BMI323_SPI->EVENTS_STOPPED = 0;

    return BMI323_SPI->EVENTS_END != 0u;
}

static bool bmi323_try_read_chip_id_mode(uint8_t addr_flag, uint8_t data_lsb_idx)
{
    uint8_t tx[5] = { 0 };
    uint8_t rx[5] = { 0 };
    uint16_t reg_word;

    tx[0] = (uint8_t)(BMI323_REG_CHIP_ID | addr_flag);
    tx[1] = 0xFFu;
    tx[2] = 0xFFu;
    tx[3] = 0xFFu;
    tx[4] = 0xFFu;

    nrf_gpio_pin_clear(BMI323_CS_PIN);
    nrf_delay_us(1u);
    if (!bmi323_spi_xfer(tx, rx, sizeof(tx)))
    {
        nrf_gpio_pin_set(BMI323_CS_PIN);
        nrf_delay_us(2u);
        return false;
    }
    nrf_delay_us(1u);
    nrf_gpio_pin_set(BMI323_CS_PIN);
    nrf_delay_us(2u);

    for (uint8_t i = 0u; i < sizeof(g_probe_rx); i++)
    {
        g_probe_rx[i] = rx[i];
    }

    if ((data_lsb_idx + 1u) >= sizeof(rx))
    {
        return false;
    }

    reg_word = (uint16_t)rx[data_lsb_idx] | ((uint16_t)rx[data_lsb_idx + 1u] << 8);
    g_chip_id_reg = reg_word;
    g_read_addr_flag = addr_flag;
    g_read_data_lsb_idx = data_lsb_idx;
    return (uint8_t)(reg_word & 0x00FFu) == BMI323_CHIP_ID_EXPECTED;
}

static bool bmi323_detect_read_mode(void)
{
    static const uint8_t addr_flags[] = { 0x80u, 0x00u };
    static const uint8_t data_idxs[] = { 1u, 2u, 3u };
    static const uint8_t cpols[] = {
        SPIM_CONFIG_CPOL_ActiveHigh,
        SPIM_CONFIG_CPOL_ActiveLow
    };
    static const uint8_t cphas[] = {
        SPIM_CONFIG_CPHA_Leading,
        SPIM_CONFIG_CPHA_Trailing
    };

    for (uint8_t m = 0u; m < (sizeof(cpols) / sizeof(cpols[0])); m++)
    {
        for (uint8_t n = 0u; n < (sizeof(cphas) / sizeof(cphas[0])); n++)
        {
            bmi323_spi_set_mode(cpols[m], cphas[n]);

            for (uint8_t i = 0u; i < (sizeof(addr_flags) / sizeof(addr_flags[0])); i++)
            {
                for (uint8_t j = 0u; j < (sizeof(data_idxs) / sizeof(data_idxs[0])); j++)
                {
                    if (bmi323_try_read_chip_id_mode(addr_flags[i], data_idxs[j]))
                    {
                        g_read_addr_flag = addr_flags[i];
                        g_read_data_lsb_idx = data_idxs[j];
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

static bool bmi323_read_reg16(uint8_t addr, uint16_t *value)
{
    uint8_t tx[5] = { 0 };
    uint8_t rx[5] = { 0 };

    if (value == NULL)
    {
        return false;
    }

    tx[0] = (uint8_t)(addr | g_read_addr_flag);
    tx[1] = 0xFFu;
    tx[2] = 0xFFu;
    tx[3] = 0xFFu;
    tx[4] = 0xFFu;

    nrf_gpio_pin_clear(BMI323_CS_PIN);
    nrf_delay_us(1u);
    if (!bmi323_spi_xfer(tx, rx, sizeof(tx)))
    {
        nrf_gpio_pin_set(BMI323_CS_PIN);
        nrf_delay_us(2u);
        return false;
    }
    nrf_delay_us(1u);
    nrf_gpio_pin_set(BMI323_CS_PIN);
    nrf_delay_us(2u);

    if ((g_read_data_lsb_idx + 1u) >= sizeof(rx))
    {
        return false;
    }

    *value = (uint16_t)rx[g_read_data_lsb_idx] |
             ((uint16_t)rx[g_read_data_lsb_idx + 1u] << 8);
    return true;
}

static bool bmi323_write_reg16(uint8_t addr, uint16_t value)
{
    uint8_t tx[3];

    tx[0] = (uint8_t)(addr & 0x7Fu);
    tx[1] = (uint8_t)(value & 0x00FFu);
    tx[2] = (uint8_t)((value >> 8) & 0x00FFu);

    nrf_gpio_pin_clear(BMI323_CS_PIN);
    nrf_delay_us(1u);
    if (!bmi323_spi_xfer(tx, NULL, sizeof(tx)))
    {
        nrf_gpio_pin_set(BMI323_CS_PIN);
        nrf_delay_us(2u);
        return false;
    }
    nrf_delay_us(1u);
    nrf_gpio_pin_set(BMI323_CS_PIN);
    nrf_delay_us(2u);
    return true;
}

static int16_t bmi323_raw_to_mg(int16_t raw)
{
    /* ACC_CONF sets range to +/-8g. Signed 16-bit full-scale => 8000 mg / 32768 LSB. */
    int32_t num = (int32_t)raw * 8000;

    if (num >= 0)
    {
        return (int16_t)((num + 16384) / 32768);
    }
    return (int16_t)((num - 16384) / 32768);
}

bool accel_init(void)
{
    uint16_t chip_id_reg = 0u;
    uint16_t err_reg;

    g_probe_ok = false;
    g_chip_id_reg = 0u;
    g_err_reg = 0u;
    for (uint8_t i = 0u; i < sizeof(g_probe_rx); i++)
    {
        g_probe_rx[i] = 0u;
    }

    bmi323_spi_init();
    nrf_delay_ms(2u);

    if (!bmi323_detect_read_mode())
    {
        bmi323_bitbang_init();
        nrf_delay_ms(2u);

        if (!bmi323_detect_read_mode())
        {
            return false;
        }
    }

    if (!bmi323_read_reg16(BMI323_REG_CHIP_ID, &chip_id_reg))
    {
        return false;
    }
    g_chip_id_reg = chip_id_reg;

    if (!bmi323_write_reg16(BMI323_REG_ACC_CONF, BMI323_ACC_CONF_VALUE))
    {
        return false;
    }

    nrf_delay_ms(5u);

    if (!bmi323_read_reg16(BMI323_REG_ERR_REG, &err_reg))
    {
        return false;
    }
    g_err_reg = err_reg;

    if ((err_reg & BMI323_ERR_FATAL_MASK) != 0u)
    {
        return false;
    }

    g_probe_ok = ((uint8_t)(chip_id_reg & 0x00FFu) == BMI323_CHIP_ID_EXPECTED);
    return true;
}

bool accel_read(accel_data_t *data)
{
    uint16_t raw_x;
    uint16_t raw_y;
    uint16_t raw_z;
    int16_t x;
    int16_t y;
    int16_t z;

    if (data == NULL)
    {
        return false;
    }

    if (!bmi323_read_reg16(BMI323_REG_ACC_DATA_X, &raw_x))
    {
        return false;
    }
    if (!bmi323_read_reg16((uint8_t)(BMI323_REG_ACC_DATA_X + 1u), &raw_y))
    {
        return false;
    }
    if (!bmi323_read_reg16((uint8_t)(BMI323_REG_ACC_DATA_X + 2u), &raw_z))
    {
        return false;
    }

    if ((raw_x == 0x8000u) || (raw_y == 0x8000u) || (raw_z == 0x8000u))
    {
        return false;
    }

    x = (int16_t)raw_x;
    y = (int16_t)raw_y;
    z = (int16_t)raw_z;

    data->x = bmi323_raw_to_mg(x);
    data->y = bmi323_raw_to_mg(y);
    data->z = bmi323_raw_to_mg(z);

    return true;
}

bool accel_get_diag(accel_diag_t *diag)
{
    if (diag == NULL)
    {
        return false;
    }

    diag->chip_id_reg = g_chip_id_reg;
    diag->err_reg = g_err_reg;
    diag->read_addr_flag = g_read_addr_flag;
    diag->read_data_lsb_idx = g_read_data_lsb_idx;
    diag->spi_cpol = g_spi_cpol;
    diag->spi_cpha = g_spi_cpha;
    for (uint8_t i = 0u; i < sizeof(diag->probe_rx); i++)
    {
        diag->probe_rx[i] = g_probe_rx[i];
    }
    diag->bitbang = g_use_bitbang;
    diag->probe_ok = g_probe_ok;
    return true;
}