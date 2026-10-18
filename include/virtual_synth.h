/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef VIRTUAL_SYNTH_H
#define VIRTUAL_SYNTH_H

#include "sensor_hub.h"

typedef struct {
    int32_t weight_batt;
    int32_t weight_pa0;
    int32_t weight_pa1;
    int32_t weight_quiet;
    int32_t weight_charger;
    int32_t weight_wifi;
    int32_t weight_cpu;
    int32_t weight_sum;
    int32_t compensation;
} virtual_weights_t;

void virtual_synth_init(void);
int32_t virtual_synth_compute(const sensor_snapshot_t *snap);
int32_t virtual_synth_get_smoothed(void);

#endif
