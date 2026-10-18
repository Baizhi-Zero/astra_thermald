/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef SAFETY_GUARD_H
#define SAFETY_GUARD_H

#include "sensor_hub.h"

typedef struct {
    int32_t current_fcc_limit_ma;
    bool torch_melt_protection_on;
    bool extreme_overheat_engaged;
    int32_t last_target_fcc_ma;
} safety_state_t;

int safety_guard_init(void);
int safety_guard_evaluate(const sensor_snapshot_t *snap, int32_t virtual_temp);
const safety_state_t* safety_guard_get_state(void);
void safety_guard_restore(void);

#endif
