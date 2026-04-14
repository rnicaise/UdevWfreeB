/*
 * main_responder.c — Module B (Responder) DS-TWR
 *
 * Attend un Poll, renvoie un Response, reçoit un Final,
 * puis calcule la distance avec les 6 timestamps.
 *
 * Séquence :
 *   1. Init DW3000 (SPI, config UWB, antenna delay)
 *   2. Boucle :
 *      a. RX Poll (réception continue)
 *      b. TX Response (delayed, après délai fixe)
 *      c. RX Final (auto après TX Response)
 *      d. Calcul distance avec formule DS-TWR
 *      e. Output UART (CSV)
 *
 * C'est le responder qui calcule la distance car il possède
 * ses propres timestamps (t2, t3, t6) + ceux de l'initiator
 * (t1, t4, t5) reçus dans le message Final.
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
#include "../ble/ble_adv.h"
#include "../accel/accel.h"

/* ── Trames du protocole ── */

/* Poll attendu de l'initiator (avec données accéléromètre) */
static uint8_t rx_poll_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',             /* Source (initiator) */
    'V', 'E',             /* Destination (responder) */
    FUNC_CODE_POLL,       /* 0x21 */
    0, 0,                 /* [10-11] accel X */
    0, 0,                 /* [12-13] accel Y */
    0, 0                  /* [14-15] accel Z */
};

/* Response : envoyé par le responder */
static uint8_t tx_resp_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',             /* Source (responder) */
    'W', 'A',             /* Destination (initiator) */
    FUNC_CODE_RESPONSE,   /* 0x10 */
    0x02,                 /* Activity code */
    0, 0, 0, 0            /* padding */
};

/* Final attendu de l'initiator (contient t1, t4, t5) */
static uint8_t rx_final_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_FINAL,      /* 0x23 */
    0, 0, 0, 0,           /* [10-13] poll_tx_ts (t1) */
    0, 0, 0, 0,           /* [14-17] resp_rx_ts (t4) */
    0, 0, 0, 0,           /* [18-21] final_tx_ts (t5) */
    0, 0                  /* padding */
};

/* ── État ── */
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

/* Timestamps locaux (responder) */
static uint64_t poll_rx_ts;  /* t2 */
static uint64_t resp_tx_ts;  /* t3 */
static uint64_t final_rx_ts; /* t6 */

/* Résultats */
static double tof;
static double distance;
static uint32_t ranging_count = 0;

/* Accéléromètre initiator (reçu via Poll UWB) */
static int16_t accel_rx[3]; /* x, y, z en mg */

/* Accéléromètre local (responder) */
static accel_data_t accel_local;
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
 * Point d'entrée du firmware responder.
 */
