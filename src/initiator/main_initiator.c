/*
 * main_initiator.c - Module A (Initiator) SS-TWR
 *
 * Sends Poll, receives Response, sends Final.
 * In this protocol, the responder computes the distance because it has
 * all required timestamps.
 *
 * This file also computes distance on the initiator side using local
 * timestamps plus clock offset ratio for monitoring.
 *
 * Sequence:
 *   1. Initialize DW3000 (SPI, UWB config, antenna delay)
 *   2. Loop:
 *      a. TX Poll (immediate)
 *      b. RX Response (auto after delay)
 *      c. TX Final (delayed, contains timestamps t1, t4, t5)
 *      d. Sleep RNG_DELAY_MS
 *
 * UART output: CSV with estimated distance and timestamps
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
#include "../common/radio_quality.h"
#include "../accel/accel.h"
#include "../board/pyro_buzzer.h"
#include "../uart/uart_log.h"

#define POLL_MSG_PROFILE_IDX      16
#define POLL_MSG_SWITCH_TOKEN_IDX 17
#define POLL_MSG_ACQ_PERIOD_IDX   18
#define POLL_MSG_ACQ_TOKEN_IDX    19
#define POLL_MSG_TEST_PROFILE_IDX 20
#define POLL_MSG_RANGING_MODE_IDX 21
#define POLL_MSG_FIRE_IDX         22
#define POLL_MSG_GYRO_X_IDX       23
#define POLL_MSG_GYRO_Y_IDX       25
#define POLL_MSG_GYRO_Z_IDX       27
#define POLL_MSG_TX_POWER_LEVEL_IDX 29

#define POLL_MSG_FIRE_NONE        0u
#define POLL_MSG_FIRE_COUNTDOWN   1u
#define POLL_MSG_FIRE_IMMEDIATE   2u
#define BOOTLOADER_DFU_START      0xB1u

#define RESP_MSG_CTRL_OPT_IDX   11
#define RESP_MSG_CTRL_TOKEN_IDX 12
#define RESP_MSG_CTRL_FLAGS_IDX 13
#define RESP_MSG_CTRL_ACQ_MS_IDX 14
#define RESP_MSG_CTRL_ACQ_TOKEN_IDX 15
#define RESP_MSG_CTRL_TEST_PROFILE_IDX 16
#define RESP_MSG_SS_POLL_RX_TS_IDX 17
#define RESP_MSG_SS_RESP_TX_TS_IDX 21
#define RESP_MSG_ACCEL_X_IDX      25
#define RESP_MSG_ACCEL_Y_IDX      27
#define RESP_MSG_ACCEL_Z_IDX      29
#define RESP_MSG_GYRO_X_IDX       31
#define RESP_MSG_GYRO_Y_IDX       33
#define RESP_MSG_GYRO_Z_IDX       35
#define RESP_MSG_LOAD_MV_IDX      37
#define RESP_MSG_LOAD_CONNECTED_IDX 39

#define RESP_FLAG_SWITCH_PENDING 0x01u
#define RESP_FLAG_ACQ_PENDING    0x02u

/* -- Protocol frames -- */

/* Poll: sent by initiator to start the exchange
 * Bytes 10-15: XYZ accelerometer (3x int16_t LE, in mg) */
static uint8_t tx_poll_msg[] = {
    0x41, 0x88,           /* Frame Control */
    0,                    /* Sequence Number (filled dynamically) */
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
    1,                    /* [21] fixed ranging mode: SS-TWR */
    0,                    /* [22] fire relay flag to responder */
    0, 0,                 /* [23-24] gyro X (int16 LE, raw LSB) */
    0, 0,                 /* [25-26] gyro Y */
    0, 0,                 /* [27-28] gyro Z */
    UWB_TX_POWER_LEVEL_DEFAULT /* [29] relative TX power level */
};

/* Response expected from responder */
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

/* Final: sent by initiator with 3 timestamps (t1, t4, t5) */
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

