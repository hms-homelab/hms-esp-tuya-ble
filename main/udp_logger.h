#ifndef UDP_LOGGER_H
#define UDP_LOGGER_H

#include <stdint.h>

/**
 * Initialize UDP logger
 * @param dest_ip Destination IP address (your Mac)
 * @param dest_port UDP port (default 9999)
 */
void udp_logger_init(const char* dest_ip, uint16_t dest_port);

/**
 * Send message via UDP
 * @param message Message to send
 */
void udp_logger_send(const char* message);

/**
 * Hook into ESP-IDF logging system to send all logs via UDP
 */
void udp_logger_hook(void);

#endif // UDP_LOGGER_H
