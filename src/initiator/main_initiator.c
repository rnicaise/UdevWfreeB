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
#include <stdlib.h>
#include <nrf.h>

#include "../common/ranging.h"
#include "../common/uwb_profiles.h"
#include "../accel/accel.h"
#include "../uart/uart_log.h"

#define POLL_MSG_PROFILE_IDX      16
#define POLL_MSG_SWITCH_TOKEN_IDX 17
#define POLL_MSG_ACQ_PERIOD_IDX   18
#define POLL_MSG_ACQ_TOKEN_IDX    19
#define POLL_MSG_TEST_PROFILE_IDX 20
#define POLL_MSG_RANGING_MODE_IDX 21

#define RESP_MSG_CTRL_OPT_IDX   11
#define RESP_MSG_CTRL_TOKEN_IDX 12
#define RESP_MSG_CTRL_FLAGS_IDX 13
#define RESP_MSG_CTRL_ACQ_MS_IDX 14
#define RESP_MSG_CTRL_ACQ_TOKEN_IDX 15
#define RESP_MSG_CTRL_TEST_PROFILE_IDX 16
#define RESP_MSG_SS_POLL_RX_TS_IDX 17
#define RESP_MSG_SS_RESP_TX_TS_IDX 21

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
    UWB_TEST_PROFILE_DEFAULT, /* [20] active safe test profile */
    0                     /* [21] ranging mode: 0=DS-TWR, 1=SS-TWR */
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
static float distance;

/* Compteur de mesures */
static uint32_t ranging_count = 0;

/* Accéléromètre */
static accel_data_t accel_data;
static bool accel_ok = false;
static uint32_t accel_retry_div = 0;
static uint32_t accel_sample_count = 0;

static uint8_t acquisition_period_ms = RNG_DELAY_MS;
static uint8_t active_test_profile = UWB_TEST_PROFILE_DEFAULT;

typedef enum
{
    RANGING_MODE_DS_TWR = 0,
    RANGING_MODE_SS_TWR = 1,
} ranging_mode_t;

static ranging_mode_t active_ranging_mode = RANGING_MODE_SS_TWR;

static uint8_t current_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_switch_token = 0;
static bool switch_request_armed = false;
static uint8_t pending_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_token = 0;
static bool acq_request_armed = false;

static const uwb_runtime_profile_t *active_profile = NULL;
static char output_buf[224];
static char cmd_buf[96];

/* ── Config UWB (depuis le SDK) ── */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

/* ── Fonction UART/debug ── */
extern void test_run_info(unsigned char *data);

static void handle_app_command(const char *cmd);

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
    return (profile == UWB_TEST_PROFILE_FAST_DISTANCE_ONLY) ||
           (profile == UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY);
}

static uint8_t test_profile_accel_decimation(uint8_t profile)
{
    (void)profile;
    return 0u;
}

static const char *test_profile_name(uint8_t profile)
{
    switch (profile)
    {
        case UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY:
            return "TURBO_DISTANCE_ONLY";
        case UWB_TEST_PROFILE_FAST_DISTANCE_ONLY:
            return "FAST_DISTANCE_ONLY";
        default:
            return "UNKNOWN";
    }
}

