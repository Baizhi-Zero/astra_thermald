/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef AES128_H
#define AES128_H

#include <stdint.h>
#include <stddef.h>

#define AES128_BLOCK_SIZE 16
#define AES128_KEY_SIZE   16

#ifdef __cplusplus
extern "C" {
#endif

int aes128_extract_vendor_keys(uint8_t key[16], uint8_t iv[16]);

int aes128_cbc_decrypt(const uint8_t *ciphertext, size_t ct_len,
                       const uint8_t *key, const uint8_t *iv,
                       uint8_t *out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
