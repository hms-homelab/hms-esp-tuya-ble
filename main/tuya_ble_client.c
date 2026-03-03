#include "tuya_ble_client.h"
#include "tuya_crypto.h"
#include "tuya_packet.h"
#include <string.h>
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "tuya_ble_client";

// BLE connection parameters
static esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
static uint16_t g_conn_id = 0;
static esp_bd_addr_t g_remote_bda;
static uint16_t g_write_handle = 0;
static uint16_t g_notify_handle = 0;
static uint16_t g_service_start_handle = 0;
static uint16_t g_service_end_handle = 0;
static uint16_t g_security_handle = 0;  // Handle 0x003b for pre-flight handshake

// State management
static tuya_connection_state_t g_state = TUYA_STATE_DISCONNECTED;
static tuya_device_config_t g_device_config;
static tuya_state_callback_t g_state_cb = NULL;
static tuya_switch_callback_t g_switch_cb = NULL;
static bool g_ble_stopped = false;  // Flag to prevent reconnection during OTA

// (MQTT removed - using ESP_LOG only)

// Encryption keys
static uint8_t g_login_key[16];
static uint8_t g_session_key[16];
static bool g_has_session_key = false;

// Sequence number for packets
static uint32_t g_seq_num = 1;

// Response buffer for multi-packet reassembly
static uint8_t g_response_buffer[512];
static size_t g_response_buffer_len = 0;
static size_t g_expected_response_len = 0;
static bool g_receiving_response = false;

// SMP pairing state
static bool g_smp_paired = false;
static bool g_smp_attempted = false;

// GATT discovery state
typedef struct {
    uint16_t uuid16;
    uint16_t start_handle;
    uint16_t end_handle;
} discovered_service_t;

#define MAX_DISCOVERED_SERVICES 16
static discovered_service_t g_services[MAX_DISCOVERED_SERVICES];
static int g_service_count = 0;
static bool g_security_handle_verified = false;
static bool g_ccc_write_done = false;
static bool g_handshake_done = false;
static int g_last_rssi = 0;
static bool g_target_found = false;
static int g_devices_seen = 0;

// Forward declarations
static void set_state(tuya_connection_state_t new_state);
static void send_security_handshake(void);
static void send_device_info_request(void);
static void send_pair_request(void);
static void process_response(const uint8_t* encrypted_data, size_t len);
static void publish_debug(const char* topic_suffix, const char* message);
static void start_service_discovery(void);
static void smp_watchdog_task(void *pvParameter);
static void enumerate_all_characteristics(void);

static void set_state(tuya_connection_state_t new_state) {
    if (g_state != new_state) {
        ESP_LOGI(TAG, "State change: %d -> %d", g_state, new_state);
        g_state = new_state;
        if (g_state_cb) {
            g_state_cb(new_state);
        }
    }
}

tuya_connection_state_t tuya_ble_get_state(void) {
    return g_state;
}

static void start_service_discovery(void) {
    set_state(TUYA_STATE_DISCOVERING_SERVICES);
    ESP_LOGI(TAG, "Starting full GATT service discovery...");
    esp_ble_gattc_search_service(g_gattc_if, g_conn_id, NULL);
}

static void smp_watchdog_task(void *pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (!g_smp_paired && g_state == TUYA_STATE_SMP_PAIRING) {
        ESP_LOGW(TAG, "SMP pairing timed out after 5s, falling back to no-SMP");
        publish_debug("smp", "{\"result\":\"timeout\",\"paired\":false}");
        start_service_discovery();
    }

    vTaskDelete(NULL);
}

