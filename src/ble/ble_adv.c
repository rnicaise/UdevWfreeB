/*
 * ble_adv.c — BLE Advertising via RADIO peripheral (no SoftDevice)
 *
 * Utilise directement le périphérique RADIO du nRF52833 pour envoyer
 * des paquets BLE advertising. Pas besoin de SoftDevice.
 *
 * Le paquet contient :
 *   - Nom local : "UWB"
 *   - Manufacturer Specific Data : distance (cm) + compteur
 *
 * Visible sur n'importe quel scanner BLE (nRF Connect, LightBlue, etc.)
 */

#include "ble_adv.h"
#include <nrf.h>
#include <string.h>

/* ── BLE Advertising constants ── */

/* Access address for BLE advertising */
#define BLE_ADV_ACCESS_ADDR  0x8E89BED6

/* CRC polynomial for BLE */
#define BLE_CRC_POLY         0x00065B

/* CRC init for advertising */
#define BLE_CRC_INIT         0x555555

/* Advertising channels: freq = 2400 + value (MHz) */
#define BLE_ADV_CH37_FREQ    2   /* 2402 MHz */
#define BLE_ADV_CH38_FREQ    26  /* 2426 MHz */
#define BLE_ADV_CH39_FREQ    80  /* 2480 MHz */

/* Data whitening IV for each channel */
#define BLE_ADV_CH37_WHITEIV 37
#define BLE_ADV_CH38_WHITEIV 38
#define BLE_ADV_CH39_WHITEIV 39

/* TX power: 0 dBm (conserve battery, sufficient for nearby scanner) */
#define BLE_TX_POWER         0

/* ── PDU layout ──
 *
 * ADV_NONCONN_IND PDU (not connectable, not scannable):
 *   [0]    PDU header: type=0x02 (ADV_NONCONN_IND), TxAdd=1 (random addr)
 *   [1]    PDU length (payload bytes)
 *   [2-7]  AdvA: 6-byte random static address
 *   [8..]  AD structures
 */

/* Random static address (bit 7:6 of last byte = 0b11 per BLE spec) */
static const uint8_t adv_addr[6] = { 0x01, 0xCD, 0x3C, 0xAA, 0xBE, 0xC6 };

/* Device name */
#define ADV_NAME "UWB"
#define ADV_NAME_LEN 3

/* Max PDU payload: 37 bytes (6 AdvA + 31 AD data) */
#define ADV_PDU_MAX 39 /* 2 header + 37 payload */

/* Packet buffer (aligned for RADIO DMA) */
static uint8_t adv_pdu[ADV_PDU_MAX] __attribute__((aligned(4)));

/* Manufacturer specific data offset within AD structures */
static uint8_t *mfr_data_ptr;

/* ── Internal helpers ── */

