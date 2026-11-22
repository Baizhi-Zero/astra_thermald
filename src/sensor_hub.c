/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "sensor_hub.h"

typedef struct {
    int fd_battery;
    int fd_battery_sec;
    int fd_pa0;
    int fd_pa1;
    int fd_quiet;
    int fd_charger0;
    int fd_wifi;
    int fd_cpu;
    int fd_flash;
    int fd_conn;
    int fd_wireless;

    int fd_cpu_core_x4;
    int fd_cpu_core_big;

    int fd_batt_soc;
    int fd_batt_current;
    int fd_batt_voltage;
    int fd_board_hw;
} sensor_fds_t;

static sensor_fds_t s_fds = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

static bool read_fd_int_strict(int *pfd, int32_t *out_val) {
    if (!pfd || *pfd < 0 || !out_val) return false;
    int fd = *pfd;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        *pfd = -1;
        return false;
    }

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        *pfd = -1;
        return false;
    }
    buf[n] = '\0';
    return astra_parse_int32_strict(buf, out_val);
}

typedef struct {
    char type[32];
    int zone_id;
} tz_cache_entry_t;

#define MAX_TZ_CACHE 128
static tz_cache_entry_t s_tz_cache[MAX_TZ_CACHE];
static int s_tz_count = 0;

static void build_thermal_zone_cache(void) {
    char type_path[64];
    char buf[64];
    s_tz_count = 0;

    for (int i = 0; i < MAX_TZ_CACHE; i++) {
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/thermal_zone%d/type", i);
        int fd = open(type_path, O_RDONLY);
        if (fd < 0) continue;

        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n > 0) {
            buf[n] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            nl = strchr(buf, '\r');
            if (nl) *nl = '\0';

            strncpy(s_tz_cache[s_tz_count].type, buf, sizeof(s_tz_cache[s_tz_count].type) - 1);
            s_tz_cache[s_tz_count].type[sizeof(s_tz_cache[s_tz_count].type) - 1] = '\0';
            s_tz_cache[s_tz_count].zone_id = i;
            s_tz_count++;
        }
    }
    ASTRA_LOG_INFO("[SENSOR-CACHE] Discovered & cached %d thermal zones into memory table (0 sysfs scan at runtime)",
                   s_tz_count);
}

static int find_thermal_zone_by_type(const char *target_type) {
    if (!target_type) return -1;
    for (int i = 0; i < s_tz_count; i++) {
        if (strcmp(s_tz_cache[i].type, target_type) == 0) {
            return s_tz_cache[i].zone_id;
        }
    }
    return -1;
}

static int open_tz_temp_fd(const char *type_name) {
    int zid = find_thermal_zone_by_type(type_name);
    if (zid < 0) {
        return -2;
    }
    char temp_path[64];
    snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", zid);
    int fd = open(temp_path, O_RDONLY);
    return (fd >= 0) ? fd : -1;
}

static int open_tz_with_fallbacks(const char *t1, const char *t2, const char *t3) {
    int fd = -2;
    if (t1) {
        fd = open_tz_temp_fd(t1);
        if (fd >= 0) return fd;
    }
    if (t2) {
        int fd2 = open_tz_temp_fd(t2);
        if (fd2 >= 0) return fd2;
        if (fd2 == -1) fd = -1;
    }
    if (t3) {
        int fd3 = open_tz_temp_fd(t3);
        if (fd3 >= 0) return fd3;
        if (fd3 == -1) fd = -1;
    }
    return fd;
}

