/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "virtual_synth.h"
#include <fcntl.h>
#include <unistd.h>

#define PATH_BOARD_TEMP_COMP    "/sys/class/thermal/thermal_message/board_sensor_temp_comp"
#define V2_SPIKE_THRESHOLD_MC   10000
#define V2_MAX_GLITCH_RETRIES   3

static virtual_weights_t s_weights;
static int32_t s_dynamic_board_comp = 0;
static int32_t s_last_valid_raw = 0;
static int32_t s_smoothed_temp = 0;
static uint32_t s_consecutive_glitches = 0;
static uint32_t s_total_glitches_rejected = 0;
static bool s_initialized = false;
static bool s_has_first_sample = false;

static int32_t read_board_temp_comp(void) {
    char buf[32];
    int fd = open(PATH_BOARD_TEMP_COMP, O_RDONLY);
    if (fd < 0) return 0;

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    buf[n] = '\0';
    int32_t val = 0;
    if (astra_parse_int32_strict(buf, &val) && val >= -10000 && val <= 10000) {
        return val;
    }
    return 0;
}

void virtual_synth_reset_default(void) {

    s_weights.weight_batt    = 544;
    s_weights.weight_pa0     = -157;
    s_weights.weight_pa1     = -724;
    s_weights.weight_quiet   = 905;
    s_weights.weight_charger = 80;
    s_weights.weight_wifi    = 321;
    s_weights.weight_cpu     = -34;
    s_weights.weight_sum     = 1000;
    s_weights.compensation   = 1337;
}

void virtual_synth_init(void) {
    virtual_synth_reset_default();

    s_dynamic_board_comp      = read_board_temp_comp();
    s_last_valid_raw          = 0;
    s_smoothed_temp           = 0;
    s_consecutive_glitches    = 0;
    s_total_glitches_rejected = 0;
    s_has_first_sample        = false;
    s_initialized             = true;

    ASTRA_LOG_INFO("[VIRTUAL-SYNTH] Initialized Formula v2 7-NTC Matrix (comp: %d, board_offset: %d, spike_clamp: 10.0°C)",
                   s_weights.compensation, s_dynamic_board_comp);
}

int virtual_synth_load_config(const parsed_virtual_sensor_t *vs) {
    if (!vs || !vs->valid || vs->num_sensors <= 0 ||
        vs->num_sensors != vs->num_weights || vs->weight_sum == 0) {
        return -EINVAL;
    }

    virtual_weights_t next_w;
    memset(&next_w, 0, sizeof(next_w));
    next_w.weight_sum   = vs->weight_sum;
    next_w.compensation = vs->compensation;

    int matched = 0;
    for (int i = 0; i < vs->num_sensors; i++) {
        const char *name = vs->sensor_names[i];
        int32_t w = vs->weights[i];
        if (strcmp(name, "battery") == 0 || strcmp(name, "batt_therm") == 0) {
            next_w.weight_batt = w;
            matched++;
        } else if (strcmp(name, "pa_therm0") == 0) {
            next_w.weight_pa0 = w;
            matched++;
        } else if (strcmp(name, "pa_therm1") == 0) {
            next_w.weight_pa1 = w;
            matched++;
        } else if (strcmp(name, "quiet_therm") == 0) {
            next_w.weight_quiet = w;
            matched++;
        } else if (strcmp(name, "charger_therm0") == 0) {
            next_w.weight_charger = w;
            matched++;
        } else if (strcmp(name, "wifi_therm") == 0) {
            next_w.weight_wifi = w;
            matched++;
        } else if (strcmp(name, "cpu_therm") == 0) {
            next_w.weight_cpu = w;
            matched++;
        }
    }

    if (matched == 0) {
        ASTRA_LOG_WARN("[VIRTUAL-SYNTH] Parsed Virtual Sensor [%s] matched 0 known NTC channels, keeping defaults",
                       vs->section);
        return -EINVAL;
    }

    s_weights = next_w;
    ASTRA_LOG_INFO("[VIRTUAL-SYNTH] Applied Custom Virtual Sensor [%s] (%d channels matched, sum=%d, comp=%d)",
                   vs->section, matched, s_weights.weight_sum, s_weights.compensation);
    return 0;
}

void virtual_synth_set_board_comp(int32_t comp) {
    s_dynamic_board_comp = comp;
}

uint32_t virtual_synth_get_glitch_count(void) {
    return s_total_glitches_rejected;
}

int32_t virtual_synth_compute(const sensor_snapshot_t *snap) {
    if (!snap || !s_initialized) return 38000;

    int32_t raw_calc = 0;

    if (!snap->sensor_health_ok) {
        int32_t max_base = (snap->temp_battery > snap->temp_quiet) ? snap->temp_battery : snap->temp_quiet;
        raw_calc = max_base + 1000;
    } else {
        int64_t sum = 0;
        sum += (int64_t)snap->temp_battery  * s_weights.weight_batt;
        sum += (int64_t)snap->temp_pa0      * s_weights.weight_pa0;
        sum += (int64_t)snap->temp_pa1      * s_weights.weight_pa1;
        sum += (int64_t)snap->temp_quiet    * s_weights.weight_quiet;
        sum += (int64_t)snap->temp_charger0 * s_weights.weight_charger;
        sum += (int64_t)snap->temp_wifi     * s_weights.weight_wifi;
        sum += (int64_t)snap->temp_cpu      * s_weights.weight_cpu;

        int32_t w_sum = (s_weights.weight_sum != 0) ? s_weights.weight_sum : 1000;
        raw_calc = (int32_t)(sum / w_sum) + s_weights.compensation + s_dynamic_board_comp;
    }

    if (raw_calc < -10000) raw_calc = -10000;
    if (raw_calc > 110000) raw_calc = 110000;

    if (!s_has_first_sample) {
        s_has_first_sample = true;
        s_last_valid_raw   = raw_calc;
        s_smoothed_temp    = raw_calc;
        return s_smoothed_temp;
    }

    int32_t delta = raw_calc - s_last_valid_raw;
    if (delta < 0) delta = -delta;

    int32_t accepted_raw = raw_calc;

    if (delta > V2_SPIKE_THRESHOLD_MC) {
        s_consecutive_glitches++;
        if (s_consecutive_glitches <= V2_MAX_GLITCH_RETRIES) {
            accepted_raw = s_last_valid_raw;
            s_total_glitches_rejected++;
            ASTRA_LOG_WARN("[VIRTUAL-SYNTH] Suppressed Transient Sensor Spike: raw=%d m°C (delta=%d m°C > 10°C), retry=%u",
                           raw_calc, delta, s_consecutive_glitches);
        } else {
            s_consecutive_glitches = 0;
            s_last_valid_raw = raw_calc;
            ASTRA_LOG_INFO("[VIRTUAL-SYNTH] Sustained Thermal Step Change Accepted: raw=%d m°C", raw_calc);
        }
    } else {
        s_consecutive_glitches = 0;
        s_last_valid_raw = raw_calc;
    }

    s_smoothed_temp = (accepted_raw + 3 * s_smoothed_temp) / 4;
    return s_smoothed_temp;
}

int32_t virtual_synth_get_smoothed(void) {
    return s_smoothed_temp;
}