int ds_twr_responder_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING RESP v1.0");

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

    /* DWM3001CDK : LNA/PA intégrés dans le module, contrôlés par DW3000 GPIO5/6 */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    /* LEDs debug */
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* ── Init accéléromètre local LIS2DH12 ── */
    accel_ok = accel_init();
    if (accel_ok) {
        test_run_info((unsigned char *)"ACCEL OK (LIS2DH12)");
    } else {
        test_run_info((unsigned char *)"ACCEL FAIL");
    }

    /* Header CSV sur UART */
    test_run_info((unsigned char *)"# sample,dist,iax,iay,iaz,rax,ray,raz");

    /* ── BLE Advertising init ── */
    ble_adv_init();
    test_run_info((unsigned char *)"BLE ADV READY");

    /* ── 4. Boucle de ranging ── */
    while (1)
    {
        /* === RX POLL === */
        /* Pas de timeout ni preamble timeout : on attend indéfiniment */
        dwt_setpreambledetecttimeout(0);
        dwt_setrxtimeout(0);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        /* Attente réception */
        waitforsysstatus(&status_reg, NULL,
            (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            uint16_t frame_len;

            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

            frame_len = dwt_getframelength(0);
            if (frame_len <= RX_BUF_LEN)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }

            /* Vérifier que c'est un Poll */
            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) == 0)
            {
                uint32_t resp_tx_time;
                int ret;

                /* Sauvegarder les données accéléromètre du Poll
                 * (rx_buffer sera écrasé par le Final plus tard) */
                accel_rx[0] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_X_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_X_IDX + 1] << 8));
                accel_rx[1] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Y_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Y_IDX + 1] << 8));
                accel_rx[2] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Z_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Z_IDX + 1] << 8));

                /* === TX RESPONSE === */

                /* Timestamp RX du Poll (t2) */
                poll_rx_ts = ranging_get_rx_timestamp_u64();

                /* Programmer le TX Response avec un délai fixe après le Poll RX */
                resp_tx_time = (poll_rx_ts + (POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8;
                dwt_setdelayedtrxtime(resp_tx_time);

                /* Configurer : après TX Response, activer RX auto pour recevoir Final */
                dwt_setrxaftertxdelay(RESP_TX_TO_FINAL_RX_DLY_UUS);
                dwt_setrxtimeout(FINAL_RX_TIMEOUT_UUS);
                dwt_setpreambledetecttimeout(PRE_TIMEOUT);

                tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
                dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
                dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);

                /* TX delayed + attend Final en réponse */
                ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);

                if (ret == DWT_ERROR)
                {
                    /* Trop tard pour le delayed TX — skip */
                    continue;
                }

                /* === RX FINAL === */
                waitforsysstatus(&status_reg, NULL,
                    (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

                frame_seq_nb++;

                if (status_reg & DWT_INT_RXFCG_BIT_MASK)
                {
                    dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

                    frame_len = dwt_getframelength(0);
                    if (frame_len <= RX_BUF_LEN)
                    {
                        dwt_readrxdata(rx_buffer, frame_len, 0);
                    }

                    /* Vérifier que c'est un Final */
                    rx_buffer[ALL_MSG_SN_IDX] = 0;
                    if (memcmp(rx_buffer, rx_final_msg, ALL_MSG_COMMON_LEN) == 0)
                    {
                        /* === CALCUL DS-TWR === */

                        /* Timestamps de l'initiator (reçus dans le Final) */
                        uint32_t poll_tx_ts, resp_rx_ts, final_tx_ts;

                        /* Timestamps locaux du responder */
                        uint32_t poll_rx_ts_32, resp_tx_ts_32, final_rx_ts_32;

                        double Ra, Rb, Da, Db;
                        int64_t tof_dtu;

                        /* Lire timestamps locaux */
                        resp_tx_ts = ranging_get_tx_timestamp_u64();
                        final_rx_ts = ranging_get_rx_timestamp_u64();

                        /* Décoder timestamps de l'initiator depuis le Final */
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_POLL_TX_TS_IDX], &poll_tx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_RESP_RX_TS_IDX], &resp_rx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX], &final_tx_ts);

                        /* Calcul en 32-bit (wrapping OK car < 67ms entre timestamps) */
                        poll_rx_ts_32  = (uint32_t)poll_rx_ts;
                        resp_tx_ts_32  = (uint32_t)resp_tx_ts;
                        final_rx_ts_32 = (uint32_t)final_rx_ts;

                        /*
                         * Formule DS-TWR :
                         *   Ra = t4 - t1 (round-trip vu par initiator)
                         *   Rb = t6 - t3 (round-trip vu par responder)
                         *   Da = t5 - t4 (délai traitement initiator)
                         *   Db = t3 - t2 (délai traitement responder)
                         *
                         *   ToF = (Ra × Rb - Da × Db) / (Ra + Rb + Da + Db)
                         */
                        Ra = (double)(resp_rx_ts - poll_tx_ts);     /* t4 - t1 */
                        Rb = (double)(final_rx_ts_32 - resp_tx_ts_32); /* t6 - t3 */
                        Da = (double)(final_tx_ts - resp_rx_ts);    /* t5 - t4 */
                        Db = (double)(resp_tx_ts_32 - poll_rx_ts_32); /* t3 - t2 */

                        tof_dtu = (int64_t)((Ra * Rb - Da * Db) / (Ra + Rb + Da + Db));

                        tof = tof_dtu * DWT_TIME_UNITS;
                        distance = tof * SPEED_OF_LIGHT;
                        ranging_count++;

                        /* Lire accéléromètre local */
                        if (accel_ok) {
                            accel_read(&accel_local);
                        }

                        /* Output UART : CSV distance + accel initiator + accel responder */
                        snprintf(output_buf, sizeof(output_buf),
                            "%lu,%3.2f,%d,%d,%d,%d,%d,%d",
                            (unsigned long)ranging_count,
                            distance,
                            (int)accel_rx[0], (int)accel_rx[1], (int)accel_rx[2],
                            (int)accel_local.x, (int)accel_local.y, (int)accel_local.z);
                        test_run_info((unsigned char *)output_buf);

                        /* BLE : diffuser la distance */
                        ble_adv_update(distance, ranging_count);
                        ble_adv_send();
                    }
                }
                else
                {
                    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
                }
            }
        }
        else
        {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }
    }
}
