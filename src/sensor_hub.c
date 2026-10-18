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

static int32_t read_fd_int(int *pfd, int32_t fallback) {
    if (!pfd || *pfd < 0) return fallback;
    int fd = *pfd;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        *pfd = -1;
        return fallback;
    }

    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        *pfd = -1;
        return fallback;
    }
    buf[n] = '\0';
    return (int32_t)atoi(buf);
}

static int find_thermal_zone_by_type(const char *target_type) {
    char type_path[64];
    char buf[64];

    for (int i = 0; i < 128; i++) {
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/thermal_zone%d/type", i);
        int fd = open(type_path, O_RDONLY);
        if (fd < 0) continue;

        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n > 0) {
            buf[n] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';

            if (strcmp(buf, target_type) == 0) {
                return i;
            }
        }
    }
    return -1;
}

static int open_tz_temp_fd(const char *type_name) {
    int zid = find_thermal_zone_by_type(type_name);
    if (zid < 0) {
        return -1;
    }
    char temp_path[64];
    snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/thermal_zone%d/temp", zid);
    return open(temp_path, O_RDONLY);
}

int sensor_hub_init(void) {
    ASTRA_LOG_INFO("Initializing Redundant Sensor Hub (Multi-Source 7-NTC + Sanity Verification)...");

    s_fds.fd_pa0      = open_tz_temp_fd("pa_therm0");
    s_fds.fd_pa1      = open_tz_temp_fd("pa_therm1");
    s_fds.fd_quiet    = open_tz_temp_fd("quiet_therm");
    s_fds.fd_charger0 = open_tz_temp_fd("charger_therm0");
    s_fds.fd_wifi     = open_tz_temp_fd("wifi_therm");
    s_fds.fd_cpu      = open_tz_temp_fd("cpu_therm");

    s_fds.fd_flash        = open_tz_temp_fd("flash_therm");
    s_fds.fd_conn         = open_tz_temp_fd("conn_therm");
    s_fds.fd_wireless     = open_tz_temp_fd("wireless_therm");
    s_fds.fd_battery_sec  = open_tz_temp_fd("battery");
    if (s_fds.fd_battery_sec < 0) s_fds.fd_battery_sec = open_tz_temp_fd("batt_therm");

    s_fds.fd_cpu_core_x4  = open_tz_temp_fd("cpu-1-7-usr");
    s_fds.fd_cpu_core_big = open_tz_temp_fd("cpu-1-5-usr");

    s_fds.fd_battery      = open("/sys/class/power_supply/battery/temp", O_RDONLY);
    s_fds.fd_batt_soc     = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    s_fds.fd_batt_current = open("/sys/class/power_supply/battery/current_now", O_RDONLY);
    s_fds.fd_batt_voltage = open("/sys/class/power_supply/battery/voltage_now", O_RDONLY);
    s_fds.fd_board_hw     = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_RDONLY);

    ASTRA_LOG_INFO("Sensor Hub pre-opened FDs (Batt_Pri:%d, Batt_Sec:%d, Quiet:%d, BoardHW:%d)",
                   s_fds.fd_battery, s_fds.fd_battery_sec, s_fds.fd_quiet, s_fds.fd_board_hw);
    return 0;
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static inline bool is_sane_temp(int32_t val_mc) {
    return (val_mc >= -5000 && val_mc <= 110000);
}