int sensor_hub_init(void) {
    ASTRA_LOG_INFO("Initializing Redundant Sensor Hub (Multi-Source 7-NTC + Strict Validation)...");

    build_thermal_zone_cache();

    s_fds.fd_pa0      = open_tz_temp_fd("pa_therm0");
    s_fds.fd_pa1      = open_tz_temp_fd("pa_therm1");
    s_fds.fd_quiet    = open_tz_temp_fd("quiet_therm");
    s_fds.fd_charger0 = open_tz_temp_fd("charger_therm0");
    s_fds.fd_wifi     = open_tz_temp_fd("wifi_therm");
    s_fds.fd_cpu      = open_tz_temp_fd("cpu_therm");

    s_fds.fd_flash        = open_tz_temp_fd("flash_therm");
    s_fds.fd_conn         = open_tz_temp_fd("conn_therm");
    s_fds.fd_wireless     = open_tz_temp_fd("wireless_therm");
    s_fds.fd_battery_sec  = open_tz_with_fallbacks("battery", "batt_therm", NULL);
    s_fds.fd_cpu_core_x4  = open_tz_with_fallbacks("cpu-2-2-0", "cpu-1-7-usr", "cpu_therm");
    s_fds.fd_cpu_core_big = open_tz_with_fallbacks("cpu-1-2-0", "cpu-1-5-usr", "cpu_therm");

    s_fds.fd_battery      = open("/sys/class/power_supply/battery/temp", O_RDONLY);
    s_fds.fd_batt_soc     = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    s_fds.fd_batt_current = open("/sys/class/power_supply/battery/current_now", O_RDONLY);
    s_fds.fd_batt_voltage = open("/sys/class/power_supply/battery/voltage_now", O_RDONLY);
    s_fds.fd_board_hw     = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_RDONLY);

    ASTRA_LOG_INFO("Sensor Hub pre-opened FDs (Batt_Pri:%d, Batt_Sec:%d, Quiet:%d, X4_Core:%d, Big_Core:%d, BoardHW:%d)",
                   s_fds.fd_battery, s_fds.fd_battery_sec, s_fds.fd_quiet,
                   s_fds.fd_cpu_core_x4, s_fds.fd_cpu_core_big, s_fds.fd_board_hw);
    return 0;
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline bool is_sane_temp(int32_t val_mc) {
    return (val_mc >= -20000 && val_mc <= 110000);
}

