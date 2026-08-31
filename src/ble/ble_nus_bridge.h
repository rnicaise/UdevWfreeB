#ifndef UWB_BLE_NUS_BRIDGE_H
#define UWB_BLE_NUS_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>

void ble_nus_bridge_set_device_name(const char *name);
void ble_nus_bridge_init(void);
bool ble_nus_bridge_is_connected(void);
bool ble_nus_bridge_is_client_ready(void);
bool ble_nus_bridge_is_advertising_enabled(void);
bool ble_nus_bridge_read_command(char *dst, size_t dst_size);
void ble_nus_bridge_send_line(const char *line);
void ble_nus_bridge_disconnect_and_stop_advertising(void);

#endif
