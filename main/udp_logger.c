#include "udp_logger.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static const char *TAG = "udp_logger";
static int udp_sock = -1;
static struct sockaddr_in dest_addr;
static bool udp_enabled = false;

void udp_logger_init(const char* dest_ip, uint16_t dest_port) {
    // Create UDP socket
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    // Set destination address
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr);

    udp_enabled = true;
    ESP_LOGI(TAG, "UDP logger initialized: %s:%d", dest_ip, dest_port);
}

void udp_logger_send(const char* message) {
    if (!udp_enabled || udp_sock < 0) return;
    
    // Send without waiting for response
    sendto(udp_sock, message, strlen(message), 0,
           (struct sockaddr*)&dest_addr, sizeof(dest_addr));
}

// Custom vprintf to intercept ESP_LOG output
static int udp_log_vprintf(const char *fmt, va_list args) {
    static char log_buffer[256];
    int len = vsnprintf(log_buffer, sizeof(log_buffer), fmt, args);
    
    // Send to UDP if initialized
    if (udp_enabled && len > 0) {
        udp_logger_send(log_buffer);
    }
    
    // Also print to UART
    return vprintf(fmt, args);
}

void udp_logger_hook(void) {
    esp_log_set_vprintf(udp_log_vprintf);
}
