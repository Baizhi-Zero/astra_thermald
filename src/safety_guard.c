/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "safety_guard.h"

#define PATH_FCC_OVERRIDE "/sys/class/power_supply/battery/constant_charge_current"
#define PATH_TEMP_STATE   "/sys/class/thermal/thermal_message/temp_state"

static safety_state_t s_state;
static int s_fd_fcc = -1;
static int s_fd_temp_state = -1;

static parsed_sic_rule_t s_sic_rule;
static int32_t s_sic_cur_fcc_ma = 22000;
static int32_t s_sic_prev_temp  = 0;
static bool    s_sic_has_prev   = false;

static bool read_sysfs_int64(int fd, int64_t *out_val) {
    if (fd < 0 || !out_val) return false;
    if (lseek(fd, 0, SEEK_SET) < 0 && errno != ESPIPE && errno != EINVAL) {
        return false;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';

    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\r' || *p == '\n') return false;

    errno = 0;
    char *endptr = NULL;
    long long v = strtoll(p, &endptr, 10);
    if (p == endptr || errno == ERANGE) return false;
    *out_val = (int64_t)v;
    return true;
}

static int write_sysfs_int(int fd, int64_t val) {
    if (fd < 0) return -EBADF;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        if (errno != ESPIPE && errno != EINVAL) {
            return -errno;
        }
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld\n", (long long)val);
    if (len <= 0 || len >= (int)sizeof(buf)) return -EINVAL;

    ssize_t written = write(fd, buf, (size_t)len);
    return (written == (ssize_t)len) ? 0 : -EIO;
}

int safety_guard_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.current_fcc_limit_ma = 22000;
    s_state.desired_fcc_ma       = 22000;
    s_state.applied_fcc_ma       = -1;
    s_state.observed_fcc_ma      = -1;
    s_state.fcc_dirty            = true;
    s_state.fcc_modified_by_us   = false;

    s_sic_cur_fcc_ma = 22000;
    s_sic_prev_temp  = 0;
    s_sic_has_prev   = false;

    s_fd_fcc = open(PATH_FCC_OVERRIDE, O_RDWR);
    if (s_fd_fcc < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s (Will retry dynamically)",
                       PATH_FCC_OVERRIDE, strerror(errno));
    } else {
        int64_t init_ua = 0;
        if (read_sysfs_int64(s_fd_fcc, &init_ua) && init_ua > 0) {
            s_state.initial_fcc_ua       = init_ua;
            s_state.initial_fcc_captured = true;
            s_state.observed_fcc_ma      = (int32_t)(init_ua / 1000LL);
        }
        ASTRA_LOG_INFO("Bound to 120W FCC Override Node (fd=%d, initial=%lld uA)",
                       s_fd_fcc, (long long)s_state.initial_fcc_ua);
    }

    s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
    if (s_fd_temp_state < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s", PATH_TEMP_STATE, strerror(errno));
    } else {
        int64_t init_ts = 0;
        if (read_sysfs_int64(s_fd_temp_state, &init_ts)) {
            s_state.initial_temp_state          = (int32_t)init_ts;
            s_state.initial_temp_state_captured = true;
        }
        ASTRA_LOG_INFO("Bound to Temp State Safety Node (fd=%d, initial=%d)",
                       s_fd_temp_state, s_state.initial_temp_state);
    }

    return 0;
}

int safety_guard_load_sic_policy(const parsed_sic_rule_t *sic) {
    if (!sic || sic->num_tiers <= 0) return -EINVAL;
    memcpy(&s_sic_rule, sic, sizeof(s_sic_rule));
    s_sic_rule.current_tier  = 0;
    s_sic_cur_fcc_ma         = 22000;
    s_sic_has_prev           = false;
    s_state.using_custom_sic = true;
    s_state.fcc_dirty        = true;
    ASTRA_LOG_INFO("[SAFETY-SIC] Loaded Custom SIC Fast-Charge PID Policy [%s] (%d tiers)",
                   sic->section, sic->num_tiers);
    return 0;
}

void safety_guard_clear_sic_policy(void) {
    s_state.using_custom_sic = false;
    s_sic_has_prev           = false;
    s_state.fcc_dirty        = true;
}

