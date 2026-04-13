/*
 * deca_spi_dwm3001cdk.c — SPI access functions for DWM3001CDK (single DW3000)
 *
 * Based on the Qorvo SDK deca_spi.c but stripped of SPI2 support
 * (the DWM3001CDK only has one DW3000/DW3110).
 *
 * Changes vs SDK original:
 *   - Removed spi2_init() and all SPI2-related code
 *   - Removed change_SPI() (only one SPI host)
 *   - No P0.07 / ARDUINO_2/3/4/5 GPIO configuration
 */

#include "deca_spi.h"
#include "port.h"
#include <deca_device_api.h>

static spi_handle_t spi1_handler;
static spi_handle_t *pgSpiHandler = &spi1_handler;

uint16_t current_cs_pin  = DW3000_CS_Pin;
uint16_t current_irq_pin = DW3000_IRQn_Pin;

dw_t s1 = {
    .irqPin  = DW3000_IRQn_Pin,
    .rstPin  = DW3000_RESET_Pin,
    .wkupPin = DW3000_WAKEUP_Pin,
    .csPin   = DW3000_CS_Pin,
    .pSpi    = &spi1_handler,
};

const dw_t *SPI1 = &s1;

/* SPI2 stub — keep the symbol so port.h / deca_spi.h references compile */
static dw_t s2_stub = {0};
const dw_t *SPI2 = &s2_stub;

static volatile bool spi_xfer_done;

static uint8_t idatabuf[DATALEN1] = {0};
static uint8_t itempbuf[DATALEN1] = {0};

/***************************************************************************
 *                              SPI init
 ***************************************************************************/

void change_SPI(host_using_spi_e spi)
{
    (void)spi;
    /* DWM3001CDK: single DW3000, always SPI1 */
    pgSpiHandler   = &spi1_handler;
    current_cs_pin  = DW3000_CS_Pin;
    current_irq_pin = DW3000_IRQn_Pin;
}

void spi2_init(void)
{
    /* No-op: DWM3001CDK has no second DW3000 */
}

void nrf52840_dk_spi_init(void)
{
    nrf_drv_spi_t        *spi_inst;
    nrf_drv_spi_config_t *spi_config;

    spi_handle_t *pSPI1 = SPI1->pSpi;

    pSPI1->frequency_slow = NRF_SPIM_FREQ_4M;
    pSPI1->frequency_fast = NRF_SPIM_FREQ_32M;
    pSPI1->lock = DW_HAL_NODE_UNLOCKED;

    spi_inst   = &pSPI1->spi_inst;
    spi_config = &pSPI1->spi_config;

    spi_inst->inst_idx       = SPI3_INSTANCE_INDEX;
    spi_inst->use_easy_dma   = SPI3_USE_EASY_DMA;
    spi_inst->u.spim.p_reg   = NRF_SPIM3;
    spi_inst->u.spim.drv_inst_idx = NRFX_SPIM3_INST_IDX;

    spi_config->sck_pin      = DW3000_CLK_Pin;
    spi_config->mosi_pin     = DW3000_MOSI_Pin;
    spi_config->miso_pin     = DW3000_MISO_Pin;
    spi_config->ss_pin       = NRF_DRV_SPI_PIN_NOT_USED;
    spi_config->irq_priority = (APP_IRQ_PRIORITY_MID - 2);
    spi_config->orc          = 0xFF;
    spi_config->frequency    = NRF_SPIM_FREQ_4M;
    spi_config->mode         = NRF_DRV_SPI_MODE_0;
    spi_config->bit_order    = NRF_DRV_SPI_BIT_ORDER_MSB_FIRST;

    /* CS as GPIOTE output toggle (active low, starts high) */
    nrf_drv_gpiote_out_config_t out_config =
        NRFX_GPIOTE_CONFIG_OUT_TASK_TOGGLE(NRF_GPIOTE_INITIAL_VALUE_HIGH);
    nrf_drv_gpiote_out_init(DW3000_CS_Pin, &out_config);

    /* No spi2_init() — DWM3001CDK has only one DW3000 */
}

/***************************************************************************
 *                         SPI speed control
 ***************************************************************************/

static int openspi(nrf_drv_spi_t *p_instance)
{
    NRF_SPIM_Type *p_spi = p_instance->u.spim.p_reg;
    nrf_spim_enable(p_spi);
    return 0;
}

static int closespi(nrf_drv_spi_t *p_instance)
{
    NRF_SPIM_Type *p_spi = p_instance->u.spim.p_reg;
    nrf_spim_disable(p_spi);
    return 0;
}

void spi_event_handler(nrf_drv_spi_evt_t const *p_event, void *p_context)
{
    UNUSED_PARAMETER(p_event);
    UNUSED_PARAMETER(p_context);
    spi_xfer_done = true;
}

