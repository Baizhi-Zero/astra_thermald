/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "arbiter_voting.h"

#define PATH_CPU_LIMITS         "/sys/class/thermal/thermal_message/cpu_limits"
#define PATH_THERMAL_BRIGHTNESS "/sys/class/thermal/thermal_message/thermal_max_brightness"

static arbiter_state_t s_state;
static int s_fd_cpu_limits = -1;
static int s_fd_gpu_cdev   = -1;
static int s_fd_brightness = -1;

static thermal_policy_t s_parsed_policy;
static bool s_has_parsed_policy = false;

static arbiter_stepwise_device_t s_dev_c0;
static arbiter_stepwise_device_t s_dev_c1;
static arbiter_stepwise_device_t s_dev_c2;
static arbiter_stepwise_device_t s_dev_c3;
static arbiter_stepwise_device_t s_dev_gpu;

static int write_sysfs_str(int fd, const char *str) {
    if (fd < 0) return -EBADF;
    if (lseek(fd, 0, SEEK_SET) < 0) {
        if (errno != ESPIPE && errno != EINVAL) {
            return -errno;
        }
    }

    size_t len = strlen(str);
    ssize_t written = write(fd, str, len);
    return (written == (ssize_t)len) ? 0 : -EIO;
}

static int find_gpu_cdev_fd(void) {
    char type_path[64];
    char cur_path[64];
    char buf[64];

    for (int i = 0; i < 64; i++) {
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/cooling_device%d/type", i);
        int tfd = open(type_path, O_RDONLY);
        if (tfd >= 0) {
            ssize_t n = read(tfd, buf, sizeof(buf) - 1);
            close(tfd);
            if (n > 0) {
                buf[n] = '\0';
                while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                    buf[--n] = '\0';
                }
                if (strcasecmp(buf, "gpu") == 0) {
                    snprintf(cur_path, sizeof(cur_path), "/sys/class/thermal/cooling_device%d/cur_state", i);
                    int cfd = open(cur_path, O_RDWR);
                    if (cfd >= 0) {
                        ASTRA_LOG_INFO("[ARBITER] Dynamically discovered GPU cooling device -> %s (type='%s', fd=%d)",
                                       cur_path, buf, cfd);
                        return cfd;
                    }
                }
            }
        }
    }

    int fallback_fd = open("/sys/class/thermal/cooling_device41/cur_state", O_RDWR);
    if (fallback_fd >= 0) {
        ASTRA_LOG_WARN("[ARBITER] GPU cooling device dynamic lookup fallback to cdev41 (fd=%d)", fallback_fd);
    }
    return fallback_fd;
}

int arbiter_force_cores_online(void) {
    char path[64];
    char status[8];
    for (int i = 1; i < 8; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            ssize_t n = read(fd, status, sizeof(status) - 1);
            if (n > 0 && status[0] == '0') {
                if (lseek(fd, 0, SEEK_SET) >= 0 || errno == ESPIPE || errno == EINVAL) {
                    write(fd, "1\n", 2);
                    ASTRA_LOG_WARN("[ARBITER] Unplugged CPU core detected! Revived cpu%d back online", i);
                }
            }
            close(fd);
        }
    }
    s_state.hotplug_locked_online = true;
    return 0;
}

static void apply_cpu_cluster_limit(const char *cluster_dev, int32_t freq_khz) {
    if (s_fd_cpu_limits < 0) return;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d\n", cluster_dev, freq_khz);
    write_sysfs_str(s_fd_cpu_limits, cmd);
}

static void apply_gpu_level(int32_t level) {
    if (s_fd_gpu_cdev < 0) return;

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%d\n", level);
    write_sysfs_str(s_fd_gpu_cdev, cmd);
}

