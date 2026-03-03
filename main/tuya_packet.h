#ifndef TUYA_PACKET_H
#define TUYA_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Tuya BLE Command Codes
#define TUYA_CMD_DEVICE_INFO  0x0000
#define TUYA_CMD_PAIR         0x0001
#define TUYA_CMD_DPS          0x0002  // Datapoint commands

// Tuya BLE Data Point Types
#define TUYA_DP_TYPE_BOOL     0x01
#define TUYA_DP_TYPE_VALUE    0x02
#define TUYA_DP_TYPE_STRING   0x03
#define TUYA_DP_TYPE_ENUM     0x04
#define TUYA_DP_TYPE_BITMAP   0x05

// Security flags
#define TUYA_SECURITY_FLAG_AUTH     0x01
#define TUYA_SECURITY_FLAG_LOGIN    0x04
#define TUYA_SECURITY_FLAG_SESSION  0x05

// BLE packet constants
#define TUYA_BLE_MTU          20
#define TUYA_BLE_PROTOCOL_VERSION  3

/**
 * Build a raw Tuya BLE packet (before encryption)
 * @param seq_num Sequence number (4 bytes, increments with each packet)
 * @param response_to Response to sequence number (0 for new requests)
 * @param code Command code (e.g., TUYA_CMD_DPS)
 * @param data Command data payload
 * @param data_len Length of data
 * @param output Output buffer
 * @param output_size Size of output buffer
 * @return Length of raw packet, or 0 on error
 */
size_t tuya_build_raw_packet(
    uint32_t seq_num,
    uint32_t response_to,
    uint16_t code,
    const uint8_t* data,
    uint16_t data_len,
    uint8_t* output,
    size_t output_size
);

/**
 * Split encrypted packet into BLE MTU-sized chunks
 * @param encrypted_data Encrypted packet data
 * @param encrypted_len Length of encrypted data
 * @param packets Array of packet pointers to fill (caller must allocate)
 * @param packet_lengths Array to store length of each packet
 * @param max_packets Maximum number of packets in array
 * @return Number of packets created, or 0 on error
 */
int tuya_split_to_ble_packets(
    const uint8_t* encrypted_data,
    size_t encrypted_len,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
);

/**
 * Build device info request packet
 * @param seq_num Sequence number
 * @param login_key Login key (16 bytes, MD5 of local_key[:6])
 * @param packets Output packets array
 * @param packet_lengths Output packet lengths
 * @param max_packets Maximum packets
 * @return Number of packets, or 0 on error
 */
int tuya_build_device_info_request(
    uint32_t seq_num,
    const uint8_t* login_key,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
);

/**
 * Build pairing request packet
 * @param seq_num Sequence number
 * @param session_key Session key (16 bytes)
 * @param uuid Device UUID string (16 hex chars)
 * @param local_key_short Local key (first 6 chars)
 * @param device_id Device ID string
 * @param packets Output packets array
 * @param packet_lengths Output packet lengths
 * @param max_packets Maximum packets
 * @return Number of packets, or 0 on error
 */
int tuya_build_pair_request(
    uint32_t seq_num,
    const uint8_t* session_key,
    const char* uuid,
    const char* local_key_short,
    const char* device_id,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
);

/**
 * Build switch command packet (datapoint command)
 * @param seq_num Sequence number
 * @param session_key Session key (16 bytes)
 * @param dp_id Datapoint ID (usually 1 for main switch)
 * @param value true for ON, false for OFF
 * @param packets Output packets array
 * @param packet_lengths Output packet lengths
 * @param max_packets Maximum packets
 * @return Number of packets, or 0 on error
 */
int tuya_build_switch_command(
    uint32_t seq_num,
    const uint8_t* session_key,
    uint8_t dp_id,
    bool value,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
);

#endif // TUYA_PACKET_H
