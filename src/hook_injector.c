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
    ASTRA_LOG_INFO("Initializing Privileged Hook Subsystem (Unconstrained KSU Power Mode)...");
    s_last_synced_profile = (astra_profile_t)-1;

    if (access(PATH_LKM_NODE, F_OK) == 0) {
        s_lkm_available = true;
        ASTRA_LOG_INFO("Detected Active AstraCore LKM at %s! Kernel-level interop enabled.", PATH_LKM_NODE);
    } else {
        s_lkm_available = false;
        ASTRA_LOG_INFO("AstraCore LKM not loaded; running in Userspace Native Root Authority mode.");
    }

    return 0;
}

int hook_sync_to_kernel_lkm(astra_profile_t profile, int32_t virtual_temp) {
    (void)virtual_temp;
    if (!s_lkm_available) return 0;
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

    ssize_t written = write(fd, prof_str, strlen(prof_str));
    close(fd);

    if (written > 0) {
        s_last_synced_profile = profile;
        ASTRA_LOG_INFO("[HOOK-LKM] Synced profile state -> '%s' to %s", prof_str, PATH_LKM_NODE);
    }
    return 0;
}

int hook_enforce_node_lock(const char *node_path, bool locked) {
    if (!node_path) return -EINVAL;

    mode_t mode = locked ? 0444 : 0644;
    if (chmod(node_path, mode) < 0) {
        ASTRA_LOG_WARN("Failed to chmod %s to %04o: %s", node_path, mode, strerror(errno));
        return -errno;
    }
    return 0;
}

void hook_injector_cleanup(void) {
    ASTRA_LOG_INFO("Cleaning up privileged hook subsystem...");
}