static int32_t evaluate_stepwise(arbiter_stepwise_device_t *dev, int32_t temp, int32_t fallback_target) {
    if (!dev || dev->num_tiers <= 0) return fallback_target;

    int tier = dev->current_tier;
    int max_step_up = 2;
    while (tier < dev->num_tiers && temp >= dev->trig[tier] && max_step_up-- > 0) {
        tier++;
    }

    int max_step_down = 2;
    while (tier > 0 && temp < dev->clr[tier - 1] && max_step_down-- > 0) {
        tier--;
    }

    dev->current_tier = tier;

    if (tier == 0) {
        return fallback_target;
    }
    return dev->target[tier - 1];
}

static void init_gentle_daily_profile(void) {
    strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name));
    s_dev_c3.num_tiers = 8;
    s_dev_c3.current_tier = 0;
    int32_t c3_trig[]   = { 36500, 38000, 39500, 41000, 42500, 44000, 45500, 47000 };
    int32_t c3_clr[]    = { 35000, 36500, 38000, 39500, 41000, 42500, 44000, 45500 };
    int32_t c3_target[] = { 3187200, 2976000, 2745600, 2496000, 2246400, 2035200, 1824000, 1612800 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));

    strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name));
    s_dev_c2.num_tiers = 8;
    s_dev_c2.current_tier = 0;
    int32_t c2_trig[]   = { 36500, 38000, 39500, 41000, 42500, 44000, 45500, 47000 };
    int32_t c2_clr[]    = { 35000, 36500, 38000, 39500, 41000, 42500, 44000, 45500 };
    int32_t c2_target[] = { 2956800, 2707200, 2476800, 2323200, 2112000, 1920000, 1728000, 1536000 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));

    strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name));
    s_dev_c1.num_tiers = 8;
    s_dev_c1.current_tier = 0;
    memcpy(s_dev_c1.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c1.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c1.target, c2_target, sizeof(c2_target));

    strncpy(s_dev_c0.device_name, "cpu0", sizeof(s_dev_c0.device_name));
    s_dev_c0.num_tiers = 6;
    s_dev_c0.current_tier = 0;
    int32_t c0_trig[]   = { 38000, 39500, 41000, 42500, 44000, 45500 };
    int32_t c0_clr[]    = { 36500, 38000, 39500, 41000, 42500, 44000 };
    int32_t c0_target[] = { 1900800, 1785600, 1670400, 1555200, 1440000, 1440000 };
    memcpy(s_dev_c0.trig, c0_trig, sizeof(c0_trig));
    memcpy(s_dev_c0.clr, c0_clr, sizeof(c0_clr));
    memcpy(s_dev_c0.target, c0_target, sizeof(c0_target));

    strncpy(s_dev_gpu.device_name, "gpu", sizeof(s_dev_gpu.device_name));
    s_dev_gpu.num_tiers = 6;
    s_dev_gpu.current_tier = 0;
    int32_t gpu_trig[]   = { 38000, 40000, 42000, 44000, 46000, 48000 };
    int32_t gpu_clr[]    = { 36500, 38500, 40500, 42500, 44500, 46500 };
    int32_t gpu_target[] = { 1, 2, 3, 4, 5, 6 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
}

static void init_esports_aggressive_profile(void) {
    strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name));
    s_dev_c3.num_tiers = 5;
    s_dev_c3.current_tier = 0;
    int32_t c3_trig[]   = { 44000, 46500, 49000, 51500, 54000 };
    int32_t c3_clr[]    = { 42000, 44500, 47000, 49500, 52000 };
    int32_t c3_target[] = { 3187200, 2976000, 2745600, 2496000, 2496000 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));

    strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name));
    s_dev_c2.num_tiers = 5;
    s_dev_c2.current_tier = 0;
    int32_t c2_trig[]   = { 44000, 46500, 49000, 51500, 54000 };
    int32_t c2_clr[]    = { 42000, 44500, 47000, 49500, 52000 };
    int32_t c2_target[] = { 3014400, 2841600, 2611200, 2323200, 2323200 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));

    strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name));
    s_dev_c1.num_tiers = 5;
    s_dev_c1.current_tier = 0;
    memcpy(s_dev_c1.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c1.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c1.target, c2_target, sizeof(c2_target));

    strncpy(s_dev_c0.device_name, "cpu0", sizeof(s_dev_c0.device_name));
    s_dev_c0.num_tiers = 3;
    s_dev_c0.current_tier = 0;
    int32_t c0_trig[]   = { 46500, 49000, 51500 };
    int32_t c0_clr[]    = { 44500, 47000, 49500 };
    int32_t c0_target[] = { 1900800, 1785600, 1670400 };
    memcpy(s_dev_c0.trig, c0_trig, sizeof(c0_trig));
    memcpy(s_dev_c0.clr, c0_clr, sizeof(c0_clr));
    memcpy(s_dev_c0.target, c0_target, sizeof(c0_target));

    strncpy(s_dev_gpu.device_name, "gpu", sizeof(s_dev_gpu.device_name));
    s_dev_gpu.num_tiers = 3;
    s_dev_gpu.current_tier = 0;
    int32_t gpu_trig[]   = { 45000, 48000, 51000 };
    int32_t gpu_clr[]    = { 43000, 46000, 49000 };
    int32_t gpu_target[] = { 1, 2, 3 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
}

