/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef SENSOR_HUB_H
#define SENSOR_HUB_H

#include "astra_thermald.h"

#define ANOMALY_BATT_CORRUPTED    (1u << 0)
#define ANOMALY_PA0_CORRUPTED     (1u << 1)
#define ANOMALY_PA1_CORRUPTED     (1u << 2)
#define ANOMALY_QUIET_CORRUPTED   (1u << 3)
#define ANOMALY_CHG_CORRUPTED     (1u << 4)
#define ANOMALY_WIFI_CORRUPTED    (1u << 5)
#define ANOMALY_CPU_CORRUPTED     (1u << 6)
#define ANOMALY_FLASH_CORRUPTED   (1u << 7)
#define ANOMALY_CROSS_VAL_DIVERGE (1u << 8)

#define SENSOR_VALID_BATT_PRI     (1u << 0)
#define SENSOR_VALID_BATT_SEC     (1u << 1)
#define SENSOR_VALID_BATT_ANY     (SENSOR_VALID_BATT_PRI | SENSOR_VALID_BATT_SEC)
#define SENSOR_VALID_PA0          (1u << 2)
#define SENSOR_VALID_PA1          (1u << 3)
#define SENSOR_VALID_QUIET        (1u << 4)
#define SENSOR_VALID_CHG0         (1u << 5)
#define SENSOR_VALID_WIFI         (1u << 6)
#define SENSOR_VALID_CPU          (1u << 7)
#define SENSOR_VALID_FLASH        (1u << 8)
#define SENSOR_VALID_CONN         (1u << 9)
#define SENSOR_VALID_WIRELESS     (1u << 10)
#define SENSOR_VALID_CPU_CORE     (1u << 11)
#define SENSOR_VALID_BOARD_HW     (1u << 12)
#define SENSOR_VALID_BATT_SOC     (1u << 13)

typedef struct {

    int32_t temp_battery;
    int32_t temp_battery_sec;
    int32_t temp_pa0;
    int32_t temp_pa1;
    int32_t temp_quiet;
    int32_t temp_charger0;
    int32_t temp_wifi;
    int32_t temp_cpu;
    int32_t temp_cpu_core_max;

    int32_t temp_flash;
    int32_t temp_conn;
    int32_t temp_wireless;

    int32_t batt_capacity_soc;
    int32_t batt_current_now_ua;
    int32_t batt_voltage_now_uv;
    int32_t board_sensor_hw;

    uint32_t valid_mask;
    uint32_t anomaly_mask;
    int valid_core_ntc_count;
    bool sensor_health_ok;
    uint64_t timestamp_ms;
} sensor_snapshot_t;

int sensor_hub_init(void);

int sensor_hub_sample(sensor_snapshot_t *snap);

int32_t sensor_hub_cross_validate(sensor_snapshot_t *snap, int32_t computed_virt_temp);

void sensor_hub_deinit(void);

#endif