static char *append_u32(char *dst, uint32_t value)
{
    char digits[10];
    uint8_t count = 0;

    do
    {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    while (count > 0u)
    {
        *dst++ = digits[--count];
    }

    return dst;
}

static char *append_distance_cm(char *dst, int32_t distance_cm)
{
    uint32_t magnitude;
    uint32_t frac;

    if (distance_cm < 0)
    {
        *dst++ = '-';
        magnitude = (uint32_t)(-distance_cm);
    }
    else
    {
        magnitude = (uint32_t)distance_cm;
    }

    dst = append_u32(dst, magnitude / 100u);
    *dst++ = '.';
    frac = magnitude % 100u;
    *dst++ = (char)('0' + (frac / 10u));
    *dst++ = (char)('0' + (frac % 10u));

    return dst;
}

static void write_distance_csv(uint32_t ms, uint32_t sample, float distance_m)
{
    char *dst = output_buf;
    float distance_cm_f = distance_m * 100.0f;
    int32_t distance_cm = (int32_t)(distance_cm_f + ((distance_cm_f >= 0.0f) ? 0.5f : -0.5f));

    dst = append_u32(dst, ms);
    *dst++ = ',';
    dst = append_u32(dst, sample);
    *dst++ = ',';
    dst = append_distance_cm(dst, distance_cm);
    *dst = '\0';

    uart_log_write(output_buf);
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
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_OFF);

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

static bool parse_rate_command(const char *cmd, uint8_t *target_opt)
{
    const char *prefix = "CFG,UWB_DATARATE_KBPS=";
    size_t prefix_len = strlen(prefix);
    int rate;
    uint8_t current_channel;

    if ((cmd == NULL) || (target_opt == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    rate = atoi(cmd + prefix_len);
    current_channel = uwb_profile_channel_for_opt(current_profile_opt);
    if (rate <= 850)
    {
        *target_opt = uwb_profile_opt_for_channel_rate_kbps(current_channel, rate);
        return true;
    }
    if (rate >= 6800)
    {
        *target_opt = uwb_profile_opt_for_channel_rate_kbps(current_channel, rate);
        return true;
    }

    return false;
}

static bool parse_channel_command(const char *cmd, uint8_t *channel)
{
    const char *prefix = "CFG,UWB_CHANNEL=";
    size_t prefix_len = strlen(prefix);
    int requested_channel;

    if ((cmd == NULL) || (channel == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    requested_channel = atoi(cmd + prefix_len);
    if ((requested_channel != 5) && (requested_channel != 9))
    {
        return false;
    }

    *channel = (uint8_t)requested_channel;
    return true;
}

static bool parse_acq_period_command(const char *cmd, uint8_t *period_ms)
{
    const char *prefix = "CFG,ACQ_PERIOD_MS=";
    size_t prefix_len = strlen(prefix);
    int period;

    if ((cmd == NULL) || (period_ms == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    period = atoi(cmd + prefix_len);
    if ((period < 1) || (period > 200))
    {
        return false;
    }

    *period_ms = (uint8_t)period;
    return true;
}

static bool parse_ranging_mode_command(const char *cmd, ranging_mode_t *mode)
{
    const char *prefix = "CFG,RANGING_MODE=";
    size_t prefix_len = strlen(prefix);

    if ((cmd == NULL) || (mode == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    if (strcmp(cmd + prefix_len, "DS_TWR") == 0)
    {
        *mode = RANGING_MODE_DS_TWR;
        return true;
    }

    if (strcmp(cmd + prefix_len, "SS_TWR") == 0)
    {
        *mode = RANGING_MODE_SS_TWR;
        return true;
    }

    return false;
}

static bool parse_test_profile_command(const char *cmd, uint8_t *profile)
{
    const char *prefix = "CFG,TEST_PROFILE=";
    size_t prefix_len = strlen(prefix);
    const char *name;

    if ((cmd == NULL) || (profile == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    name = cmd + prefix_len;
    if (strcmp(name, "TURBO_DISTANCE_ONLY") == 0)
    {
        *profile = UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY;
        return true;
    }
    if (strcmp(name, "FAST_DISTANCE_ONLY") == 0)
    {
        *profile = UWB_TEST_PROFILE_FAST_DISTANCE_ONLY;
        return true;
    }
    return false;
}

static void process_app_commands(void)
{
    uart_log_poll_rx();
    while (uart_log_read_command(cmd_buf, sizeof(cmd_buf)))
    {
        handle_app_command(cmd_buf);
    }
}

static void handle_app_command(const char *cmd)
{
    uint8_t requested_opt;
    uint8_t requested_channel;
    uint8_t requested_period;
    uint8_t requested_test_profile;
    ranging_mode_t requested_mode;

    if ((strcmp(cmd, "CFG,GET_ROLE") == 0) || (strcmp(cmd, "INFO?") == 0))
    {
        uart_log_write("ROLE,INITIATOR");
        return;
    }

    if (parse_test_profile_command(cmd, &requested_test_profile))
    {
        if (!is_supported_test_profile(requested_test_profile))
        {
            uart_log_write("ERR,TEST_PROFILE_UNSUPPORTED");
            return;
        }

        active_test_profile = requested_test_profile;
        accel_sample_count = 0;

        snprintf(output_buf, sizeof(output_buf),
                 "ACK,TEST_PROFILE_APPLIED,profile=%s,id=%u",
                 test_profile_name(active_test_profile),
                 (unsigned int)active_test_profile);
        uart_log_write(output_buf);
        return;
    }

    if (parse_channel_command(cmd, &requested_channel))
    {
        requested_opt = uwb_profile_opt_for_channel_rate_kbps(requested_channel, active_profile != NULL ? active_profile->data_rate_kbps : 6800);

        if (!is_supported_profile_opt(requested_opt))
        {
            uart_log_write("ERR,UWB_CHANNEL_UNSUPPORTED");
            return;
        }

        if (requested_opt == current_profile_opt)
        {
            snprintf(output_buf, sizeof(output_buf),
                     "ACK,UWB_CHANNEL_ALREADY_APPLIED,ch=%u,opt=%u",
                     (unsigned int)requested_channel,
                     (unsigned int)current_profile_opt);
            uart_log_write(output_buf);
            return;
        }

        pending_profile_opt = requested_opt;
        pending_switch_token++;
        if (pending_switch_token == 0u)
        {
            pending_switch_token = 1u;
        }
        switch_request_armed = true;

        snprintf(output_buf, sizeof(output_buf),
                 "ACK,UWB_CHANNEL_PENDING,ch=%u,opt=%u,token=%u",
                 (unsigned int)requested_channel,
                 (unsigned int)pending_profile_opt,
                 (unsigned int)pending_switch_token);
        uart_log_write(output_buf);
        return;
    }

    if (parse_rate_command(cmd, &requested_opt))
    {
        if (requested_opt == UWB_PROFILE_OPT_850K_ROBUST)
        {
            uart_log_write("ERR,UWB_DATARATE_REQUIRES_BOOT_PROFILE,kbps=850");
            return;
        }

        if (!is_supported_profile_opt(requested_opt))
        {
            uart_log_write("ERR,UWB_DATARATE_UNSUPPORTED");
            return;
        }

        if (requested_opt == current_profile_opt)
        {
            uart_log_write("ACK,UWB_DATARATE_ALREADY_APPLIED");
            return;
        }

        if (apply_profile_option(requested_opt) == DWT_SUCCESS)
        {
            current_profile_opt = requested_opt;
            uart_log_write("ACK,UWB_DATARATE_APPLIED");
        }
        else
        {
            uart_log_write("ERR,UWB_DATARATE_APPLY_FAILED");
        }
        return;
    }

    if (parse_acq_period_command(cmd, &requested_period))
    {
        acquisition_period_ms = requested_period;
        snprintf(output_buf, sizeof(output_buf),
                 "ACK,ACQ_PERIOD_APPLIED,init_ms=%u",
                 (unsigned int)acquisition_period_ms);
        uart_log_write(output_buf);
        return;
    }

    if (parse_ranging_mode_command(cmd, &requested_mode))
    {
        (void)requested_mode;
        active_ranging_mode = RANGING_MODE_SS_TWR;
        uart_log_write("ACK,RANGING_MODE_APPLIED,mode=SS_TWR");
        return;
    }

    uart_log_write("ERR,UNKNOWN_CMD");
}

/*
 * Point d'entrée du firmware initiator.
 */
int ds_twr_initiator_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING INIT v1.0");
    uart_log_init();
    uart_log_write("UWB RANGING INIT v1.0");
    uart_log_write("ROLE,INITIATOR");

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
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_OFF);

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
    uart_log_write("# ms,sample,dist");

    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;

    /* ── 5. Boucle de ranging ── */
    while (1)
    {
        process_app_commands();

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
        tx_poll_msg[POLL_MSG_PROFILE_IDX] = switch_request_armed ? pending_profile_opt : current_profile_opt;
        tx_poll_msg[POLL_MSG_SWITCH_TOKEN_IDX] = switch_request_armed ? pending_switch_token : 0u;
        tx_poll_msg[POLL_MSG_ACQ_PERIOD_IDX] = acquisition_period_ms;
        tx_poll_msg[POLL_MSG_ACQ_TOKEN_IDX] = acq_request_armed ? pending_acq_token : 0u;
        tx_poll_msg[POLL_MSG_TEST_PROFILE_IDX] = active_test_profile;
        tx_poll_msg[POLL_MSG_RANGING_MODE_IDX] = (uint8_t)active_ranging_mode;

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

                if (active_ranging_mode == RANGING_MODE_SS_TWR)
                {
                    if (frame_len > (RESP_MSG_SS_RESP_TX_TS_IDX + FINAL_MSG_TS_LEN - 1))
                    {
                        uint32_t responder_poll_rx_ts;
                        uint32_t responder_resp_tx_ts;
                        uint32_t poll_tx_ts_32 = (uint32_t)poll_tx_ts;
                        uint32_t resp_rx_ts_32 = (uint32_t)resp_rx_ts;
                        uint32_t rtd_init;
                        uint32_t reply_resp;
                        float clock_offset_ratio;
                        float tof_dtu;
                        uint32_t ms;

                        ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_POLL_RX_TS_IDX], &responder_poll_rx_ts);
                        ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_RESP_TX_TS_IDX], &responder_resp_tx_ts);

                        rtd_init = resp_rx_ts_32 - poll_tx_ts_32;
                        reply_resp = responder_resp_tx_ts - responder_poll_rx_ts;
                        clock_offset_ratio = (float)dwt_readclockoffset() * (float)CLOCK_OFFSET_PPM_TO_RATIO;
                        tof_dtu = ((float)rtd_init - ((float)reply_resp * (1.0f - clock_offset_ratio))) / 2.0f;

                        distance = tof_dtu * (float)DWT_TIME_UNITS * (float)SPEED_OF_LIGHT;
                        ranging_count++;

                        ms = (uint32_t)(((uint64_t)NRF_RTC2->COUNTER * 1000u) / 32768u);
                        write_distance_csv(ms, ranging_count, distance);

                        if (switch_request_armed && (pending_switch_token != 0u) && (tx_poll_msg[POLL_MSG_SWITCH_TOKEN_IDX] == pending_switch_token))
                        {
                            if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                            {
                                current_profile_opt = pending_profile_opt;
                                switch_request_armed = false;
                                test_run_info((unsigned char *)"UWB CHANNEL SWITCHED");
                            }
                        }
                    }
                }
                else
                {

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
        }
        else
        {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }

        if (active_test_profile != UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY)
        {
            Sleep(acquisition_period_ms);
        }
    }
}