static void init_beast_unlocked_profile(void) {
    strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name));
    s_dev_c3.num_tiers = 2;
    s_dev_c3.current_tier = 0;
    int32_t c3_trig[]   = { 52000, 55000 };
    int32_t c3_clr[]    = { 50000, 53000 };
    int32_t c3_target[] = { 2976000, 2496000 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));

    strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name));
    s_dev_c2.num_tiers = 2;
    s_dev_c2.current_tier = 0;
    int32_t c2_trig[]   = { 52000, 55000 };
    int32_t c2_clr[]    = { 50000, 53000 };
    int32_t c2_target[] = { 2841600, 2323200 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));

    s_dev_c1 = s_dev_c2;
    s_dev_c0.num_tiers = 0;

    s_dev_gpu.num_tiers = 2;
    s_dev_gpu.current_tier = 0;
    int32_t gpu_trig[]   = { 52000, 55000 };
    int32_t gpu_clr[]    = { 50000, 53000 };
    int32_t gpu_target[] = { 1, 2 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
}

static void init_stock_replica_profile(void) {
    strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name));
    s_dev_c3.num_tiers = 4;
    s_dev_c3.current_tier = 0;
    int32_t c3_trig[]   = { 25000, 32000, 37000, 42000 };
    int32_t c3_clr[]    = { 24000, 31000, 36000, 41000 };
    int32_t c3_target[] = { 3187200, 2745600, 2246400, 2035200 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));

    strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name));
    s_dev_c2.num_tiers = 4;
    s_dev_c2.current_tier = 0;
    int32_t c2_trig[]   = { 25000, 32000, 37000, 42000 };
    int32_t c2_clr[]    = { 24000, 31000, 36000, 41000 };
    int32_t c2_target[] = { 2956800, 2707200, 2323200, 2035200 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));

    s_dev_c1 = s_dev_c2;
    s_dev_c0.num_tiers = 0;

    strncpy(s_dev_gpu.device_name, "gpu", sizeof(s_dev_gpu.device_name));
    s_dev_gpu.num_tiers = 4;
    s_dev_gpu.current_tier = 0;
    int32_t gpu_trig[]   = { 32000, 37000, 42000, 45000 };
    int32_t gpu_clr[]    = { 31000, 36000, 41000, 44000 };
    int32_t gpu_target[] = { 2, 4, 6, 8 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
}

static astra_profile_t s_current_loaded_profile = (astra_profile_t)-1;