/* -- State -- */
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

/* Timestamps */
static uint64_t poll_tx_ts;
static uint64_t resp_rx_ts;
static uint64_t final_tx_ts;
static float distance;

/* Measurement counter */
static uint32_t ranging_count = 0;

/* Accelerometer */
static accel_data_t accel_data;
static gyro_data_t gyro_data;
static bool accel_ok = false;
static uint32_t accel_retry_div = 0;
static uint32_t accel_sample_count = 0;

static uint8_t acquisition_period_ms = RNG_DELAY_MS;
static uint8_t active_test_profile = UWB_TEST_PROFILE_DEFAULT;

#define RANGING_MODE_SS_TWR 1u

static uint8_t current_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_profile_opt = UWB_PROFILE_OPT_6M8_STABLE;
static uint8_t pending_switch_token = 0;
static uint8_t next_switch_token = 1;
static bool switch_request_armed = false;
static uint8_t pending_acq_period_ms = RNG_DELAY_MS;
static uint8_t pending_acq_token = 0;
static bool acq_request_armed = false;
static uint8_t fire_request_frames_remaining = 0;
static uint8_t fire_request_code = POLL_MSG_FIRE_NONE;
static uint8_t current_tx_power_level = UWB_TX_POWER_LEVEL_DEFAULT;

static const uwb_runtime_profile_t *active_profile = NULL;
static char output_buf[320];
static char cmd_buf[96];

/* -- Physical plausibility gate --
 * Rejects (flags) distance samples that are physically impossible for the
 * rider/horse use case. Invalid samples are still logged with valid=0 so
 * nothing is lost for analysis; consumers (app trigger logic) should only
 * act on valid=1 samples. */
#define GATE_MIN_DISTANCE_M    (-0.5f)
#define GATE_MAX_DISTANCE_M    (100.0f)
#define GATE_MAX_SPEED_MPS     (12.0f)
#define GATE_MARGIN_M          (0.3f)
#define GATE_RESYNC_REJECTS    (8u)

static float gate_last_valid_distance_m = 0.0f;
static uint32_t gate_last_valid_ms = 0;
static bool gate_has_baseline = false;
static uint8_t gate_consecutive_rejects = 0;

/* Returns true when the sample is plausible. After GATE_RESYNC_REJECTS
 * consecutive rejections the new level is accepted as baseline so the
 * gate can never lock out a genuine fast change. */
static bool gate_check_distance(float distance_m, uint32_t now_ms)
{
    float max_delta_m;
    float delta_m;
    uint32_t dt_ms;

    if ((distance_m < GATE_MIN_DISTANCE_M) || (distance_m > GATE_MAX_DISTANCE_M))
    {
        return false;
    }

    if (!gate_has_baseline)
    {
        gate_last_valid_distance_m = distance_m;
        gate_last_valid_ms = now_ms;
        gate_has_baseline = true;
        gate_consecutive_rejects = 0;
        return true;
    }

    dt_ms = now_ms - gate_last_valid_ms;
    max_delta_m = (GATE_MAX_SPEED_MPS * (float)dt_ms / 1000.0f) + GATE_MARGIN_M;
    delta_m = distance_m - gate_last_valid_distance_m;
    if (delta_m < 0.0f)
    {
        delta_m = -delta_m;
    }

    if (delta_m <= max_delta_m)
    {
        gate_last_valid_distance_m = distance_m;
        gate_last_valid_ms = now_ms;
        gate_consecutive_rejects = 0;
        return true;
    }

    gate_consecutive_rejects++;
    if (gate_consecutive_rejects >= GATE_RESYNC_REJECTS)
    {
        /* Persistent new level: re-baseline and accept. */
        gate_last_valid_distance_m = distance_m;
        gate_last_valid_ms = now_ms;
        gate_consecutive_rejects = 0;
        return true;
    }

    return false;
}