static void enumerate_all_characteristics(void) {
    ESP_LOGI(TAG, "Enumerating characteristics for ALL %d discovered services...", g_service_count);

    char json[256];

    for (int s = 0; s < g_service_count; s++) {
        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
            g_gattc_if, g_conn_id,
            ESP_GATT_DB_CHARACTERISTIC,
            g_services[s].start_handle,
            g_services[s].end_handle,
            0, &count);

        ESP_LOGI(TAG, "Service 0x%04x (0x%04x-0x%04x): %d characteristics",
                 g_services[s].uuid16, g_services[s].start_handle,
                 g_services[s].end_handle, count);

        if (status != ESP_GATT_OK || count == 0) continue;

        esp_gattc_char_elem_t *chars = malloc(sizeof(esp_gattc_char_elem_t) * count);
        if (!chars) continue;

        status = esp_ble_gattc_get_all_char(
            g_gattc_if, g_conn_id,
            g_services[s].start_handle,
            g_services[s].end_handle,
            chars, &count, 0);

        if (status == ESP_GATT_OK) {
            for (int c = 0; c < count; c++) {
                uint16_t char_uuid = chars[c].uuid.uuid.uuid16;
                uint16_t char_handle = chars[c].char_handle;
                uint8_t char_prop = chars[c].properties;

                ESP_LOGI(TAG, "  Char[%d]: UUID 0x%04x, handle 0x%04x, prop 0x%02x",
                         c, char_uuid, char_handle, char_prop);

                // Publish each characteristic to MQTT
                snprintf(json, sizeof(json),
                    "{\"service\":\"0x%04x\",\"uuid\":\"0x%04x\",\"handle\":\"0x%04x\",\"properties\":\"0x%02x\"}",
                    g_services[s].uuid16, char_uuid, char_handle, char_prop);
                publish_debug("gatt/char", json);

                // Check if this is handle 0x003b
                if (char_handle == 0x003b) {
                    g_security_handle = 0x003b;
                    g_security_handle_verified = true;
                    ESP_LOGI(TAG, "  --> Handle 0x003b FOUND in service 0x%04x (UUID 0x%04x)",
                             g_services[s].uuid16, char_uuid);
                    snprintf(json, sizeof(json),
                        "{\"found\":true,\"handle\":\"0x003b\",\"service\":\"0x%04x\",\"char_uuid\":\"0x%04x\"}",
                        g_services[s].uuid16, char_uuid);
                    publish_debug("gatt/security_handle", json);
                }

                // Track Tuya service handles
                if (g_services[s].uuid16 == 0x1910) {
                    if (char_prop & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) {
                        g_write_handle = char_handle;
                        ESP_LOGI(TAG, "  --> Tuya write handle: 0x%04x", g_write_handle);
                    }
                    if (char_prop & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                        g_notify_handle = char_handle;
                        ESP_LOGI(TAG, "  --> Tuya notify handle: 0x%04x", g_notify_handle);
                    }
                }
            }
        }

        free(chars);
    }

    // If 0x003b was not found via discovery, use the write handle for security handshake
    if (!g_security_handle_verified) {
        g_security_handle = g_write_handle;
        ESP_LOGW(TAG, "Handle 0x003b NOT found via discovery, using write handle 0x%04x for security handshake", g_write_handle);
        publish_debug("gatt/security_handle",
            "{\"found\":false,\"handle\":\"write_handle\",\"source\":\"write_handle_fallback\"}");
    }

    // Publish discovery summary
    snprintf(json, sizeof(json),
        "{\"total_services\":%d,\"tuya_found\":%s,\"security_handle_verified\":%s,\"smp_paired\":%s}",
        g_service_count,
        (g_service_start_handle != 0) ? "true" : "false",
        g_security_handle_verified ? "true" : "false",
        g_smp_paired ? "true" : "false");
    publish_debug("gatt/discovery_summary", json);

    // Now setup Tuya notifications
    if (g_notify_handle != 0) {
        esp_err_t ret = esp_ble_gattc_register_for_notify(g_gattc_if, g_remote_bda, g_notify_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Registered for notifications on handle 0x%04x", g_notify_handle);
        } else {
            ESP_LOGE(TAG, "Failed to register for notifications: %s", esp_err_to_name(ret));
        }

        // Write CCC descriptor (typically notify_handle + 1)
        uint16_t notify_en = 1;
        uint16_t ccc_handle = g_notify_handle + 1;
        ESP_LOGI(TAG, "Writing CCC descriptor at handle 0x%04x", ccc_handle);

        ret = esp_ble_gattc_write_char_descr(
            g_gattc_if, g_conn_id, ccc_handle,
            sizeof(notify_en), (uint8_t*)&notify_en,
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "CCC descriptor write initiated");
        } else {
            ESP_LOGE(TAG, "Failed to write CCC descriptor: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "No notify handle found, cannot setup notifications");
    }

    if (g_write_handle == 0) {
        ESP_LOGW(TAG, "No write handle found in Tuya service!");
    }
}

static void send_security_handshake(void) {
    if (g_security_handle == 0) {
        ESP_LOGW(TAG, "Security handle not found, skipping handshake");
        return;
    }

    // Tuya BLE Security Handshake Frame: 0a0100000303
    // Byte 0: 0x0a = Frame Header (Protocol Control/Handshake)
    // Byte 1: 0x01 = Protocol Version (V2/V3)
    // Bytes 2-3: 0x0000 = Sequence Number
    // Byte 4: 0x03 = Command Type (Security Bind/Session Init)
    // Byte 5: 0x03 = CheckSum/Flag
    const uint8_t handshake_data[] = {0x0a, 0x01, 0x00, 0x00, 0x03, 0x03};

    ESP_LOGI(TAG, "🔐 Sending security handshake to handle 0x%04x", g_security_handle);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, handshake_data, sizeof(handshake_data), ESP_LOG_INFO);

    esp_err_t ret = esp_ble_gattc_write_char(
        g_gattc_if,
        g_conn_id,
        g_security_handle,
        sizeof(handshake_data),
        (uint8_t*)handshake_data,
        ESP_GATT_WRITE_TYPE_RSP,
        ESP_GATT_AUTH_REQ_NONE
    );

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Security handshake sent (write-with-response), awaiting confirmation...");
        publish_debug("handshake", "{\"status\":\"sent\",\"write_type\":\"with_response\"}");
    } else {
        ESP_LOGE(TAG, "Failed to send security handshake: %d", ret);
        publish_debug("handshake", "{\"status\":\"send_failed\",\"error\":%d}");
    }
}

