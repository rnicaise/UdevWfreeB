#include "ble_nus_bridge.h"

#include <boards.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "deca_probe_interface.h"
#include "deca_device_api.h"
#include "deca_spi.h"
#include "nrf.h"
#include "nrf_delay.h"
#include "port.h"
#include "qio.h"

#include "../accel/accel.h"
#include "../common/radio_quality.h"
#include "../common/ranging.h"
#include "../common/uwb_profiles.h"

extern dwt_config_t config_options;

static const char *m_stage_status = "BOOT";

static void standalone_run_stage_init(void)
{
#if UWB_BLE_GATT_STANDALONE_STAGE >= 4
    uint32_t guard;

    port_set_dw_ic_spi_fastrate();

    m_stage_status = "DW_RESET";
    reset_DWIC();
    Sleep(2);

    if (dwt_probe((struct dwt_probe_s *)&dw3000_probe_interf) == DWT_ERROR)
    {
        m_stage_status = "DW_PROBE_FAIL";
        return;
    }
    m_stage_status = "DW_PROBE_OK";

    guard = 0;
    while (!dwt_checkidlerc() && (guard < 1000000u))
    {
        guard++;
    }
    if (guard >= 1000000u)
    {
        m_stage_status = "DW_IDLE_TIMEOUT";
        return;
    }
    m_stage_status = "DW_IDLE_OK";
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 5
    if (dwt_initialise(DWT_READ_OTP_ALL) == DWT_ERROR)
    {
        m_stage_status = "DW_INIT_FAIL";
        return;
    }
    m_stage_status = "DW_INIT_OK";
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 6
    if (dwt_configure(&config_options) != DWT_SUCCESS)
    {
        m_stage_status = "DW_CONFIG_FAIL";
        return;
    }
    m_stage_status = "DW_CONFIG_OK";
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 7
    {
        const uwb_runtime_profile_t *profile = uwb_profile_find(35u);
        if (profile == NULL)
        {
            m_stage_status = "DW_PROFILE_FAIL";
            return;
        }
        radio_quality_enable_diagnostics();
        dwt_setrxantennadelay(RX_ANT_DLY);
        dwt_settxantennadelay(TX_ANT_DLY);
        dwt_setrxaftertxdelay(profile->initiator_poll_tx_to_resp_rx_dly_uus);
        dwt_setrxtimeout(profile->initiator_resp_rx_timeout_uus);
        dwt_setpreambledetecttimeout(profile->pre_timeout_symbols);
        dwt_setlnapamode(DWT_LNA_ENABLE | DWT_PA_ENABLE);
        dwt_setleds(DWT_LEDS_ENABLE | DWT_LEDS_INIT_BLINK);
        m_stage_status = "DW_SETTINGS_OK";
    }
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 8
    if (!accel_init())
    {
        m_stage_status = "ACCEL_FAIL";
        return;
    }
    m_stage_status = "ACCEL_OK";
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 9
    NRF_RTC2->PRESCALER = 0;
    NRF_RTC2->TASKS_START = 1;
    m_stage_status = "RTC2_OK";
#endif
}

void test_run_info(unsigned char *data)
{
#ifdef DEBUG
    printf("%s\n", data);
#else
    (void)data;
#endif
}

static void handle_ble_command(const char *cmd)
{
    if ((strcmp(cmd, "CFG,GET_ROLE") == 0) || (strcmp(cmd, "INFO?") == 0))
    {
        ble_nus_bridge_send_line("ROLE,BLE_STANDALONE");
        return;
    }

    if (strcmp(cmd, "CFG,GET_PROFILE") == 0)
    {
        ble_nus_bridge_send_line("PROFILE,STANDALONE");
        return;
    }

    if (strcmp(cmd, "PING") == 0)
    {
        ble_nus_bridge_send_line("PONG");
        return;
    }

    if (strcmp(cmd, "DIAG,STATUS") == 0)
    {
        ble_nus_bridge_send_line(m_stage_status);
        return;
    }

    ble_nus_bridge_send_line("ERR,STANDALONE_UNKNOWN_CMD");
}

int main(void)
{
    char cmd[96];

    qio_init();
    bsp_board_init(BSP_INIT_LEDS);

#if UWB_BLE_GATT_STANDALONE_STAGE >= 1
    gpio_init();
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 2
    nrf52840_dk_spi_init();
#endif

#if UWB_BLE_GATT_STANDALONE_STAGE >= 3
    dw_irq_init();
#endif

    nrf_delay_ms(2);

    standalone_run_stage_init();

    ble_nus_bridge_init();

    while (true)
    {
        while (ble_nus_bridge_read_command(cmd, sizeof(cmd)))
        {
            handle_ble_command(cmd);
        }
        __WFE();
    }
}