/* -- Median-of-5 filter on gate-valid distances --
 * At ~390 Hz this adds only ~13 ms of latency while killing isolated
 * spikes of 1-2 samples. Invalid samples never enter the window, so a
 * burst of NLOS outliers cannot drag the filtered output. */
#define MEDIAN_FILTER_LEN 5u

static float median_buf[MEDIAN_FILTER_LEN];
static uint8_t median_count = 0;
static uint8_t median_head = 0;
static bool smooth_has_baseline = false;
static float smooth_distance_m = 0.0f;
static float filtered_distance_m = 0.0f;

static void reset_distance_filters(void)
{
    gate_has_baseline = false;
    gate_consecutive_rejects = 0;
    median_count = 0;
    median_head = 0;
    smooth_has_baseline = false;
    smooth_distance_m = 0.0f;
    filtered_distance_m = 0.0f;
}

static float median_filter_push(float distance_m)
{
    float sorted[MEDIAN_FILTER_LEN];
    uint8_t i;
    uint8_t j;

    median_buf[median_head] = distance_m;
    median_head = (uint8_t)((median_head + 1u) % MEDIAN_FILTER_LEN);
    if (median_count < MEDIAN_FILTER_LEN)
    {
        median_count++;
    }

    for (i = 0; i < median_count; i++)
    {
        float v = median_buf[i];
        j = i;
        while ((j > 0u) && (sorted[j - 1u] > v))
        {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = v;
    }

    return sorted[median_count / 2u];
}

static float smooth_filter_push(float distance_m, bool valid)
{
    const float innovation_gate_m = 0.12f;
    const float alpha = 0.05f;

    if (!valid)
    {
        return smooth_distance_m;
    }

    if (!smooth_has_baseline)
    {
        smooth_distance_m = distance_m;
        smooth_has_baseline = true;
        return smooth_distance_m;
    }

    if ((distance_m >= (smooth_distance_m - innovation_gate_m)) &&
        (distance_m <= (smooth_distance_m + innovation_gate_m)))
    {
        smooth_distance_m = smooth_distance_m + (alpha * (distance_m - smooth_distance_m));
    }

    return smooth_distance_m;
}

/* -- UWB config (from SDK) -- */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;
extern dwt_txconfig_t txconfig_options_ch9;

/* -- UART/debug function -- */
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
    return profile == UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY;
}

