/*
 * main_responder_pure.c - Pure UWB responder (SS-TWR only)
 *
 * "Épuré" build: no BLE, no IMU/accel, no pyro, no load sensing,
 * no runtime profile switching, no UART commands.
 *
 * Qorvo support advice applied:
 *   - External 32 MHz HFXO started before any DW3000 access (main.c)
 *   - Zero non-UWB work in the hot loop (no UART RX polling, no sensors)
 *   - Delayed TX scheduled back-to-back as soon as the Poll is received
 *   - Response frame stripped to 18 bytes (2 timestamps only)
 *
 * Sequence per cycle:
 *   1. RX enable (immediate, no timeout)
 *   2. On Poll: read poll_rx_ts, schedule Response at
 *      poll_rx_ts + PURE_POLL_RX_TO_RESP_TX_DLY_UUS (delayed TX)
 *   3. Embed poll_rx_ts / resp_tx_ts, transmit, loop
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

/* Expected Poll header */
static const uint8_t rx_poll_hdr[ALL_MSG_COMMON_LEN] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'W', 'A',             /* Destination (responder) */
    'V', 'E',             /* Source (initiator) */
    FUNC_CODE_POLL
};

static uint8_t tx_resp_msg[PURE_RESP_MSG_LEN] = {
    0x41, 0x88,
    0,
    0xCA, 0xDE,
    'V', 'E',             /* Destination (initiator) */
    'W', 'A',             /* Source (responder) */
    FUNC_CODE_RESPONSE,
    0, 0, 0, 0,           /* [10..13] poll_rx_ts */
    0, 0, 0, 0            /* [14..17] resp_tx_ts */
};

/* -- State -- */
static uint8_t frame_seq_nb = 0;
static uint8_t rx_buffer[RX_BUF_LEN];
static uint32_t status_reg = 0;

/* -- UWB config from SDK (CMAKE_UWB_CONFIG_OPTION=35: ch5, PLEN128, 6.8M) -- */
extern dwt_config_t config_options;
extern dwt_txconfig_t txconfig_options;

extern void test_run_info(unsigned char *data);

/*
 * Entry point for the pure responder firmware.
 */
int ss_twr_responder_pure(void)
{
    uart_log_init();
    uart_log_write("UWB PURE INIT v1.0");
    uart_log_write("ROLE,RESPONDER");

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

    /* No CIA diagnostics: minimum per-frame processing in the DW3000. */
    dwt_configciadiag((uint8_t)DW_CIA_DIAG_LOG_OFF);

    dwt_configuretxrf(&txconfig_options);

    /* -- 3. Antenna delay -- */
    dwt_setrxantennadelay(RX_ANT_DLY);
    dwt_settxantennadelay(TX_ANT_DLY);

    /* DWM3001CDK: LNA/PA integrated, controlled by DW3000 GPIO5/6 */
    dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
    dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);

    /* RX: always listening, no timeout */
    dwt_setpreambledetecttimeout(0);
    dwt_setrxtimeout(0);

    /* -- 4. Hot loop: RX Poll -> delayed TX Response, nothing else -- */
    while (1)
    {
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

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

            rx_buffer[ALL_MSG_SN_IDX] = 0;
            if (memcmp(rx_buffer, rx_poll_hdr, ALL_MSG_COMMON_LEN) == 0)
            {
                uint64_t poll_rx_ts;
                uint64_t resp_tx_ts;
                uint32_t resp_tx_time;
                int ret;

                /* Back-to-back delayed TX (Qorvo advice): schedule the
                 * Response immediately, relative to the Poll RMARKER. */
                poll_rx_ts = ranging_get_rx_timestamp_u64();

                resp_tx_time = (uint32_t)((poll_rx_ts +
                    ((uint64_t)PURE_POLL_RX_TO_RESP_TX_DLY_UUS * UUS_TO_DWT_TIME)) >> 8);
                dwt_setdelayedtrxtime(resp_tx_time);
                resp_tx_ts = (((uint64_t)(resp_tx_time & 0xFFFFFFFEUL)) << 8) + TX_ANT_DLY;

                ranging_msg_set_ts(&tx_resp_msg[PURE_RESP_POLL_RX_TS_IDX], poll_rx_ts);
                ranging_msg_set_ts(&tx_resp_msg[PURE_RESP_RESP_TX_TS_IDX], resp_tx_ts);

                tx_resp_msg[ALL_MSG_SN_IDX] = frame_seq_nb;
                dwt_writetxdata(sizeof(tx_resp_msg), tx_resp_msg, 0);
                dwt_writetxfctrl(sizeof(tx_resp_msg) + FCS_LEN, 0, 1); /* ranging bit */

                ret = dwt_starttx(DWT_START_TX_DELAYED);
                if (ret == DWT_SUCCESS)
                {
                    waitforsysstatus(NULL, NULL, DWT_INT_TXFRS_BIT_MASK, 0);
                    dwt_writesysstatuslo(DWT_INT_TXFRS_BIT_MASK);
                    frame_seq_nb++;
                }
                /* On DWT_ERROR (scheduled too late) just loop: the
                 * initiator times out and retries immediately. */
            }
        }
        else
        {
            dwt_writesysstatuslo(SYS_STATUS_ALL_RX_TO | SYS_STATUS_ALL_RX_ERR);
        }
    }
}
