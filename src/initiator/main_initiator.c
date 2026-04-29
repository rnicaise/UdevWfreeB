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
#include "../common/uwb_profiles.h"
#include "../accel/accel.h"

#define POLL_MSG_PROFILE_IDX      16
#define POLL_MSG_SWITCH_TOKEN_IDX 17
#define POLL_MSG_ACQ_PERIOD_IDX   18
#define POLL_MSG_ACQ_TOKEN_IDX    19
#define POLL_MSG_TEST_PROFILE_IDX 20

#define RESP_MSG_CTRL_OPT_IDX   11
#define RESP_MSG_CTRL_TOKEN_IDX 12
#define RESP_MSG_CTRL_FLAGS_IDX 13
#define RESP_MSG_CTRL_ACQ_MS_IDX 14
#define RESP_MSG_CTRL_ACQ_TOKEN_IDX 15
#define RESP_MSG_CTRL_TEST_PROFILE_IDX 16

#define RESP_FLAG_SWITCH_PENDING 0x01u
#define RESP_FLAG_ACQ_PENDING    0x02u

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
    0, 0,                 /* [14-15] accel Z */
    UWB_PROFILE_OPT_6M8_STABLE,  /* [16] profile option used by initiator */
    0,                    /* [17] rate switch token */
    RNG_DELAY_MS,         /* [18] acquisition period currently applied (ms) */
    0,                    /* [19] period switch token */
    UWB_TEST_PROFILE_DEFAULT /* [20] active safe test profile */
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
static uint32_t accel_retry_div = 0;
static uint32_t accel_sample_count = 0;

static uint8_t acquisition_period_ms = RNG_DELAY_MS;
static uint8_t active_test_profile = UWB_TEST_PROFILE_DEFAULT;

static uint8_t current_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_switch_token = 0;
static bool switch_request_armed = false;
static uint8_t pending_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_token = 0;
static bool acq_request_armed = false;

static const uwb_runtime_profile_t *active_profile = NULL;

/* ── Config UWB (depuis le SDK) ── */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

/* ── Fonction UART/debug ── */
extern void test_run_info(unsigned char *data);

static bool is_supported_profile_opt(uint8_t opt)
{
    return uwb_profile_find(opt) != NULL;
}

static bool is_supported_acq_period(uint8_t period_ms)
{
    return (period_ms >= 1u) && (period_ms <= 200u);
}

static bool is_supported_test_profile(uint8_t profile)
{
    return profile <= UWB_TEST_PROFILE_DIAGNOSTICS_FULL;
}

static uint8_t test_profile_accel_decimation(uint8_t profile)
{
    switch (profile)
    {
        case UWB_TEST_PROFILE_FAST_DISTANCE_ONLY:
            return 0u;
        case UWB_TEST_PROFILE_FAST_ACCEL_DECIMATED:
            return 4u;
        case UWB_TEST_PROFILE_ROBUST_DETECTION:
            return 2u;
        case UWB_TEST_PROFILE_STABLE_FULL:
        case UWB_TEST_PROFILE_DIAGNOSTICS_FULL:
        default:
            return 1u;
    }
}

