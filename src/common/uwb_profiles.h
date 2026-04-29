/*
 * uwb_profiles.h — runtime-selectable UWB radio/timing profiles.
 *
 * A profile is more than the DW3000 dataRate field: preamble length, PAC,
 * SFD timeout and protocol delays/timeouts must move together on both boards.
 */

#ifndef UWB_PROFILES_H
#define UWB_PROFILES_H

#include <stdint.h>
#include <deca_device_api.h>

#define UWB_PROFILE_OPT_6M8_STABLE  35u
#define UWB_PROFILE_OPT_850K_ROBUST 40u

typedef struct
{
    uint8_t opt;
    uint16_t data_rate_kbps;
    dwt_config_t config;

    uint16_t initiator_poll_tx_to_resp_rx_dly_uus;
    uint16_t initiator_resp_rx_timeout_uus;
    uint16_t initiator_resp_rx_to_final_tx_dly_uus;

    uint16_t responder_poll_rx_to_resp_tx_dly_uus;
    uint16_t responder_resp_tx_to_final_rx_dly_uus;
    uint16_t responder_final_rx_timeout_uus;

    uint16_t pre_timeout_symbols;
} uwb_runtime_profile_t;

const uwb_runtime_profile_t *uwb_profile_find(uint8_t opt);
uint8_t uwb_profile_opt_for_rate_kbps(int rate_kbps);

#endif /* UWB_PROFILES_H */
