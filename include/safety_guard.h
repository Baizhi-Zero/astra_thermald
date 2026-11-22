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
#include "conf_parser.h"

typedef struct {
    int32_t current_fcc_limit_ma;
    int32_t desired_fcc_ma;
    int32_t applied_fcc_ma;
    int32_t observed_fcc_ma;
    bool    fcc_dirty;
    bool    fcc_modified_by_us;

    int64_t initial_fcc_ua;
    bool    initial_fcc_captured;

    int32_t initial_temp_state;
    bool    initial_temp_state_captured;
    bool    temp_state_modified_by_us;

    bool    torch_melt_protection_on;
    bool    extreme_overheat_engaged;
    bool    using_custom_sic;
} safety_state_t;

int safety_guard_init(void);

int safety_guard_load_sic_policy(const parsed_sic_rule_t *sic);

void safety_guard_clear_sic_policy(void);

int safety_guard_evaluate(const sensor_snapshot_t *snap, int32_t virtual_temp);

const safety_state_t* safety_guard_get_state(void);

void safety_guard_restore(void);

#endif
