#include "uwb_profiles.h"
#include "ranging.h"

static const uwb_runtime_profile_t profiles[] = {
    {
        .opt = UWB_PROFILE_OPT_6M8_STABLE,
        .data_rate_kbps = 6800,
        .config = {
            5,
            DWT_PLEN_128,
            DWT_PAC8,
            9,
            9,
            1,
            DWT_BR_6M8,
            DWT_PHRMODE_STD,
            DWT_PHRRATE_STD,
            (128 + 1 + 8 - 8),
            DWT_STS_MODE_OFF,
            DWT_STS_LEN_64,
            DWT_PDOA_M0
        },
        .initiator_poll_tx_to_resp_rx_dly_uus = POLL_TX_TO_RESP_RX_DLY_UUS,
        .initiator_resp_rx_timeout_uus = RESP_RX_TIMEOUT_UUS,
        .initiator_resp_rx_to_final_tx_dly_uus = RESP_RX_TO_FINAL_TX_DLY_UUS,
        .responder_poll_rx_to_resp_tx_dly_uus = POLL_RX_TO_RESP_TX_DLY_UUS,
        .responder_resp_tx_to_final_rx_dly_uus = RESP_TX_TO_FINAL_RX_DLY_UUS,
        .responder_final_rx_timeout_uus = FINAL_RX_TIMEOUT_UUS,
        .pre_timeout_symbols = PRE_TIMEOUT
    },
    {
        .opt = UWB_PROFILE_OPT_850K_ROBUST,
        .data_rate_kbps = 850,
        .config = {
            5,
            DWT_PLEN_1024,
            DWT_PAC32,
            9,
            9,
            1,
            DWT_BR_850K,
            DWT_PHRMODE_STD,
            DWT_PHRRATE_STD,
            (1025 + 8 - 32),
            DWT_STS_MODE_OFF,
            DWT_STS_LEN_64,
            DWT_PDOA_M0
        },
        .initiator_poll_tx_to_resp_rx_dly_uus = 900,
        .initiator_resp_rx_timeout_uus = 2500,
        .initiator_resp_rx_to_final_tx_dly_uus = 1200,
        .responder_poll_rx_to_resp_tx_dly_uus = 1800,
        .responder_resp_tx_to_final_rx_dly_uus = 900,
        .responder_final_rx_timeout_uus = 2500,
        .pre_timeout_symbols = 64
    }
};

const uwb_runtime_profile_t *uwb_profile_find(uint8_t opt)
{
    uint32_t i;
    for (i = 0; i < (sizeof(profiles) / sizeof(profiles[0])); i++)
    {
        if (profiles[i].opt == opt)
        {
            return &profiles[i];
        }
    }
    return 0;
}


uint8_t uwb_profile_opt_for_rate_kbps(int rate_kbps)
{
    if (rate_kbps <= 850)
    {
        return UWB_PROFILE_OPT_850K_ROBUST;
    }
    return UWB_PROFILE_OPT_6M8_STABLE;
}