static void send_device_info_request(void) {
    ESP_LOGI(TAG, "Sending device info request");
    ESP_LOGI(TAG, "Using write_handle: 0x%04x, conn_id: %d, gattc_if: %d",
             g_write_handle, g_conn_id, g_gattc_if);

    uint8_t packets[10][TUYA_BLE_MTU];
    uint8_t packet_lengths[10];

    int num_packets = tuya_build_device_info_request(
        g_seq_num++, g_login_key,
        packets, packet_lengths, 10
    );

    if (num_packets == 0) {
        ESP_LOGE(TAG, "Failed to build device info request");
        return;
    }

    ESP_LOGI(TAG, "Built %d packets for device info request", num_packets);

    // Send all packets
    for (int i = 0; i < num_packets; i++) {
        ESP_LOGI(TAG, "Sending packet %d/%d, length: %d", i+1, num_packets, packet_lengths[i]);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, packets[i], packet_lengths[i], ESP_LOG_INFO);

        esp_err_t ret = esp_ble_gattc_write_char(
            g_gattc_if, g_conn_id, g_write_handle,
            packet_lengths[i], packets[i],
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE
        );

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Packet %d sent successfully", i+1);
        } else {
            ESP_LOGE(TAG, "❌ Failed to send packet %d: %s", i+1, esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(50));  // Small delay between packets
    }

    ESP_LOGI(TAG, "All packets sent, waiting for response...");
    set_state(TUYA_STATE_GETTING_DEVICE_INFO);
}

