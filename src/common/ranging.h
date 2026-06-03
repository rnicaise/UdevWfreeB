/*
 * ranging.h - Shared types and constants for initiator and responder
 *
 * Minimal DS-TWR protocol between two DW3000 modules.
 *
 * SDK-defined constants (SPEED_OF_LIGHT, UUS_TO_DWT_TIME, FCS_LEN, etc.)
 * are provided by <shared_defines.h> and <deca_device_api.h>.
 * This file only defines protocol-specific constants.
 */

#ifndef RANGING_H
#define RANGING_H

#include <stdint.h>

/* -- Utility function prototypes -- */
uint64_t ranging_get_tx_timestamp_u64(void);
uint64_t ranging_get_rx_timestamp_u64(void);
void ranging_msg_set_ts(uint8_t *ts_field, uint64_t ts);
void ranging_msg_get_ts(const uint8_t *ts_field, uint32_t *ts);

/* -- Default antenna delay (must be calibrated) --
 * Typical factory value for DW3000 @ 64 MHz PRF.
 * In production, each module should be calibrated individually.
 * Calibration method: place modules at known distance (e.g. 5m)
 * and adjust until the measured distance matches. */
#define TX_ANT_DLY 16385
#define RX_ANT_DLY 16385

/* -- Protocol timing (in UWB microseconds) -- */

/* CPU processing overhead (SDK default = 400 UUS) */
#define CPU_PROCESSING_TIME 400

/* Delay from Poll TX end -> RX enable (initiator waits for Response) */
#define POLL_TX_TO_RESP_RX_DLY_UUS (300 + CPU_PROCESSING_TIME)

/* Delay from Response RX -> Final TX (initiator prepares Final) */
#define RESP_RX_TO_FINAL_TX_DLY_UUS (300 + CPU_PROCESSING_TIME)

/* Delay from Poll RX -> Response TX (responder replies) */
#define POLL_RX_TO_RESP_TX_DLY_UUS 900

/* SS-TWR delay: must remain after initiator RX opens. */
#define SS_POLL_RX_TO_RESP_TX_DLY_UUS 900

/* Delay from Response TX end -> RX enable (responder waits for Final) */
#define RESP_TX_TO_FINAL_RX_DLY_UUS 500

/* Response RX timeout (initiator) */
#define RESP_RX_TIMEOUT_UUS 300

/* Final RX timeout (responder) */
#define FINAL_RX_TIMEOUT_UUS 220

/* Preamble detection timeout */
#define PRE_TIMEOUT 5

/* -- Period between two measurements (ms) -- */
#define RNG_DELAY_MS 1

/* -- Runtime test profiles (non-PHY, safe for live switching) -- */
#define UWB_TEST_PROFILE_FAST_DISTANCE_ONLY  0u
#define UWB_TEST_PROFILE_FAST_ACCEL_DECIMATED 1u
#define UWB_TEST_PROFILE_STABLE_FULL         2u
#define UWB_TEST_PROFILE_ROBUST_DETECTION    3u
#define UWB_TEST_PROFILE_DIAGNOSTICS_FULL    4u
#define UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY 5u
#define UWB_TEST_PROFILE_DEFAULT UWB_TEST_PROFILE_TURBO_DISTANCE_ONLY

/* -- IEEE 802.15.4 frame format --
 *
 * All frames share this format:
 *   Byte 0-1 : Frame Control (0x8841 = data frame, 16-bit addr)
 *   Byte 2   : Sequence Number (auto-incremented)
 *   Byte 3-4 : PAN ID (0xDECA)
 *   Byte 5-6 : Destination address
 *   Byte 7-8 : Source address
 *   Byte 9   : Function code (identifies message type)
 *   ...       : Message-specific payload
 *   +2 bytes  : FCS (added automatically by DW3000)
 */

/* Common frame size (up to and including function code) */
#define ALL_MSG_COMMON_LEN 10

/* Sequence number index in frame */
#define ALL_MSG_SN_IDX 2

/* Function codes used to identify each message */
#define FUNC_CODE_POLL     0x21
#define FUNC_CODE_RESPONSE 0x10
#define FUNC_CODE_FINAL    0x23

/* Timestamp field indices in Final message */
#define FINAL_MSG_POLL_TX_TS_IDX  10
#define FINAL_MSG_RESP_RX_TS_IDX  14
#define FINAL_MSG_FINAL_TX_TS_IDX 18
#define FINAL_MSG_TS_LEN          4

/* Accelerometer field indices in Poll message */
#define POLL_MSG_ACCEL_X_IDX  10
#define POLL_MSG_ACCEL_Y_IDX  12
#define POLL_MSG_ACCEL_Z_IDX  14

/* Maximum RX buffer size */
#define RX_BUF_LEN 30

#endif /* RANGING_H */
