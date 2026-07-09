/*
 * main_initiator_pure.c - Pure UWB initiator (SS-TWR only)
 *
 * "Épuré" build: no BLE, no IMU/accel, no pyro, no runtime profile
 * switching, no UART commands. Just the fastest possible SS-TWR
 * distance stream over UART.
 *
 * Qorvo support advice applied:
 *   - External 32 MHz HFXO started before any DW3000 access (main.c)
 *   - No unnecessary operations between measurements
 *   - Delayed/auto RX scheduled back-to-back with TX (rxaftertxdelay)
 *   - Tight <=1 ms SS-TWR cycle with PLEN128 @ 6.8 Mbps
 *   - UART output at 1 Mbps, pipelined DMA (never blocks the hot loop)
 *
 * Sequence per cycle:
 *   1. TX Poll (immediate, header-only frame) + auto RX enable
 *   2. RX Response (contains responder poll_rx/resp_tx timestamps)
 *   3. Compute distance with clock-offset correction
 *   4. Gate + median + smooth filters, CSV out (DMA, non-blocking)
 */

#include "deca_probe_interface.h"
#include <deca_device_api.h>
#include <deca_spi.h>
#include <port.h>
#include <shared_defines.h>
#include <shared_functions.h>
#include <string.h>
#include <nrf.h>

#include "../common/ranging_pure.h"
#include "../uart/uart_log.h"

/* -- Protocol frames -- */

static uint8_t tx_poll_msg[PURE_POLL_MSG_LEN] = {
    0x41, 0x88,           /* Frame Control */
    0,                    /* Sequence Number */
    0xCA, 0xDE,           /* PAN ID */
    'W', 'A',             /* Destination (responder) */
    'V', 'E',             /* Source (initiator) */
    FUNC_CODE_POLL
};

/* Expected Response header */
static const uint8_t rx_resp_hdr[ALL_MSG_COMMON_LEN] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',             /* Destination (initiator) */
    'W', 'A',             /* Source (responder) */
    FUNC_CODE_RESPONSE
};

/* -- State -- */
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;
static uint32_t ranging_count = 0;
static uint32_t rx_fail_count = 0;
static char output_buf[96];

/* -- Physical plausibility gate (unchanged from main firmware) -- */
#define GATE_MIN_DISTANCE_M    (-0.5f)
#define GATE_MAX_DISTANCE_M    (100.0f)
#define GATE_MAX_SPEED_MPS     (12.0f)
#define GATE_MARGIN_M          (0.3f)
#define GATE_RESYNC_REJECTS    (8u)

static float gate_last_valid_distance_m = 0.0f;
static uint32_t gate_last_valid_ms = 0;
static bool gate_has_baseline = false;
static uint8_t gate_consecutive_rejects = 0;

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
        gate_last_valid_distance_m = distance_m;
        gate_last_valid_ms = now_ms;
        gate_consecutive_rejects = 0;
        return true;
    }

    return false;
}

/* -- Median-of-5 filter on gate-valid distances -- */
#define MEDIAN_FILTER_LEN 5u

static float median_buf[MEDIAN_FILTER_LEN];
static uint8_t median_count = 0;
static uint8_t median_head = 0;
static bool smooth_has_baseline = false;
static float smooth_distance_m = 0.0f;
static float filtered_distance_m = 0.0f;

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

/* -- Lightweight CSV formatting (no printf in the hot loop) -- */

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

static char *append_cm(char *dst, float meters)
{
    float cm_f = meters * 100.0f;
    int32_t cm = (int32_t)(cm_f + ((cm_f >= 0.0f) ? 0.5f : -0.5f));
    uint32_t magnitude;

    if (cm < 0)
    {
        *dst++ = '-';
        magnitude = (uint32_t)(-cm);
    }
    else
    {
        magnitude = (uint32_t)cm;
    }

    dst = append_u32(dst, magnitude / 100u);
    *dst++ = '.';
    *dst++ = (char)('0' + ((magnitude / 10u) % 10u));
    *dst++ = (char)('0' + (magnitude % 10u));

    return dst;
}

static char *append_ppm_x100(char *dst, int32_t ppm_x100)
{
    uint32_t magnitude;

    if (ppm_x100 < 0)
    {
        *dst++ = '-';
        magnitude = (uint32_t)(-ppm_x100);
    }
    else
    {
        magnitude = (uint32_t)ppm_x100;
    }

    dst = append_u32(dst, magnitude / 100u);
    *dst++ = '.';
    *dst++ = (char)('0' + ((magnitude / 10u) % 10u));
    *dst++ = (char)('0' + (magnitude % 10u));

    return dst;
}

/* -- UWB config from SDK (CMAKE_UWB_CONFIG_OPTION=35: ch5, PLEN128, 6.8M) -- */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;

extern void test_run_info(unsigned char *data);

/*
 * Entry point for the pure initiator firmware.
 */
