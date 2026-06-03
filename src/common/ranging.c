/*
 * ranging.c - Shared utility functions for initiator and responder
 */

#include "ranging.h"
#include <deca_device_api.h>
#include <string.h>

/*
 * Read a 64-bit TX timestamp from DW3000.
 * DW3000 stores timestamps on 40 bits (5 bytes).
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
 * Read a 64-bit RX timestamp from DW3000.
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
 * Write a 32-bit timestamp into a message buffer (little-endian).
 * Used to encode timestamps in the Final message.
 */
void ranging_msg_set_ts(uint8_t *ts_field, uint64_t ts)
{
    ts_field[0] = (uint8_t)ts;
    ts_field[1] = (uint8_t)(ts >> 8);
    ts_field[2] = (uint8_t)(ts >> 16);
    ts_field[3] = (uint8_t)(ts >> 24);
}

/*
 * Read a 32-bit timestamp from a message buffer (little-endian).
 * Used to decode timestamps received in the Final message.
 */
void ranging_msg_get_ts(const uint8_t *ts_field, uint32_t *ts)
{
    *ts = 0;
    *ts |= (uint32_t)ts_field[0];
    *ts |= (uint32_t)ts_field[1] << 8;
    *ts |= (uint32_t)ts_field[2] << 16;
    *ts |= (uint32_t)ts_field[3] << 24;
}