static uint8_t test_profile_accel_decimation(uint8_t profile)
{
    (void)profile;
    return 1u;
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

static char *append_i32(char *dst, int32_t value)
{
    uint32_t magnitude;

    if (value < 0)
    {
        *dst++ = '-';
        magnitude = (uint32_t)(-value);
    }
    else
    {
        magnitude = (uint32_t)value;
    }

    return append_u32(dst, magnitude);
}

/* Append a fixed-point value: scaled = value * 10^decimals (rounded). */
static char *append_fixed(char *dst, int32_t scaled, uint8_t decimals)
{
    uint32_t magnitude;
    uint32_t div = 1;
    uint32_t frac;
    uint8_t i;

    for (i = 0; i < decimals; i++)
    {
        div *= 10u;
    }

    if (scaled < 0)
    {
        *dst++ = '-';
        magnitude = (uint32_t)(-scaled);
    }
    else
    {
        magnitude = (uint32_t)scaled;
    }

    dst = append_u32(dst, magnitude / div);
    *dst++ = '.';
    frac = magnitude % div;
    div /= 10u;
    while (div > 0u)
    {
        *dst++ = (char)('0' + ((frac / div) % 10u));
        div /= 10u;
    }

    return dst;
}

static int32_t float_scaled(float value, float scale)
{
    float scaled = value * scale;
    return (int32_t)(scaled + ((scaled >= 0.0f) ? 0.5f : -0.5f));
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

static void write_distance_csv(uint32_t ms, uint32_t sample, float distance_m,
                               const radio_quality_t *radio_quality,
                               const accel_data_t *initiator_accel,
                               const int16_t responder_accel[3],
                               const gyro_data_t *initiator_gyro,
                               const int16_t responder_gyro[3],
                               bool valid, float distance_filt_m, float distance_smooth_m,
                               uint16_t responder_load_mv, uint8_t responder_load_connected)
{
    char *dst = output_buf;
    float distance_cm_f = distance_m * 100.0f;
    int32_t distance_cm = (int32_t)(distance_cm_f + ((distance_cm_f >= 0.0f) ? 0.5f : -0.5f));

    dst = append_u32(dst, ms);
    *dst++ = ',';
    dst = append_u32(dst, sample);
    *dst++ = ',';
    dst = append_distance_cm(dst, distance_cm);
    *dst++ = ',';
    dst = append_fixed(dst, float_scaled(radio_quality->rx_power_dbm, 10.0f), 1);
    *dst++ = ',';
    dst = append_fixed(dst, float_scaled(radio_quality->fp_power_dbm, 10.0f), 1);
    *dst++ = ',';
    dst = append_fixed(dst, float_scaled(radio_quality->clock_offset_ppm, 100.0f), 2);
    *dst++ = ',';
    dst = append_u32(dst, radio_quality->score_10);
    *dst++ = ',';
    dst = append_u32(dst, radio_quality->nlos_score_10);
    *dst++ = ',';
    dst = append_fixed(dst, float_scaled(radio_quality->peak_to_fp_samples, 100.0f), 2);
    *dst++ = ',';
    dst = append_u32(dst, radio_quality->fp_conf_level);
    *dst++ = ',';
    dst = append_i32(dst, radio_quality->sts_quality);
    *dst++ = ',';
    dst = append_i32(dst, initiator_accel->x);
    *dst++ = ',';
    dst = append_i32(dst, initiator_accel->y);
    *dst++ = ',';
    dst = append_i32(dst, initiator_accel->z);
    *dst++ = ',';
    dst = append_i32(dst, responder_accel[0]);
    *dst++ = ',';
    dst = append_i32(dst, responder_accel[1]);
    *dst++ = ',';
    dst = append_i32(dst, responder_accel[2]);
    *dst++ = ',';
    dst = append_u32(dst, acquisition_period_ms);
    *dst++ = ',';
    dst = append_u32(dst, acquisition_period_ms);
    *dst++ = ',';
    dst = append_u32(dst, current_profile_opt);
    *dst++ = ',';
    dst = append_u32(dst, current_profile_opt);
    *dst++ = ',';
    dst = append_i32(dst, initiator_gyro->x);
    *dst++ = ',';
    dst = append_i32(dst, initiator_gyro->y);
    *dst++ = ',';
    dst = append_i32(dst, initiator_gyro->z);
    *dst++ = ',';
    dst = append_i32(dst, responder_gyro[0]);
    *dst++ = ',';
    dst = append_i32(dst, responder_gyro[1]);
    *dst++ = ',';
    dst = append_i32(dst, responder_gyro[2]);
    *dst++ = ',';
    dst = append_u32(dst, valid ? 1u : 0u);
    *dst++ = ',';
    {
        float filt_cm_f = distance_filt_m * 100.0f;
        int32_t filt_cm = (int32_t)(filt_cm_f + ((filt_cm_f >= 0.0f) ? 0.5f : -0.5f));
        dst = append_distance_cm(dst, filt_cm);
    }
    *dst++ = ',';
    {
        float smooth_cm_f = distance_smooth_m * 100.0f;
        int32_t smooth_cm = (int32_t)(smooth_cm_f + ((smooth_cm_f >= 0.0f) ? 0.5f : -0.5f));
        dst = append_distance_cm(dst, smooth_cm);
    }
    *dst++ = ',';
    dst = append_u32(dst, responder_load_mv);
    *dst++ = ',';
    dst = append_u32(dst, responder_load_connected ? 1u : 0u);
    *dst = '\0';

    uart_log_write(output_buf);
}

static void apply_tx_power_config(void)
{
    dwt_txconfig_t tx_config = (config_options.chan == 9) ? txconfig_options_ch9 : txconfig_options;
    tx_config.power = uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level);
    dwt_configuretxrf(&tx_config);
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
    radio_quality_enable_diagnostics();

    apply_tx_power_config();

    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);
    dwt_setrxaftertxdelay(profile->initiator_poll_tx_to_resp_rx_dly_uus);
    dwt_setrxtimeout(profile->initiator_resp_rx_timeout_uus);
    dwt_setpreambledetecttimeout(profile->pre_timeout_symbols);

    active_profile = profile;

    return DWT_SUCCESS;
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
    if (strcmp(cmd, "BOOT,DFU") == 0)
    {
        uart_log_write("ACK,BOOT_DFU");
        uart_log_flush();
        NRF_POWER->GPREGRET = (uint32_t)BOOTLOADER_DFU_START;
        NVIC_SystemReset();
        return;
    }

    if ((strcmp(cmd, "CFG,GET_ROLE") == 0) || (strcmp(cmd, "INFO?") == 0))
    {
        uart_log_write("ROLE,INITIATOR");
        return;
    }

    if (strcmp(cmd, "CFG,GET_PROFILE") == 0)
    {
        snprintf(output_buf, sizeof(output_buf), "PROFILE,%u", (unsigned int)current_profile_opt);
        uart_log_write(output_buf);
        return;
    }

    if (strcmp(cmd, "CFG,GET_TXPWR") == 0)
    {
        snprintf(output_buf, sizeof(output_buf), "TXPWR,%u,0x%08lx",
                 (unsigned int)current_tx_power_level,
                 (unsigned long)uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level));
        uart_log_write(output_buf);
        return;
    }

    if (strncmp(cmd, "CFG,TXPWR,", 10) == 0)
    {
        uint8_t level = (uint8_t)strtoul(cmd + 10, NULL, 10);
        if (!uwb_tx_power_level_is_supported(level))
        {
            uart_log_write("ERR,UNSUPPORTED_TXPWR");
            return;
        }
        current_tx_power_level = level;
        apply_tx_power_config();
        reset_distance_filters();
        snprintf(output_buf, sizeof(output_buf), "ACK,TXPWR,%u,0x%08lx",
                 (unsigned int)current_tx_power_level,
                 (unsigned long)uwb_tx_power_value_for_level((uint8_t)config_options.chan, current_tx_power_level));
        uart_log_write(output_buf);
        return;
    }

    if (strncmp(cmd, "CFG,PROFILE,", 12) == 0)
    {
        uint8_t opt = (uint8_t)strtoul(cmd + 12, NULL, 10);
        if (!is_supported_profile_opt(opt))
        {
            uart_log_write("ERR,UNSUPPORTED_PROFILE");
            return;
        }
        if (opt == current_profile_opt)
        {
            switch_request_armed = false;
            uart_log_write("ACK,PROFILE_ALREADY_ACTIVE");
            return;
        }
        pending_profile_opt = opt;
        pending_switch_token = next_switch_token++;
        if (next_switch_token == 0u)
        {
            next_switch_token = 1u;
        }
        switch_request_armed = true;
        snprintf(output_buf, sizeof(output_buf), "ACK,PROFILE_SWITCH_ARMED,%u", (unsigned int)opt);
        uart_log_write(output_buf);
        return;
    }

    if (strncmp(cmd, "CFG,CHANNEL,", 12) == 0)
    {
        uint8_t channel = (uint8_t)strtoul(cmd + 12, NULL, 10);
        uint8_t opt = uwb_profile_opt_for_channel_rate_kbps(channel, 6800);
        if ((channel != 5u) && (channel != 9u))
        {
            uart_log_write("ERR,UNSUPPORTED_CHANNEL");
            return;
        }
        snprintf(output_buf, sizeof(output_buf), "ACK,CHANNEL_PROFILE,%u", (unsigned int)opt);
        uart_log_write(output_buf);
        pending_profile_opt = opt;
        pending_switch_token = next_switch_token++;
        if (next_switch_token == 0u)
        {
            next_switch_token = 1u;
        }
        switch_request_armed = (opt != current_profile_opt);
        return;
    }

    if (strncmp(cmd, "CFG,RATE,", 9) == 0)
    {
        int rate_kbps = atoi(cmd + 9);
        uint8_t channel = uwb_profile_channel_for_opt(current_profile_opt);
        uint8_t opt = uwb_profile_opt_for_channel_rate_kbps(channel, rate_kbps);
        if ((rate_kbps != 6800) && (rate_kbps != 850))
        {
            uart_log_write("ERR,UNSUPPORTED_RATE");
            return;
        }
        snprintf(output_buf, sizeof(output_buf), "ACK,RATE_PROFILE,%u", (unsigned int)opt);
        uart_log_write(output_buf);
        pending_profile_opt = opt;
        pending_switch_token = next_switch_token++;
        if (next_switch_token == 0u)
        {
            next_switch_token = 1u;
        }
        switch_request_armed = (opt != current_profile_opt);
        return;
    }

    if ((strcmp(cmd, "PYRO,FIRE") == 0) || (strcmp(cmd, "FIRE") == 0))
    {
        fire_request_frames_remaining = 200u;
        fire_request_code = POLL_MSG_FIRE_COUNTDOWN;
        uart_log_write("ACK,PYRO_FORWARD_ARMED");
        return;
    }

    if ((strcmp(cmd, "PYRO,FIRE_NOW") == 0) || (strcmp(cmd, "FIRE_NOW") == 0))
    {
        fire_request_frames_remaining = 200u;
        fire_request_code = POLL_MSG_FIRE_IMMEDIATE;
        uart_log_write("ACK,PYRO_FORWARD_NOW_ARMED");
        return;
    }

    uart_log_write("ERR,READ_ONLY_SS_TWR");
}

