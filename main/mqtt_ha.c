#include "mqtt_ha.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_ha";

// Runtime topic buffers built from CONFIG_MQTT_TOPIC_PREFIX
static char s_discovery_topic[128];
static char s_state_topic[64];
static char s_command_topic[64];
static char s_avail_topic[64];

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static mqtt_command_cb_t s_command_cb = NULL;
static bool s_connected = false;

static void build_topics(void) {
    snprintf(s_discovery_topic, sizeof(s_discovery_topic),
             "homeassistant/switch/%s/switch/config", CONFIG_MQTT_TOPIC_PREFIX);
    snprintf(s_state_topic, sizeof(s_state_topic),
             "%s/state", CONFIG_MQTT_TOPIC_PREFIX);
    snprintf(s_command_topic, sizeof(s_command_topic),
             "%s/command", CONFIG_MQTT_TOPIC_PREFIX);
    snprintf(s_avail_topic, sizeof(s_avail_topic),
             "%s/availability", CONFIG_MQTT_TOPIC_PREFIX);
}

void mqtt_ha_publish_discovery(void) {
    if (!s_mqtt_client || !s_connected) return;

    // Build unique_id from device ID
    char unique_id[64];
    snprintf(unique_id, sizeof(unique_id), "tuya_ble_%s", CONFIG_TUYA_DEVICE_ID);

    // Build discovery JSON at runtime from Kconfig values
    char payload[768];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"manufacturer\":\"Tuya\","
            "\"model\":\"BLE Breaker\","
            "\"sw_version\":\"1.0.0\""
        "}"
        "}",
        CONFIG_HA_DEVICE_NAME,
        unique_id,
        s_command_topic,
        s_state_topic,
        s_avail_topic,
        unique_id,
        CONFIG_HA_DEVICE_NAME);

    esp_mqtt_client_publish(s_mqtt_client, s_discovery_topic,
                            payload, 0, 0, 1);  // QoS 0, retained
    ESP_LOGI(TAG, "Published HA discovery config");
}

void mqtt_ha_publish_state(bool on) {
    if (!s_mqtt_client || !s_connected) return;
    const char *payload = on ? "ON" : "OFF";
    esp_mqtt_client_publish(s_mqtt_client, s_state_topic,
                            payload, 0, 0, 1);  // QoS 0, retained
    ESP_LOGI(TAG, "Published state: %s", payload);
}

void mqtt_ha_publish_availability(bool online) {
    if (!s_mqtt_client || !s_connected) return;
    const char *payload = online ? "online" : "offline";
    esp_mqtt_client_publish(s_mqtt_client, s_avail_topic,
                            payload, 0, 0, 1);  // QoS 0, retained
    ESP_LOGI(TAG, "Published availability: %s", payload);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to broker");
        s_connected = true;
        // Subscribe to command topic
        esp_mqtt_client_subscribe(s_mqtt_client, s_command_topic, 0);
        // Publish discovery and availability
        mqtt_ha_publish_discovery();
        mqtt_ha_publish_availability(false);  // Will go online when BLE is ready
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribed to command topic");
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && event->data_len > 0) {
            // Check if it's our command topic
            if (strncmp(event->topic, s_command_topic, event->topic_len) == 0) {
                bool on = (strncmp(event->data, "ON", event->data_len) == 0);
                ESP_LOGI(TAG, "MQTT command received: %s", on ? "ON" : "OFF");
                if (s_command_cb) {
                    s_command_cb(on);
                }
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

esp_err_t mqtt_ha_init(mqtt_command_cb_t cmd_cb) {
    s_command_cb = cmd_cb;

    // Build topic strings from Kconfig prefix
    build_topics();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.last_will.topic = s_avail_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client started, connecting to %s", CONFIG_MQTT_BROKER_URI);
    return ESP_OK;
}
