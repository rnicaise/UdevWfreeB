#include "ble_nus_bridge.h"

#include <stdint.h>
#include <string.h>

#include "app_error.h"
#include "ble.h"
#include "ble_advdata.h"
#include "ble_conn_state.h"
#include "ble_gap.h"
#include "ble_hci.h"
#include "ble_nus.h"
#include "ble_srv_common.h"
#include "nrf_ble_gatt.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_soc.h"

#define APP_BLE_CONN_CFG_TAG 1u
#define APP_BLE_OBSERVER_PRIO 3u
#define DEVICE_NAME "UWB"
#define DEVICE_NAME_MAX_LEN 15u
#define NUS_SERVICE_UUID_TYPE BLE_UUID_TYPE_VENDOR_BEGIN
#define APP_ADV_INTERVAL 160u
#define MIN_CONN_INTERVAL MSEC_TO_UNITS(20, UNIT_1_25_MS)
#define MAX_CONN_INTERVAL MSEC_TO_UNITS(75, UNIT_1_25_MS)
#define SLAVE_LATENCY 0u
#define CONN_SUP_TIMEOUT MSEC_TO_UNITS(4000, UNIT_10_MS)
#define BLE_ADV_HANDLE BLE_GAP_ADV_SET_HANDLE_NOT_SET
#define COMMAND_QUEUE_LEN 4u
#define COMMAND_MAX_LEN 96u

BLE_NUS_DEF(m_nus, NRF_SDH_BLE_TOTAL_LINK_COUNT);
NRF_BLE_GATT_DEF(m_gatt);

static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint16_t m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - 3u;
static char m_device_name[DEVICE_NAME_MAX_LEN + 1u] = DEVICE_NAME;
static bool m_notifications_enabled = false;
static bool m_advertising_enabled = true;
static bool m_advertising_active = false;
static uint8_t m_adv_handle = BLE_ADV_HANDLE;
static uint8_t m_enc_advdata[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint8_t m_enc_scan_response_data[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static char m_rx_line[COMMAND_MAX_LEN];
static uint8_t m_rx_line_len = 0u;
static char m_command_queue[COMMAND_QUEUE_LEN][COMMAND_MAX_LEN];
static uint8_t m_command_read = 0u;
static uint8_t m_command_write = 0u;
static uint8_t m_command_count = 0u;

static ble_uuid_t m_adv_uuids[] =
{
    {BLE_UUID_NUS_SERVICE, NUS_SERVICE_UUID_TYPE}
};

static void command_queue_push(const char *cmd)
{
    size_t len = strlen(cmd);
    if (len == 0u)
    {
        return;
    }

    memcpy(m_command_queue[m_command_write], cmd, len + 1u);
    m_command_write = (uint8_t)((m_command_write + 1u) % COMMAND_QUEUE_LEN);
    if (m_command_count < COMMAND_QUEUE_LEN)
    {
        m_command_count++;
    }
    else
    {
        m_command_read = (uint8_t)((m_command_read + 1u) % COMMAND_QUEUE_LEN);
    }
}

static void rx_byte(uint8_t byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if (m_rx_line_len > 0u)
        {
            m_rx_line[m_rx_line_len] = '\0';
            command_queue_push(m_rx_line);
            m_rx_line_len = 0u;
        }
        return;
    }

    if (m_rx_line_len < (COMMAND_MAX_LEN - 1u))
    {
        m_rx_line[m_rx_line_len++] = (char)byte;
    }
    else
    {
        m_rx_line_len = 0u;
    }
}

static void gap_params_init(void)
{
    uint32_t err_code;
    ble_gap_conn_params_t gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode, (const uint8_t *)m_device_name, strlen(m_device_name));
    APP_ERROR_CHECK(err_code);

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));
    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

static void gatt_evt_handler(nrf_ble_gatt_t *p_gatt, nrf_ble_gatt_evt_t const *p_evt)
{
    if ((p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED) && (p_evt->conn_handle == m_conn_handle))
    {
        m_ble_nus_max_data_len = (uint16_t)(p_evt->params.att_mtu_effective - 3u);
    }
    (void)p_gatt;
}

static void gatt_init(void)
{
    uint32_t err_code = nrf_ble_gatt_init(&m_gatt, gatt_evt_handler);
    APP_ERROR_CHECK(err_code);
}