static void send_pair_request(void) {
    ESP_LOGI(TAG, "Sending pair request");

    uint8_t packets[10][TUYA_BLE_MTU];
    uint8_t packet_lengths[10];

    // Extract first 6 chars of local_key
    char local_key_short[7];
    strncpy(local_key_short, g_device_config.local_key, 6);
    local_key_short[6] = '\0';

    int num_packets = tuya_build_pair_request(
        g_seq_num++, g_session_key,
        g_device_config.uuid, local_key_short, g_device_config.device_id,
        packets, packet_lengths, 10
    );

    if (num_packets == 0) {
        ESP_LOGE(TAG, "Failed to build pair request");
        return;
    }

    // Send all packets
    for (int i = 0; i < num_packets; i++) {
        esp_err_t ret = esp_ble_gattc_write_char(
            g_gattc_if, g_conn_id, g_write_handle,
            packet_lengths[i], packets[i],
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE
        );

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send packet %d: %d", i, ret);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    set_state(TUYA_STATE_PAIRING);
}

static void process_response(const uint8_t* encrypted_data, size_t len) {
    ESP_LOGI(TAG, "Processing response, length: %zu", len);

    // Response format: security_flag (1) + IV (16) + encrypted_payload
    if (len < 17) {
        ESP_LOGE(TAG, "Response too short: %zu bytes", len);
        return;
    }

    uint8_t security_flag = encrypted_data[0];
    ESP_LOGI(TAG, "Security flag: 0x%02x", security_flag);

    // Select decryption key based on security flag
    const uint8_t* decrypt_key = NULL;
    if (security_flag == 0x04) {
        decrypt_key = g_login_key;
        ESP_LOGI(TAG, "Using login_key for decryption");
    } else if (security_flag == 0x05) {
        decrypt_key = g_session_key;
        ESP_LOGI(TAG, "Using session_key for decryption");
    } else {
        ESP_LOGE(TAG, "Unknown security flag: 0x%02x", security_flag);
        return;
    }

    // Decrypt the response (skip security flag byte)
    uint8_t decrypted[512];
    size_t decrypted_len = tuya_aes_decrypt(
        decrypt_key,
        encrypted_data + 1,  // Skip security flag
        len - 1,
        decrypted,
        sizeof(decrypted)
    );

    if (decrypted_len == 0) {
        ESP_LOGE(TAG, "Decryption failed");
        return;
    }

    ESP_LOGI(TAG, "Decrypted %zu bytes", decrypted_len);

    // Parse packet structure (12-byte header):
    // seq_num (4) + response_to (4) + code (2) + data_len (2) + data + crc (2)
    if (decrypted_len < 14) {
        ESP_LOGE(TAG, "Decrypted data too short: %zu", decrypted_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, decrypted, decrypted_len, ESP_LOG_INFO);
        return;
    }

    uint32_t seq_num = (decrypted[0] << 24) | (decrypted[1] << 16) |
                       (decrypted[2] << 8) | decrypted[3];
    uint32_t response_to = (decrypted[4] << 24) | (decrypted[5] << 16) |
                           (decrypted[6] << 8) | decrypted[7];
    uint16_t code = (decrypted[8] << 8) | decrypted[9];
    uint16_t data_len = (decrypted[10] << 8) | decrypted[11];

    ESP_LOGI(TAG, "Response: seq=%lu, response_to=%lu, code=0x%04x, data_len=%u",
             seq_num, response_to, code, data_len);

    // Extract data payload
    const uint8_t* data = &decrypted[12];

    // Process based on command code
    switch (code) {
        case TUYA_CMD_DEVICE_INFO: {
            ESP_LOGI(TAG, "DEVICE_INFO response received");

            if (data_len < 46) {
                ESP_LOGE(TAG, "DEVICE_INFO response too short: %u bytes", data_len);
                break;
            }

            // Extract device information
            uint8_t device_version_major = data[0];
            uint8_t device_version_minor = data[1];
            uint8_t protocol_version_major = data[2];
            uint8_t protocol_version_minor = data[3];
            uint8_t flags = data[4];
            uint8_t is_bound = data[5];

            ESP_LOGI(TAG, "Device version: %u.%u", device_version_major, device_version_minor);
            ESP_LOGI(TAG, "Protocol version: %u.%u", protocol_version_major, protocol_version_minor);
            ESP_LOGI(TAG, "Flags: 0x%02x, Bound: %u", flags, is_bound);

            // Extract srand (6 bytes at offset 6-11)
            uint8_t srand[6];
            memcpy(srand, &data[6], 6);

            ESP_LOGI(TAG, "srand: %02x%02x%02x%02x%02x%02x",
                     srand[0], srand[1], srand[2], srand[3], srand[4], srand[5]);

            // Calculate session_key = MD5(local_key[:6] + srand)
            // Only first 6 bytes of local_key, matching tuya_ble.py
            uint8_t combined[6 + 6];
            memcpy(combined, g_device_config.local_key, 6);
            memcpy(combined + 6, srand, 6);

            tuya_calculate_md5(combined, 12, g_session_key);
            g_has_session_key = true;

            ESP_LOGI(TAG, "Session key calculated successfully!");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, g_session_key, 16, ESP_LOG_INFO);

            // Move to pairing state
            set_state(TUYA_STATE_GETTING_DEVICE_INFO);

            // Send pair request after short delay
            vTaskDelay(pdMS_TO_TICKS(100));
            send_pair_request();
            break;
        }

        case TUYA_CMD_PAIR: {
            ESP_LOGI(TAG, "PAIR response received");

            if (data_len < 1) {
                ESP_LOGE(TAG, "PAIR response too short");
                break;
            }

            uint8_t result = data[0];
            ESP_LOGI(TAG, "Pairing result: %u (0=success, 2=already paired)", result);

            if (result == 0 || result == 2) {
                set_state(TUYA_STATE_READY);
                ESP_LOGI(TAG, "✅ Pairing complete! Device is ready for commands.");
            } else {
                ESP_LOGE(TAG, "Pairing failed with result: %u", result);
            }
            break;
        }

        case TUYA_CMD_DPS:
        case 0x8001: {
            // DPS format: dp_id(1) + dp_type(1) + value_len(1) + value(N), repeating
            ESP_LOGI(TAG, "DPS data (code=0x%04x, len=%u)", code, data_len);
            size_t offset = 0;
            while (offset + 3 <= data_len) {
                uint8_t dp_id = data[offset];
                uint8_t dp_type = data[offset + 1];
                uint8_t dp_len = data[offset + 2];
                offset += 3;
                if (offset + dp_len > data_len) break;
                if (dp_type == TUYA_DP_TYPE_BOOL && dp_len == 1) {
                    bool val = data[offset] != 0;
                    ESP_LOGI(TAG, "DP%u (bool) = %s", dp_id, val ? "ON" : "OFF");
                    if (g_switch_cb) g_switch_cb(dp_id, val);
                } else if (dp_type == TUYA_DP_TYPE_VALUE && dp_len == 4) {
                    int32_t val = (data[offset] << 24) | (data[offset+1] << 16) |
                                  (data[offset+2] << 8) | data[offset+3];
                    ESP_LOGI(TAG, "DP%u (value) = %ld", dp_id, (long)val);
                } else if (dp_type == TUYA_DP_TYPE_ENUM && dp_len == 1) {
                    ESP_LOGI(TAG, "DP%u (enum) = %u", dp_id, data[offset]);
                } else {
                    ESP_LOGI(TAG, "DP%u type=%u len=%u", dp_id, dp_type, dp_len);
                }
                offset += dp_len;
            }
            break;
        }

        // Network time query (0x001e) and session notification (0x8011) - ignore
        case 0x001e:
        case 0x8011:
            ESP_LOGD(TAG, "Info response code: 0x%04x (ignored)", code);
            break;

        default:
            ESP_LOGI(TAG, "Unknown response code: 0x%04x", code);
            break;
    }
}

