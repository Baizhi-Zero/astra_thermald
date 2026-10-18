/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef HOOK_INJECTOR_H
#define HOOK_INJECTOR_H

#include "astra_thermald.h"

typedef enum {
    HOOK_TYPE_NONE           = 0,
    HOOK_TYPE_LKM_PROC       = 1,
    HOOK_TYPE_SYSFS_LOCK     = 2,
    HOOK_TYPE_QTI_PERF_STUB  = 3
} astra_hook_type_t;

int hook_injector_init(void);
int hook_sync_to_kernel_lkm(astra_profile_t profile, int32_t virtual_temp);
int hook_enforce_node_lock(const char *node_path, bool locked);
void hook_injector_cleanup(void);

#endif
