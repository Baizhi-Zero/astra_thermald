/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef ASTRA_THERMALD_H
#define ASTRA_THERMALD_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#define ASTRA_THERMALD_VERSION "2.1.0-hardened"
#define ASTRA_LOG_TAG          "AstraThermald"

#define ASTRA_MOD_DIR          "/data/local/tmp"
#define ASTRA_DEFAULT_LOG_PATH "/data/local/tmp/astra_thermald.log"
#define ASTRA_DEFAULT_PID_PATH "/data/local/tmp/astra_thermald.pid"
#define ASTRA_CTRL_NODE_PATH   "/data/local/tmp/astra_profile"

typedef enum {
    PROFILE_STOCK_REPLICA  = 0,
    PROFILE_BALANCED       = 1,
    PROFILE_ESPORTS        = 2,
    PROFILE_BEAST_UNLOCKED = 3,
    PROFILE_AUTO           = 4
} astra_profile_t;

typedef struct {
    volatile sig_atomic_t running;
    astra_profile_t active_profile;
    bool auto_sconfig;
    bool manual_mode_locked;
    int last_sconfig;
    bool takeover_mi_thermald;
    uint32_t poll_interval_ms;
    char config_path[256];
    char log_path[256];
} astra_context_t;

extern astra_context_t g_astra_ctx;

static inline bool astra_parse_int32_strict(const char *str, int32_t *out_val) {
    if (!str || !out_val) return false;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '\0' || *str == '\n' || *str == '\r') return false;

    errno = 0;
    char *endptr = NULL;
    long val = strtol(str, &endptr, 10);
    if (endptr == str || errno == ERANGE || val < INT32_MIN || val > INT32_MAX) {
        return false;
    }
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        endptr++;
    }
    if (*endptr != '\0') {
        return false;
    }
    *out_val = (int32_t)val;
    return true;
}

#define ASTRA_LOG_INFO(fmt, ...) \
    do { fprintf(stdout, "[ASTRA-TH] [I] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

#define ASTRA_LOG_WARN(fmt, ...) \
    do { fprintf(stderr, "[ASTRA-TH] [W] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#define ASTRA_LOG_ERROR(fmt, ...) \
    do { fprintf(stderr, "[ASTRA-TH] [E] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#define ASTRA_LOG_DEBUG(fmt, ...) \
    do { fprintf(stdout, "[ASTRA-TH] [D] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

#endif
