#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include <stdbool.h>

// Callback for switch commands from web UI
typedef void (*web_switch_cb_t)(bool on);

esp_err_t web_server_start(web_switch_cb_t switch_cb);

// Update the switch state displayed on the dashboard
void web_server_set_switch_state(bool on);

// Add a log line to the ring buffer (called from log hook)
void web_server_add_log(const char* line);

#endif // WEB_SERVER_H