int sensor_hub_sample(sensor_snapshot_t *snap) {
    if (!snap) return -EINVAL;

    snap->timestamp_ms         = get_time_ms();
    snap->valid_mask           = 0;
    snap->anomaly_mask         = 0;
    snap->valid_core_ntc_count = 0;
    snap->sensor_health_ok     = true;

    if (s_fds.fd_battery == -1) {
        s_fds.fd_battery = open("/sys/class/power_supply/battery/temp", O_RDONLY);
    }
    if (s_fds.fd_battery_sec == -1) {
        s_fds.fd_battery_sec = open_tz_with_fallbacks("battery", "batt_therm", NULL);
    }

    int32_t b_pri_raw = 0;
    int32_t b_pri_mc  = -999999;
    bool b_pri_ok = read_fd_int_strict(&s_fds.fd_battery, &b_pri_raw);
    if (b_pri_ok) {

        if (b_pri_raw >= -200 && b_pri_raw <= 1100) {
            b_pri_mc = b_pri_raw * 100;
        } else {
            b_pri_mc = b_pri_raw;
        }
        if (is_sane_temp(b_pri_mc)) {
            snap->valid_mask |= SENSOR_VALID_BATT_PRI;
        } else {
            b_pri_ok = false;
        }
    }

    int32_t b_sec_mc = -999999;
    bool b_sec_ok = read_fd_int_strict(&s_fds.fd_battery_sec, &b_sec_mc) && is_sane_temp(b_sec_mc);
    if (b_sec_ok) {
        snap->valid_mask |= SENSOR_VALID_BATT_SEC;
    }

    if (snap->valid_mask & SENSOR_VALID_BATT_PRI) {
        snap->temp_battery = b_pri_mc;
        snap->valid_core_ntc_count++;
    } else if (snap->valid_mask & SENSOR_VALID_BATT_SEC) {
        snap->temp_battery = b_sec_mc;
        snap->valid_core_ntc_count++;
        snap->anomaly_mask |= ANOMALY_BATT_CORRUPTED;
    } else {
        snap->temp_battery = 38000;
        snap->anomaly_mask |= ANOMALY_BATT_CORRUPTED;
    }
    snap->temp_battery_sec = (snap->valid_mask & SENSOR_VALID_BATT_SEC) ? b_sec_mc : snap->temp_battery;

    if (s_fds.fd_quiet == -1) {
        s_fds.fd_quiet = open_tz_temp_fd("quiet_therm");
    }
    int32_t q_raw = 0;
    if (read_fd_int_strict(&s_fds.fd_quiet, &q_raw) && is_sane_temp(q_raw)) {
        snap->temp_quiet = q_raw;
        snap->valid_mask |= SENSOR_VALID_QUIET;
        snap->valid_core_ntc_count++;
    } else {
        snap->temp_quiet = snap->temp_battery;
        snap->anomaly_mask |= ANOMALY_QUIET_CORRUPTED;
    }

    #define SAMPLE_CORE_NTC(field, pfd, tz_type, valid_bit, anomaly_bit, fallback_ref) \
        do { \
            if (*(pfd) == -1) *(pfd) = open_tz_temp_fd(tz_type); \
            int32_t v = 0; \
            if (read_fd_int_strict(pfd, &v) && is_sane_temp(v)) { \
                snap->field = v; \
                snap->valid_mask |= (valid_bit); \
                snap->valid_core_ntc_count++; \
            } else { \
                snap->field = (fallback_ref); \
                snap->anomaly_mask |= (anomaly_bit); \
            } \
        } while (0)

    SAMPLE_CORE_NTC(temp_pa0,      &s_fds.fd_pa0,      "pa_therm0",      SENSOR_VALID_PA0,  ANOMALY_PA0_CORRUPTED,  snap->temp_quiet);
    SAMPLE_CORE_NTC(temp_pa1,      &s_fds.fd_pa1,      "pa_therm1",      SENSOR_VALID_PA1,  ANOMALY_PA1_CORRUPTED,  snap->temp_pa0);
    SAMPLE_CORE_NTC(temp_charger0, &s_fds.fd_charger0, "charger_therm0", SENSOR_VALID_CHG0, ANOMALY_CHG_CORRUPTED,  snap->temp_battery);
    SAMPLE_CORE_NTC(temp_wifi,     &s_fds.fd_wifi,     "wifi_therm",     SENSOR_VALID_WIFI, ANOMALY_WIFI_CORRUPTED, snap->temp_quiet);
    SAMPLE_CORE_NTC(temp_cpu,      &s_fds.fd_cpu,      "cpu_therm",      SENSOR_VALID_CPU,  ANOMALY_CPU_CORRUPTED,  snap->temp_quiet);

    if (s_fds.fd_flash == -1) s_fds.fd_flash = open_tz_temp_fd("flash_therm");
    int32_t flash_v = 0;
    if (read_fd_int_strict(&s_fds.fd_flash, &flash_v) && is_sane_temp(flash_v)) {
        snap->temp_flash = flash_v;
        snap->valid_mask |= SENSOR_VALID_FLASH;
    } else {
        snap->temp_flash = snap->temp_quiet;
        snap->anomaly_mask |= ANOMALY_FLASH_CORRUPTED;
    }

    if (s_fds.fd_conn == -1) s_fds.fd_conn = open_tz_temp_fd("conn_therm");
    int32_t conn_v = 0;
    if (read_fd_int_strict(&s_fds.fd_conn, &conn_v) && is_sane_temp(conn_v)) {
        snap->temp_conn = conn_v;
        snap->valid_mask |= SENSOR_VALID_CONN;
    } else {
        snap->temp_conn = snap->temp_battery;
    }

    if (s_fds.fd_wireless == -1) s_fds.fd_wireless = open_tz_temp_fd("wireless_therm");
    int32_t wl_v = 0;
    if (read_fd_int_strict(&s_fds.fd_wireless, &wl_v) && is_sane_temp(wl_v)) {
        snap->temp_wireless = wl_v;
        snap->valid_mask |= SENSOR_VALID_WIRELESS;
    } else {
        snap->temp_wireless = snap->temp_battery;
    }

    if (s_fds.fd_cpu_core_x4 == -1) {
        s_fds.fd_cpu_core_x4 = open_tz_with_fallbacks("cpu-2-2-0", "cpu-1-7-usr", "cpu_therm");
    }
    if (s_fds.fd_cpu_core_big == -1) {
        s_fds.fd_cpu_core_big = open_tz_with_fallbacks("cpu-1-2-0", "cpu-1-5-usr", "cpu_therm");
    }
    int32_t x4_t = 0, big_t = 0;
    bool x4_ok  = read_fd_int_strict(&s_fds.fd_cpu_core_x4, &x4_t) && is_sane_temp(x4_t);
    bool big_ok = read_fd_int_strict(&s_fds.fd_cpu_core_big, &big_t) && is_sane_temp(big_t);
    if (x4_ok && big_ok) {
        snap->temp_cpu_core_max = (x4_t > big_t) ? x4_t : big_t;
        snap->valid_mask |= SENSOR_VALID_CPU_CORE;
    } else if (x4_ok) {
        snap->temp_cpu_core_max = x4_t;
        snap->valid_mask |= SENSOR_VALID_CPU_CORE;
    } else if (big_ok) {
        snap->temp_cpu_core_max = big_t;
        snap->valid_mask |= SENSOR_VALID_CPU_CORE;
    } else {
        snap->temp_cpu_core_max = snap->temp_cpu;
    }

    if (s_fds.fd_batt_soc == -1)     s_fds.fd_batt_soc     = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    if (s_fds.fd_batt_current == -1) s_fds.fd_batt_current = open("/sys/class/power_supply/battery/current_now", O_RDONLY);
    if (s_fds.fd_batt_voltage == -1) s_fds.fd_batt_voltage = open("/sys/class/power_supply/battery/voltage_now", O_RDONLY);
    if (s_fds.fd_board_hw == -1)     s_fds.fd_board_hw     = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_RDONLY);

    int32_t soc_v = 50, cur_v = 0, vol_v = 3800000, brd_v = 0;
    if (read_fd_int_strict(&s_fds.fd_batt_soc, &soc_v) && soc_v >= 0 && soc_v <= 100) {
        snap->batt_capacity_soc = soc_v;
        snap->valid_mask |= SENSOR_VALID_BATT_SOC;
    } else {
        snap->batt_capacity_soc = 50;
    }

    snap->batt_current_now_ua = read_fd_int_strict(&s_fds.fd_batt_current, &cur_v) ? cur_v : 0;
    snap->batt_voltage_now_uv = read_fd_int_strict(&s_fds.fd_batt_voltage, &vol_v) ? vol_v : 3800000;

    if (read_fd_int_strict(&s_fds.fd_board_hw, &brd_v) && is_sane_temp(brd_v) && brd_v > 0) {
        snap->board_sensor_hw = brd_v;
        snap->valid_mask |= SENSOR_VALID_BOARD_HW;
    } else {
        snap->board_sensor_hw = snap->temp_quiet;
    }

    bool has_primary_anchor = ((snap->valid_mask & SENSOR_VALID_BATT_ANY) != 0) &&
                              ((snap->valid_mask & SENSOR_VALID_QUIET) != 0);
    snap->sensor_health_ok = (has_primary_anchor && snap->valid_core_ntc_count >= 4);

    return 0;
}