void port_set_dw_ic_spi_slowrate(void)
{
    nrf_drv_spi_uninit(&pgSpiHandler->spi_inst);

    pgSpiHandler->spi_config.frequency = pgSpiHandler->frequency_slow;

    APP_ERROR_CHECK(nrf_drv_spi_init(&pgSpiHandler->spi_inst,
                                     &pgSpiHandler->spi_config,
                                     NULL, NULL));

    nrf_delay_ms(2);
}

void port_set_dw_ic_spi_fastrate(void)
{
    nrf_drv_spi_uninit(&pgSpiHandler->spi_inst);

    pgSpiHandler->spi_config.frequency = pgSpiHandler->frequency_fast;

    APP_ERROR_CHECK(nrf_drv_spi_init(&pgSpiHandler->spi_inst,
                                     &pgSpiHandler->spi_config,
                                     NULL, NULL));

    nrf_gpio_cfg(pgSpiHandler->spi_config.sck_pin,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_CONNECT,
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_H0H1,
                 NRF_GPIO_PIN_NOSENSE);
    nrf_gpio_cfg(pgSpiHandler->spi_config.mosi_pin,
                 NRF_GPIO_PIN_DIR_OUTPUT,
                 NRF_GPIO_PIN_INPUT_DISCONNECT,
                 NRF_GPIO_PIN_NOPULL,
                 NRF_GPIO_PIN_H0H1,
                 NRF_GPIO_PIN_NOSENSE);

    nrf_delay_ms(2);
}

/***************************************************************************
 *                         SPI read/write
 ***************************************************************************/

int32_t writetospiwithcrc(uint16_t headerLength, const uint8_t *headerBuffer,
                          uint16_t bodyLength, const uint8_t *bodyBuffer, uint8_t crc8)
{
#ifdef DWT_ENABLE_CRC
    uint8_t *p1;
    uint32_t idatalength = headerLength + bodyLength + sizeof(crc8);

    if (idatalength > DATALEN1)
        return NRF_ERROR_NO_MEM;

    while (pgSpiHandler->lock)
        ;
    __HAL_LOCK(pgSpiHandler);

    openspi(&pgSpiHandler->spi_inst);

    p1 = idatabuf;
    memcpy(p1, headerBuffer, headerLength);
    p1 += headerLength;
    memcpy(p1, bodyBuffer, bodyLength);
    p1 += bodyLength;
    memcpy(p1, &crc8, 1);

    nrfx_gpiote_out_toggle(current_cs_pin);

    spi_xfer_done = false;
    nrf_drv_spi_transfer(&pgSpiHandler->spi_inst, idatabuf, idatalength, itempbuf, idatalength);

    closespi(&pgSpiHandler->spi_inst);
    nrfx_gpiote_out_toggle(current_cs_pin);

    __HAL_UNLOCK(pgSpiHandler);
#endif
    return 0;
}

int32_t writetospi(uint16_t headerLength, const uint8_t *headerBuffer,
                   uint16_t bodyLength, const uint8_t *bodyBuffer)
{
    uint8_t *p1;
    uint32_t idatalength = headerLength + bodyLength;

    if (idatalength > DATALEN1)
        return NRF_ERROR_NO_MEM;

    while (pgSpiHandler->lock)
        ;
    __HAL_LOCK(pgSpiHandler);

    openspi(&pgSpiHandler->spi_inst);

    p1 = idatabuf;
    memcpy(p1, headerBuffer, headerLength);
    p1 += headerLength;
    memcpy(p1, bodyBuffer, bodyLength);

    nrfx_gpiote_out_toggle(current_cs_pin);

    spi_xfer_done = false;
    nrf_drv_spi_transfer(&pgSpiHandler->spi_inst, idatabuf, idatalength, itempbuf, idatalength);

    closespi(&pgSpiHandler->spi_inst);
    nrfx_gpiote_out_toggle(current_cs_pin);

    __HAL_UNLOCK(pgSpiHandler);

    return 0;
}

int32_t readfromspi(uint16_t headerLength, uint8_t *headerBuffer,
                    uint16_t readLength, uint8_t *readBuffer)
{
    uint8_t *p1;
    uint32_t idatalength = headerLength + readLength;

    if (idatalength > DATALEN1)
        return NRF_ERROR_NO_MEM;

    while (pgSpiHandler->lock)
        ;
    __HAL_LOCK(pgSpiHandler);

    openspi(&pgSpiHandler->spi_inst);

    p1 = idatabuf;
    memcpy(p1, headerBuffer, headerLength);
    p1 += headerLength;
    memset(p1, 0x00, readLength);

    idatalength = headerLength + readLength;

    nrfx_gpiote_out_toggle(current_cs_pin);

    spi_xfer_done = false;
    nrf_drv_spi_transfer(&pgSpiHandler->spi_inst, idatabuf, idatalength, itempbuf, idatalength);

    p1 = itempbuf + headerLength;
    memcpy(readBuffer, p1, readLength);

    closespi(&pgSpiHandler->spi_inst);
    nrfx_gpiote_out_toggle(current_cs_pin);

    __HAL_UNLOCK(pgSpiHandler);

    return 0;
}
