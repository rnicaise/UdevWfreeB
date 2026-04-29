/*
 * main_responder.c — Module B (Responder) DS-TWR
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

#define RESP_MSG_CTRL_OPT_IDX         11
#define RESP_MSG_CTRL_TOKEN_IDX       12
#define RESP_MSG_CTRL_FLAGS_IDX       13
#define RESP_MSG_CTRL_ACQ_MS_IDX      14
#define RESP_MSG_CTRL_ACQ_TOKEN_IDX   15
#define RESP_MSG_CTRL_TEST_PROFILE_IDX 16

#define RESP_FLAG_SWITCH_PENDING 0x01u
#define RESP_FLAG_ACQ_PENDING    0x02u

static uint8_t rx_poll_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_POLL,
    0, 0,
    0, 0,
    0, 0
};

static uint8_t tx_resp_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',
    'W', 'A',
    FUNC_CODE_RESPONSE,
    0x02,
    0, 0, 0, 0, 0, 0
};

static uint8_t rx_final_msg[] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',
    'V', 'E',
    FUNC_CODE_FINAL,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    0, 0
};

static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

static uint64_t poll_rx_ts;
static uint64_t resp_tx_ts;
static uint64_t final_rx_ts;

static double tof;
static double distance;
static uint32_t ranging_count = 0;

static int16_t accel_rx[3];

static accel_data_t accel_local;
static bool accel_ok = false;

static uint8_t current_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_switch_token = 0;
static bool switch_pending = false;
static bool switch_after_final = false;

static uint8_t current_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_token = 0;
static bool period_pending = false;
static bool period_after_final = false;

static uint8_t last_initiator_acq_period_ms = RNG_DELAY_MS;
static uint8_t last_initiator_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t current_test_profile = UWB_TEST_PROFILE_DEFAULT;
static uint8_t last_initiator_test_profile = UWB_TEST_PROFILE_DEFAULT;
static uint32_t responder_accel_sample_count = 0;

static const uwb_runtime_profile_t *active_profile = NULL;

static char output_buf[192];
static char cmd_buf[96];

typedef enum
{
    RANGING_MODE_DS_TWR = 0,
    RANGING_MODE_SS_TWR = 1,
} ranging_mode_t;

extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

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
    return profile <= UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY;
}

static uint8_t test_profile_accel_decimation(uint8_t profile)
{
    switch (profile)
    {
        case UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY:
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

static const char *test_profile_name(uint8_t profile)
{
    switch (profile)
    {
        case UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY:
            return "TURBO_DISTANCE_ONLY";
        case UWB_TEST_PROFILE_FAST_DISTANCE_ONLY:
            return "FAST_DISTANCE_ONLY";
        case UWB_TEST_PROFILE_FAST_ACCEL_DECIMATED:
            return "FAST_ACCEL_DECIMATED";
        case UWB_TEST_PROFILE_STABLE_FULL:
            return "STABLE_FULL";
        case UWB_TEST_PROFILE_ROBUST_DETECTION:
            return "ROBUST_DETECTION";
        case UWB_TEST_PROFILE_DIAGNOSTICS_FULL:
            return "DIAGNOSTICS_FULL";
        default:
            return "UNKNOWN";
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
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    active_profile = profile;

    return DWT_SUCCESS;
}

static bool parse_rate_command(const char *cmd, uint8_t *target_opt)
{
    const char *prefix = "CFG,UWB_DATARATE_KBPS=";
    size_t prefix_len = strlen(prefix);
    int rate;

    if ((cmd == NULL) || (target_opt == NULL))
    {
        return false;
    }

    if (strncmp(cmd, prefix, prefix_len) != 0)
    {
        return false;
    }

    rate = atoi(cmd + prefix_len);
    if (rate <= 850)
    {
        *target_opt = uwb_profile_opt_for_rate_kbps(rate);
        return true;
    }
    if (rate >= 6800)
    {
        *target_opt = uwb_profile_opt_for_rate_kbps(rate);
        return true;
    }

    return false;
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
    if (strcmp(name, "FAST_ACCEL_DECIMATED") == 0)
    {
        *profile = UWB_TEST_PROFILE_FAST_ACCEL_DECIMATED;
        return true;
    }
    if (strcmp(name, "STABLE_FULL") == 0)
    {
        *profile = UWB_TEST_PROFILE_STABLE_FULL;
        return true;
    }
    if (strcmp(name, "ROBUST_DETECTION") == 0)
    {
        *profile = UWB_TEST_PROFILE_ROBUST_DETECTION;
        return true;
    }
    if (strcmp(name, "DIAGNOSTICS_FULL") == 0)
    {
        *profile = UWB_TEST_PROFILE_DIAGNOSTICS_FULL;
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
    uint8_t requested_period;
    uint8_t requested_test_profile;
    ranging_mode_t requested_mode;

    if (parse_test_profile_command(cmd, &requested_test_profile))
    {
        if (!is_supported_test_profile(requested_test_profile))
        {
            uart_log_write("ERR,TEST_PROFILE_UNSUPPORTED");
            return;
        }

        current_test_profile = requested_test_profile;
        responder_accel_sample_count = 0;

        snprintf(output_buf, sizeof(output_buf),
                 "ACK,TEST_PROFILE_APPLIED,profile=%s,id=%u",
                 test_profile_name(current_test_profile),
                 (unsigned int)current_test_profile);
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

        pending_profile_opt = requested_opt;
        pending_switch_token++;
        if (pending_switch_token == 0u)
        {
            pending_switch_token = 1u;
        }
        switch_pending = true;

        snprintf(output_buf, sizeof(output_buf),
                 "ACK,UWB_DATARATE_PENDING,opt=%u,token=%u",
                 (unsigned int)pending_profile_opt,
                 (unsigned int)pending_switch_token);
        uart_log_write(output_buf);
        return;
    }

    if (parse_acq_period_command(cmd, &requested_period))
    {
        if (!is_supported_acq_period(requested_period))
        {
            uart_log_write("ERR,ACQ_PERIOD_OUT_OF_RANGE");
            return;
        }

        if (requested_period == current_acq_period_ms)
        {
            snprintf(output_buf, sizeof(output_buf),
                     "ACK,ACQ_PERIOD_ALREADY_APPLIED,resp_ms=%u,init_ms=%u",
                     (unsigned int)current_acq_period_ms,
                     (unsigned int)last_initiator_acq_period_ms);
            uart_log_write(output_buf);
            return;
        }

        /* Period only controls loop pacing, so we can apply it immediately.
         * Initiator will follow the responder-advertised period in the response control field. */
        current_acq_period_ms = requested_period;
        period_pending = false;
        period_after_final = false;

        snprintf(output_buf, sizeof(output_buf),
                 "ACK,ACQ_PERIOD_APPLIED,resp_ms=%u,init_ms=%u",
                 (unsigned int)current_acq_period_ms,
                 (unsigned int)last_initiator_acq_period_ms);
        uart_log_write(output_buf);
        return;
    }

    if (parse_ranging_mode_command(cmd, &requested_mode))
    {
        if (requested_mode == RANGING_MODE_DS_TWR)
        {
            uart_log_write("ACK,RANGING_MODE_APPLIED,mode=DS_TWR");
            return;
        }

        uart_log_write("ERR,RANGING_MODE_UNSUPPORTED,mode=SS_TWR");
        return;
    }

    uart_log_write("ERR,UNKNOWN_CMD");
}

