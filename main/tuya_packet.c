#include "tuya_packet.h"
#include "tuya_crypto.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "tuya_packet";

// Helper to write big-endian values
static void write_be16(uint8_t* buf, uint16_t value) {
    buf[0] = (value >> 8) & 0xFF;
    buf[1] = value & 0xFF;
}

static void write_be32(uint8_t* buf, uint32_t value) {
    buf[0] = (value >> 24) & 0xFF;
    buf[1] = (value >> 16) & 0xFF;
    buf[2] = (value >> 8) & 0xFF;
    buf[3] = value & 0xFF;
}

size_t tuya_build_raw_packet(
    uint32_t seq_num,
    uint32_t response_to,
    uint16_t code,
    const uint8_t* data,
    uint16_t data_len,
    uint8_t* output,
    size_t output_size
) {
    // Packet structure (matches PlusPlus-ua/ha_tuya_ble):
    // seq_num (4) + response_to (4) + code (2) + data_len (2) + data + crc16 (2)
    size_t total_len = 4 + 4 + 2 + 2 + data_len + 2;

    if (output_size < total_len) {
        ESP_LOGE(TAG, "Output buffer too small for raw packet");
        return 0;
    }

    uint8_t* ptr = output;

    // Write header (12-byte header with response_to)
    write_be32(ptr, seq_num); ptr += 4;
    write_be32(ptr, response_to); ptr += 4;
    write_be16(ptr, code); ptr += 2;
    write_be16(ptr, data_len); ptr += 2;

    // Write data
    if (data_len > 0 && data != NULL) {
        memcpy(ptr, data, data_len);
        ptr += data_len;
    }

    // Calculate and write CRC16
    uint16_t crc = tuya_calculate_crc16(output, ptr - output);
    write_be16(ptr, crc);
    ptr += 2;

    return ptr - output;
}

// Tuya varint encoding (matches _pack_int in tuya_ble.py)
// 7 bits per byte, high bit = continuation
static size_t pack_varint(uint8_t* buf, int value) {
    size_t len = 0;
    while (1) {
        uint8_t curr_byte = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            curr_byte |= 0x80;
        }
        buf[len++] = curr_byte;
        if (value == 0) break;
    }
    return len;
}

int tuya_split_to_ble_packets(
    const uint8_t* encrypted_data,
    size_t encrypted_len,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
) {
    int packet_num = 0;
    size_t pos = 0;

    while (pos < encrypted_len && packet_num < max_packets) {
        uint8_t* packet = packets[packet_num];
        size_t packet_len = 0;

        // Packet sequence number (varint)
        packet_len += pack_varint(&packet[packet_len], packet_num);

        // First packet includes total length (varint) and protocol version
        if (packet_num == 0) {
            packet_len += pack_varint(&packet[packet_len], (int)encrypted_len);
            packet[packet_len++] = TUYA_BLE_PROTOCOL_VERSION << 4;
        }

        // Fill rest of packet with data (up to MTU)
        size_t remaining = TUYA_BLE_MTU - packet_len;
        size_t to_copy = (encrypted_len - pos) < remaining ?
                         (encrypted_len - pos) : remaining;

        memcpy(&packet[packet_len], &encrypted_data[pos], to_copy);
        packet_len += to_copy;
        pos += to_copy;

        packet_lengths[packet_num] = packet_len;
        packet_num++;
    }

    if (pos < encrypted_len) {
        ESP_LOGE(TAG, "Too many packets needed, increase max_packets");
        return 0;
    }

    return packet_num;
}