static void radio_configure(void)
{
    /* Mode: BLE 1 Mbit/s */
    NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;

    /* Packet configuration for BLE:
     *   S0 = 1 byte (PDU header byte 0)
     *   LENGTH field = 8 bits (PDU header byte 1)
     *   S1 = 0
     *   Preamble = 8 bits (BLE 1M)
     */
    NRF_RADIO->PCNF0 =
        (1    << RADIO_PCNF0_S0LEN_Pos)  |
        (8    << RADIO_PCNF0_LFLEN_Pos)  |
        (0    << RADIO_PCNF0_S1LEN_Pos)  |
        (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);

    /* Packet configuration 1:
     *   Max payload = 37 bytes
     *   Base address = 3 bytes (+ 1 prefix = 4 total for access address)
     *   Big-endian on air (BLE standard)
     *   Data whitening enabled
     */
    NRF_RADIO->PCNF1 =
        (37   << RADIO_PCNF1_MAXLEN_Pos)  |
        (0    << RADIO_PCNF1_STATLEN_Pos) |
        (3    << RADIO_PCNF1_BALEN_Pos)   |
        (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos) |
        (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);

    /* Access address for BLE advertising:
     * BASE0 = lower 3 bytes (shifted left), PREFIX0.AP0 = top byte
     */
    NRF_RADIO->BASE0   = (BLE_ADV_ACCESS_ADDR << 8) & 0xFFFFFF00;
    NRF_RADIO->PREFIX0 = (BLE_ADV_ACCESS_ADDR >> 24) & 0xFF;
    NRF_RADIO->TXADDRESS = 0; /* Use logical address 0 (BASE0 + PREFIX0.AP0) */

    /* CRC: 3 bytes, BLE polynomial, skip access address */
    NRF_RADIO->CRCCNF  = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                          (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCPOLY = BLE_CRC_POLY;
    NRF_RADIO->CRCINIT = BLE_CRC_INIT;

    /* TX power */
    NRF_RADIO->TXPOWER = BLE_TX_POWER << RADIO_TXPOWER_TXPOWER_Pos;

    /* Point to packet buffer */
    NRF_RADIO->PACKETPTR = (uint32_t)adv_pdu;
}

static void build_adv_pdu(void)
{
    uint8_t *p = adv_pdu;
    uint8_t *ad_start;

    /* PDU Header */
    *p++ = 0x42; /* ADV_NONCONN_IND (0x02) | TxAdd=1 (random addr) */
    *p++ = 0;    /* Length — filled at the end */

    /* AdvA: advertiser address (6 bytes) */
    memcpy(p, adv_addr, 6);
    p += 6;

    ad_start = p;

    /* AD Structure 1: Flags */
    *p++ = 2;    /* Length */
    *p++ = 0x01; /* Type: Flags */
    *p++ = 0x06; /* LE General Discoverable + BR/EDR Not Supported */

    /* AD Structure 2: Complete Local Name */
    *p++ = ADV_NAME_LEN + 1; /* Length */
    *p++ = 0x09;              /* Type: Complete Local Name */
    memcpy(p, ADV_NAME, ADV_NAME_LEN);
    p += ADV_NAME_LEN;

    /* AD Structure 3: Manufacturer Specific Data */
    *p++ = 7;    /* Length: 1 type + 2 company + 2 dist + 2 count */
    *p++ = 0xFF; /* Type: Manufacturer Specific Data */
    *p++ = 0xFF; /* Company ID low (0xFFFF = test/development) */
    *p++ = 0xFF; /* Company ID high */
    mfr_data_ptr = p;  /* Save pointer for runtime updates */
    *p++ = 0;    /* Distance cm low */
    *p++ = 0;    /* Distance cm high */
    *p++ = 0;    /* Count low */
    *p++ = 0;    /* Count high */

    /* Set PDU length (everything after the header: AdvA + AD data) */
    adv_pdu[1] = (uint8_t)(p - &adv_pdu[2]);
}

static void radio_tx_on_channel(uint8_t freq, uint8_t whiteiv)
{
    NRF_RADIO->FREQUENCY  = freq;
    NRF_RADIO->DATAWHITEIV = whiteiv;

    /* Clear events */
    NRF_RADIO->EVENTS_READY = 0;
    NRF_RADIO->EVENTS_END   = 0;
    NRF_RADIO->EVENTS_DISABLED = 0;

    /* Ramp-up TX */
    NRF_RADIO->TASKS_TXEN = 1;
    while (NRF_RADIO->EVENTS_READY == 0) { }

    /* Start TX */
    NRF_RADIO->TASKS_START = 1;
    while (NRF_RADIO->EVENTS_END == 0) { }

    /* Disable */
    NRF_RADIO->TASKS_DISABLE = 1;
    while (NRF_RADIO->EVENTS_DISABLED == 0) { }
}

/* ── Public API ── */

void ble_adv_init(void)
{
    /* Start HFXO (16 MHz crystal) — required for RADIO */
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;
    while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0) { }

    radio_configure();
    build_adv_pdu();
}

void ble_adv_update(float distance_m, uint32_t count)
{
    int16_t dist_cm = (int16_t)(distance_m * 100.0f);
    uint16_t cnt    = (uint16_t)(count & 0xFFFF);

    /* Update manufacturer data in-place (little-endian) */
    mfr_data_ptr[0] = (uint8_t)(dist_cm & 0xFF);
    mfr_data_ptr[1] = (uint8_t)((dist_cm >> 8) & 0xFF);
    mfr_data_ptr[2] = (uint8_t)(cnt & 0xFF);
    mfr_data_ptr[3] = (uint8_t)((cnt >> 8) & 0xFF);
}

void ble_adv_send(void)
{
    /* Transmit on all 3 advertising channels */
    radio_tx_on_channel(BLE_ADV_CH37_FREQ, BLE_ADV_CH37_WHITEIV);
    radio_tx_on_channel(BLE_ADV_CH38_FREQ, BLE_ADV_CH38_WHITEIV);
    radio_tx_on_channel(BLE_ADV_CH39_FREQ, BLE_ADV_CH39_WHITEIV);
}
