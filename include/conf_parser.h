/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef CONF_PARSER_H
#define CONF_PARSER_H

#include "astra_thermald.h"
#include "../crypto/aes128.h"

#define MAX_TIERS 16
#define MAX_RULES 16

typedef struct {
    char section[64];
    char sensor[64];
    char device[64];
    int polling_ms;
    int num_tiers;
    int32_t trig[MAX_TIERS];
    int32_t clr[MAX_TIERS];
    int32_t target[MAX_TIERS];
    int current_tier;
} parsed_ss_rule_t;

typedef struct {
    char section[64];
    char sensor[64];
    char device[64];
    int polling_ms;
    int num_tiers;
    int32_t trig[MAX_TIERS];
    int32_t clr[MAX_TIERS];
    int32_t target[MAX_TIERS];
    int current_tier;
} parsed_monitor_rule_t;

typedef struct {
    char section[64];
    char sensor[64];
    char device[64];
    int polling_ms;
    int proportion;
    int num_tiers;
    int32_t trig[MAX_TIERS];
    int32_t clr[MAX_TIERS];
    int32_t target[MAX_TIERS];
    int32_t ks[MAX_TIERS];
    int32_t ki[MAX_TIERS];
    int32_t kc[MAX_TIERS];
    int32_t max[MAX_TIERS];
    int32_t min[MAX_TIERS];
    int current_tier;
} parsed_sic_rule_t;

typedef struct {
    char section[64];
    char sensor_names[8][32];
    int32_t weights[8];
    int num_sensors;
    int weight_sum;
    int compensation;
    int polling_ms;
} parsed_virtual_sensor_t;

typedef struct {
    char profile_name[64];
    parsed_virtual_sensor_t v_sensor;
    parsed_ss_rule_t ss_rules[MAX_RULES];
    int num_ss_rules;
    parsed_monitor_rule_t mon_rules[MAX_RULES];
    int num_mon_rules;
    parsed_sic_rule_t sic_rules[4];
    int num_sic_rules;
    bool loaded;
} thermal_policy_t;

int conf_parser_load_file(const char *filepath, thermal_policy_t *out_policy);
int conf_parser_parse_string(const char *text, size_t len, thermal_policy_t *out_policy);
void conf_parser_dump_summary(const thermal_policy_t *policy);

#endif