int tuya_build_device_info_request(
    uint32_t seq_num,
    const uint8_t* login_key,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
) {
    // Device info request - empty data for v2/v3
    uint8_t raw_packet[256];
    size_t raw_len = tuya_build_raw_packet(
        seq_num, 0, TUYA_CMD_DEVICE_INFO, NULL, 0,
        raw_packet, sizeof(raw_packet)
    );

    if (raw_len == 0) {
        return 0;
    }

    // Debug: dump raw packet before encryption
    ESP_LOGI(TAG, "DEVICE_INFO raw (%zu bytes): seq=%lu code=0x%04x", raw_len, seq_num, TUYA_CMD_DEVICE_INFO);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, raw_packet, raw_len, ESP_LOG_INFO);

    // Encrypt with login_key
    uint8_t encrypted[512];
    size_t encrypted_len = tuya_aes_encrypt(
        login_key, raw_packet, raw_len,
        TUYA_SECURITY_FLAG_LOGIN,
        encrypted, sizeof(encrypted)
    );

    if (encrypted_len == 0) {
        return 0;
    }

    // Split into BLE packets
    return tuya_split_to_ble_packets(
        encrypted, encrypted_len,
        packets, packet_lengths, max_packets
    );
}

int tuya_build_pair_request(
    uint32_t seq_num,
    const uint8_t* session_key,
    const char* uuid,
    const char* local_key_short,
    const char* device_id,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
) {
    // Pairing data: uuid (16) + local_key (6) + device_id (22) + padding to 44 bytes
    uint8_t pair_data[44];
    memset(pair_data, 0, sizeof(pair_data));

    // Copy UUID (convert hex string to bytes)
    for (int i = 0; i < 16 && uuid[i] != '\0'; i++) {
        pair_data[i] = uuid[i];
    }

    // Copy local key (first 6 chars)
    for (int i = 0; i < 6 && local_key_short[i] != '\0'; i++) {
        pair_data[16 + i] = local_key_short[i];
    }

    // Copy device ID
    size_t device_id_len = strlen(device_id);
    if (device_id_len > 22) device_id_len = 22;
    memcpy(&pair_data[22], device_id, device_id_len);

    // Build raw packet
    uint8_t raw_packet[256];
    size_t raw_len = tuya_build_raw_packet(
        seq_num, 0, TUYA_CMD_PAIR, pair_data, sizeof(pair_data),
        raw_packet, sizeof(raw_packet)
    );

    if (raw_len == 0) {
        return 0;
    }

    // Encrypt with session_key (PAIR uses session key, not login key)
    uint8_t encrypted[512];
    size_t encrypted_len = tuya_aes_encrypt(
        session_key, raw_packet, raw_len,
        TUYA_SECURITY_FLAG_SESSION,
        encrypted, sizeof(encrypted)
    );

    if (encrypted_len == 0) {
        return 0;
    }

    // Split into BLE packets
    return tuya_split_to_ble_packets(
        encrypted, encrypted_len,
        packets, packet_lengths, max_packets
    );
}

int tuya_build_switch_command(
    uint32_t seq_num,
    const uint8_t* session_key,
    uint8_t dp_id,
    bool value,
    uint8_t packets[][TUYA_BLE_MTU],
    uint8_t packet_lengths[],
    int max_packets
) {
    // Datapoint command: dp_id (1) + dp_type (1) + value_len (1) + value
    uint8_t dp_data[4];
    dp_data[0] = dp_id;
    dp_data[1] = TUYA_DP_TYPE_BOOL;
    dp_data[2] = 1;  // value length
    dp_data[3] = value ? 0x01 : 0x00;

    // Build raw packet
    uint8_t raw_packet[256];
    size_t raw_len = tuya_build_raw_packet(
        seq_num, 0, TUYA_CMD_DPS, dp_data, sizeof(dp_data),
        raw_packet, sizeof(raw_packet)
    );

    if (raw_len == 0) {
        return 0;
    }

    // Encrypt with session_key
    uint8_t encrypted[512];
    size_t encrypted_len = tuya_aes_encrypt(
        session_key, raw_packet, raw_len,
        TUYA_SECURITY_FLAG_SESSION,
        encrypted, sizeof(encrypted)
    );

    if (encrypted_len == 0) {
        return 0;
    }

    // Split into BLE packets
    return tuya_split_to_ble_packets(
        encrypted, encrypted_len,
        packets, packet_lengths, max_packets
    );
}