esp_err_t tuya_ble_send_switch_command(uint8_t dp_id, bool on) {
    if (g_state != TUYA_STATE_READY) {
        ESP_LOGW(TAG, "Not ready to send commands, state: %d", g_state);
        return ESP_ERR_INVALID_STATE;
    }

    if (!g_has_session_key) {
        ESP_LOGE(TAG, "No session key available");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Sending switch command: DP%d = %s", dp_id, on ? "ON" : "OFF");

    uint8_t packets[10][TUYA_BLE_MTU];
    uint8_t packet_lengths[10];

    int num_packets = tuya_build_switch_command(
        g_seq_num++, g_session_key, dp_id, on,
        packets, packet_lengths, 10
    );

    if (num_packets == 0) {
        ESP_LOGE(TAG, "Failed to build switch command");
        return ESP_FAIL;
    }

    // Send all packets
    for (int i = 0; i < num_packets; i++) {
        esp_err_t ret = esp_ble_gattc_write_char(
            g_gattc_if, g_conn_id, g_write_handle,
            packet_lengths[i], packets[i],
            ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE
        );

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send packet %d: %d", i, ret);
            return ret;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

// GATT event handler
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            ESP_LOGI(TAG, "GATT client registered, app_id: %04x", param->reg.app_id);
            g_gattc_if = gattc_if;
            break;

        case ESP_GATTC_CONNECT_EVT:
            ESP_LOGI(TAG, "Connected to device, conn_id: %d", param->connect.conn_id);

            // Read RSSI to check signal strength
            esp_ble_gap_read_rssi(param->connect.remote_bda);

            g_conn_id = param->connect.conn_id;
            memcpy(g_remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));

            // Reset all discovery state
            g_smp_paired = false;
            g_smp_attempted = false;
            g_service_count = 0;
            g_security_handle_verified = false;
            g_ccc_write_done = false;
            g_handshake_done = false;

            // Skip SMP pairing - Tuya uses application-layer AES encryption, not BLE-layer SMP.
            // SMP negotiation was interfering with GATT operations and causing device disconnect.
            ESP_LOGI(TAG, "Skipping SMP, requesting MTU and starting GATT discovery...");

            // Request MTU exchange (some devices need this before accepting data)
            esp_ble_gattc_send_mtu_req(gattc_if, param->connect.conn_id);

            start_service_discovery();
            break;

        case ESP_GATTC_DISCONNECT_EVT:
            ESP_LOGW(TAG, "❌ Disconnected from device!");
            ESP_LOGW(TAG, "Disconnect reason code: %d", param->disconnect.reason);

            // Determine disconnect reason
            const char* reason_str;
            const char* reason_desc;

            // Explain disconnect reason
            switch (param->disconnect.reason) {
                case 8:
                    ESP_LOGW(TAG, "Reason: Connection Timeout");
                    reason_str = "timeout";
                    reason_desc = "Connection timeout";
                    break;
                case 19:
                    ESP_LOGW(TAG, "Reason: Remote User Terminated Connection");
                    ESP_LOGW(TAG, "        (Device actively disconnected - likely rejected our command)");
                    reason_str = "remote_term";
                    reason_desc = "Remote device terminated";
                    break;
                case 22:
                    ESP_LOGW(TAG, "Reason: Connection Timeout (LMP Response)");
                    reason_str = "lmp_timeout";
                    reason_desc = "LMP response timeout";
                    break;
                case 62:
                    ESP_LOGW(TAG, "Reason: Connection Terminated by Local Host");
                    reason_str = "local_term";
                    reason_desc = "Local host terminated";
                    break;
                default:
                    ESP_LOGW(TAG, "Reason: Unknown (%d)", param->disconnect.reason);
                    reason_str = "unknown";
                    reason_desc = "Unknown reason";
                    break;
            }

            // Publish disconnect reason to MQTT in JSON format
            char disconnect_json[256];
            snprintf(disconnect_json, sizeof(disconnect_json),
                "{\"reason_code\":%d,\"reason\":\"%s\",\"description\":\"%s\",\"state\":%d,\"uptime\":%lld}",
                param->disconnect.reason, reason_str, reason_desc, g_state, esp_timer_get_time() / 1000000);
            publish_debug("disconnect", disconnect_json);

            ESP_LOGW(TAG, "Previous state: %d", g_state);

            if (g_state == TUYA_STATE_GETTING_DEVICE_INFO) {
                ESP_LOGE(TAG, "⚠️  Device disconnected while waiting for device info response!");
                ESP_LOGE(TAG, "Most likely cause: BLE signal too weak!");
                ESP_LOGE(TAG, "Action: Move ESP32 MUCH closer to Tuya device (< 2 meters)");
                publish_debug("error", "weak_signal_timeout");
            }

            set_state(TUYA_STATE_DISCONNECTED);
            g_write_handle = 0;
            g_notify_handle = 0;
            g_security_handle = 0;
            g_service_start_handle = 0;
            g_service_end_handle = 0;
            g_has_session_key = false;
            g_smp_paired = false;
            g_smp_attempted = false;
            g_service_count = 0;
            g_security_handle_verified = false;
            g_ccc_write_done = false;
            g_handshake_done = false;
            break;

        case ESP_GATTC_SEARCH_RES_EVT: {
            uint16_t uuid16 = 0;
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
                uuid16 = param->search_res.srvc_id.uuid.uuid.uuid16;
            }

            uint16_t start_h = param->search_res.start_handle;
            uint16_t end_h = param->search_res.end_handle;

            ESP_LOGI(TAG, "Service found: UUID 0x%04x, handles 0x%04x-0x%04x", uuid16, start_h, end_h);

            // Store in discovery array
            if (g_service_count < MAX_DISCOVERED_SERVICES) {
                g_services[g_service_count].uuid16 = uuid16;
                g_services[g_service_count].start_handle = start_h;
                g_services[g_service_count].end_handle = end_h;
                g_service_count++;
            }

            // Publish to MQTT
            char svc_json[128];
            snprintf(svc_json, sizeof(svc_json),
                "{\"uuid\":\"0x%04x\",\"start\":\"0x%04x\",\"end\":\"0x%04x\"}",
                uuid16, start_h, end_h);
            publish_debug("gatt/service", svc_json);

            // Track Tuya service specifically
            if (uuid16 == 0x1910) {
                ESP_LOGI(TAG, "Found Tuya service 0x1910!");
                g_service_start_handle = start_h;
                g_service_end_handle = end_h;
            }
            break;
        }

        case ESP_GATTC_SEARCH_CMPL_EVT:
            ESP_LOGI(TAG, "Service search complete, found %d services", g_service_count);

            if (g_service_count > 0) {
                enumerate_all_characteristics();
            } else {
                ESP_LOGW(TAG, "No services found!");
            }
            break;

        case ESP_GATTC_WRITE_DESCR_EVT:
            if (param->write.status == ESP_GATT_OK) {
                ESP_LOGI(TAG, "CCC descriptor write completed successfully");
                g_ccc_write_done = true;

                // Skip security handshake - protocol v2 sends DEVICE_INFO directly
                if (g_write_handle != 0 && g_notify_handle != 0) {
                    ESP_LOGI(TAG, "CCC done, sending DEVICE_INFO directly (protocol v2, no handshake)...");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    send_device_info_request();
                }
            } else {
                ESP_LOGE(TAG, "CCC descriptor write failed: status=%d", param->write.status);
            }
            break;

        case ESP_GATTC_CFG_MTU_EVT:
            ESP_LOGI(TAG, "MTU configured: status=%d, mtu=%d", param->cfg_mtu.status, param->cfg_mtu.mtu);
            break;

        case ESP_GATTC_WRITE_CHAR_EVT:
            ESP_LOGI(TAG, "Write char event: handle=0x%04x, status=%d", param->write.handle, param->write.status);
            break;

        case ESP_GATTC_NOTIFY_EVT: {
            const uint8_t* value = param->notify.value;
            uint16_t value_len = param->notify.value_len;

            ESP_LOGI(TAG, "Notification received, handle: 0x%04x, length: %d", param->notify.handle, value_len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, value, value_len, ESP_LOG_INFO);

            if (value_len == 0) {
                break;
            }

            // Parse varint packet sequence number
            size_t pos = 0;
            int packet_num = 0;
            int shift = 0;
            while (pos < value_len) {
                uint8_t b = value[pos++];
                packet_num |= (b & 0x7F) << (shift * 7);
                shift++;
                if ((b & 0x80) == 0) break;
            }
            ESP_LOGI(TAG, "Packet #%d (header bytes: %zu)", packet_num, pos);

            // First packet contains total length (varint) and protocol version
            if (packet_num == 0) {
                // Parse total length varint
                int total_len_val = 0;
                shift = 0;
                while (pos < value_len) {
                    uint8_t b = value[pos++];
                    total_len_val |= (b & 0x7F) << (shift * 7);
                    shift++;
                    if ((b & 0x80) == 0) break;
                }
                g_expected_response_len = total_len_val;

                // Protocol version byte
                uint8_t protocol_ver = 0;
                if (pos < value_len) {
                    protocol_ver = value[pos++] >> 4;
                }

                ESP_LOGI(TAG, "Starting new response: total_len=%zu, protocol=%u",
                         g_expected_response_len, protocol_ver);

                // Reset buffer
                g_response_buffer_len = 0;
                g_receiving_response = true;

                // Copy remaining data
                size_t data_len = value_len - pos;
                if (data_len > 0) {
                    memcpy(g_response_buffer, &value[pos], data_len);
                    g_response_buffer_len = data_len;
                }
            } else {
                // Continuation packet - data starts after varint packet_num
                if (!g_receiving_response) {
                    ESP_LOGW(TAG, "Received continuation packet without starting packet");
                    break;
                }

                size_t data_len = value_len - pos;
                if (g_response_buffer_len + data_len > sizeof(g_response_buffer)) {
                    ESP_LOGE(TAG, "Response buffer overflow");
                    g_receiving_response = false;
                    break;
                }

                memcpy(&g_response_buffer[g_response_buffer_len], &value[pos], data_len);
                g_response_buffer_len += data_len;
            }

            ESP_LOGI(TAG, "Response buffer: %zu / %zu bytes",
                     g_response_buffer_len, g_expected_response_len);

            // Check if we've received the complete response
            if (g_response_buffer_len >= g_expected_response_len) {
                ESP_LOGI(TAG, "✅ Complete response received!");
                g_receiving_response = false;

                // Process the complete response
                process_response(g_response_buffer, g_expected_response_len);
            }
            break;
        }

        default:
            break;
    }
}

