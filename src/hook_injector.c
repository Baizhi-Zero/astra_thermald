/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "hook_injector.h"

#define PATH_LKM_NODE "/proc/k70perf"

static bool s_lkm_available = false;
static astra_profile_t s_last_synced_profile = (astra_profile_t)-1;

int hook_injector_init(void) {
    s_last_synced_profile = (astra_profile_t)-1;

    if (access(PATH_LKM_NODE, F_OK) == 0) {
        s_lkm_available = true;
        ASTRA_LOG_INFO("Detected Active AstraCore LKM at %s! Optional kernel-level interop enabled.", PATH_LKM_NODE);
    } else {
        s_lkm_available = false;
        ASTRA_LOG_INFO("Custom LKM not loaded; running in standalone Userspace Root mode.");
    }

    return 0;
}

int hook_sync_to_kernel_lkm(astra_profile_t profile) {

    if (access(PATH_LKM_NODE, F_OK) != 0) {
        s_lkm_available = false;
        s_last_synced_profile = (astra_profile_t)-1;
        return 0;
    }
    s_lkm_available = true;

    if (profile == s_last_synced_profile) return 0;

    int fd = open(PATH_LKM_NODE, O_WRONLY);
    if (fd < 0) return -errno;

    const char *prof_str = "balance";
    switch (profile) {
        case PROFILE_BEAST_UNLOCKED: prof_str = "beast"; break;
        case PROFILE_ESPORTS:        prof_str = "performance"; break;
        case PROFILE_BALANCED:       prof_str = "balance"; break;
        case PROFILE_STOCK_REPLICA:  prof_str = "stock"; break;
        default:                     prof_str = "balance"; break;
    }

    size_t expected_len = strlen(prof_str);
    ssize_t written = write(fd, prof_str, expected_len);
    int saved_err = errno;
    close(fd);

    if (written != (ssize_t)expected_len) {
        return (written < 0) ? -saved_err : -EIO;
    }

    s_last_synced_profile = profile;
    ASTRA_LOG_INFO("[HOOK-LKM] Synced profile state -> '%s' to %s", prof_str, PATH_LKM_NODE);
    return 0;
}

void hook_injector_cleanup(void) {
    s_last_synced_profile = (astra_profile_t)-1;
}