static int32_t evaluate_sic_pid_ma(int32_t temp_mc) {
    parsed_sic_rule_t *r = &s_sic_rule;
    int tier = r->current_tier;

    while (tier < r->num_tiers && temp_mc >= r->trig[tier]) {
        tier++;
    }
    while (tier > 0 && temp_mc < r->clr[tier - 1]) {
        tier--;
    }
    r->current_tier = tier;

    if (!s_sic_has_prev) {
        s_sic_prev_temp = temp_mc;
        s_sic_has_prev  = true;
    }

    if (tier == 0) {
        s_sic_prev_temp  = temp_mc;
        s_sic_cur_fcc_ma = 22000;
        return 22000;
    }

    int idx = tier - 1;
    int32_t max_ma = (r->max[idx] > 0) ? r->max[idx] : 22000;
    int32_t min_ma = (r->min[idx] > 0) ? r->min[idx] : 300;
    if (min_ma > max_ma) min_ma = max_ma;

    int32_t tgt_temp = r->target[idx];
    if (tgt_temp <= 0) {
        s_sic_prev_temp  = temp_mc;
        s_sic_cur_fcc_ma = max_ma;
        return max_ma;
    }

    int32_t err   = temp_mc - tgt_temp;
    int32_t d_temp = temp_mc - s_sic_prev_temp;
    s_sic_prev_temp = temp_mc;

    int32_t delta_ma = -(err / 10) - (d_temp / 5);
    int32_t next_ma  = s_sic_cur_fcc_ma + delta_ma;

    if (next_ma > max_ma) next_ma = max_ma;
    if (next_ma < min_ma) next_ma = min_ma;

    s_sic_cur_fcc_ma = next_ma;
    return s_sic_cur_fcc_ma;
}

static int32_t calculate_builtin_fcc_ma(int32_t t_batt, int32_t t_virtual) {
    int32_t max_t = (t_batt > t_virtual) ? t_batt : t_virtual;

    if (max_t < 33000) {
        return 22000;
    } else if (max_t < 35000) {
        return 18000;
    } else if (max_t < 38000) {
        return 16000;
    } else if (max_t < 40000) {
        return 9000;
    } else if (max_t < 41500) {
        return 8000;
    } else if (max_t < 44000) {
        return 6500;
    } else if (max_t < 45000) {
        return 2500;
    } else if (max_t < 46000) {
        return 1000;
    } else {
        return 500;
    }
}

