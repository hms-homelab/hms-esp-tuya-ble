#ifndef TUYA_CRYPTO_H
#define TUYA_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/**
 * Calculate MD5 hash
 * @param input Input data
 * @param len Length of input data
 * @param output Output buffer (must be 16 bytes)
 */
void tuya_calculate_md5(const uint8_t* input, size_t len, uint8_t* output);

/**
 * Calculate CRC-16/MODBUS checksum
 * @param data Input data
 * @param len Length of input data
 * @return CRC-16 value
 */
uint16_t tuya_calculate_crc16(const uint8_t* data, size_t len);

/**
 * Encrypt data using AES-128-CBC
 * @param key Encryption key (16 bytes)
 * @param data Data to encrypt
 * @param data_len Length of data
 * @param security_flag Security flag (1, 4, or 5)
 * @param output Output buffer (must be >= data_len + 17 bytes)
 * @param output_size Size of output buffer
 * @return Length of encrypted data (including security flag + IV), or 0 on error
 */
size_t tuya_aes_encrypt(
    const uint8_t* key,
    const uint8_t* data,
    size_t data_len,
    uint8_t security_flag,
    uint8_t* output,
    size_t output_size
);

/**
 * Decrypt data using AES-128-CBC
 * @param key Decryption key (16 bytes)
 * @param encrypted_data Encrypted data (including IV, without security flag)
 * @param encrypted_len Length of encrypted data
 * @param output Output buffer
 * @param output_size Size of output buffer
 * @return Length of decrypted data, or 0 on error
 */
size_t tuya_aes_decrypt(
    const uint8_t* key,
    const uint8_t* encrypted_data,
    size_t encrypted_len,
    uint8_t* output,
    size_t output_size
);

#endif // TUYA_CRYPTO_H