int ss_twr_initiator_pure(void)
{
    uart_log_init();
    uart_log_write("UWB PURE INIT v1.0");
    uart_log_write("ROLE,INITIATOR");

    /* -- 1. DW3000 init -- */
    port_set_dw_ic_spi_fastrate();

    reset_DWIC();
    Sleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        uart_log_write("ERR,PROBE_FAILED");
        while (1) { };
    }

    while (!dwt_checkidlerc()) { };

    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        uart_log_write("ERR,INIT_FAILED");
        while (1) { };
    }

    /* -- 2. UWB config -- */
    if (dwt_configure(&config_options))
    {
        uart_log_write("ERR,CONFIG_FAILED");
        while (1) { };
    }

    dwt_configuretxrf(&txconfig_options);

    /* -- 3. Antenna delay -- */
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    /* -- 4. Back-to-back timing: auto RX after Poll TX -- */
    dwt_setrxaftertxdelay(PURE_POLL_TX_TO_RESP_RX_DLY_UUS);
    dwt_setrxtimeout(PURE_RESP_RX_TIMEOUT_UUS);
    dwt_setpreambledetecttimeout(PURE_PRE_TIMEOUT_PAC);

    /* DWM3001CDK: LNA/PA integrated, controlled by DW3000 GPIO5/6 */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* Millisecond timebase */
    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;

    uart_log_write("# ms,sample,dist,cppm,valid,dist_filt,dist_smooth,rx_fail");

    /* -- 5. Ranging loop: nothing but UWB + buffered UART -- */
    while (1)
    {
        /* === TX POLL (immediate) + auto RX enable === */
        tx_poll_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
        dwt_writetxdata(sizeof(tx_poll_msg), tx_poll_msg, 0);
        dwt_writetxfctrl(sizeof(tx_poll_msg) + FCS_LEN, 0, 1); /* ranging bit */

        dwt_starttx(DWT_START_TX_IMMEDIATE | DWT_RESPONSE_EXPECTED);

        waitforsysstatus(&status_reg, NULL,
            (DWT_INT_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR), 0);

        frame_seq_nb++;

        if (status_reg & DWT_INT_RXFCG_BIT_MASK)
        {
            uint16_t frame_len;

            dwt_writesysstatuslo(DWT_INT_RXFCG_BIT_MASK | DWT_INT_TXFRS_BIT_MASK);

            frame_len = dwt_getframelength(0);
            if (frame_len <= RX_BUF_LEN)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }

            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if ((frame_len >= (PURE_RESP_MSG_LEN + FCS_LEN)) &&
                (memcmp(rx_buffer, rx_resp_hdr, ALL_MSG_COMMON_LEN) == 0))
            {
                uint32_t poll_tx_ts_32;
                uint32_t resp_rx_ts_32;
                uint32_t responder_poll_rx_ts;
                uint32_t responder_resp_tx_ts;
                uint32_t rtd_init;
                uint32_t reply_resp;
                int16_t clock_offset_raw;
                float clock_offset_ratio;
                float tof_dtu;
                float distance;
                uint32_t ms;
                bool valid;

                poll_tx_ts_32 = (uint32_t)ranging_get_tx_timestamp_u64();
                resp_rx_ts_32 = (uint32_t)ranging_get_rx_timestamp_u64();

                ranging_msg_get_ts(&rx_buffer[PURE_RESP_POLL_RX_TS_IDX], &responder_poll_rx_ts);
                ranging_msg_get_ts(&rx_buffer[PURE_RESP_RESP_TX_TS_IDX], &responder_resp_tx_ts);

                rtd_init = resp_rx_ts_32 - poll_tx_ts_32;
                reply_resp = responder_resp_tx_ts - responder_poll_rx_ts;
                clock_offset_raw = dwt_readclockoffset();
                clock_offset_ratio = (float)clock_offset_raw * (float)CLOCK_OFFSET_PPM_TO_RATIO;
                tof_dtu = ((float)rtd_init - ((float)reply_resp * (1.0f - clock_offset_ratio))) / 2.0f;

                distance = tof_dtu * (float)DWT_TIME_UNITS * (float)SPEED_OF_LIGHT;
                ranging_count++;

                ms = (uint32_t)(((uint64_t)NRF_RTC2->COUNTER * 1000u) / 32768u);

                valid = gate_check_distance(distance, ms);
                if (valid)
                {
                    filtered_distance_m = median_filter_push(distance);
                }
                smooth_distance_m = smooth_filter_push(filtered_distance_m, valid);

                {
                    /* clock offset in ppm*100 (raw * ratio * 1e6 * 100) */
                    float ppm = (float)clock_offset_raw * (float)CLOCK_OFFSET_PPM_TO_RATIO * 1000000.0f;
                    int32_t ppm_x100 = (int32_t)((ppm * 100.0f) + ((ppm >= 0.0f) ? 0.5f : -0.5f));
                    char *dst = output_buf;

                    dst = append_u32(dst, ms);
                    *dst++ = ',';
                    dst = append_u32(dst, ranging_count);
                    *dst++ = ',';
                    dst = append_cm(dst, distance);
                    *dst++ = ',';
                    dst = append_ppm_x100(dst, ppm_x100);
                    *dst++ = ',';
                    *dst++ = valid ? '1' : '0';
                    *dst++ = ',';
                    dst = append_cm(dst, filtered_distance_m);
                    *dst++ = ',';
                    dst = append_cm(dst, smooth_distance_m);
                    *dst++ = ',';
                    dst = append_u32(dst, rx_fail_count);
                    *dst = '\0';

                    /* Pipelined DMA: returns immediately, TX overlaps
                     * the next UWB exchange. */
                    uart_log_write(output_buf);
                }
            }
            else
            {
                rx_fail_count++;
            }
        }
        else
        {
            rx_fail_count++;
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }
    }
}