static void nus_data_handler(ble_nus_evt_t *p_evt)
{
    if (p_evt->type == BLE_NUS_EVT_RX_DATA)
    {
        for (uint16_t i = 0u; i < p_evt->params.rx_data.length; i++)
        {
            rx_byte(p_evt->params.rx_data.p_data[i]);
        }
    }
    else if (p_evt->type == BLE_NUS_EVT_COMM_STARTED)
    {
        m_notifications_enabled = true;
    }
    else if (p_evt->type == BLE_NUS_EVT_COMM_STOPPED)
    {
        m_notifications_enabled = false;
    }
}

static void services_init(void)
{
    ble_nus_init_t nus_init;
    memset(&nus_init, 0, sizeof(nus_init));
    nus_init.data_handler = nus_data_handler;

    uint32_t err_code = ble_nus_init(&m_nus, &nus_init);
    APP_ERROR_CHECK(err_code);
}

static void advertising_init(void)
{
    uint32_t err_code;
    ble_advdata_t advdata;
    ble_advdata_t scanrsp;
    ble_gap_adv_params_t adv_params;
    ble_gap_adv_data_t adv_data;

    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type = BLE_ADVDATA_FULL_NAME;
    advdata.include_appearance = false;
    advdata.flags = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

    memset(&scanrsp, 0, sizeof(scanrsp));
    scanrsp.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    scanrsp.uuids_complete.p_uuids = m_adv_uuids;

    memset(&adv_data, 0, sizeof(adv_data));
    adv_data.adv_data.p_data = m_enc_advdata;
    adv_data.adv_data.len = BLE_GAP_ADV_SET_DATA_SIZE_MAX;
    adv_data.scan_rsp_data.p_data = m_enc_scan_response_data;
    adv_data.scan_rsp_data.len = BLE_GAP_ADV_SET_DATA_SIZE_MAX;

    err_code = ble_advdata_encode(&advdata, adv_data.adv_data.p_data, &adv_data.adv_data.len);
    APP_ERROR_CHECK(err_code);

    err_code = ble_advdata_encode(&scanrsp, adv_data.scan_rsp_data.p_data, &adv_data.scan_rsp_data.len);
    APP_ERROR_CHECK(err_code);

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    adv_params.p_peer_addr = NULL;
    adv_params.filter_policy = BLE_GAP_ADV_FP_ANY;
    adv_params.interval = APP_ADV_INTERVAL;
    adv_params.duration = 0;

    err_code = sd_ble_gap_adv_set_configure(&m_adv_handle, &adv_data, &adv_params);
    APP_ERROR_CHECK(err_code);
}

static void advertising_start(void)
{
    if (!m_advertising_enabled)
    {
        return;
    }

    uint32_t err_code = sd_ble_gap_adv_start(m_adv_handle, APP_BLE_CONN_CFG_TAG);
    if (err_code == NRF_ERROR_INVALID_STATE)
    {
        m_advertising_active = false;
        return;
    }
    APP_ERROR_CHECK(err_code);
    m_advertising_active = true;
}

static void advertising_stop(void)
{
    uint32_t err_code;

    if (!m_advertising_active)
    {
        return;
    }

    err_code = sd_ble_gap_adv_stop(m_adv_handle);
    if ((err_code != NRF_SUCCESS) && (err_code != NRF_ERROR_INVALID_STATE))
    {
        APP_ERROR_CHECK(err_code);
    }
    m_advertising_active = false;
}

static void ble_evt_handler(ble_evt_t const *p_ble_evt, void *p_context)
{
    (void)p_context;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            m_advertising_active = false;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            m_notifications_enabled = false;
            advertising_start();
            break;

        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            ble_gap_phys_t phys = {BLE_GAP_PHY_AUTO, BLE_GAP_PHY_AUTO};
            uint32_t err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
            break;
        }

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
        {
            uint32_t err_code = sd_ble_gatts_sys_attr_set(m_conn_handle, NULL, 0, 0);
            APP_ERROR_CHECK(err_code);
            break;
        }

        default:
            break;
    }
}

NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);

