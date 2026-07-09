/*
 * ranging_pure.h - Minimal SS-TWR protocol for the "pure UWB" builds.
 *
 * No IMU, no pyro, no BLE, no runtime profile switching.
 * Frames are stripped to the minimum so airtime and SPI traffic stay low:
 *
 *   Poll     (initiator -> responder): 10 bytes (header only) + FCS
 *   Response (responder -> initiator): 18 bytes + FCS
 *       [10..13] poll_rx_ts  (32-bit LE, DW3000 units)
 *       [14..17] resp_tx_ts  (32-bit LE, DW3000 units)
 *
 * Timing follows the Qorvo support advice: delayed TX scheduled
 * back-to-back right after the Poll is received, targeting <=1 ms
 * full SS-TWR cycles (>=1000 Hz with PLEN128 @ 6.8 Mbps).
 */

#ifndef RANGING_PURE_H
#define RANGING_PURE_H

#include "ranging.h"

/* Response frame layout (after the 10-byte common header) */
#define PURE_RESP_POLL_RX_TS_IDX 10
#define PURE_RESP_RESP_TX_TS_IDX 14
#define PURE_RESP_MSG_LEN        18

/* Poll frame is header-only */
#define PURE_POLL_MSG_LEN        10

/* -- Pure timing (UWB microseconds), tuned for PLEN128 / 6.8 Mbps --
 *
 * Responder: delay from Poll RMARKER to Response RMARKER.
 * Budget: rest of Poll frame (~40 us) + SW turnaround (read frame,
 * read timestamp, write TX data, schedule) + Response preamble+SFD
 * (~146 us before its RMARKER). 650 uus keeps margin while staying
 * well below the previous 900 uus slot.
 */
#define PURE_POLL_RX_TO_RESP_TX_DLY_UUS 650

/* Initiator: RX enable delay after end of Poll TX.
 * Response preamble starts ~464 us after Poll TX end; enable a bit
 * earlier so the preamble hunt is already running. */
#define PURE_POLL_TX_TO_RESP_RX_DLY_UUS 440

/* Initiator: RX frame-wait timeout from RX enable. */
#define PURE_RESP_RX_TIMEOUT_UUS 400

/* Initiator: preamble detect timeout (in PAC units, PAC8). */
#define PURE_PRE_TIMEOUT_PAC 8

#endif /* RANGING_PURE_H */
