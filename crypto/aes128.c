/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "aes128.h"
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t rsbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

static const uint8_t Rcon[11] = {
    0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static void KeyExpansion(uint8_t *RoundKey, const uint8_t *Key) {
    uint32_t i, j, k;
    uint8_t tempa[4];

    for (i = 0; i < 4; ++i) {
        RoundKey[(i * 4) + 0] = Key[(i * 4) + 0];
        RoundKey[(i * 4) + 1] = Key[(i * 4) + 1];
        RoundKey[(i * 4) + 2] = Key[(i * 4) + 2];
        RoundKey[(i * 4) + 3] = Key[(i * 4) + 3];
    }

    for (i = 4; i < 44; ++i) {
        k = (i - 1) * 4;
        tempa[0] = RoundKey[k + 0];
        tempa[1] = RoundKey[k + 1];
        tempa[2] = RoundKey[k + 2];
        tempa[3] = RoundKey[k + 3];

        if (i % 4 == 0) {
            const uint8_t u8tmp = tempa[0];
            tempa[0] = tempa[1];
            tempa[1] = tempa[2];
            tempa[2] = tempa[3];
            tempa[3] = u8tmp;

            tempa[0] = sbox[tempa[0]];
            tempa[1] = sbox[tempa[1]];
            tempa[2] = sbox[tempa[2]];
            tempa[3] = sbox[tempa[3]];

            tempa[0] ^= Rcon[i / 4];
        }

        j = i * 4;
        k = (i - 4) * 4;
        RoundKey[j + 0] = RoundKey[k + 0] ^ tempa[0];
        RoundKey[j + 1] = RoundKey[k + 1] ^ tempa[1];
        RoundKey[j + 2] = RoundKey[k + 2] ^ tempa[2];
        RoundKey[j + 3] = RoundKey[k + 3] ^ tempa[3];
    }
}

static void AddRoundKey(uint8_t round, uint8_t (*state)[4], const uint8_t *RoundKey) {
    for (uint8_t r = 0; r < 4; ++r) {
        for (uint8_t c = 0; c < 4; ++c) {
            state[r][c] ^= RoundKey[(round * 16) + (c * 4) + r];
        }
    }
}

static void InvSubBytes(uint8_t (*state)[4]) {
    for (uint8_t r = 0; r < 4; ++r) {
        for (uint8_t c = 0; c < 4; ++c) {
            state[r][c] = rsbox[state[r][c]];
        }
    }
}

static void InvShiftRows(uint8_t (*state)[4]) {
    uint8_t temp;

    temp = state[1][3];
    state[1][3] = state[1][2];
    state[1][2] = state[1][1];
    state[1][1] = state[1][0];
    state[1][0] = temp;

    temp = state[2][0];
    state[2][0] = state[2][2];
    state[2][2] = temp;
    temp = state[2][1];
    state[2][1] = state[2][3];
    state[2][3] = temp;

    temp = state[3][0];
    state[3][0] = state[3][1];
    state[3][1] = state[3][2];
    state[3][2] = state[3][3];
    state[3][3] = temp;
}

static uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

static uint8_t Multiply(uint8_t x, uint8_t y) {
    return (uint8_t)(((y & 1) * x) ^
                     (((y >> 1) & 1) * xtime(x)) ^
                     (((y >> 2) & 1) * xtime(xtime(x))) ^
                     (((y >> 3) & 1) * xtime(xtime(xtime(x)))) ^
                     (((y >> 4) & 1) * xtime(xtime(xtime(xtime(x))))));
}

static void InvMixColumns(uint8_t (*state)[4]) {
    uint8_t a, b, c, d;
    for (int col = 0; col < 4; ++col) {
        a = state[0][col];
        b = state[1][col];
        c = state[2][col];
        d = state[3][col];

        state[0][col] = Multiply(a, 0x0e) ^ Multiply(b, 0x0b) ^ Multiply(c, 0x0d) ^ Multiply(d, 0x09);
        state[1][col] = Multiply(a, 0x09) ^ Multiply(b, 0x0e) ^ Multiply(c, 0x0b) ^ Multiply(d, 0x0d);
        state[2][col] = Multiply(a, 0x0d) ^ Multiply(b, 0x09) ^ Multiply(c, 0x0e) ^ Multiply(d, 0x0b);
        state[3][col] = Multiply(a, 0x0b) ^ Multiply(b, 0x0d) ^ Multiply(c, 0x09) ^ Multiply(d, 0x0e);
    }
}

static void InvCipher(uint8_t (*state)[4], const uint8_t *RoundKey) {
    AddRoundKey(10, state, RoundKey);

    for (int round = 9; round > 0; --round) {
        InvShiftRows(state);
        InvSubBytes(state);
        AddRoundKey(round, state, RoundKey);
        InvMixColumns(state);
    }

    InvShiftRows(state);
    InvSubBytes(state);
    AddRoundKey(0, state, RoundKey);
}

static void XorWithIv(uint8_t *buf, const uint8_t *Iv) {
    for (uint8_t i = 0; i < 16; ++i) {
        buf[i] ^= Iv[i];
    }
}

int aes128_ecb_decrypt_block(const uint8_t in_block[16],
                             const uint8_t key[16],
                             uint8_t out_block[16]) {
    if (!in_block || !key || !out_block) return -1;

    uint8_t RoundKey[176];
    KeyExpansion(RoundKey, key);

    uint8_t state[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            state[r][c] = in_block[r + c * 4];
        }
    }

    InvCipher(state, RoundKey);

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out_block[r + c * 4] = state[r][c];
        }
    }
    return 0;
}