void ble_nus_bridge_set_device_name(const char *name)
{
    if ((name == NULL) || (name[0] == '\0'))
    {
        return;
    }
    strncpy(m_device_name, name, DEVICE_NAME_MAX_LEN);
    m_device_name[DEVICE_NAME_MAX_LEN] = '\0';
}

void ble_nus_bridge_init(void)
{
    uint32_t ram_start = 0u;
    uint32_t err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    gap_params_init();
    gatt_init();
    services_init();
    advertising_init();
    advertising_start();
}

bool ble_nus_bridge_is_connected(void)
{
    return m_conn_handle != BLE_CONN_HANDLE_INVALID;
}

bool ble_nus_bridge_is_client_ready(void)
{
    return (m_conn_handle != BLE_CONN_HANDLE_INVALID) && m_notifications_enabled;
}

bool ble_nus_bridge_is_advertising_enabled(void)
{
    return m_advertising_enabled;
}

bool ble_nus_bridge_read_command(char *dst, size_t dst_size)
{
    if ((dst == NULL) || (dst_size == 0u) || (m_command_count == 0u))
    {
        return false;
    }

    strncpy(dst, m_command_queue[m_command_read], dst_size - 1u);
    dst[dst_size - 1u] = '\0';
    m_command_read = (uint8_t)((m_command_read + 1u) % COMMAND_QUEUE_LEN);
    m_command_count--;
    return true;
}

void ble_nus_bridge_send_line(const char *line)
{
    if ((line == NULL) || (m_conn_handle == BLE_CONN_HANDLE_INVALID) || !m_notifications_enabled)
    {
        return;
    }

    uint8_t *data = (uint8_t *)line;
    size_t remaining = strlen(line);
    while (remaining > 0u)
    {
        uint16_t chunk_len = (remaining > m_ble_nus_max_data_len) ? m_ble_nus_max_data_len : (uint16_t)remaining;
        uint32_t err_code = ble_nus_data_send(&m_nus, data, &chunk_len, m_conn_handle);
        if ((err_code == NRF_ERROR_RESOURCES) || (err_code == NRF_ERROR_BUSY) || (err_code == NRF_ERROR_INVALID_STATE))
        {
            return;
        }
        APP_ERROR_CHECK(err_code);
        data += chunk_len;
        remaining -= chunk_len;
    }

    uint8_t newline = '\n';
    uint16_t newline_len = 1u;
    (void)ble_nus_data_send(&m_nus, &newline, &newline_len, m_conn_handle);
}

bool ble_nus_bridge_send_line_wait(const char *line)
{
    static const uint8_t newline = '\n';
    const uint8_t *data = (const uint8_t *)line;
    size_t remaining;
    bool newline_sent = false;

    if (line == NULL)
    {
        return false;
    }
    remaining = strlen(line);

    while (!newline_sent)
    {
        const uint8_t *src;
        uint16_t chunk_len;
        uint32_t err_code;

        if ((m_conn_handle == BLE_CONN_HANDLE_INVALID) || !m_notifications_enabled)
        {
            return false;
        }

        if (remaining > 0u)
        {
            src = data;
            chunk_len = (remaining > m_ble_nus_max_data_len) ? m_ble_nus_max_data_len : (uint16_t)remaining;
        }
        else
        {
            src = &newline;
            chunk_len = 1u;
        }

        err_code = ble_nus_data_send(&m_nus, (uint8_t *)src, &chunk_len, m_conn_handle);
        if ((err_code == NRF_ERROR_RESOURCES) || (err_code == NRF_ERROR_BUSY))
        {
            /* TX buffers full: sleep until the next BLE event frees one. */
            (void)sd_app_evt_wait();
            continue;
        }
        if (err_code != NRF_SUCCESS)
        {
            return false;
        }

        if (remaining > 0u)
        {
            data += chunk_len;
            remaining -= chunk_len;
        }
        else
        {
            newline_sent = true;
        }
    }
    return true;
}

void ble_nus_bridge_disconnect_and_stop_advertising(void)
{
    uint32_t err_code;

    m_advertising_enabled = false;
    advertising_stop();

    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return;
    }

    err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    if ((err_code != NRF_SUCCESS) && (err_code != NRF_ERROR_INVALID_STATE))
    {
        APP_ERROR_CHECK(err_code);
    }
}