int safety_guard_evaluate(const sensor_snapshot_t *snap, int32_t virtual_temp) {
    if (!snap) return -EINVAL;

    if (virtual_temp >= 55000 || snap->temp_battery >= 48000) {
        if (!s_state.extreme_overheat_engaged) {
            s_state.extreme_overheat_engaged = true;
            s_state.fcc_dirty = true;
            ASTRA_LOG_ERROR("[SAFETY-EXTREME] Critical thermal threshold breached! (Virt: %.1f°C, Batt: %.1f°C). Enforcing 500mA trickle limit.",
                            virtual_temp / 1000.0f, snap->temp_battery / 1000.0f);
        }
    } else if (virtual_temp <= 50000 && snap->temp_battery <= 45000) {
        if (s_state.extreme_overheat_engaged) {
            s_state.extreme_overheat_engaged = false;
            s_state.fcc_dirty = true;
            ASTRA_LOG_INFO("[SAFETY-EXTREME] Temperature normalized below critical threshold. Emergency override released.");
        }
    }

    int32_t normal_fcc_ma = 22000;
    if (s_state.using_custom_sic) {
        int32_t max_t = (snap->temp_battery > virtual_temp) ? snap->temp_battery : virtual_temp;
        normal_fcc_ma = evaluate_sic_pid_ma(max_t);
    } else {
        normal_fcc_ma = calculate_builtin_fcc_ma(snap->temp_battery, virtual_temp);
    }

    if (!snap->sensor_health_ok && normal_fcc_ma > 6500) {
        normal_fcc_ma = 6500;
    }

    if (s_state.extreme_overheat_engaged) {
        s_state.desired_fcc_ma = 500;
    } else {
        s_state.desired_fcc_ma = normal_fcc_ma;
    }

    if (s_state.desired_fcc_ma != s_state.applied_fcc_ma || s_state.fcc_dirty) {
        if (s_fd_fcc < 0) {
            s_fd_fcc = open(PATH_FCC_OVERRIDE, O_RDWR);
        }

        if (s_fd_fcc >= 0) {
            int64_t fcc_ua = (int64_t)s_state.desired_fcc_ma * 1000LL;
            if (write_sysfs_int(s_fd_fcc, fcc_ua) == 0) {
                s_state.applied_fcc_ma       = s_state.desired_fcc_ma;
                s_state.current_fcc_limit_ma = s_state.desired_fcc_ma;
                s_state.fcc_dirty            = false;
                s_state.fcc_modified_by_us   = true;

                int64_t obs_ua = 0;
                if (read_sysfs_int64(s_fd_fcc, &obs_ua)) {
                    s_state.observed_fcc_ma = (int32_t)(obs_ua / 1000LL);
                } else {
                    s_state.observed_fcc_ma = s_state.applied_fcc_ma;
                }

                ASTRA_LOG_INFO("[SAFETY-FCC] Applied FCC limit -> %d mA (Batt: %.1f°C, Virt: %.1f°C%s)",
                               s_state.applied_fcc_ma,
                               snap->temp_battery / 1000.0f,
                               virtual_temp / 1000.0f,
                               s_state.extreme_overheat_engaged ? ", EXTREME-OVERRIDE" : "");
            } else {

                close(s_fd_fcc);
                s_fd_fcc = -1;
                s_state.fcc_dirty = true;
            }
        } else {
            s_state.fcc_dirty = true;
        }
    }

    if (snap->valid_mask & SENSOR_VALID_FLASH) {
        if (snap->temp_flash >= 65000) {
            if (!s_state.torch_melt_protection_on) {
                if (s_fd_temp_state < 0) {
                    s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
                }
                if (s_fd_temp_state >= 0 && write_sysfs_int(s_fd_temp_state, 110100004LL) == 0) {
                    s_state.torch_melt_protection_on = true;
                    s_state.temp_state_modified_by_us = true;
                    ASTRA_LOG_WARN("[SAFETY-TORCH] Real flash_therm reached %d m°C! Enforced LED shutdown (110100004).",
                                   snap->temp_flash);
                }
            }
        } else if (snap->temp_flash <= 62000) {
            if (s_state.torch_melt_protection_on) {
                if (s_fd_temp_state < 0) {
                    s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
                }
                int64_t restore_ts = s_state.initial_temp_state_captured ? s_state.initial_temp_state : 0LL;
                if (s_fd_temp_state >= 0 && write_sysfs_int(s_fd_temp_state, restore_ts) == 0) {
                    s_state.torch_melt_protection_on = false;
                    ASTRA_LOG_INFO("[SAFETY-TORCH] Flashlight temp recovered to %d m°C. Restored temp_state=%lld.",
                                   snap->temp_flash, (long long)restore_ts);
                }
            }
        }
    }

    return 0;
}

const safety_state_t* safety_guard_get_state(void) {
    return &s_state;
}

void safety_guard_restore(void) {

    if (s_state.fcc_modified_by_us) {
        if (s_fd_fcc < 0) {
            s_fd_fcc = open(PATH_FCC_OVERRIDE, O_RDWR);
        }
        if (s_fd_fcc >= 0) {
            int64_t restore_ua = s_state.initial_fcc_captured ? s_state.initial_fcc_ua : 22000000LL;
            write_sysfs_int(s_fd_fcc, restore_ua);
            ASTRA_LOG_INFO("[SAFETY-RESTORE] Restored FCC limit to %lld uA", (long long)restore_ua);
        }
        s_state.fcc_modified_by_us = false;
    }

    if (s_state.temp_state_modified_by_us) {
        if (s_fd_temp_state < 0) {
            s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
        }
        if (s_fd_temp_state >= 0) {
            int64_t restore_ts = s_state.initial_temp_state_captured ? s_state.initial_temp_state : 0LL;
            write_sysfs_int(s_fd_temp_state, restore_ts);
            ASTRA_LOG_INFO("[SAFETY-RESTORE] Restored temp_state to %lld", (long long)restore_ts);
        }
        s_state.temp_state_modified_by_us = false;
    }

    if (s_fd_fcc >= 0) {
        close(s_fd_fcc);
        s_fd_fcc = -1;
    }
    if (s_fd_temp_state >= 0) {
        close(s_fd_temp_state);
        s_fd_temp_state = -1;
    }
}
