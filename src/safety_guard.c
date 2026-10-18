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

static int write_sysfs_int(int fd, int64_t val) {
    if (fd < 0) return -EBADF;
    if (lseek(fd, 0, SEEK_SET) < 0) return -errno;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld\n", (long long)val);
    ssize_t written = write(fd, buf, len);
    return (written == len) ? 0 : -EIO;
}

int safety_guard_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.current_fcc_limit_ma = 22000;
    s_state.last_target_fcc_ma   = 22000;

    s_fd_fcc = open(PATH_FCC_OVERRIDE, O_RDWR);
    if (s_fd_fcc < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s", PATH_FCC_OVERRIDE, strerror(errno));
    } else {
        ASTRA_LOG_INFO("Bound to 120W FCC Override Node (fd=%d)", s_fd_fcc);
    }

    s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
    if (s_fd_temp_state < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s", PATH_TEMP_STATE, strerror(errno));
    } else {
        ASTRA_LOG_INFO("Bound to Temp State Safety Node (fd=%d)", s_fd_temp_state);
    }

    return 0;
}

static int32_t calculate_safe_fcc_ma(int32_t t_batt, int32_t t_virtual) {
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

    int32_t target_fcc = calculate_safe_fcc_ma(snap->temp_battery, virtual_temp);
    if (s_fd_fcc < 0) {
        s_fd_fcc = open(PATH_FCC_OVERRIDE, O_RDWR);
    }
    if (target_fcc != s_state.current_fcc_limit_ma || s_fd_fcc >= 0) {
        if (target_fcc != s_state.current_fcc_limit_ma) {
            s_state.current_fcc_limit_ma = target_fcc;
            int64_t fcc_ua = (int64_t)target_fcc * 1000LL;
            if (s_fd_fcc >= 0) {
                write_sysfs_int(s_fd_fcc, fcc_ua);
                ASTRA_LOG_INFO("[SAFETY-FCC] Adjusted FCC limit -> %d mA (Batt: %.1f°C, Virt: %.1f°C)",
                               target_fcc, snap->temp_battery / 1000.0f, virtual_temp / 1000.0f);
            }
        }
    }

    if (s_fd_temp_state < 0) {
        s_fd_temp_state = open(PATH_TEMP_STATE, O_RDWR);
    }
    if (snap->temp_flash >= 65000) {
        if (!s_state.torch_melt_protection_on) {
            s_state.torch_melt_protection_on = true;
            if (s_fd_temp_state >= 0) {
                write_sysfs_int(s_fd_temp_state, 110100004LL);
                ASTRA_LOG_WARN("[SAFETY-TORCH] Flashlight temp reached %d m°C! Enforcing LED shutdown.",
                               snap->temp_flash);
            }
        }
    } else if (snap->temp_flash <= 62000) {
        if (s_state.torch_melt_protection_on) {
            s_state.torch_melt_protection_on = false;
            ASTRA_LOG_INFO("[SAFETY-TORCH] Flashlight temp recovered to %d m°C. Guard released.",
                            snap->temp_flash);
        }
    }

    if (virtual_temp >= 55000 || snap->temp_battery >= 48000) {
        if (!s_state.extreme_overheat_engaged) {
            s_state.extreme_overheat_engaged = true;
            ASTRA_LOG_ERROR("[SAFETY-EXTREME] Critical thermal threshold breached! (Virt: %.1f°C, Batt: %.1f°C). Emergency 500mA throttle engaged.",
                            virtual_temp / 1000.0f, snap->temp_battery / 1000.0f);
            if (s_fd_fcc >= 0) {
                write_sysfs_int(s_fd_fcc, 500000LL);
            }
        }
    } else if (virtual_temp <= 50000 && snap->temp_battery <= 45000) {
        if (s_state.extreme_overheat_engaged) {
            s_state.extreme_overheat_engaged = false;
            ASTRA_LOG_INFO("[SAFETY-EXTREME] Temperature normalized below critical threshold. Guard released.");
        }
    }

    return 0;
}

const safety_state_t* safety_guard_get_state(void) {
    return &s_state;
}

void safety_guard_restore(void) {
    ASTRA_LOG_INFO("Restoring default hardware safety parameters...");
    if (s_fd_fcc >= 0) {
        write_sysfs_int(s_fd_fcc, 22000000LL);
        close(s_fd_fcc);
        s_fd_fcc = -1;
    }
    if (s_fd_temp_state >= 0) {
        close(s_fd_temp_state);
        s_fd_temp_state = -1;
    }
}
