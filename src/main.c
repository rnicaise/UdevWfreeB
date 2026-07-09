/*
 * main.c - UWB ranging firmware entry point
 *
 * Replaces the Qorvo SDK main.c.
 * Initializes nRF52840 hardware, then starts initiator or responder
 * firmware depending on compile-time role.
 */

#include <boards.h>
#include <deca_spi.h>
#include <port.h>
#include <qio.h>
#include <stdlib.h>
#ifdef DEBUG
#include <stdio.h>
#endif

/*
 * Debug output over UART/RTT.
 * In Debug: printf (redirected to RTT by the SDK).
 * In Release: no-op.
 */
void test_run_info(unsigned char *data)
{
#ifdef DEBUG
    printf("%s\n", data);
#else
    (void)data;
#endif
}

/* Ranging function - defined in the role-specific main file */
#if defined(UWB_PURE) && defined(UWB_ROLE_INITIATOR)
extern int ss_twr_initiator_pure(void);
#define RANGING_ENTRY ss_twr_initiator_pure
#elif defined(UWB_PURE) && defined(UWB_ROLE_RESPONDER)
extern int ss_twr_responder_pure(void);
#define RANGING_ENTRY ss_twr_responder_pure
#elif defined(UWB_ROLE_INITIATOR)
extern int ss_twr_initiator_custom(void);
#define RANGING_ENTRY ss_twr_initiator_custom
#elif defined(UWB_ROLE_RESPONDER)
extern int ss_twr_responder_custom(void);
#define RANGING_ENTRY ss_twr_responder_custom
#else
#error "Define UWB_ROLE_INITIATOR or UWB_ROLE_RESPONDER"
#endif

#if defined(UWB_PURE)
#include "nrf_hfxo.h"
#endif

int main(void)
{
#if defined(UWB_PURE)
    /* Qorvo advice: run the CPU/peripherals from the external 32 MHz
     * crystal (HFXO) instead of the internal 64 MHz RC before any
     * DW3000 access, so SPI timing and timestamps are accurate. */
    nrf_hfxo_start_blocking();
#endif

    /* Initialize RTT for printf -> SEGGER RTT */
    qio_init();

    /* Initialize BSP: LEDs + buttons */
    bsp_board_init(BSP_INIT_LEDS | BSP_INIT_BUTTONS);

    /* Initialize nRF52840 GPIO for DW3000 */
    gpio_init();

    /* Initialize SPI to DW3000 */
    nrf52840_dk_spi_init();

    /* Initialize DW3000 interrupts */
    dw_irq_init();

    /* Short stabilization delay */
    nrf_delay_ms(2);

    /* Start ranging firmware */
    RANGING_ENTRY();

    /* Should never be reached (ranging loop is infinite) */
    while (1) { }
}