// GAP event handler
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "✅ Scan params set successfully, starting scan...");
                // Start scanning (duration: 0 = continuous)
                esp_err_t ret = esp_ble_gap_start_scanning(0);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "❌ Failed to start scanning: %s", esp_err_to_name(ret));
                }
            } else {
                ESP_LOGE(TAG, "❌ Set scan params failed, status: %d", param->scan_param_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "✅ Scan started successfully");
            } else {
                ESP_LOGE(TAG, "❌ Scan start failed, status: %d", param->scan_start_cmpl.status);
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                g_devices_seen++;

                // Only log every 20th device to reduce spam
                if (g_devices_seen % 20 == 0) {
                    ESP_LOGI(TAG, "BLE scan: %d devices seen so far...", g_devices_seen);
                }

                // Check if this is our device (only connect once)
                if (memcmp(param->scan_rst.bda, g_remote_bda, sizeof(esp_bd_addr_t)) == 0 &&
                    g_state == TUYA_STATE_CONNECTING) {
                    g_target_found = true;
                    g_last_rssi = param->scan_rst.rssi;
                    ESP_LOGI(TAG, "Found target device! RSSI: %d. Stopping scan and connecting...",
                             param->scan_rst.rssi);

                    // Dump full advertisement data to understand Tuya protocol version
                    uint8_t adv_len = param->scan_rst.adv_data_len;
                    uint8_t *adv = param->scan_rst.ble_adv;
                    ESP_LOGI(TAG, "ADV data (%d bytes):", adv_len);
                    ESP_LOG_BUFFER_HEX_LEVEL(TAG, adv, adv_len, ESP_LOG_INFO);

                    // Parse Tuya manufacturer data if present
                    // Tuya company ID is typically 0x07D0
                    uint8_t *p = adv;
                    while (p < adv + adv_len) {
                        uint8_t len = p[0];
                        if (len == 0 || p + len >= adv + adv_len) break;
                        uint8_t type = p[1];
                        if (type == 0xFF && len >= 3) {  // Manufacturer Specific Data
                            uint16_t company_id = p[2] | (p[3] << 8);
                            ESP_LOGI(TAG, "Manufacturer data: company=0x%04x, len=%d", company_id, len-1);
                            ESP_LOG_BUFFER_HEX_LEVEL(TAG, p+2, len-1, ESP_LOG_INFO);
                        }
                        p += len + 1;
                    }

                    set_state(TUYA_STATE_CONNECTED);  // Prevent duplicate connect
                    esp_ble_gap_stop_scanning();
                    esp_ble_gattc_open(g_gattc_if, g_remote_bda, param->scan_rst.ble_addr_type, true);
                }
            }
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            ESP_LOGI(TAG, "Scan stopped");
            break;

        case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
            if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                int rssi = param->read_rssi_cmpl.rssi;
                g_last_rssi = rssi;
                ESP_LOGI(TAG, "📶 Current RSSI: %d dBm", rssi);

                // Determine signal quality
                const char* signal_quality;
                if (rssi > -60) {
                    signal_quality = "excellent";
                    ESP_LOGI(TAG, "   Signal: Excellent");
                } else if (rssi > -70) {
                    signal_quality = "good";
                    ESP_LOGI(TAG, "   Signal: Good");
                } else if (rssi > -80) {
                    signal_quality = "fair";
                    ESP_LOGW(TAG, "   Signal: Fair (may affect reliability)");
                } else {
                    signal_quality = "poor";
                    ESP_LOGE(TAG, "   Signal: Poor (connection issues likely)");
                }

                // Publish detailed RSSI to MQTT in JSON format
                char rssi_json[128];
                snprintf(rssi_json, sizeof(rssi_json),
                    "{\"rssi\":%d,\"signal\":\"%s\",\"uptime\":%lld}",
                    rssi, signal_quality, esp_timer_get_time() / 1000000);
                publish_debug("rssi", rssi_json);
            }
            break;

        case ESP_GAP_BLE_SEC_REQ_EVT:
            ESP_LOGI(TAG, "SMP: Security request from device, accepting...");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            esp_bd_addr_t bd_addr;
            memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "SMP: Auth complete, addr: %02x:%02x:%02x:%02x:%02x:%02x",
                     bd_addr[0], bd_addr[1], bd_addr[2], bd_addr[3], bd_addr[4], bd_addr[5]);

            if (param->ble_security.auth_cmpl.success) {
                g_smp_paired = true;
                ESP_LOGI(TAG, "SMP: Pairing SUCCEEDED (auth_mode=%d)",
                         param->ble_security.auth_cmpl.auth_mode);

                char smp_json[128];
                snprintf(smp_json, sizeof(smp_json),
                    "{\"result\":\"success\",\"auth_mode\":%d,\"paired\":true}",
                    param->ble_security.auth_cmpl.auth_mode);
                publish_debug("smp", smp_json);
            } else {
                ESP_LOGW(TAG, "SMP: Pairing FAILED (reason=0x%x), proceeding without SMP",
                         param->ble_security.auth_cmpl.fail_reason);

                char smp_json[128];
                snprintf(smp_json, sizeof(smp_json),
                    "{\"result\":\"failed\",\"fail_reason\":\"0x%x\",\"paired\":false}",
                    param->ble_security.auth_cmpl.fail_reason);
                publish_debug("smp", smp_json);
            }

            // Proceed to service discovery regardless of SMP result
            if (g_state == TUYA_STATE_SMP_PAIRING) {
                start_service_discovery();
            }
            break;
        }

        case ESP_GAP_BLE_KEY_EVT:
            ESP_LOGI(TAG, "SMP: Key exchange event, key_type=%d", param->ble_security.ble_key.key_type);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT:
            ESP_LOGI(TAG, "SMP: Numeric comparison request, auto-confirming");
            esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
            break;

        default:
            ESP_LOGD(TAG, "GAP event: %d", event);
            break;
    }
}

