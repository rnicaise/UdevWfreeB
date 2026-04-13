/*
 * ranging.h — Types et constantes partagés entre initiator et responder
 *
 * Protocole DS-TWR minimal entre deux modules DW3000.
 *
 * Les constantes déjà définies par le SDK (SPEED_OF_LIGHT, UUS_TO_DWT_TIME,
 * FCS_LEN, etc.) sont dans <shared_defines.h> et <deca_device_api.h>.
 * Ici on ne définit que les constantes spécifiques à notre protocole.
 */

#ifndef RANGING_H
#define RANGING_H

#include <stdint.h>

/* ── Prototypes fonctions utilitaires ── */
uint64_t ranging_get_tx_timestamp_u64(void);
uint64_t ranging_get_rx_timestamp_u64(void);
void ranging_msg_set_ts(uint8_t *ts_field, uint64_t ts);
void ranging_msg_get_ts(const uint8_t *ts_field, uint32_t *ts);

/* ── Antenna delay par défaut (à calibrer !) ──
 * Valeur usine typique pour DW3000 @ 64 MHz PRF.
 * En production, chaque module doit être calibré individuellement.
 * Pour calibrer : placer les modules à distance connue (ex: 5m)
 * et ajuster jusqu'à ce que la mesure corresponde. */
#define TX_ANT_DLY 16385
#define RX_ANT_DLY 16385

/* ── Timing du protocole (en UWB microseconds) ── */

/* CPU processing overhead (SDK default = 400 UUS) */
#define CPU_PROCESSING_TIME 400

/* Délai entre fin TX Poll → activation RX (initiator attend Response) */
#define POLL_TX_TO_RESP_RX_DLY_UUS (300 + CPU_PROCESSING_TIME)

/* Délai entre RX Response → TX Final (initiator prépare Final) */
#define RESP_RX_TO_FINAL_TX_DLY_UUS (300 + CPU_PROCESSING_TIME)

/* Délai entre RX Poll → TX Response (responder répond) */
#define POLL_RX_TO_RESP_TX_DLY_UUS 900

/* Délai entre fin TX Response → activation RX (responder attend Final) */
#define RESP_TX_TO_FINAL_RX_DLY_UUS 500

/* Timeout réception Response (initiator) */
#define RESP_RX_TIMEOUT_UUS 300

/* Timeout réception Final (responder) */
#define FINAL_RX_TIMEOUT_UUS 220

/* Timeout détection préambule */
#define PRE_TIMEOUT 5

/* ── Période entre deux mesures (ms) ── */
#define RNG_DELAY_MS 100

/* ── Format des trames IEEE 802.15.4 ──
 *
 * Toutes les trames partagent ce format :
 *   Byte 0-1 : Frame Control (0x8841 = data frame, 16-bit addr)
 *   Byte 2   : Sequence Number (auto-incrémenté)
 *   Byte 3-4 : PAN ID (0xDECA)
 *   Byte 5-6 : Destination address
 *   Byte 7-8 : Source address
 *   Byte 9   : Function code (identifie le type de message)
 *   ...       : Payload spécifique
 *   +2 bytes  : FCS (ajouté automatiquement par le DW3000)
 */

/* Taille commune des trames (jusqu'au function code inclus) */
#define ALL_MSG_COMMON_LEN 10

/* Index du numéro de séquence dans la trame */
#define ALL_MSG_SN_IDX 2

/* Function codes pour identifier chaque message */
#define FUNC_CODE_POLL     0x21
#define FUNC_CODE_RESPONSE 0x10
#define FUNC_CODE_FINAL    0x23

/* Index des timestamps dans le message Final */
#define FINAL_MSG_POLL_TX_TS_IDX  10
#define FINAL_MSG_RESP_RX_TS_IDX  14
#define FINAL_MSG_FINAL_TX_TS_IDX 18
#define FINAL_MSG_TS_LEN          4

/* Taille max du buffer RX */
#define RX_BUF_LEN 24

#endif /* RANGING_H */
