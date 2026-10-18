/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AstraThermald - Qualcomm Snapdragon 8 Gen 3 (SM8650) Thermal Daemon
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
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#define ASTRA_THERMALD_VERSION "2.0.0-dual-engine"
#define ASTRA_THERMALD_AUTHOR  "Baizhi, Gemini"
#define ASTRA_THERMALD_LICENSE "GPL-3.0-or-later"
#define ASTRA_THERMALD_WATERMARK "AstraThermald-SM8650-Release-(C)2026-Baizhi-Gemini"
#define ASTRA_LOG_TAG "AstraThermald"

typedef enum {
    PROFILE_STOCK_REPLICA = 0,
    PROFILE_BALANCED      = 1,
    PROFILE_ESPORTS       = 2,
    PROFILE_BEAST_UNLOCKED= 3,
    PROFILE_AUTO          = 4
} astra_profile_t;

typedef struct {
    volatile sig_atomic_t running;
    astra_profile_t active_profile;
    bool auto_sconfig;
    int last_sconfig;
    bool takeover_mi_thermald;
    bool enforce_permissions;
    uint32_t poll_interval_ms;
    char config_path[256];
    char log_dump_path[256];
    pthread_mutex_t lock;
} astra_context_t;

extern astra_context_t g_astra_ctx;

#define ASTRA_LOG_INFO(fmt, ...) \
    do { fprintf(stdout, "[ASTRA-TH] [I] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

#define ASTRA_LOG_WARN(fmt, ...) \
    do { fprintf(stderr, "[ASTRA-TH] [W] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#define ASTRA_LOG_ERROR(fmt, ...) \
    do { fprintf(stderr, "[ASTRA-TH] [E] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#define ASTRA_LOG_DEBUG(fmt, ...) \
    do { fprintf(stdout, "[ASTRA-TH] [D] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

#endif