esp_err_t tuya_ble_client_init(
    const tuya_device_config_t* config,
    tuya_state_callback_t state_cb,
    tuya_switch_callback_t switch_cb
) {
    // Store configuration
    memcpy(&g_device_config, config, sizeof(tuya_device_config_t));
    g_state_cb = state_cb;
    g_switch_cb = switch_cb;

    // Copy MAC address (already parsed as bytes in main.c)
    memcpy(g_remote_bda, config->mac_address, 6);

    // Calculate login_key = MD5(local_key[:6])
    ESP_LOGI(TAG, "Local key: %s", config->local_key);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, config->local_key, 6, ESP_LOG_INFO);

    tuya_calculate_md5((uint8_t*)config->local_key, 6, g_login_key);

    ESP_LOGI(TAG, "Calculated login_key:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, g_login_key, 16, ESP_LOG_INFO);

    ESP_LOGI(TAG, "Tuya BLE client initialized");
    ESP_LOGI(TAG, "Device ID: %s", config->device_id);
    ESP_LOGI(TAG, "UUID: %s", config->uuid);

    // Register GATT client
    esp_err_t ret = esp_ble_gattc_register_callback(gattc_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATT callback: %d", ret);
        return ret;
    }

    ret = esp_ble_gattc_app_register(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GATT app: %d", ret);
        return ret;
    }

    // Register GAP callback
    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %d", ret);
        return ret;
    }

    // Configure BLE SMP (Just Works bonding)
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

    ESP_LOGI(TAG, "BLE SMP configured (Just Works, bonding enabled)");

    return ESP_OK;
}

