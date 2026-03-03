#include "tuya_crypto.h"
#include <string.h>
#include "mbedtls/md5.h"
#include "mbedtls/aes.h"
#include "esp_random.h"
#include "esp_log.h"

static const char *TAG = "tuya_crypto";

void tuya_calculate_md5(const uint8_t* input, size_t len, uint8_t* output) {
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, input, len);
    mbedtls_md5_finish(&ctx, output);
    mbedtls_md5_free(&ctx);
}

uint16_t tuya_calculate_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

size_t tuya_aes_encrypt(
    const uint8_t* key,
    const uint8_t* data,
    size_t data_len,
    uint8_t security_flag,
    uint8_t* output,
    size_t output_size
) {
    // Zero padding to next multiple of 16 (matches tuya_ble.py exactly)
    size_t padded_len = ((data_len + 15) / 16) * 16;
    if (padded_len == 0) padded_len = 16;  // Minimum one block

    // Check output buffer size (1 byte flag + 16 bytes IV + padded data)
    if (output_size < 1 + 16 + padded_len) {
        ESP_LOGE(TAG, "Output buffer too small");
        return 0;
    }

    // Generate random IV
    uint8_t iv[16];
    esp_fill_random(iv, 16);

    // Prepare zero-padded data (matching Python: while len(raw) % 16 != 0: raw += b"\x00")
    uint8_t* padded_data = malloc(padded_len);
    if (!padded_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for padding");
        return 0;
    }
    memcpy(padded_data, data, data_len);
    memset(padded_data + data_len, 0, padded_len - data_len);

    // Perform AES-CBC encryption
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);  // mbedtls modifies IV, so we need a copy

    int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set AES key: %d", ret);
        free(padded_data);
        mbedtls_aes_free(&aes);
        return 0;
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_len,
                                 iv_copy, padded_data, output + 1 + 16);
    mbedtls_aes_free(&aes);

    if (ret != 0) {
        ESP_LOGE(TAG, "AES encryption failed: %d", ret);
        free(padded_data);
        return 0;
    }

    // Build output: security_flag + IV + encrypted_data
    output[0] = security_flag;
    memcpy(output + 1, iv, 16);

    free(padded_data);
    return 1 + 16 + padded_len;
}

size_t tuya_aes_decrypt(
    const uint8_t* key,
    const uint8_t* encrypted_data,
    size_t encrypted_len,
    uint8_t* output,
    size_t output_size
) {
    // encrypted_data should start with IV (16 bytes) followed by encrypted payload
    if (encrypted_len < 16 || encrypted_len % 16 != 0) {
        ESP_LOGE(TAG, "Invalid encrypted data length");
        return 0;
    }

    // Extract IV
    uint8_t iv[16];
    memcpy(iv, encrypted_data, 16);

    // Decrypt
    size_t payload_len = encrypted_len - 16;
    if (output_size < payload_len) {
        ESP_LOGE(TAG, "Output buffer too small for decryption");
        return 0;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set AES key for decryption: %d", ret);
        mbedtls_aes_free(&aes);
        return 0;
    }

    ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, payload_len,
                                 iv, encrypted_data + 16, output);
    mbedtls_aes_free(&aes);

    if (ret != 0) {
        ESP_LOGE(TAG, "AES decryption failed: %d", ret);
        return 0;
    }

    // Remove padding (assuming PKCS7-style padding or zero padding)
    // For simplicity, we return the full decrypted length
    // The caller should handle padding removal based on packet structure
    return payload_len;
}
