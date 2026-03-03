#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"

#include "tuya_ble_client.h"
#include "esp_crt_bundle.h"
#include "udp_logger.h"
#include "web_server.h"
#include "mqtt_ha.h"

static const char *TAG = "main";

// WiFi event group
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// Current switch state
static bool g_switch_state = false;

// Pending command for connect-on-demand pattern
static bool g_pending_command = false;
static bool g_pending_command_on = false;
static esp_timer_handle_t g_ble_disconnect_timer = NULL;
#define BLE_DISCONNECT_TIMEOUT_MS  15000  // Disconnect BLE after 15s idle

// Forward declarations
static void wifi_init_sta(void);
static void ble_disconnect_timer_cb(void *arg);

// Log hook for web dashboard
static vprintf_like_t g_original_vprintf = NULL;

static int web_log_vprintf(const char *fmt, va_list args) {
    // Copy args before consuming - va_list can only be consumed once
    va_list args_copy;
    va_copy(args_copy, args);

    // Format for web ring buffer
    char buf[200];
    int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);

    if (len > 0) {
        web_server_add_log(buf);
    }

    // Call original vprintf (serial + UDP) with the original args
    if (g_original_vprintf) {
        return g_original_vprintf(fmt, args);
    }
    return len;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

#ifdef CONFIG_USE_STATIC_IP
    // Configure static IP before starting WiFi
    ESP_LOGI(TAG, "Configuring static IP: %s", CONFIG_STATIC_IP);
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(netif));

    esp_netif_ip_info_t ip_info;
    esp_netif_str_to_ip4(CONFIG_STATIC_IP, &ip_info.ip);
    esp_netif_str_to_ip4(CONFIG_STATIC_GW, &ip_info.gw);
    esp_netif_str_to_ip4(CONFIG_STATIC_NETMASK, &ip_info.netmask);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_LOGI(TAG, "Static IP configured: %s, Gateway: %s", CONFIG_STATIC_IP, CONFIG_STATIC_GW);
#else
    ESP_LOGI(TAG, "Using DHCP");
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to %s", CONFIG_WIFI_SSID);
}

// BLE disconnect timer - disconnects BLE after idle period to save resources
static void ble_disconnect_timer_cb(void *arg) {
    ESP_LOGI(TAG, "BLE idle timeout, disconnecting to save resources");
    tuya_ble_client_disconnect();
}

static void reset_ble_disconnect_timer(void) {
    if (g_ble_disconnect_timer) {
        esp_timer_stop(g_ble_disconnect_timer);  // OK if not running
        esp_timer_start_once(g_ble_disconnect_timer,
                             BLE_DISCONNECT_TIMEOUT_MS * 1000);
    }
}

// Send a switch command - connects BLE if needed
static void send_switch_command(bool on) {
    tuya_connection_state_t state = tuya_ble_get_state();
    if (state == TUYA_STATE_READY) {
        // Already connected, send directly
        tuya_ble_send_switch_command(CONFIG_TUYA_SWITCH_DP_ID, on);
        reset_ble_disconnect_timer();
    } else {
        // Need to connect first, queue the command
        ESP_LOGI(TAG, "BLE not connected, connecting to send %s...", on ? "ON" : "OFF");
        g_pending_command = true;
        g_pending_command_on = on;
        if (state == TUYA_STATE_DISCONNECTED) {
            tuya_ble_client_connect();
        }
        // Command will be sent when state becomes READY
    }
}

// Tuya state change callback
static void tuya_state_callback(tuya_connection_state_t state) {
    ESP_LOGI(TAG, "Tuya state changed: %d (%s)", state, tuya_ble_state_str(state));
    if (state == TUYA_STATE_READY) {
        mqtt_ha_publish_availability(true);
        // If there's a pending command, send it now
        if (g_pending_command) {
            g_pending_command = false;
            ESP_LOGI(TAG, "Sending pending command: %s", g_pending_command_on ? "ON" : "OFF");
            tuya_ble_send_switch_command(CONFIG_TUYA_SWITCH_DP_ID, g_pending_command_on);
        }
        // Start disconnect timer
        reset_ble_disconnect_timer();
    } else if (state == TUYA_STATE_DISCONNECTED) {
        mqtt_ha_publish_availability(false);
        if (g_ble_disconnect_timer) {
            esp_timer_stop(g_ble_disconnect_timer);
        }
    }
}

// Switch state update callback (from BLE DPS response)
static void tuya_switch_callback(uint8_t dp_id, bool state) {
    ESP_LOGI(TAG, "Switch DP%d state: %s", dp_id, state ? "ON" : "OFF");
    g_switch_state = state;
    mqtt_ha_publish_state(state);
    web_server_set_switch_state(state);
    // Reset disconnect timer - we just got activity
    reset_ble_disconnect_timer();
}

// MQTT command callback (from HA or other MQTT clients)
static void mqtt_command_callback(bool on) {
    ESP_LOGI(TAG, "MQTT command: %s", on ? "ON" : "OFF");
    send_switch_command(on);
}

// Web switch command callback
static void web_switch_callback(bool on) {
    ESP_LOGI(TAG, "Web command: %s", on ? "ON" : "OFF");
    send_switch_command(on);
}

void app_main(void) {
    ESP_LOGI(TAG, "Tuya BLE Bridge starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi
    wifi_init_sta();

    // Wait for WiFi connection
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                       false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");

#ifdef CONFIG_UDP_LOGGER_ENABLED
    // Initialize UDP logger for remote debugging
    udp_logger_init(CONFIG_UDP_LOGGER_IP, CONFIG_UDP_LOGGER_PORT);
    udp_logger_hook();
    ESP_LOGI(TAG, "UDP logger active - sending logs to %s:%d", CONFIG_UDP_LOGGER_IP, CONFIG_UDP_LOGGER_PORT);
#endif

    // Initialize Bluetooth
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Bluetooth initialized");

    // Initialize Tuya BLE client
    tuya_device_config_t tuya_config = {
        .mac_address = {0},  // Will be parsed from string
        .device_id = CONFIG_TUYA_DEVICE_ID,
        .local_key = CONFIG_TUYA_LOCAL_KEY,
        .uuid = CONFIG_TUYA_UUID,
    };

    // Parse MAC address from string format "AA:BB:CC:DD:EE:FF"
    int values[6];
    if (sscanf(CONFIG_TUYA_DEVICE_MAC, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            tuya_config.mac_address[i] = (uint8_t)values[i];
        }
    } else {
        ESP_LOGE(TAG, "Invalid MAC address format: %s", CONFIG_TUYA_DEVICE_MAC);
        return;
    }

    ret = tuya_ble_client_init(&tuya_config, tuya_state_callback,
                               tuya_switch_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Tuya BLE client: %s",
                esp_err_to_name(ret));
        return;
    }

    // Create BLE disconnect timer (fires after idle period)
    const esp_timer_create_args_t timer_args = {
        .callback = ble_disconnect_timer_cb,
        .name = "ble_disconnect",
    };
    esp_timer_create(&timer_args, &g_ble_disconnect_timer);

    // Start web server with switch callback
    web_server_start(web_switch_callback);
    g_original_vprintf = esp_log_set_vprintf(web_log_vprintf);

    // Initialize MQTT with HA auto-discovery
    mqtt_ha_init(mqtt_command_callback);

    ESP_LOGI(TAG, "Initialization complete. BLE connects on demand (MQTT/Web command).");

    // Main loop - just keep task alive, no polling
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