esp_err_t tuya_ble_client_connect(void) {
    if (g_ble_stopped) {
        ESP_LOGW(TAG, "BLE client stopped (OTA in progress?), not connecting");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_state != TUYA_STATE_DISCONNECTED) {
        ESP_LOGW(TAG, "Already connecting or connected");
        return ESP_ERR_INVALID_STATE;
    }

    set_state(TUYA_STATE_CONNECTING);

    ESP_LOGI(TAG, "Looking for device MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             g_remote_bda[0], g_remote_bda[1], g_remote_bda[2],
             g_remote_bda[3], g_remote_bda[4], g_remote_bda[5]);

    // Start scanning for the device using regular (legacy) scan
    // More compatible with older BLE devices
    static esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan params failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Scan params set, waiting for ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT...");

    // Note: esp_ble_gap_start_scanning() will be called in the GAP event handler
    // after receiving ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT

    return ESP_OK;
}

void tuya_ble_client_disconnect(void) {
    if (g_state != TUYA_STATE_DISCONNECTED) {
        esp_ble_gap_stop_scanning();
        esp_ble_gattc_close(g_gattc_if, g_conn_id);
        set_state(TUYA_STATE_DISCONNECTED);
        g_devices_seen = 0;
        g_target_found = false;
    }
}

void tuya_ble_client_stop(void) {
    ESP_LOGI(TAG, "Stopping BLE client operations");
    g_ble_stopped = true;

    // Stop scanning if in progress
    esp_ble_gap_stop_scanning();

    // Disconnect if connected
    if (g_state != TUYA_STATE_DISCONNECTED) {
        esp_ble_gattc_close(g_gattc_if, g_conn_id);
    }

    set_state(TUYA_STATE_DISCONNECTED);
}

void tuya_ble_client_resume(void) {
    ESP_LOGI(TAG, "Resuming BLE client operations");
    g_ble_stopped = false;
}

// publish_debug is a no-op now that MQTT is removed
static void publish_debug(const char* topic_suffix, const char* message) {
    (void)topic_suffix;
    (void)message;
}

void tuya_ble_get_status(tuya_ble_status_t *status) {
    status->last_rssi = g_last_rssi;
    status->target_found = g_target_found;
    status->smp_paired = g_smp_paired;
    status->security_handle_verified = g_security_handle_verified;
    status->discovered_services = g_service_count;
    status->write_handle = g_write_handle;
    status->notify_handle = g_notify_handle;
    status->security_handle = g_security_handle;
    status->devices_seen = g_devices_seen;
}

const char* tuya_ble_state_str(tuya_connection_state_t state) {
    switch (state) {
        case TUYA_STATE_DISCONNECTED: return "disconnected";
        case TUYA_STATE_CONNECTING: return "scanning";
        case TUYA_STATE_CONNECTED: return "connected";
        case TUYA_STATE_SMP_PAIRING: return "smp_pairing";
        case TUYA_STATE_DISCOVERING_SERVICES: return "discovering_services";
        case TUYA_STATE_GETTING_DEVICE_INFO: return "getting_device_info";
        case TUYA_STATE_PAIRING: return "pairing";
        case TUYA_STATE_PAIRED: return "paired";
        case TUYA_STATE_READY: return "ready";
        default: return "unknown";
    }
}
