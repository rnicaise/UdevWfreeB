/*
 * main_initiator.c — Module A (Initiator) DS-TWR
 *
 * Envoie un Poll, reçoit un Response, envoie un Final.
 * Le responder calcule la distance (dans ce protocole, c'est le
 * responder qui a tous les timestamps nécessaires).
 *
 * Ici on calcule aussi la distance côté initiator en utilisant
 * les timestamps locaux + clock offset ratio pour du monitoring.
 *
 * Séquence :
 *   1. Init DW3000 (SPI, config UWB, antenna delay)
 *   2. Boucle :
 *      a. TX Poll (immédiat)
 *      b. RX Response (auto après délai)
 *      c. TX Final (delayed, contient timestamps t1, t4, t5)
 *      d. Sleep RNG_DELAY_MS
 *
 * Output UART : CSV avec distance estimée et timestamps
 */

#include "deca_probe_interface.h"
#include <deca_device_api.h>
#include <deca_spi.h>
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include <stdio.h>
#include <string.h>

#include "../common/ranging.h"
#include "../accel/accel.h"

/* ── Trames du protocole ── */

/* Poll : envoyé par l'initiator pour démarrer l'échange
 * Bytes 10-15 : accéléromètre XYZ (3× int16_t LE, en mg) */
static uint8_t tx_poll_msg[] = {
    0x41, 0x88,           /* Frame Control */
    0,                    /* Sequence Number (rempli dynamiquement) */
    0xCA, 0xDE,           /* PAN ID */
    'W', 'A',             /* Destination */
    'V', 'E',             /* Source */
    FUNC_CODE_POLL,       /* Function code = 0x21 */
    0, 0,                 /* [10-11] accel X (int16 LE, mg) */
    0, 0,                 /* [12-13] accel Y */
    0, 0                  /* [14-15] accel Z */
};

/* Response attendu du responder */
static uint8_t rx_resp_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',             /* Source (responder) */
    'W', 'A',             /* Destination (initiator) */
    FUNC_CODE_RESPONSE,   /* Function code = 0x10 */
    0x02,                 /* Activity code */
    0, 0                  /* padding */
};

/* Final : envoyé par l'initiator avec les 3 timestamps (t1, t4, t5) */
static uint8_t tx_final_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_FINAL,      /* Function code = 0x23 */
    0, 0, 0, 0,           /* [10-13] poll_tx_ts */
    0, 0, 0, 0,           /* [14-17] resp_rx_ts */
    0, 0, 0, 0            /* [18-21] final_tx_ts */
};

/* ── État ── */
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

/* Timestamps */
static uint64_t poll_tx_ts;
static uint64_t resp_rx_ts;
static uint64_t final_tx_ts;

/* Compteur de mesures */
static uint32_t ranging_count = 0;

/* Accéléromètre */
static accel_data_t accel_data;
static bool accel_ok = false;

/* Buffer pour output UART */
static char output_buf[128];

/* ── Config UWB (depuis le SDK) ── */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

/* ── Fonction UART/debug ── */
extern void test_run_info(unsigned char *data);

/*
 * Point d'entrée du firmware initiator.
 */