int ds_twr_responder_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING RESP v1.0");
    uart_log_init();
    uart_log_write("UWB RANGING RESP v1.0");

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

    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    accel_ok = accel_init();
    if (accel_ok) {
        test_run_info((unsigned char *)"ACCEL OK (LIS2DH12)");
    } else {
        test_run_info((unsigned char *)"ACCEL FAIL");
    }

    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;

    test_run_info((unsigned char *)"# ms,sample,dist,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt");
    uart_log_write("# ms,sample,dist,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt");

    while (1)
    {
        process_app_commands();

        dwt_setpreambledetecttimeout(0);
        dwt_setrxtimeout(0);
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        do
        {
            process_app_commands();
            status_reg = dwt_readsysstatuslo();
        } while ((status_reg & (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR)) == 0u);

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            uint16_t frame_len;

            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK);

            frame_len = dwt_getframelength(0);
            if (frame_len <= RX_BUF_LEN)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }

            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if (memcmp(rx_buffer, rx_poll_msg, ALL_MSG_COMMON_LEN) == 0)
            {
                uint32_t resp_tx_time;
                int ret;

                accel_rx[0] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_X_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_X_IDX + 1] << 8));
                accel_rx[1] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Y_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Y_IDX + 1] << 8));
                accel_rx[2] = (int16_t)(rx_buffer[POLL_MSG_ACCEL_Z_IDX] |
                              (rx_buffer[POLL_MSG_ACCEL_Z_IDX + 1] << 8));

                if (frame_len > POLL_MSG_ACQ_PERIOD_IDX)
                {
                    last_initiator_acq_period_ms = rx_buffer[POLL_MSG_ACQ_PERIOD_IDX];
                }
                if (frame_len > POLL_MSG_PROFILE_IDX)
                {
                    last_initiator_profile_opt = rx_buffer[POLL_MSG_PROFILE_IDX];
                }
                if (frame_len > POLL_MSG_TEST_PROFILE_IDX)
                {
                    last_initiator_test_profile = rx_buffer[POLL_MSG_TEST_PROFILE_IDX];
                }

                if (switch_pending && (frame_len > POLL_MSG_SWITCH_TOKEN_IDX))
                {
                    uint8_t initiator_opt = rx_buffer[POLL_MSG_PROFILE_IDX];
                    uint8_t req_token = rx_buffer[POLL_MSG_SWITCH_TOKEN_IDX];

                    if ((initiator_opt == current_profile_opt) && (req_token == pending_switch_token))
                    {
                        switch_after_final = true;
                    }
                }

                if (period_pending && (frame_len > POLL_MSG_ACQ_TOKEN_IDX))
                {
                    uint8_t req_token = rx_buffer[POLL_MSG_ACQ_TOKEN_IDX];

                    /* The initiator advertises its current period in POLL and sends the pending token
                     * when it is ready to switch. Match on token to complete the coordinated change. */
                    if (req_token == pending_acq_token)
                    {
                        period_after_final = true;
                    }
                }

                poll_rx_ts = ranging_get_rx_timestamp_u64();

                resp_tx_time = (poll_rx_ts + (active_profile->responder_poll_rx_to_resp_tx_dly_uus * UUS_TO_DWT_TIME)) >> 8;
                dwt_setdelayedtrxtime(resp_tx_time);

                dwt_setrxaftertxdelay(active_profile->responder_resp_tx_to_final_rx_dly_uus);
                dwt_setrxtimeout(active_profile->responder_final_rx_timeout_uus);
                dwt_setpreambledetecttimeout(active_profile->pre_timeout_symbols);

                tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
                tx_resp_msg[RESP_MSG_CTRL_OPT_IDX] = switch_pending ? pending_profile_opt : 0u;
                tx_resp_msg[RESP_MSG_CTRL_TOKEN_IDX] = switch_pending ? pending_switch_token : 0u;
                tx_resp_msg[RESP_MSG_CTRL_FLAGS_IDX] = (switch_pending ? RESP_FLAG_SWITCH_PENDING : 0u)
                                                      | (period_pending ? RESP_FLAG_ACQ_PENDING : 0u);
                tx_resp_msg[RESP_MSG_CTRL_ACQ_MS_IDX] = period_pending ? pending_acq_period_ms : current_acq_period_ms;
                tx_resp_msg[RESP_MSG_CTRL_ACQ_TOKEN_IDX] = period_pending ? pending_acq_token : 0u;
                tx_resp_msg[RESP_MSG_CTRL_TEST_PROFILE_IDX] = current_test_profile;
                dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
                dwt_writetxfctrl(sizeof(tx_resp_msg), 0, 1);

                ret = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);

                if (ret == DWT_ERROR)
                {
                    continue;
                }

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

                    rx_buffer[ALL_MSG_SN_IDX] = 0;
                    if (memcmp(rx_buffer, rx_final_msg, ALL_MSG_COMMON_LEN) == 0)
                    {
                        uint32_t poll_tx_ts, resp_rx_ts, final_tx_ts;
                        uint32_t poll_rx_ts_32, resp_tx_ts_32, final_rx_ts_32;
                        double Ra, Rb, Da, Db;
                        int64_t tof_dtu;

                        resp_tx_ts = ranging_get_tx_timestamp_u64();
                        final_rx_ts = ranging_get_rx_timestamp_u64();

                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_POLL_TX_TS_IDX], &poll_tx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_RESP_RX_TS_IDX], &resp_rx_ts);
                        ranging_msg_get_ts(&rx_buffer[FINAL_MSG_FINAL_TX_TS_IDX], &final_tx_ts);

                        poll_rx_ts_32  = (uint32_t)poll_rx_ts;
                        resp_tx_ts_32  = (uint32_t)resp_tx_ts;
                        final_rx_ts_32 = (uint32_t)final_rx_ts;

                        Ra = (double)(resp_rx_ts - poll_tx_ts);
                        Rb = (double)(final_rx_ts_32 - resp_tx_ts_32);
                        Da = (double)(final_tx_ts - resp_rx_ts);
                        Db = (double)(resp_tx_ts_32 - poll_rx_ts_32);

                        tof_dtu = (int64_t)((Ra * Rb - Da * Db) / (Ra + Rb + Da + Db));

                        tof = tof_dtu * DWT_TIME_UNITS;
                        distance = tof * SPEED_OF_LIGHT;
                        ranging_count++;

                        if (accel_ok) {
                            uint8_t accel_decimation = test_profile_accel_decimation(current_test_profile);
                            if (accel_decimation > 0u)
                            {
                                responder_accel_sample_count++;
                                if ((accel_decimation == 1u) || ((responder_accel_sample_count % accel_decimation) == 0u))
                                {
                                    accel_read(&accel_local);
                                }
                            }
                        }

                        {
                            uint32_t ms = (NRF_RTC2->COUNTER * 1000) / 32768;
                            if ((current_test_profile == UWB_TEST_PROFILE_FAST_DISTANCE_ONLY) ||
                                (current_test_profile == UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY))
                            {
                                snprintf(output_buf, sizeof(output_buf),
                                "%lu,%lu,%.2f",
                                (unsigned long)ms,
                                (unsigned long)ranging_count,
                                distance);
                            }
                            else if (current_test_profile == UWB_TEST_PROFILE_DIAGNOSTICS_FULL)
                            {
                                snprintf(output_buf, sizeof(output_buf),
                                "%lu,%lu,%.2f,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u",
                                (unsigned long)ms,
                                (unsigned long)ranging_count,
                                distance,
                                (int)accel_rx[0], (int)accel_rx[1], (int)accel_rx[2],
                                (int)accel_local.x, (int)accel_local.y, (int)accel_local.z,
                                (unsigned int)current_acq_period_ms,
                                (unsigned int)last_initiator_acq_period_ms,
                                (unsigned int)current_profile_opt,
                                (unsigned int)last_initiator_profile_opt,
                                (unsigned int)current_test_profile,
                                (unsigned int)last_initiator_test_profile);
                            }
                            else
                            {
                                snprintf(output_buf, sizeof(output_buf),
                                "%lu,%lu,%.2f,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u",
                                (unsigned long)ms,
                                (unsigned long)ranging_count,
                                distance,
                                (int)accel_rx[0], (int)accel_rx[1], (int)accel_rx[2],
                                (int)accel_local.x, (int)accel_local.y, (int)accel_local.z,
                                (unsigned int)current_acq_period_ms,
                                (unsigned int)last_initiator_acq_period_ms,
                                (unsigned int)current_profile_opt,
                                (unsigned int)last_initiator_profile_opt);
                            }
                        }
                        uart_log_write(output_buf);

                        if (switch_after_final)
                        {
                            if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                            {
                                current_profile_opt = pending_profile_opt;
                                switch_pending = false;
                                switch_after_final = false;
                                uart_log_write("ACK,UWB_DATARATE_SWITCHED");
                            }
                        }

                        if (period_after_final)
                        {
                            current_acq_period_ms = pending_acq_period_ms;
                            period_pending = false;
                            period_after_final = false;
                            uart_log_write("ACK,ACQ_PERIOD_SWITCHED");
                        }
                    }
                }
                else
                {
                    switch_after_final = false;
                    period_after_final = false;
                    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
                }
            }
        }
        else
        {
            switch_after_final = false;
            period_after_final = false;
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }
    }
}
