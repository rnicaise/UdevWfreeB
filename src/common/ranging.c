/*
 * ranging.c — Fonctions utilitaires partagées entre initiator et responder
 */

#include "ranging.h"
#include <deca_device_api.h>
#include <string.h>

/*
 * Lire un timestamp TX 64-bit depuis le DW3000.
 * Le DW3000 stocke les timestamps sur 40 bits (5 octets).
 */
uint64_t ranging_get_tx_timestamp_u64(void)
{
    uint8_t ts_tab[5];
    uint64_t ts = 0;

    dwt_readtxtimestamp(ts_tab);
    for (int i = 4; i >= 0; i--)
    {
        ts <<= 8;
        ts |= ts_tab[i];
    }
    return ts;
}

/*
 * Lire un timestamp RX 64-bit depuis le DW3000.
 */
uint64_t ranging_get_rx_timestamp_u64(void)
{
    uint8_t ts_tab[5];
    uint64_t ts = 0;

    dwt_readrxtimestamp(ts_tab, 0);
    for (int i = 4; i >= 0; i--)
    {
        ts <<= 8;
        ts |= ts_tab[i];
    }
    return ts;
}

/*
 * Écrire un timestamp 32-bit dans un buffer de message (little-endian).
 * Utilisé pour encoder les timestamps dans le message Final.
 */
void ranging_msg_set_ts(uint8_t *ts_field, uint64_t ts)
{
    ts_field[0] = (uint8_t)ts;
    ts_field[1] = (uint8_t)(ts >> 8);
    ts_field[2] = (uint8_t)(ts >> 16);
    ts_field[3] = (uint8_t)(ts >> 24);
}

/*
 * Lire un timestamp 32-bit depuis un buffer de message (little-endian).
 * Utilisé pour décoder les timestamps reçus dans le message Final.
 */
void ranging_msg_get_ts(const uint8_t *ts_field, uint32_t *ts)
{
    *ts = 0;
    *ts |= (uint32_t)ts_field[0];
    *ts |= (uint32_t)ts_field[1] << 8;
    *ts |= (uint32_t)ts_field[2] << 16;
    *ts |= (uint32_t)ts_field[3] << 24;
}