static int apply_profile_option(uint8_t opt)
{
    const uwb_runtime_profile_t *profile = uwb_profile_find(opt);

    if (profile == NULL)
    {
        return DWT_ERROR;
    }

    config_options = profile->config;

    if (dwt_configure(&config_options))
    {
        return DWT_ERROR;
    }

    if (config_options.chan == 5)
    {
        dwt_configuretxrf(&txconfig_options);
    }
    else
    {
        dwt_configuretxrf(&txconfig_options_ch9);
    }

    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);
    dwt_setrxaftertxdelay(profile->initiator_poll_tx_to_resp_rx_dly_uus);
    dwt_setrxtimeout(profile->initiator_resp_rx_timeout_uus);
    dwt_setpreambledetecttimeout(profile->pre_timeout_symbols);

    active_profile = profile;

    return DWT_SUCCESS;
}

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
    active_profile = uwb_profile_find(current_profile_opt);
    if (active_profile != NULL)
    {
        config_options = active_profile->config;
    }

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
    dwt_setrxaftertxdelay(active_profile->initiator_poll_tx_to_resp_rx_dly_uus);
    dwt_setrxtimeout(active_profile->initiator_resp_rx_timeout_uus);
    dwt_setpreambledetecttimeout(active_profile->pre_timeout_symbols);

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
        /* === Lire accéléromètre (robuste) ===
         * Si l'init échoue au boot (ou plus tard), on retente périodiquement.
         */
        uint8_t accel_decimation = test_profile_accel_decimation(active_test_profile);

        if (accel_decimation == 0u)
        {
            accel_data.x = 0;
            accel_data.y = 0;
            accel_data.z = 0;
        }
        else if (!accel_ok)
        {
            accel_retry_div++;
            if ((accel_retry_div & 0x3Fu) == 0u)
            {
                accel_ok = accel_init();
                if (accel_ok)
                {
                    test_run_info((unsigned char *)"ACCEL RECOVERED");
                }
            }
        }
        else
        {
            accel_sample_count++;
            if ((accel_decimation == 1u) || ((accel_sample_count % accel_decimation) == 0u))
            {
                if (!accel_read(&accel_data))
                {
                    accel_ok = false;
                }
            }
        }

        /* Encoder XYZ dans le Poll (little-endian) */
        tx_poll_msg[POLL_MSG_ACCEL_X_IDX]      = (uint8_t)(accel_data.x & 0xFF);
        tx_poll_msg[POLL_MSG_ACCEL_X_IDX + 1]  = (uint8_t)((accel_data.x >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_ACCEL_Y_IDX]      = (uint8_t)(accel_data.y & 0xFF);
        tx_poll_msg[POLL_MSG_ACCEL_Y_IDX + 1]  = (uint8_t)((accel_data.y >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_ACCEL_Z_IDX]      = (uint8_t)(accel_data.z & 0xFF);
        tx_poll_msg[POLL_MSG_ACCEL_Z_IDX + 1]  = (uint8_t)((accel_data.z >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_PROFILE_IDX] = current_profile_opt;
        tx_poll_msg[POLL_MSG_SWITCH_TOKEN_IDX] = switch_request_armed ? pending_switch_token : 0u;
        tx_poll_msg[POLL_MSG_ACQ_PERIOD_IDX] = acquisition_period_ms;
        tx_poll_msg[POLL_MSG_ACQ_TOKEN_IDX] = acq_request_armed ? pending_acq_token : 0u;
        tx_poll_msg[POLL_MSG_TEST_PROFILE_IDX] = active_test_profile;

        /* === TX POLL === */
        tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
        dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1); /* ranging bit = 1 */

        /* TX immédiat + active RX auto après délai pour recevoir Response */
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        /* Attente : bonne réception, timeout, ou erreur */
        waitforsysstatus(&status_reg, NULL,
            (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

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

                if (frame_len > RESP_MSG_CTRL_FLAGS_IDX)
                {
                    uint8_t ctrl_flags = rx_buffer[RESP_MSG_CTRL_FLAGS_IDX];
                    uint8_t ctrl_opt = rx_buffer[RESP_MSG_CTRL_OPT_IDX];
                    uint8_t ctrl_token = rx_buffer[RESP_MSG_CTRL_TOKEN_IDX];

                    if ((ctrl_flags & RESP_FLAG_SWITCH_PENDING) && is_supported_profile_opt(ctrl_opt) && (ctrl_token != 0u) && (ctrl_opt != current_profile_opt))
                    {
                        pending_profile_opt = ctrl_opt;
                        pending_switch_token = ctrl_token;
                        switch_request_armed = true;
                    }

                    if ((frame_len > RESP_MSG_CTRL_ACQ_TOKEN_IDX) && (ctrl_flags & RESP_FLAG_ACQ_PENDING))
                    {
                        uint8_t acq_period = rx_buffer[RESP_MSG_CTRL_ACQ_MS_IDX];
                        uint8_t acq_token = rx_buffer[RESP_MSG_CTRL_ACQ_TOKEN_IDX];

                        if (is_supported_acq_period(acq_period) && (acq_token != 0u) && (acq_period != acquisition_period_ms))
                        {
                            pending_acq_period_ms = acq_period;
                            pending_acq_token = acq_token;
                            acq_request_armed = true;
                        }
                    }

                    if (frame_len > RESP_MSG_CTRL_TEST_PROFILE_IDX)
                    {
                        uint8_t announced_test_profile = rx_buffer[RESP_MSG_CTRL_TEST_PROFILE_IDX];
                        if (is_supported_test_profile(announced_test_profile) && (announced_test_profile != active_test_profile))
                        {
                            active_test_profile = announced_test_profile;
                            accel_sample_count = 0;
                            test_run_info((unsigned char *)"TEST PROFILE APPLIED");
                        }
                    }

                    /* Robust behavior: always follow responder-advertised acquisition period.
                     * This avoids deadlocks in token-based coordination and keeps both sides aligned. */
                    if (frame_len > RESP_MSG_CTRL_ACQ_MS_IDX)
                    {
                        uint8_t announced_period = rx_buffer[RESP_MSG_CTRL_ACQ_MS_IDX];
                        if (is_supported_acq_period(announced_period) && (announced_period != acquisition_period_ms))
                        {
                            acquisition_period_ms = announced_period;
                            test_run_info((unsigned char *)"UWB PERIOD APPLIED");
                        }
                    }
                }

                /* === PRÉPARER TX FINAL === */

                /* Lire timestamps locaux */
                poll_tx_ts = ranging_get_tx_timestamp_u64();
                resp_rx_ts = ranging_get_rx_timestamp_u64();

                /* Calculer le moment d'envoi du Final (delayed TX) */
                final_tx_time = (resp_rx_ts + (active_profile->initiator_resp_rx_to_final_tx_dly_uus * UUS_TO_DWT_TIME)) >> 8;
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

                    if (switch_request_armed && (pending_switch_token != 0u) && (tx_poll_msg[POLL_MSG_SWITCH_TOKEN_IDX] == pending_switch_token))
                    {
                        if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                        {
                            current_profile_opt = pending_profile_opt;
                            switch_request_armed = false;
                            test_run_info((unsigned char *)"UWB RATE SWITCHED");
                        }
                    }

                    if (acq_request_armed && (pending_acq_token != 0u) && (tx_poll_msg[POLL_MSG_ACQ_TOKEN_IDX] == pending_acq_token) && is_supported_acq_period(pending_acq_period_ms))
                    {
                        acquisition_period_ms = pending_acq_period_ms;
                        acq_request_armed = false;
                        test_run_info((unsigned char *)"UWB PERIOD SWITCHED");
                    }

                    /* Hot path: no per-frame debug prints to maximize ranging rate. */
                }
                else
                {
                    /* Delayed TX raté (trop tard) — on skip */
                }
            }
        }
        else
        {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }

        Sleep(acquisition_period_ms);
    }
}