int ds_twr_initiator_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING INIT v1.0");

    /* ── 1. Init hardware ── */
    port_set_dw_ic_spi_fastrate();

    reset_DWIC();
    Sleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"PROBE FAILED");
        while (1) { };
    }

    while (!dwt_checkidlerc()) { };

    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED");
        while (1) { };
    }

    /* ── 2. Config UWB ── */
    if (dwt_configure(&config_options))
    {
        test_run_info((unsigned char *)"CONFIG FAILED");
        while (1) { };
    }

    /* Config puissance TX selon le canal */
    if (config_options.chan == 5)
    {
        dwt_configuretxrf(&txconfig_options);
    }
    else
    {
        dwt_configuretxrf(&txconfig_options_ch9);
    }

    /* ── 3. Antenna delay ── */
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    /* ── 4. Timing : délais et timeouts ── */
    dwt_setrxaftertxdelay(POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(PRE_TIMEOUT);

    /* DWM3001CDK : LNA/PA intégrés dans le module, contrôlés par DW3000 GPIO5/6 */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    /* LEDs pour debug visuel */
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* ── Init accéléromètre LIS2DH12 (I2C interne au module DWM3001C) ── */
    accel_ok = accel_init();
    if (accel_ok) {
        test_run_info((unsigned char *)"ACCEL OK (LIS2DH12)");
    } else {
        test_run_info((unsigned char *)"ACCEL FAIL — check I2C pins");
    }

    /* Header CSV sur UART */
    test_run_info((unsigned char *)"# sample,distance_m,poll_tx,resp_rx,final_tx");

    /* ── 5. Boucle de ranging ── */
    while (1)
    {
        /* === Lire accéléromètre === */
        if (accel_ok) {
            accel_read(&accel_data);
            /* Encoder XYZ dans le Poll (little-endian) */
            tx_poll_msg[POLL_MSG_ACCEL_X_IDX]     = (uint8_t)(accel_data.x & 0xFF);
            tx_poll_msg[POLL_MSG_ACCEL_X_IDX + 1]  = (uint8_t)((accel_data.x >> 8) & 0xFF);
            tx_poll_msg[POLL_MSG_ACCEL_Y_IDX]     = (uint8_t)(accel_data.y & 0xFF);
            tx_poll_msg[POLL_MSG_ACCEL_Y_IDX + 1]  = (uint8_t)((accel_data.y >> 8) & 0xFF);
            tx_poll_msg[POLL_MSG_ACCEL_Z_IDX]     = (uint8_t)(accel_data.z & 0xFF);
            tx_poll_msg[POLL_MSG_ACCEL_Z_IDX + 1]  = (uint8_t)((accel_data.z >> 8) & 0xFF);
        }

        /* === TX POLL === */
        tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
        dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1); /* ranging bit = 1 */

        /* TX immédiat + active RX auto après délai pour recevoir Response */
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        /* Attente : bonne réception, timeout, ou erreur */
        waitforsysstatus(&status_reg, NULL,
            (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

        /* Log TX status pour debug */
        {
            uint32_t tx_status = dwt_readsysstatuslo();
            snprintf(output_buf, sizeof(output_buf),
                "POLL seq=%u rxstat=0x%08lX txstat=0x%08lX",
                (unsigned)frame_seq_nb, (unsigned long)status_reg, (unsigned long)tx_status);
            test_run_info((unsigned char *)output_buf);
        }

        frame_seq_nb++;

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            uint16_t frame_len;

            /* Clear RX good + TX done */
            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

            frame_len = dwt_getframelength(0);
            if (frame_len <= RX_BUF_LEN)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }

            /* Vérifier que c'est bien un Response */
            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if (memcmp(rx_buffer, rx_resp_msg, ALL_MSG_COMMON_LEN) == 0)
            {
                uint32_t final_tx_time;
                int ret;

                /* === PRÉPARER TX FINAL === */

                /* Lire timestamps locaux */
                poll_tx_ts = ranging_get_tx_timestamp_u64();
                resp_rx_ts = ranging_get_rx_timestamp_u64();

                /* Calculer le moment d'envoi du Final (delayed TX) */
                final_tx_time = (resp_rx_ts + (RESP_RX_TO_FINAL_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
                dwt_setdelayedtrxtime(final_tx_time);

                /* Le timestamp Final TX = temps programmé + antenna delay */
                final_tx_ts = (((uint64_t)(final_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

                /* Encoder les 3 timestamps dans le message Final */
                ranging_msg_set_ts(&tx_final_msg[FINAL_MSG_POLL_TX_TS_IDX], poll_tx_ts);
                ranging_msg_set_ts(&tx_final_msg[FINAL_MSG_RESP_RX_TS_IDX], resp_rx_ts);
                ranging_msg_set_ts(&tx_final_msg[FINAL_MSG_FINAL_TX_TS_IDX], final_tx_ts);

                /* === TX FINAL === */
                tx_final_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
                dwt_writetxdata(sizeof(tx_final_msg), tx_final_msg, 0);
                dwt_writetxfctrl(sizeof(tx_final_msg) + FCS_LEN, 0, 1);

                ret = dwt_starttx(DWT_START_TX_DELAYED);

                if (ret == DWT_SUCCESS)
                {
                    /* Attendre que le Final soit envoyé */
                    waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
                    dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);

                    frame_seq_nb++;
                    ranging_count++;

                    /* Output UART : numéro de mesure + timestamps bruts */
                    snprintf(output_buf, sizeof(output_buf),
                        "%lu,TX_OK,0x%08lX,0x%08lX,0x%08lX",
                        (unsigned long)ranging_count,
                        (unsigned long)(uint32_t)poll_tx_ts,
                        (unsigned long)(uint32_t)resp_rx_ts,
                        (unsigned long)(uint32_t)final_tx_ts);
                    test_run_info((unsigned char *)output_buf);
                }
                else
                {
                    /* Delayed TX raté (trop tard) — on skip */
                    test_run_info((unsigned char *)"FINAL_TX_LATE");
                }
            }
        }
        else
        {
            /* Timeout ou erreur RX — log status pour debug */
            snprintf(output_buf, sizeof(output_buf),
                "RX_FAIL status=0x%08lX", (unsigned long)status_reg);
            test_run_info((unsigned char *)output_buf);
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }

        Sleep(RNG_DELAY_MS);
    }
}
