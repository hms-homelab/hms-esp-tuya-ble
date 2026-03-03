#ifndef TUYA_BLE_CLIENT_H
#define TUYA_BLE_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Tuya BLE Service and Characteristic UUIDs
// Note: Device advertises 0xa201, but actual GATT service is 0x1910
#define TUYA_SERVICE_UUID        "00001910-0000-1000-8000-00805f9b34fb"
#define TUYA_WRITE_CHAR_UUID     "00002b11-0000-1000-8000-00805f9b34fb"
#define TUYA_NOTIFY_CHAR_UUID    "00002b10-0000-1000-8000-00805f9b34fb"

// Connection states
typedef enum {
    TUYA_STATE_DISCONNECTED,
    TUYA_STATE_CONNECTING,
    TUYA_STATE_CONNECTED,
    TUYA_STATE_SMP_PAIRING,
    TUYA_STATE_DISCOVERING_SERVICES,
    TUYA_STATE_GETTING_DEVICE_INFO,
    TUYA_STATE_PAIRING,
    TUYA_STATE_PAIRED,
    TUYA_STATE_READY
} tuya_connection_state_t;

// Device configuration
typedef struct {
    uint8_t mac_address[6];
    char device_id[32];
    char local_key[32];
    char uuid[32];
} tuya_device_config_t;

// State callback function type
typedef void (*tuya_state_callback_t)(tuya_connection_state_t state);

// Switch state callback
typedef void (*tuya_switch_callback_t)(uint8_t dp_id, bool state);

/**
 * Initialize Tuya BLE client
 * @param config Device configuration
 * @param state_cb State change callback
 * @param switch_cb Switch state update callback
 * @return ESP_OK on success
 */
esp_err_t tuya_ble_client_init(
    const tuya_device_config_t* config,
    tuya_state_callback_t state_cb,
    tuya_switch_callback_t switch_cb
);

/**
 * Start BLE connection to Tuya device
 * @return ESP_OK on success
 */
esp_err_t tuya_ble_client_connect(void);

/**
 * Disconnect from Tuya device
 */
void tuya_ble_client_disconnect(void);

/**
 * Stop BLE client (disconnect and prevent reconnection)
 */
void tuya_ble_client_stop(void);

/**
 * Resume BLE client operations after stop
 */
void tuya_ble_client_resume(void);

/**
 * Send switch command
 * @param dp_id Datapoint ID (usually 1)
 * @param on true for ON, false for OFF
 * @return ESP_OK on success
 */
esp_err_t tuya_ble_send_switch_command(uint8_t dp_id, bool on);

/**
 * Get current connection state
 * @return Current state
 */
tuya_connection_state_t tuya_ble_get_state(void);

/**
 * BLE status info for web dashboard
 */
typedef struct {
    int last_rssi;
    bool target_found;
    bool smp_paired;
    bool security_handle_verified;
    int discovered_services;
    uint16_t write_handle;
    uint16_t notify_handle;
    uint16_t security_handle;
    int devices_seen;
} tuya_ble_status_t;

/**
 * Get BLE status info
 */
void tuya_ble_get_status(tuya_ble_status_t *status);

/**
 * Get state name string
 */
const char* tuya_ble_state_str(tuya_connection_state_t state);

#endif // TUYA_BLE_CLIENT_H
