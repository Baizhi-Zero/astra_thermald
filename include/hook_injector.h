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

int hook_injector_init(void);

int hook_sync_to_kernel_lkm(astra_profile_t profile);

void hook_injector_cleanup(void);

#endif