static void switch_profile_gradient_table(astra_profile_t profile) {
    if (s_current_loaded_profile == profile && !s_has_parsed_policy) return;

    s_current_loaded_profile = profile;
    s_state.using_custom_policy = false;

    switch (profile) {
        case PROFILE_BEAST_UNLOCKED:
            init_beast_unlocked_profile();
            strncpy(s_state.active_profile_name, "BEAST_UNLOCKED", sizeof(s_state.active_profile_name));
            break;
        case PROFILE_ESPORTS:
            init_esports_aggressive_profile();
            strncpy(s_state.active_profile_name, "ESPORTS_AGGRESSIVE", sizeof(s_state.active_profile_name));
            break;
        case PROFILE_BALANCED:
            init_gentle_daily_profile();
            strncpy(s_state.active_profile_name, "GENTLE_DAILY", sizeof(s_state.active_profile_name));
            break;
        case PROFILE_STOCK_REPLICA:
        default:
            init_stock_replica_profile();
            strncpy(s_state.active_profile_name, "STOCK_REPLICA", sizeof(s_state.active_profile_name));
            break;
    }

    ASTRA_LOG_INFO("[ARBITER] Active Profile Gradient Tables Switched -> %s", s_state.active_profile_name);
}

int arbiter_voting_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_has_parsed_policy = false;

    s_fd_cpu_limits = open(PATH_CPU_LIMITS, O_RDWR);
    if (s_fd_cpu_limits < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s", PATH_CPU_LIMITS, strerror(errno));
    } else {
        ASTRA_LOG_INFO("Bound to CPU Limits Arbiter Node (fd=%d)", s_fd_cpu_limits);
    }

    s_fd_gpu_cdev = find_gpu_cdev_fd();
    if (s_fd_gpu_cdev < 0) {
        ASTRA_LOG_WARN("Failed to locate or open GPU cooling device cur_state: %s", strerror(errno));
    }

    s_fd_brightness = open(PATH_THERMAL_BRIGHTNESS, O_RDWR);

    switch_profile_gradient_table(PROFILE_BALANCED);
    arbiter_force_cores_online();
    return 0;
}

int arbiter_load_parsed_policy(const thermal_policy_t *policy) {
    if (!policy || !policy->loaded) return -EINVAL;

    memcpy(&s_parsed_policy, policy, sizeof(s_parsed_policy));
    s_has_parsed_policy = true;
    s_state.using_custom_policy = true;
    strncpy(s_state.active_profile_name, policy->profile_name, sizeof(s_state.active_profile_name));

    for (int i = 0; i < policy->num_ss_rules; i++) {
        const parsed_ss_rule_t *r = &policy->ss_rules[i];
        if (strcmp(r->device, "cpu7") == 0 || strcmp(r->device, "cluster3") == 0) {
            strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name));
            s_dev_c3.num_tiers = r->num_tiers;
            s_dev_c3.current_tier = 0;
            memcpy(s_dev_c3.trig, r->trig, sizeof(s_dev_c3.trig));
            memcpy(s_dev_c3.clr, r->clr, sizeof(s_dev_c3.clr));
            memcpy(s_dev_c3.target, r->target, sizeof(s_dev_c3.target));
        } else if (strcmp(r->device, "cpu5") == 0 || strcmp(r->device, "cluster2") == 0) {
            strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name));
            s_dev_c2.num_tiers = r->num_tiers;
            s_dev_c2.current_tier = 0;
            memcpy(s_dev_c2.trig, r->trig, sizeof(s_dev_c2.trig));
            memcpy(s_dev_c2.clr, r->clr, sizeof(s_dev_c2.clr));
            memcpy(s_dev_c2.target, r->target, sizeof(s_dev_c2.target));
        } else if (strcmp(r->device, "cpu2") == 0 || strcmp(r->device, "cluster1") == 0) {
            strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name));
            s_dev_c1.num_tiers = r->num_tiers;
            s_dev_c1.current_tier = 0;
            memcpy(s_dev_c1.trig, r->trig, sizeof(s_dev_c1.trig));
            memcpy(s_dev_c1.clr, r->clr, sizeof(s_dev_c1.clr));
            memcpy(s_dev_c1.target, r->target, sizeof(s_dev_c1.target));
        } else if (strcmp(r->device, "cpu0") == 0 || strcmp(r->device, "cluster0") == 0) {
            strncpy(s_dev_c0.device_name, "cpu0", sizeof(s_dev_c0.device_name));
            s_dev_c0.num_tiers = r->num_tiers;
            s_dev_c0.current_tier = 0;
            memcpy(s_dev_c0.trig, r->trig, sizeof(s_dev_c0.trig));
            memcpy(s_dev_c0.clr, r->clr, sizeof(s_dev_c0.clr));
            memcpy(s_dev_c0.target, r->target, sizeof(s_dev_c0.target));
        }
    }

    ASTRA_LOG_INFO("[ARBITER] Bound Parsed Policy: %s (ss_rules=%d, mon_rules=%d)",
                   policy->profile_name, policy->num_ss_rules, policy->num_mon_rules);
    return 0;
}