/*
 * Entry point for initiator firmware.
 */
int ss_twr_initiator_custom(void)
{
    test_run_info((unsigned char *)"UWB RANGING INIT v1.0");
    uart_log_init();
    uart_log_write("UWB RANGING INIT v1.0");
    uart_log_write("ROLE,INITIATOR");

    /* -- 1. Hardware init -- */
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

    /* -- 2. UWB config -- */
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
    radio_quality_enable_diagnostics();

    apply_tx_power_config();

    /* -- 3. Antenna delay -- */
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    /* -- 4. Timing: delays and timeouts -- */
    dwt_setrxaftertxdelay(active_profile->initiator_poll_tx_to_resp_rx_dly_uus);
    dwt_setrxtimeout(active_profile->initiator_resp_rx_timeout_uus);
    dwt_setpreambledetecttimeout(active_profile->pre_timeout_symbols);

    /* DWM3001CDK: LNA/PA integrated in module, controlled by DW3000 GPIO5/6 */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);

    /* LEDs for visual debugging */
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* -- Initialize accelerometer backend -- */
    accel_ok = accel_init();
    if (accel_ok) {
        test_run_info((unsigned char *)accel_backend_name());
        if (accel_backend_is_bmi323())
        {
            pyro_buzzer_beep_three_times();
        }
    } else {
        test_run_info((unsigned char *)"ACCEL FAIL");
    }

    /* CSV header on UART */
    test_run_info((unsigned char *)"# sample,distance_m,poll_tx,resp_rx,final_tx");
    uart_log_write("# ms,sample,dist,rx_power,fp_power,clock_ppm,score,nlos,peak_fp,fp_conf,sts,iax,iay,iaz,rax,ray,raz,resp_acq_ms,init_acq_ms,resp_profile_opt,init_profile_opt,igx,igy,igz,rgx,rgy,rgz,valid,dist_filt,dist_smooth,rload_mv,rload_connected");

    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;

    /* -- 5. Ranging loop -- */
    while (1)
    {
        process_app_commands();

        /* === Read accelerometer (robust) ===
         * If init fails at boot (or later), retry periodically.
         */
        uint8_t accel_decimation = test_profile_accel_decimation(active_test_profile);

        if (accel_decimation == 0u)
        {
            accel_data.x = 0;
            accel_data.y = 0;
            accel_data.z = 0;
            gyro_data.x = 0;
            gyro_data.y = 0;
            gyro_data.z = 0;
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
                    gyro_data.x = 0;
                    gyro_data.y = 0;
                    gyro_data.z = 0;
                }
                else if (!accel_read_gyro(&gyro_data))
                {
                    gyro_data.x = 0;
                    gyro_data.y = 0;
                    gyro_data.z = 0;
                }
            }
        }

        /* Encode XYZ into Poll (little-endian) */
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
        tx_poll_msg[POLL_MSG_RANGING_MODE_IDX] = RANGING_MODE_SS_TWR;
        tx_poll_msg[POLL_MSG_FIRE_IDX] = (fire_request_frames_remaining > 0u) ? fire_request_code : POLL_MSG_FIRE_NONE;
        tx_poll_msg[POLL_MSG_GYRO_X_IDX]      = (uint8_t)(gyro_data.x & 0xFF);
        tx_poll_msg[POLL_MSG_GYRO_X_IDX + 1]  = (uint8_t)((gyro_data.x >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_GYRO_Y_IDX]      = (uint8_t)(gyro_data.y & 0xFF);
        tx_poll_msg[POLL_MSG_GYRO_Y_IDX + 1]  = (uint8_t)((gyro_data.y >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_GYRO_Z_IDX]      = (uint8_t)(gyro_data.z & 0xFF);
        tx_poll_msg[POLL_MSG_GYRO_Z_IDX + 1]  = (uint8_t)((gyro_data.z >> 8) & 0xFF);
        tx_poll_msg[POLL_MSG_TX_POWER_LEVEL_IDX] = current_tx_power_level;
        if (fire_request_frames_remaining > 0u)
        {
            fire_request_frames_remaining--;
            if (fire_request_frames_remaining == 0u)
            {
                fire_request_code = POLL_MSG_FIRE_NONE;
            }
        }

        /* === TX POLL === */
        tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
        dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1); /* ranging bit = 1 */

        /* Immediate TX + auto-enable RX after delay to receive Response */
        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        /* Wait for good RX, timeout, or RX error */
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

            /* Verify this is a Response frame */
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

                /* === PREPARE FINAL TX === */

                /* Read local timestamps */
                poll_tx_ts = ranging_get_tx_timestamp_u64();
                resp_rx_ts = ranging_get_rx_timestamp_u64();

                if (RANGING_MODE_SS_TWR == 1u)
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
                        int16_t responder_accel[3] = { 0, 0, 0 };
                        int16_t responder_gyro[3] = { 0, 0, 0 };
                        uint16_t responder_load_mv = 0u;
                        uint8_t responder_load_connected = 0u;

                        ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_POLL_RX_TS_IDX], &responder_poll_rx_ts);
                        ranging_msg_get_ts(&rx_buffer[RESP_MSG_SS_RESP_TX_TS_IDX], &responder_resp_tx_ts);

                        if (frame_len > RESP_MSG_ACCEL_Z_IDX + 1)
                        {
                            responder_accel[0] = (int16_t)(rx_buffer[RESP_MSG_ACCEL_X_IDX] |
                                                  (rx_buffer[RESP_MSG_ACCEL_X_IDX + 1] << 8));
                            responder_accel[1] = (int16_t)(rx_buffer[RESP_MSG_ACCEL_Y_IDX] |
                                                  (rx_buffer[RESP_MSG_ACCEL_Y_IDX + 1] << 8));
                            responder_accel[2] = (int16_t)(rx_buffer[RESP_MSG_ACCEL_Z_IDX] |
                                                  (rx_buffer[RESP_MSG_ACCEL_Z_IDX + 1] << 8));
                        }
                        if (frame_len > RESP_MSG_GYRO_Z_IDX + 1)
                        {
                            responder_gyro[0] = (int16_t)(rx_buffer[RESP_MSG_GYRO_X_IDX] |
                                                 (rx_buffer[RESP_MSG_GYRO_X_IDX + 1] << 8));
                            responder_gyro[1] = (int16_t)(rx_buffer[RESP_MSG_GYRO_Y_IDX] |
                                                 (rx_buffer[RESP_MSG_GYRO_Y_IDX + 1] << 8));
                            responder_gyro[2] = (int16_t)(rx_buffer[RESP_MSG_GYRO_Z_IDX] |
                                                 (rx_buffer[RESP_MSG_GYRO_Z_IDX + 1] << 8));
                        }
                        if (frame_len > RESP_MSG_LOAD_CONNECTED_IDX)
                        {
                            responder_load_mv = (uint16_t)(rx_buffer[RESP_MSG_LOAD_MV_IDX] |
                                                (rx_buffer[RESP_MSG_LOAD_MV_IDX + 1] << 8));
                            responder_load_connected = rx_buffer[RESP_MSG_LOAD_CONNECTED_IDX] ? 1u : 0u;
                        }
                        rtd_init = resp_rx_ts_32 - poll_tx_ts_32;
                        reply_resp = responder_resp_tx_ts - responder_poll_rx_ts;
                        clock_offset_ratio = (float)dwt_readclockoffset() * (float)CLOCK_OFFSET_PPM_TO_RATIO;
                        tof_dtu = ((float)rtd_init - ((float)reply_resp * (1.0f - clock_offset_ratio))) / 2.0f;

                        distance = tof_dtu * (float)DWT_TIME_UNITS * (float)SPEED_OF_LIGHT;
                        ranging_count++;

                        ms = (uint32_t)(((uint64_t)NRF_RTC2->COUNTER * 1000u) / 32768u);

                        {
                            radio_quality_t radio_quality;
                            bool valid;
                            float dist_filt;
                            float dist_smooth;

                            radio_quality_read(&radio_quality);
                            valid = gate_check_distance(distance, ms);

                            if (valid)
                            {
                                filtered_distance_m = median_filter_push(distance);
                            }
                            dist_filt = filtered_distance_m;
                            dist_smooth = smooth_filter_push(dist_filt, valid);

                            write_distance_csv(ms, ranging_count, distance, &radio_quality,
                                               &accel_data, responder_accel, &gyro_data, responder_gyro,
                                               valid, dist_filt, dist_smooth,
                                               responder_load_mv, responder_load_connected);
                        }

                        if (switch_request_armed && (pending_switch_token != 0u) && (tx_poll_msg[POLL_MSG_SWITCH_TOKEN_IDX] == pending_switch_token))
                        {
                            if (apply_profile_option(pending_profile_opt) == DWT_SUCCESS)
                            {
                                current_profile_opt = pending_profile_opt;
                                switch_request_armed = false;
                                reset_distance_filters();
                                test_run_info((unsigned char *)"UWB CHANNEL SWITCHED");
                            }
                        }
                    }
                }
                else
                {

                /* Compute Final send time (delayed TX) */
                final_tx_time = (resp_rx_ts + (active_profile->initiator_resp_rx_to_final_tx_dly_uus * UUS_TO_DWT_TIME)) >> 8;
                dwt_setdelayedtrxtime(final_tx_time);

                /* Final TX timestamp = scheduled time + antenna delay */
                final_tx_ts = (((uint64_t)(final_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

                /* Encode 3 timestamps into Final message */
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
                    /* Wait until Final is sent */
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
                            reset_distance_filters();
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
                    /* Delayed TX failed (too late) - skip cycle */
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