static bool is_printable_16byte_cstr(const uint8_t *data, size_t file_len, size_t offset) {
    if (offset + 17 > file_len) return false;
    if (data[offset + 16] != 0) return false;
    for (size_t k = 0; k < 16; k++) {
        uint8_t c = data[offset + k];
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static bool test_candidate_key_iv_on_ct(const uint8_t *sample_ct, size_t ct_len,
                                        const uint8_t cand_key[16], const uint8_t cand_iv[16]) {
    if (!sample_ct || ct_len == 0 || (ct_len % 16) != 0) return true;

    const uint8_t *last_blk = sample_ct + (ct_len - 16);
    const uint8_t *prev_iv  = (ct_len >= 32) ? (sample_ct + ct_len - 32) : cand_iv;
    uint8_t pt[16];
    if (aes128_ecb_decrypt_block(last_blk, cand_key, pt) != 0) return false;
    for (int i = 0; i < 16; i++) pt[i] ^= prev_iv[i];

    uint8_t pad = pt[15];
    if (pad == 0 || pad > 16) return false;
    for (uint8_t i = 0; i < pad; i++) {
        if (pt[15 - i] != pad) return false;
    }

    if (ct_len >= 16) {
        uint8_t first_pt[16];
        if (aes128_ecb_decrypt_block(sample_ct, cand_key, first_pt) != 0) return false;
        for (int i = 0; i < 16; i++) first_pt[i] ^= cand_iv[i];
        size_t check_n = (ct_len == 16) ? (size_t)(16 - pad) : 16;
        for (size_t i = 0; i < check_n; i++) {
            uint8_t c = first_pt[i];
            if (c < 0x09 || (c > 0x0D && c < 0x20) || c == 0x7F) return false;
        }
    }
    return true;
}

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

static bool vaddr_to_file_offset(const uint8_t *elf, size_t elf_len, uint64_t vaddr, size_t *out_off) {
    if (elf_len < 64) return false;
    uint64_t e_phoff     = *(const uint64_t *)(elf + 32);
    uint16_t e_phentsize = *(const uint16_t *)(elf + 54);
    uint16_t e_phnum     = *(const uint16_t *)(elf + 56);

    if (e_phentsize < sizeof(elf64_phdr_t) || e_phoff + (uint64_t)e_phentsize * e_phnum > elf_len) {
        return false;
    }

    for (uint16_t i = 0; i < e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(elf + e_phoff + (size_t)i * e_phentsize);
        if (ph->p_type == 1) {
            if (vaddr >= ph->p_vaddr && vaddr < ph->p_vaddr + ph->p_filesz) {
                uint64_t foff = ph->p_offset + (vaddr - ph->p_vaddr);
                if (foff < elf_len) {
                    *out_off = (size_t)foff;
                    return true;
                }
            }
        }
    }
    return false;
}

int aes128_extract_and_verify_vendor_keys(const uint8_t *sample_ct, size_t ct_len,
                                          uint8_t key[16], uint8_t iv[16]) {
    static uint8_t cached_key[16];
    static uint8_t cached_iv[16];
    static bool keys_cached = false;

    if (keys_cached && test_candidate_key_iv_on_ct(sample_ct, ct_len, cached_key, cached_iv)) {
        memcpy(key, cached_key, 16);
        memcpy(iv, cached_iv, 16);
        return 0;
    }

    const char *paths[] = {
        "/vendor/bin/mi_thermald",
        "/system/vendor/bin/mi_thermald",
        "/data/local/tmp/mi_thermald"
    };

    int fd = -1;
    for (size_t p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        fd = open(paths[p], O_RDONLY);
        if (fd >= 0) break;
    }
    if (fd < 0) return -ENOENT;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 64 || st.st_size > 8 * 1024 * 1024) {
        close(fd);
        return -EINVAL;
    }

    size_t file_len = (size_t)st.st_size;
    uint8_t *bin = (uint8_t *)malloc(file_len);
    if (!bin) {
        close(fd);
        return -ENOMEM;
    }

    ssize_t rd = read(fd, bin, file_len);
    close(fd);
    if (rd != (ssize_t)file_len) {
        free(bin);
        return -EIO;
    }

    bool found = false;

    if (file_len >= 64 && bin[0] == 0x7F && bin[1] == 'E' && bin[2] == 'L' && bin[3] == 'F' && bin[4] == 2) {
        for (size_t off = 64; off + sizeof(elf64_rela_t) * 2 <= file_len; off += 8) {
            const elf64_rela_t *r1 = (const elf64_rela_t *)(bin + off);
            const elf64_rela_t *r2 = (const elf64_rela_t *)(bin + off + sizeof(elf64_rela_t));

            if ((r1->r_info & 0xFFFFFFFFULL) == 1027ULL &&
                (r2->r_info & 0xFFFFFFFFULL) == 1027ULL &&
                r2->r_offset == r1->r_offset + 8 &&
                r1->r_addend > 0 && r2->r_addend > 0) {
                size_t k_off = 0, iv_off = 0;
                if (vaddr_to_file_offset(bin, file_len, (uint64_t)r1->r_addend, &k_off) &&
                    vaddr_to_file_offset(bin, file_len, (uint64_t)r2->r_addend, &iv_off)) {
                    if (is_printable_16byte_cstr(bin, file_len, k_off) &&
                        is_printable_16byte_cstr(bin, file_len, iv_off)) {
                        if (test_candidate_key_iv_on_ct(sample_ct, ct_len, bin + k_off, bin + iv_off)) {
                            memcpy(cached_key, bin + k_off, 16);
                            memcpy(cached_iv, bin + iv_off, 16);
                            keys_cached = true;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (!found && file_len >= 34) {
        for (size_t i = 0; i <= file_len - 34; i++) {
            if (is_printable_16byte_cstr(bin, file_len, i) &&
                is_printable_16byte_cstr(bin, file_len, i + 17)) {
                if (test_candidate_key_iv_on_ct(sample_ct, ct_len, bin + i, bin + i + 17)) {
                    memcpy(cached_key, bin + i, 16);
                    memcpy(cached_iv, bin + i + 17, 16);
                    keys_cached = true;
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found && sample_ct && ct_len >= 16 && file_len >= 17) {
        for (size_t i = 0; i <= file_len - 17; i++) {
            if (i == 0 || bin[i - 1] == 0) {
                if (is_printable_16byte_cstr(bin, file_len, i)) {
                    if (test_candidate_key_iv_on_ct(sample_ct, ct_len, bin + i, bin + i)) {
                        memcpy(cached_key, bin + i, 16);
                        memcpy(cached_iv, bin + i, 16);
                        keys_cached = true;
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    free(bin);

    if (found) {
        memcpy(key, cached_key, 16);
        memcpy(iv, cached_iv, 16);
        return 0;
    }
    return -ESRCH;
}

int aes128_extract_vendor_keys(uint8_t key[16], uint8_t iv[16]) {
    return aes128_extract_and_verify_vendor_keys(NULL, 0, key, iv);
}

int aes128_cbc_decrypt(const uint8_t *ciphertext, size_t ct_len,
                       const uint8_t *key, const uint8_t *iv,
                       uint8_t *out_buf, size_t *out_len) {
    if (!ciphertext || !key || !iv || !out_buf || !out_len) return -1;
    if (ct_len == 0 || (ct_len % AES128_BLOCK_SIZE) != 0) return -1;

    uint8_t RoundKey[176];
    KeyExpansion(RoundKey, key);

    uint8_t iv_buf[16];
    memcpy(iv_buf, iv, 16);

    uint8_t state[4][4];

    for (size_t b = 0; b < ct_len; b += 16) {
        const uint8_t *cur_ct = ciphertext + b;
        uint8_t ct_copy[16];
        memcpy(ct_copy, cur_ct, 16);

        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                state[r][c] = ct_copy[r + c * 4];
            }
        }

        InvCipher(state, RoundKey);

        uint8_t pt_block[16];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                pt_block[r + c * 4] = state[r][c];
            }
        }

        XorWithIv(pt_block, iv_buf);

        memcpy(iv_buf, ct_copy, 16);
        memcpy(out_buf + b, pt_block, 16);
    }

    uint8_t pad_len = out_buf[ct_len - 1];
    if (pad_len == 0 || pad_len > AES128_BLOCK_SIZE || pad_len > ct_len) {
        *out_len = 0;
        return -2;
    }

    for (size_t i = 0; i < pad_len; ++i) {
        if (out_buf[ct_len - 1 - i] != pad_len) {
            *out_len = 0;
            return -2;
        }
    }

    *out_len = ct_len - pad_len;
    out_buf[*out_len] = '\0';
    return 0;
}