int arbiter_voting_dispatch(astra_profile_t profile, int32_t virtual_temp) {
    if (!s_has_parsed_policy) {
        switch_profile_gradient_table(profile);
    }

    const int32_t FULL_C0 = 2016000;
    const int32_t FULL_C1 = 3148800;
    const int32_t FULL_C2 = 3148800;
    const int32_t FULL_C3 = 3302400;

    int32_t lim_c0 = evaluate_stepwise(&s_dev_c0, virtual_temp, FULL_C0);
    int32_t lim_c1 = evaluate_stepwise(&s_dev_c1, virtual_temp, FULL_C1);
    int32_t lim_c2 = evaluate_stepwise(&s_dev_c2, virtual_temp, FULL_C2);
    int32_t lim_c3 = evaluate_stepwise(&s_dev_c3, virtual_temp, FULL_C3);
    int32_t gpu_lvl = evaluate_stepwise(&s_dev_gpu, virtual_temp, 0);

    bool cpu_changed = (s_state.cpu_cluster0_limit_khz != lim_c0 ||
                        s_state.cpu_cluster1_limit_khz != lim_c1 ||
                        s_state.cpu_cluster2_limit_khz != lim_c2 ||
                        s_state.cpu_cluster3_limit_khz != lim_c3);

    if (cpu_changed) {
        apply_cpu_cluster_limit("cpu0", lim_c0);
        apply_cpu_cluster_limit("cpu2", lim_c1);
        apply_cpu_cluster_limit("cpu5", lim_c2);
        apply_cpu_cluster_limit("cpu7", lim_c3);

        s_state.cpu_cluster0_limit_khz = lim_c0;
        s_state.cpu_cluster1_limit_khz = lim_c1;
        s_state.cpu_cluster2_limit_khz = lim_c2;
        s_state.cpu_cluster3_limit_khz = lim_c3;

        ASTRA_LOG_INFO("[ARBITER-DISPATCH] StepWise Clocks -> c0:%d c2:%d c5:%d c7:%d kHz | Virt: %.1f°C | Profile: %s",
                       lim_c0, lim_c1, lim_c2, lim_c3,
                       virtual_temp / 1000.0f,
                       s_state.active_profile_name);
    }

    if (s_state.gpu_cool_level != gpu_lvl) {
        apply_gpu_level(gpu_lvl);
        s_state.gpu_cool_level = gpu_lvl;
        ASTRA_LOG_INFO("[ARBITER-DISPATCH] GPU Cooling Level -> %d | Virt: %.1f°C",
                       gpu_lvl, virtual_temp / 1000.0f);
    }

    arbiter_force_cores_online();
    return 0;
}

const arbiter_state_t* arbiter_get_state(void) {
    return &s_state;
}

void arbiter_voting_restore(void) {
    ASTRA_LOG_INFO("Releasing Arbiter Cooling Device Limits...");

    apply_cpu_cluster_limit("cpu0", 2016000);
    apply_cpu_cluster_limit("cpu2", 3148800);
    apply_cpu_cluster_limit("cpu5", 3148800);
    apply_cpu_cluster_limit("cpu7", 3302400);
    apply_gpu_level(0);
    arbiter_force_cores_online();

    if (s_fd_cpu_limits >= 0) { close(s_fd_cpu_limits); s_fd_cpu_limits = -1; }
    if (s_fd_gpu_cdev >= 0)   { close(s_fd_gpu_cdev);   s_fd_gpu_cdev = -1; }
    if (s_fd_brightness >= 0) { close(s_fd_brightness); s_fd_brightness = -1; }
}
