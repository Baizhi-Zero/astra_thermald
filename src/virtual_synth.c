/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "virtual_synth.h"

static virtual_weights_t s_weights;
static int32_t s_smoothed_temp = 0;
static bool s_initialized = false;

void virtual_synth_init(void) {
    s_weights.weight_batt    = 544;
    s_weights.weight_pa0     = -157;
    s_weights.weight_pa1     = -724;
    s_weights.weight_quiet   = 905;
    s_weights.weight_charger = 80;
    s_weights.weight_wifi    = 321;
    s_weights.weight_cpu     = -34;
    s_weights.weight_sum     = 1000;
    s_weights.compensation   = 1337;

    s_smoothed_temp = 0;
    s_initialized = true;

    ASTRA_LOG_INFO("Virtual Synth initialized with Stock 7-NTC Matrix Weights (comp: %d)",
                   s_weights.compensation);
}

int32_t virtual_synth_compute(const sensor_snapshot_t *snap) {
    if (!snap || !s_initialized) return 35000;

    int64_t sum = 0;
    sum += (int64_t)snap->temp_battery  * s_weights.weight_batt;
    sum += (int64_t)snap->temp_pa0      * s_weights.weight_pa0;
    sum += (int64_t)snap->temp_pa1      * s_weights.weight_pa1;
    sum += (int64_t)snap->temp_quiet    * s_weights.weight_quiet;
    sum += (int64_t)snap->temp_charger0 * s_weights.weight_charger;
    sum += (int64_t)snap->temp_wifi     * s_weights.weight_wifi;
    sum += (int64_t)snap->temp_cpu      * s_weights.weight_cpu;

    int32_t w_sum = (s_weights.weight_sum > 0) ? s_weights.weight_sum : 1000;
    int32_t result = (int32_t)(sum / w_sum) + s_weights.compensation;

    if (result < -10000) result = -10000;
    if (result > 110000) result = 110000;

    if (s_smoothed_temp == 0) {
        s_smoothed_temp = result;
    } else {
        s_smoothed_temp = (result + 3 * s_smoothed_temp) / 4;
    }

    return s_smoothed_temp;
}

int32_t virtual_synth_get_smoothed(void) {
    return s_smoothed_temp;
}
