#ifndef MQTT_HA_H
#define MQTT_HA_H

#include "esp_err.h"
#include <stdbool.h>

// Callback for MQTT switch commands (ON/OFF)
typedef void (*mqtt_command_cb_t)(bool on);

/**
 * Initialize MQTT client, connect to broker, publish HA discovery, subscribe to commands
 * @param cmd_cb Callback invoked when ON/OFF command received via MQTT
 * @return ESP_OK on success
 */
esp_err_t mqtt_ha_init(mqtt_command_cb_t cmd_cb);

/**
 * Publish switch state (ON/OFF) to state topic
 */
void mqtt_ha_publish_state(bool on);

/**
 * Publish availability (online/offline)
 */
void mqtt_ha_publish_availability(bool online);

/**
 * Re-publish HA auto-discovery config (retained)
 */
void mqtt_ha_publish_discovery(void);

#endif // MQTT_HA_H