int sensor_hub_sample(sensor_snapshot_t *snap) {
    if (!snap) return -EINVAL;

    snap->timestamp_ms = get_time_ms();
    snap->anomaly_mask = 0;
    snap->sensor_health_ok = true;

    if (s_fds.fd_battery < 0) {
        s_fds.fd_battery = open("/sys/class/power_supply/battery/temp", O_RDONLY);
    }
    if (s_fds.fd_battery_sec < 0) {
        s_fds.fd_battery_sec = open_tz_temp_fd("battery");
        if (s_fds.fd_battery_sec < 0) s_fds.fd_battery_sec = open_tz_temp_fd("batt_therm");
    }

    int32_t b_pri_raw = read_fd_int(&s_fds.fd_battery, -9999);
    int32_t b_pri = (b_pri_raw > 0 && b_pri_raw < 1000) ? (b_pri_raw * 100) : b_pri_raw;
    int32_t b_sec = read_fd_int(&s_fds.fd_battery_sec, -9999);

    if (is_sane_temp(b_pri)) {
        snap->temp_battery = b_pri;
    } else if (is_sane_temp(b_sec)) {
        snap->temp_battery = b_sec;
        snap->anomaly_mask |= ANOMALY_BATT_CORRUPTED;
    } else {
        snap->temp_battery = 35000;
        snap->anomaly_mask |= ANOMALY_BATT_CORRUPTED;
        snap->sensor_health_ok = false;
    }
    snap->temp_battery_sec = is_sane_temp(b_sec) ? b_sec : snap->temp_battery;

    if (s_fds.fd_quiet < 0) s_fds.fd_quiet = open_tz_temp_fd("quiet_therm");
    int32_t q_raw = read_fd_int(&s_fds.fd_quiet, -9999);
    if (is_sane_temp(q_raw)) {
        snap->temp_quiet = q_raw;
    } else {
        snap->temp_quiet = snap->temp_battery;
        snap->anomaly_mask |= ANOMALY_QUIET_CORRUPTED;
    }

    #define SAMPLE_NTC_WITH_FALLBACK(field, pfd, tz_type, mask_bit, fallback_ref) \
        do { \
            if (*(pfd) < 0) *(pfd) = open_tz_temp_fd(tz_type); \
            int32_t v = read_fd_int(pfd, -9999); \
            if (is_sane_temp(v)) { \
                snap->field = v; \
            } else { \
                snap->field = fallback_ref; \
                snap->anomaly_mask |= mask_bit; \
            } \
        } while(0)

    SAMPLE_NTC_WITH_FALLBACK(temp_pa0,      &s_fds.fd_pa0,      "pa_therm0",      ANOMALY_PA0_CORRUPTED,  snap->temp_quiet);
    SAMPLE_NTC_WITH_FALLBACK(temp_pa1,      &s_fds.fd_pa1,      "pa_therm1",      ANOMALY_PA1_CORRUPTED,  snap->temp_pa0);
    SAMPLE_NTC_WITH_FALLBACK(temp_charger0, &s_fds.fd_charger0, "charger_therm0", ANOMALY_CHG_CORRUPTED,  snap->temp_battery);
    SAMPLE_NTC_WITH_FALLBACK(temp_wifi,     &s_fds.fd_wifi,     "wifi_therm",     ANOMALY_WIFI_CORRUPTED, snap->temp_quiet);
    SAMPLE_NTC_WITH_FALLBACK(temp_cpu,      &s_fds.fd_cpu,      "cpu_therm",      ANOMALY_CPU_CORRUPTED,  snap->temp_quiet);

    SAMPLE_NTC_WITH_FALLBACK(temp_flash,    &s_fds.fd_flash,    "flash_therm",    ANOMALY_FLASH_CORRUPTED, snap->temp_charger0);

    if (s_fds.fd_conn < 0) s_fds.fd_conn = open_tz_temp_fd("conn_therm");
    snap->temp_conn = read_fd_int(&s_fds.fd_conn, snap->temp_battery);

    if (s_fds.fd_wireless < 0) s_fds.fd_wireless = open_tz_temp_fd("wireless_therm");
    snap->temp_wireless = read_fd_int(&s_fds.fd_wireless, snap->temp_battery);

    if (s_fds.fd_cpu_core_x4 < 0)  s_fds.fd_cpu_core_x4  = open_tz_temp_fd("cpu-1-7-usr");
    if (s_fds.fd_cpu_core_big < 0) s_fds.fd_cpu_core_big = open_tz_temp_fd("cpu-1-5-usr");
    int32_t x4_t  = read_fd_int(&s_fds.fd_cpu_core_x4, snap->temp_cpu);
    int32_t big_t = read_fd_int(&s_fds.fd_cpu_core_big, snap->temp_cpu);
    snap->temp_cpu_core_max = (x4_t > big_t) ? x4_t : big_t;

    if (s_fds.fd_batt_soc < 0)     s_fds.fd_batt_soc     = open("/sys/class/power_supply/battery/capacity", O_RDONLY);
    if (s_fds.fd_batt_current < 0) s_fds.fd_batt_current = open("/sys/class/power_supply/battery/current_now", O_RDONLY);
    if (s_fds.fd_batt_voltage < 0) s_fds.fd_batt_voltage = open("/sys/class/power_supply/battery/voltage_now", O_RDONLY);
    if (s_fds.fd_board_hw < 0)     s_fds.fd_board_hw     = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_RDONLY);

    snap->batt_capacity_soc   = read_fd_int(&s_fds.fd_batt_soc, 50);
    snap->batt_current_now_ua = read_fd_int(&s_fds.fd_batt_current, 0);
    snap->batt_voltage_now_uv = read_fd_int(&s_fds.fd_batt_voltage, 3800000);
    snap->board_sensor_hw     = read_fd_int(&s_fds.fd_board_hw, snap->temp_quiet);

    return 0;
}

int32_t sensor_hub_cross_validate(sensor_snapshot_t *snap, int32_t computed_virt_temp) {
    if (!snap) return computed_virt_temp;

    int32_t hw_board = snap->board_sensor_hw;
    int32_t diff = computed_virt_temp - hw_board;
    if (diff < 0) diff = -diff;

    if (diff > 12000) {
        snap->anomaly_mask |= ANOMALY_CROSS_VAL_DIVERGE;
        ASTRA_LOG_WARN("[CROSS-VAL] Significant disparity detected! Virt: %.1f°C vs BoardHW: %.1f°C (diff: %.1f°C)",
                       computed_virt_temp / 1000.0f, hw_board / 1000.0f, diff / 1000.0f);
        return (hw_board * 7 + computed_virt_temp * 3) / 10;
    }

    return computed_virt_temp;
}

void sensor_hub_deinit(void) {
    int *fd_ptr = (int *)&s_fds;
    size_t count = sizeof(sensor_fds_t) / sizeof(int);
    for (size_t i = 0; i < count; i++) {
        if (fd_ptr[i] >= 0) {
            close(fd_ptr[i]);
            fd_ptr[i] = -1;
        }
    }
    ASTRA_LOG_INFO("Sensor Hub fds released.");
}