int32_t sensor_hub_cross_validate(sensor_snapshot_t *snap, int32_t computed_virt_temp) {
    if (!snap) return computed_virt_temp;

    bool batt_valid  = (snap->valid_mask & SENSOR_VALID_BATT_ANY) != 0;
    bool quiet_valid = (snap->valid_mask & SENSOR_VALID_QUIET) != 0;

    if (!batt_valid || !quiet_valid) {
        return computed_virt_temp;
    }

    int32_t phys_anchor = (snap->temp_battery * 6 + snap->temp_quiet * 4) / 10;
    int32_t diff = computed_virt_temp - phys_anchor;
    if (diff < 0) diff = -diff;

    if (diff > 12000) {
        snap->anomaly_mask |= ANOMALY_CROSS_VAL_DIVERGE;
        ASTRA_LOG_WARN("[CROSS-VAL] Significant disparity detected! Virt: %.1f°C vs PhysAnchor: %.1f°C (diff: %.1f°C)",
                       computed_virt_temp / 1000.0f, phys_anchor / 1000.0f, diff / 1000.0f);

        return (phys_anchor * 6 + computed_virt_temp * 4) / 10;
    }

    return computed_virt_temp;
}

static void close_fd_safe(int *pfd) {
    if (pfd && *pfd >= 0) {
        close(*pfd);
        *pfd = -1;
    }
}

void sensor_hub_deinit(void) {
    close_fd_safe(&s_fds.fd_battery);
    close_fd_safe(&s_fds.fd_battery_sec);
    close_fd_safe(&s_fds.fd_pa0);
    close_fd_safe(&s_fds.fd_pa1);
    close_fd_safe(&s_fds.fd_quiet);
    close_fd_safe(&s_fds.fd_charger0);
    close_fd_safe(&s_fds.fd_wifi);
    close_fd_safe(&s_fds.fd_cpu);
    close_fd_safe(&s_fds.fd_flash);
    close_fd_safe(&s_fds.fd_conn);
    close_fd_safe(&s_fds.fd_wireless);
    close_fd_safe(&s_fds.fd_cpu_core_x4);
    close_fd_safe(&s_fds.fd_cpu_core_big);
    close_fd_safe(&s_fds.fd_batt_soc);
    close_fd_safe(&s_fds.fd_batt_current);
    close_fd_safe(&s_fds.fd_batt_voltage);
    close_fd_safe(&s_fds.fd_board_hw);
    ASTRA_LOG_INFO("Sensor Hub fds released.");
}
