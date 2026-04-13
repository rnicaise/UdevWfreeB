/*
 * main.c — Point d'entrée du firmware UWB Ranging
 *
 * Remplace le main.c du SDK Qorvo.
 * Initialise le hardware nRF52840, puis lance le firmware
 * initiator ou responder selon la compilation.
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
 * Output debug vers UART/RTT.
 * En Debug : printf (redirigé vers RTT par le SDK).
 * En Release : no-op.
 */
void test_run_info(unsigned char *data)
{
#ifdef DEBUG
    printf("%s\n", data);
#else
    (void)data;
#endif
}

/* Fonction ranging — définie dans main_initiator.c ou main_responder.c */
#if defined(UWB_ROLE_INITIATOR)
extern int ds_twr_initiator_custom(void);
#define RANGING_ENTRY ds_twr_initiator_custom
#elif defined(UWB_ROLE_RESPONDER)
extern int ds_twr_responder_custom(void);
#define RANGING_ENTRY ds_twr_responder_custom
#else
#error "Définir UWB_ROLE_INITIATOR ou UWB_ROLE_RESPONDER"
#endif

int main(void)
{
    /* Init RTT pour printf → SEGGER RTT */
    qio_init();

    /* Init BSP : LEDs + boutons */
    bsp_board_init(BSP_INIT_LEDS | BSP_INIT_BUTTONS);

    /* Init GPIO nRF52840 pour le DW3000 */
    gpio_init();

    /* Init SPI vers le DW3000 */
    nrf52840_dk_spi_init();

    /* Init interruptions DW3000 */
    dw_irq_init();

    /* Petit délai de stabilisation */
    nrf_delay_ms(2);

    /* Lancer le firmware ranging */
    RANGING_ENTRY();

    /* Ne devrait jamais arriver (boucle infinie dans le ranging) */
    while (1) { }
}
