/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef ARBITER_VOTING_H
#define ARBITER_VOTING_H

#include "astra_thermald.h"
#include "sensor_hub.h"
#include "conf_parser.h"

#define ARBITER_MAX_TIERS 16

typedef struct {
    char device_name[32];
    int num_tiers;
    int current_tier;
    int32_t trig[ARBITER_MAX_TIERS];
    int32_t clr[ARBITER_MAX_TIERS];
    int32_t target[ARBITER_MAX_TIERS];
} arbiter_stepwise_device_t;

typedef struct {
    int32_t cpu_cluster0_limit_khz;
    int32_t cpu_cluster1_limit_khz;
    int32_t cpu_cluster2_limit_khz;
    int32_t cpu_cluster3_limit_khz;
    
    int32_t gpu_cool_level;
    int32_t brightness_max_limit;
    
    bool hotplug_locked_online;
    
    bool using_custom_policy;
    char active_profile_name[64];
} arbiter_state_t;

int arbiter_voting_init(void);
int arbiter_load_parsed_policy(const thermal_policy_t *policy);
int arbiter_voting_dispatch(astra_profile_t profile, int32_t virtual_temp);
int arbiter_force_cores_online(void);
const arbiter_state_t* arbiter_get_state(void);
void arbiter_voting_restore(void);

#endif
