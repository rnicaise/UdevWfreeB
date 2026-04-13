/*
 * port_dwm3001cdk.c — HW-specific port functions for DWM3001CDK
 *
 * Based on the Qorvo SDK port.c but stripped of second-DW3000 support
 * (IRQ2, WAKEUP on invalid pin) that doesn't exist on DWM3001CDK.
 *
 * Changes vs SDK original:
 *   - Removed IRQ2 (DW3000_IRQ2n_Pin) GPIOTE init
 *   - WAKEUP pin configured only if it's a valid GPIO
 */

#include "port.h"

extern uint16_t current_irq_pin;

/* DW IC IRQ handler */
static port_dwic_isr_t port_dwic_isr = NULL;

/***************************************************************************
 *                              Time section
 ***************************************************************************/

__INLINE void Sleep(uint32_t x)
{
    nrf_delay_ms(x);
}

/***************************************************************************
 *                          Configuration section
 ***************************************************************************/

int peripherals_init(void)
{
    return 0;
}

void gpio_init(void)
{
    ret_code_t err_code;
    err_code = nrfx_gpiote_init();
    APP_ERROR_CHECK(err_code);
}

void deca_irq_handler(nrf_drv_gpiote_pin_t irqPin, nrf_gpiote_polarity_t irq_action)
{
    process_deca_irq();
}

void dw_irq_init(void)
{
    ret_code_t err_code;

    /* IRQ from DW3000 — only one on DWM3001CDK */
    nrf_drv_gpiote_in_config_t in_config = GPIOTE_CONFIG_IN_SENSE_LOTOHI(true);
    in_config.pull = NRF_GPIO_PIN_PULLDOWN;

    err_code = nrf_drv_gpiote_in_init(DW3000_IRQn_Pin, &in_config, deca_irq_handler);
    APP_ERROR_CHECK(err_code);

    nrf_drv_gpiote_in_event_enable(DW3000_IRQn_Pin, false);

    /* WAKEUP pin — configure as output only if valid.
     * On DWM3001CDK the real WAKEUP is P1.19 which doesn't exist on nRF52833.
     * We still configure our safe substitute (P1.9) so SET_WAKEUP_PIN_IO macros work. */
    nrf_gpio_cfg_output(DW3000_WAKEUP_Pin);

    /* No IRQ2 — DWM3001CDK has only one DW3000 */
}

/***************************************************************************
 *                          DW IC port section
 ***************************************************************************/

void reset_DWIC(void)
{
    nrf_gpio_cfg_output(DW3000_RESET_Pin);
    nrf_gpio_pin_clear(DW3000_RESET_Pin);
    nrf_delay_ms(2);
    nrf_gpio_cfg_input(DW3000_RESET_Pin, NRF_GPIO_PIN_NOPULL);
    nrf_delay_ms(2);
}

void wakeup_device_with_io(void)
{
    SET_WAKEUP_PIN_IO_HIGH;
    WAIT_200uSEC;
    SET_WAKEUP_PIN_IO_LOW;
}

void make_very_short_wakeup_io(void)
{
    uint8_t cnt;
    SET_WAKEUP_PIN_IO_HIGH;
    for (cnt = 0; cnt < 10; cnt++)
        __NOP();
    SET_WAKEUP_PIN_IO_LOW;
}

/***************************************************************************
 *                              IRQ section
 ***************************************************************************/

__INLINE void process_deca_irq(void)
{
    while (port_CheckEXT_IRQ() != 0)
    {
        if (port_dwic_isr)
        {
            port_dwic_isr();
        }
    }
}

__INLINE void port_DisableEXT_IRQ(void)
{
    nrf_drv_gpiote_in_event_disable(current_irq_pin);
}

__INLINE void port_EnableEXT_IRQ(void)
{
    nrf_drv_gpiote_in_event_enable(current_irq_pin, true);
}

__INLINE uint32_t port_GetEXT_IRQStatus(void)
{
    return nrfx_gpiote_in_is_set(current_irq_pin) ? 1 : 0;
}

__INLINE uint32_t port_CheckEXT_IRQ(void)
{
    return nrf_gpio_pin_read(current_irq_pin);
}

void port_set_dwic_isr(port_dwic_isr_t dwic_isr)
{
    uint8_t en = port_GetEXT_IRQStatus();

    port_DisableEXT_IRQ();
    port_dwic_isr = dwic_isr;

    if (!en)
    {
        port_EnableEXT_IRQ();
    }
}
