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
static char s_gpu_cdev_cur_path[64] = "";

static int32_t s_full_cpu_khz[4] = { 2265600, 3148800, 2956800, 3302400 };

static int32_t s_applied_cpu_khz[4] = { -1, -1, -1, -1 };
static int32_t s_applied_gpu_level  = -1;
static int32_t s_applied_brightness = -1;

static int32_t  s_initial_gpu_level       = 0;
static bool     s_initial_gpu_captured    = false;
static int32_t  s_initial_brightness      = 0;
static bool     s_initial_bright_captured = false;
static uint8_t  s_initial_cpu_online[8]   = { 1, 1, 1, 1, 1, 1, 1, 1 };
static uint32_t s_cpu_online_modified_mask = 0;
static bool     s_cpu_limits_modified     = false;
static bool     s_gpu_cdev_modified       = false;
static bool     s_brightness_modified     = false;

static thermal_policy_t s_parsed_policy;
static bool s_has_parsed_policy = false;

static arbiter_stepwise_device_t s_dev_c0;
static arbiter_stepwise_device_t s_dev_c1;
static arbiter_stepwise_device_t s_dev_c2;
static arbiter_stepwise_device_t s_dev_c3;
static arbiter_stepwise_device_t s_dev_gpu;
static arbiter_stepwise_device_t s_dev_brightness;

static uint64_t s_last_dispatch_ms = 0;

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool read_file_int32(const char *path, int32_t *out_val) {
    if (!path || !out_val) return false;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return astra_parse_int32_strict(buf, out_val);
}

static int write_sysfs_str(int fd, const char *str) {
    if (fd < 0 || !str) return -EBADF;
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
    char max_path[64];
    char cur_path[64];
    char buf[64];

    s_state.gpu_max_state = 11;
    s_gpu_cdev_cur_path[0] = '\0';

    for (int i = 0; i < 128; i++) {
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/cooling_device%d/type", i);
        int tfd = open(type_path, O_RDONLY);
        if (tfd < 0) continue;

        ssize_t n = read(tfd, buf, sizeof(buf) - 1);
        close(tfd);
        if (n <= 0) continue;

        buf[n] = '\0';
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
            buf[--n] = '\0';
        }

        if (strcasecmp(buf, "gpu") == 0) {
            snprintf(max_path, sizeof(max_path), "/sys/class/thermal/cooling_device%d/max_state", i);
            int32_t max_st = 0;
            if (read_file_int32(max_path, &max_st) && max_st > 0) {
                s_state.gpu_max_state = max_st;
            }

            snprintf(cur_path, sizeof(cur_path), "/sys/class/thermal/cooling_device%d/cur_state", i);
            int32_t cur_st = 0;
            if (!s_initial_gpu_captured && read_file_int32(cur_path, &cur_st) && cur_st >= 0) {
                s_initial_gpu_level    = cur_st;
                s_initial_gpu_captured = true;
            }

            int cfd = open(cur_path, O_RDWR);
            if (cfd >= 0) {
                strncpy(s_gpu_cdev_cur_path, cur_path, sizeof(s_gpu_cdev_cur_path) - 1);
                ASTRA_LOG_INFO("[ARBITER] Verified GPU cooling device -> %s (type='%s', max_state=%d, init_state=%d, fd=%d)",
                               cur_path, buf, s_state.gpu_max_state, s_initial_gpu_level, cfd);
                return cfd;
            }
        }
    }

    ASTRA_LOG_WARN("[ARBITER] No cooling_device with type=='gpu' found; GPU cdev control disabled for safety.");
    return -1;
}

static void probe_hardware_topology_and_initial_state(void) {
    const int policies[4] = { 0, 2, 5, 7 };
    char path[96];

    for (int c = 0; c < 4; c++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/cpuinfo_max_freq", policies[c]);
        int32_t max_khz = 0;
        if (read_file_int32(path, &max_khz) && max_khz > 500000) {
            s_full_cpu_khz[c] = max_khz;
        }
    }
    ASTRA_LOG_INFO("[ARBITER] Probed SM8650 Cluster Max Clocks -> c0:%d c2:%d c5:%d c7:%d kHz",
                   s_full_cpu_khz[0], s_full_cpu_khz[1], s_full_cpu_khz[2], s_full_cpu_khz[3]);

    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        int32_t on = 1;
        if (read_file_int32(path, &on)) {
            s_initial_cpu_online[i] = (on != 0) ? 1 : 0;
        } else {
            s_initial_cpu_online[i] = 1;
        }
    }

    int32_t init_br = 0;
    if (read_file_int32(PATH_THERMAL_BRIGHTNESS, &init_br)) {
        s_initial_brightness      = init_br;
        s_initial_bright_captured = true;
    }
}

int arbiter_force_cores_online(void) {
    char path[64];
    char status[8];
    bool all_online = true;

    for (int i = 1; i < 8; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) {
            int32_t ro_on = 0;
            if (!read_file_int32(path, &ro_on) || ro_on != 1) {
                all_online = false;
            }
            continue;
        }

        ssize_t n = read(fd, status, sizeof(status) - 1);
        if (n > 0 && status[0] == '0') {
            if (lseek(fd, 0, SEEK_SET) >= 0 || errno == ESPIPE || errno == EINVAL) {
                ssize_t w = write(fd, "1\n", 2);
                if (w == 2) {

                    char verify_buf[8] = {0};
                    if (lseek(fd, 0, SEEK_SET) >= 0 && read(fd, verify_buf, sizeof(verify_buf) - 1) > 0 &&
                        verify_buf[0] == '1') {
                        s_cpu_online_modified_mask |= (1u << i);
                        ASTRA_LOG_WARN("[ARBITER] Unplugged CPU core detected! Revived & verified cpu%d online", i);
                    } else {
                        all_online = false;
                    }
                } else {
                    all_online = false;
                }
            } else {
                all_online = false;
            }
        } else if (n <= 0 || status[0] != '1') {
            all_online = false;
        }
        close(fd);
    }

    s_state.hotplug_locked_online = all_online;
    return all_online ? 0 : -EIO;
}

static int apply_cpu_cluster_limit(const char *cluster_dev, int32_t freq_khz) {
    if (s_fd_cpu_limits < 0) {
        s_fd_cpu_limits = open(PATH_CPU_LIMITS, O_RDWR);
        if (s_fd_cpu_limits < 0) return -errno;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d\n", cluster_dev, freq_khz);
    int ret = write_sysfs_str(s_fd_cpu_limits, cmd);
    if (ret < 0) {
        close(s_fd_cpu_limits);
        s_fd_cpu_limits = -1;
        return ret;
    }
    s_cpu_limits_modified = true;
    return 0;
}

static int apply_gpu_level(int32_t level) {

    if (level < 0) level = 0;
    if (s_state.gpu_max_state > 0 && level > s_state.gpu_max_state) {
        level = s_state.gpu_max_state;
    }

    if (s_fd_gpu_cdev < 0) {
        if (s_gpu_cdev_cur_path[0] != '\0') {
            s_fd_gpu_cdev = open(s_gpu_cdev_cur_path, O_RDWR);
        } else {
            s_fd_gpu_cdev = find_gpu_cdev_fd();
        }
        if (s_fd_gpu_cdev < 0) return -ENODEV;
    }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%d\n", level);
    int ret = write_sysfs_str(s_fd_gpu_cdev, cmd);
    if (ret < 0) {
        close(s_fd_gpu_cdev);
        s_fd_gpu_cdev = -1;
        return ret;
    }
    s_gpu_cdev_modified = true;
    return 0;
}

static int apply_brightness_limit(int32_t limit_val) {
    if (limit_val < 0) limit_val = 0;
    if (s_fd_brightness < 0) {
        s_fd_brightness = open(PATH_THERMAL_BRIGHTNESS, O_RDWR);
        if (s_fd_brightness < 0) return -ENODEV;
    }

    char cmd[16];
    snprintf(cmd, sizeof(cmd), "%d\n", limit_val);
    int ret = write_sysfs_str(s_fd_brightness, cmd);
    if (ret < 0) {
        close(s_fd_brightness);
        s_fd_brightness = -1;
        return ret;
    }
    s_brightness_modified = true;
    return 0;
}

static void finalize_device_thresholds(arbiter_stepwise_device_t *dev, int32_t max_hw_cap) {
    if (!dev || dev->num_tiers <= 0) {
        if (dev) dev->emergency_temp_mc = DEFAULT_EMERGENCY_TEMP_MC;
        return;
    }
    for (int i = 0; i < dev->num_tiers; i++) {
        if (max_hw_cap > 0 && dev->target[i] > max_hw_cap) {
            dev->target[i] = max_hw_cap;
        }
        if (dev->clr[i] >= dev->trig[i]) {
            dev->clr[i] = dev->trig[i] - 1000;
        }
    }
    int32_t top_trig = dev->trig[dev->num_tiers - 1];
    dev->emergency_temp_mc = (top_trig > DEFAULT_EMERGENCY_TEMP_MC) ? top_trig : DEFAULT_EMERGENCY_TEMP_MC;
}

static int32_t evaluate_stepwise(arbiter_stepwise_device_t *dev, int32_t temp, uint32_t dt_ms, int32_t fallback_target) {
    if (!dev || dev->num_tiers <= 0) return fallback_target;

    int tier = dev->current_tier;
    int32_t emerg_mc = (dev->emergency_temp_mc > 0) ? dev->emergency_temp_mc : DEFAULT_EMERGENCY_TEMP_MC;

    if (temp >= emerg_mc && tier < dev->num_tiers) {
        dev->current_tier  = dev->num_tiers;
        dev->hot_dwell_ms  = 0;
        dev->cool_dwell_ms = 0;
        ASTRA_LOG_WARN("[CHRONO-SAFETY] Emergency Overheat Triggered (%d m°C >= %d m°C)! Hard Clamped %s to Tier %d",
                       temp, emerg_mc, dev->device_name, dev->current_tier);
        return dev->target[dev->current_tier - 1];
    }

    uint32_t req_up_ms = (dev->dwell_up_req_ms > 0) ? dev->dwell_up_req_ms : 3000;
    if (tier < dev->num_tiers && temp >= dev->trig[tier]) {
        dev->cool_dwell_ms = 0;
        dev->hot_dwell_ms += dt_ms;
        if (dev->hot_dwell_ms >= req_up_ms) {
            dev->current_tier++;
            dev->hot_dwell_ms = 0;
            ASTRA_LOG_INFO("[CHRONO-STEP] %s Escalated to Tier %d (target=%d) after %u ms dwell",
                           dev->device_name, dev->current_tier,
                           dev->target[dev->current_tier - 1], req_up_ms);
        }
    } else {
        dev->hot_dwell_ms = 0;
    }

    tier = dev->current_tier;
    uint32_t req_down_ms = (dev->dwell_down_req_ms > 0) ? dev->dwell_down_req_ms : 2000;
    if (tier > 0 && temp < dev->clr[tier - 1]) {
        dev->hot_dwell_ms = 0;
        dev->cool_dwell_ms += dt_ms;
        if (dev->cool_dwell_ms >= req_down_ms) {
            while (dev->current_tier > 0 && temp < dev->clr[dev->current_tier - 1]) {
                dev->current_tier--;
            }
            dev->cool_dwell_ms = 0;
            ASTRA_LOG_INFO("[CHRONO-STEP] %s Recovered to Tier %d (target=%d) after %u ms cooldown",
                           dev->device_name, dev->current_tier,
                           (dev->current_tier > 0) ? dev->target[dev->current_tier - 1] : fallback_target,
                           req_down_ms);
        }
    } else {
        dev->cool_dwell_ms = 0;
    }

    if (dev->current_tier == 0) {
        return fallback_target;
    }
    return dev->target[dev->current_tier - 1];
}

static void reset_all_devices(void) {
    memset(&s_dev_c0, 0, sizeof(s_dev_c0));
    memset(&s_dev_c1, 0, sizeof(s_dev_c1));
    memset(&s_dev_c2, 0, sizeof(s_dev_c2));
    memset(&s_dev_c3, 0, sizeof(s_dev_c3));
    memset(&s_dev_gpu, 0, sizeof(s_dev_gpu));
    memset(&s_dev_brightness, 0, sizeof(s_dev_brightness));

    strncpy(s_dev_c0.device_name, "cpu0", sizeof(s_dev_c0.device_name) - 1);
    strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name) - 1);
    strncpy(s_dev_c2.device_name, "cpu5", sizeof(s_dev_c2.device_name) - 1);
    strncpy(s_dev_c3.device_name, "cpu7", sizeof(s_dev_c3.device_name) - 1);
    strncpy(s_dev_gpu.device_name, "gpu", sizeof(s_dev_gpu.device_name) - 1);
    strncpy(s_dev_brightness.device_name, "brightness", sizeof(s_dev_brightness.device_name) - 1);
}

static void init_gentle_daily_profile(void) {
    reset_all_devices();

    s_dev_c3.num_tiers = 6;
    s_dev_c3.dwell_up_req_ms   = 3000;
    s_dev_c3.dwell_down_req_ms = 2000;
    int32_t c3_trig[]   = { 42000, 44000, 45500, 47000, 48500, 50000 };
    int32_t c3_clr[]    = { 40500, 42500, 44000, 45500, 47000, 48500 };
    int32_t c3_target[] = { 3187200, 2976000, 2745600, 2496000, 2246400, 2035200 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));
    finalize_device_thresholds(&s_dev_c3, s_full_cpu_khz[3]);

    s_dev_c2.num_tiers = 6;
    s_dev_c2.dwell_up_req_ms   = 3000;
    s_dev_c2.dwell_down_req_ms = 2000;
    int32_t c2_trig[]   = { 42000, 44000, 45500, 47000, 48500, 50000 };
    int32_t c2_clr[]    = { 40500, 42500, 44000, 45500, 47000, 48500 };
    int32_t c2_target[] = { 2841600, 2707200, 2476800, 2323200, 2112000, 1920000 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));
    finalize_device_thresholds(&s_dev_c2, s_full_cpu_khz[2]);

    s_dev_c1.num_tiers = 6;
    s_dev_c1.dwell_up_req_ms   = 3000;
    s_dev_c1.dwell_down_req_ms = 2000;
    int32_t c1_target[] = { 2956800, 2707200, 2476800, 2323200, 2112000, 1920000 };
    memcpy(s_dev_c1.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c1.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c1.target, c1_target, sizeof(c1_target));
    finalize_device_thresholds(&s_dev_c1, s_full_cpu_khz[1]);

    s_dev_c0.num_tiers = 4;
    s_dev_c0.dwell_up_req_ms   = 4000;
    s_dev_c0.dwell_down_req_ms = 2000;
    int32_t c0_trig[]   = { 43000, 45000, 47000, 49000 };
    int32_t c0_clr[]    = { 41500, 43500, 45500, 47500 };
    int32_t c0_target[] = { 2016000, 1900800, 1785600, 1670400 };
    memcpy(s_dev_c0.trig, c0_trig, sizeof(c0_trig));
    memcpy(s_dev_c0.clr, c0_clr, sizeof(c0_clr));
    memcpy(s_dev_c0.target, c0_target, sizeof(c0_target));
    finalize_device_thresholds(&s_dev_c0, s_full_cpu_khz[0]);

    s_dev_gpu.num_tiers = 4;
    s_dev_gpu.dwell_up_req_ms   = 3000;
    s_dev_gpu.dwell_down_req_ms = 2000;
    int32_t gpu_trig[]   = { 44000, 46500, 48500, 50500 };
    int32_t gpu_clr[]    = { 42500, 45000, 47000, 49000 };
    int32_t gpu_target[] = { 1, 2, 3, 4 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
    finalize_device_thresholds(&s_dev_gpu, s_state.gpu_max_state);
}

static void init_esports_aggressive_profile(void) {
    reset_all_devices();

    s_dev_c3.num_tiers = 5;
    s_dev_c3.dwell_up_req_ms   = 4000;
    s_dev_c3.dwell_down_req_ms = 2000;
    int32_t c3_trig[]   = { 44000, 46500, 49000, 51500, 54000 };
    int32_t c3_clr[]    = { 42000, 44500, 47000, 49500, 52000 };
    int32_t c3_target[] = { 3187200, 2976000, 2745600, 2496000, 2246400 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));
    finalize_device_thresholds(&s_dev_c3, s_full_cpu_khz[3]);

    s_dev_c2.num_tiers = 5;
    s_dev_c2.dwell_up_req_ms   = 4000;
    s_dev_c2.dwell_down_req_ms = 2000;
    int32_t c2_trig[]   = { 44000, 46500, 49000, 51500, 54000 };
    int32_t c2_clr[]    = { 42000, 44500, 47000, 49500, 52000 };
    int32_t c2_target[] = { 2841600, 2707200, 2592000, 2323200, 2112000 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));
    finalize_device_thresholds(&s_dev_c2, s_full_cpu_khz[2]);

    s_dev_c1.num_tiers = 5;
    s_dev_c1.dwell_up_req_ms   = 4000;
    s_dev_c1.dwell_down_req_ms = 2000;
    int32_t c1_target[] = { 3014400, 2841600, 2611200, 2323200, 2112000 };
    memcpy(s_dev_c1.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c1.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c1.target, c1_target, sizeof(c1_target));
    finalize_device_thresholds(&s_dev_c1, s_full_cpu_khz[1]);

    s_dev_c0.num_tiers = 3;
    s_dev_c0.dwell_up_req_ms   = 4000;
    s_dev_c0.dwell_down_req_ms = 2000;
    int32_t c0_trig[]   = { 46500, 49000, 51500 };
    int32_t c0_clr[]    = { 44500, 47000, 49500 };
    int32_t c0_target[] = { 2016000, 1900800, 1785600 };
    memcpy(s_dev_c0.trig, c0_trig, sizeof(c0_trig));
    memcpy(s_dev_c0.clr, c0_clr, sizeof(c0_clr));
    memcpy(s_dev_c0.target, c0_target, sizeof(c0_target));
    finalize_device_thresholds(&s_dev_c0, s_full_cpu_khz[0]);

    s_dev_gpu.num_tiers = 3;
    s_dev_gpu.dwell_up_req_ms   = 3000;
    s_dev_gpu.dwell_down_req_ms = 2000;
    int32_t gpu_trig[]   = { 45000, 48000, 51000 };
    int32_t gpu_clr[]    = { 43000, 46000, 49000 };
    int32_t gpu_target[] = { 1, 2, 3 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
    finalize_device_thresholds(&s_dev_gpu, s_state.gpu_max_state);
}

static void init_beast_unlocked_profile(void) {
    reset_all_devices();

    s_dev_c3.num_tiers = 2;
    s_dev_c3.dwell_up_req_ms   = 3000;
    s_dev_c3.dwell_down_req_ms = 2000;
    int32_t c3_trig[]   = { 52000, 55000 };
    int32_t c3_clr[]    = { 50000, 53000 };
    int32_t c3_target[] = { 2976000, 2496000 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));
    finalize_device_thresholds(&s_dev_c3, s_full_cpu_khz[3]);

    s_dev_c2.num_tiers = 2;
    s_dev_c2.dwell_up_req_ms   = 3000;
    s_dev_c2.dwell_down_req_ms = 2000;
    int32_t c2_trig[]   = { 52000, 55000 };
    int32_t c2_clr[]    = { 50000, 53000 };
    int32_t c2_target[] = { 2841600, 2323200 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));
    finalize_device_thresholds(&s_dev_c2, s_full_cpu_khz[2]);

    s_dev_c1 = s_dev_c2;
    strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name) - 1);
    finalize_device_thresholds(&s_dev_c1, s_full_cpu_khz[1]);

    s_dev_c0.num_tiers = 0;

    s_dev_gpu.num_tiers = 2;
    s_dev_gpu.dwell_up_req_ms   = 3000;
    s_dev_gpu.dwell_down_req_ms = 2000;
    int32_t gpu_trig[]   = { 52000, 55000 };
    int32_t gpu_clr[]    = { 50000, 53000 };
    int32_t gpu_target[] = { 1, 2 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
    finalize_device_thresholds(&s_dev_gpu, s_state.gpu_max_state);
}

static void init_stock_replica_profile(void) {
    reset_all_devices();

    s_dev_c3.num_tiers = 11;
    s_dev_c3.dwell_up_req_ms   = 2000;
    s_dev_c3.dwell_down_req_ms = 2000;
    int32_t c3_trig[]   = { 25000, 32000, 37000, 39000, 41000, 43000, 44000, 45000, 46000, 47000, 48000 };
    int32_t c3_clr[]    = { 23000, 30000, 35000, 37000, 39000, 41000, 43000, 44000, 45000, 46000, 47000 };
    int32_t c3_target[] = { 2745600, 2246400, 2035200, 1824000, 1593600, 1363200, 1248000, 1132800, 1017600, 902400, 787200 };
    memcpy(s_dev_c3.trig, c3_trig, sizeof(c3_trig));
    memcpy(s_dev_c3.clr, c3_clr, sizeof(c3_clr));
    memcpy(s_dev_c3.target, c3_target, sizeof(c3_target));
    finalize_device_thresholds(&s_dev_c3, s_full_cpu_khz[3]);

    s_dev_c2.num_tiers = 13;
    s_dev_c2.dwell_up_req_ms   = 2000;
    s_dev_c2.dwell_down_req_ms = 2000;
    int32_t c2_trig[]   = { 25000, 32000, 37000, 39000, 41000, 43000, 44000, 45000, 46000, 47000, 48000, 49000, 50000 };
    int32_t c2_clr[]    = { 23000, 30000, 35000, 37000, 39000, 41000, 43000, 44000, 45000, 46000, 47000, 48000, 49000 };
    int32_t c2_target[] = { 2707200, 2323200, 2035200, 1824000, 1497600, 1497600, 1497600, 1401600, 1401600, 1286400, 844800, 729600, 499200 };
    memcpy(s_dev_c2.trig, c2_trig, sizeof(c2_trig));
    memcpy(s_dev_c2.clr, c2_clr, sizeof(c2_clr));
    memcpy(s_dev_c2.target, c2_target, sizeof(c2_target));
    finalize_device_thresholds(&s_dev_c2, s_full_cpu_khz[2]);

    s_dev_c1 = s_dev_c2;
    strncpy(s_dev_c1.device_name, "cpu2", sizeof(s_dev_c1.device_name) - 1);
    finalize_device_thresholds(&s_dev_c1, s_full_cpu_khz[1]);

    s_dev_c0.num_tiers = 3;
    s_dev_c0.dwell_up_req_ms   = 2000;
    s_dev_c0.dwell_down_req_ms = 2000;
    int32_t c0_trig[]   = { 40000, 45000, 48000 };
    int32_t c0_clr[]    = { 38000, 43000, 46000 };
    int32_t c0_target[] = { 1804800, 1459200, 1132800 };
    memcpy(s_dev_c0.trig, c0_trig, sizeof(c0_trig));
    memcpy(s_dev_c0.clr, c0_clr, sizeof(c0_clr));
    memcpy(s_dev_c0.target, c0_target, sizeof(c0_target));
    finalize_device_thresholds(&s_dev_c0, s_full_cpu_khz[0]);

    s_dev_gpu.num_tiers = 3;
    s_dev_gpu.dwell_up_req_ms   = 2000;
    s_dev_gpu.dwell_down_req_ms = 2000;
    int32_t gpu_trig[]   = { 15000, 40000, 45000 };
    int32_t gpu_clr[]    = { 13000, 38000, 43000 };
    int32_t gpu_target[] = { 4, 5, 8 };
    memcpy(s_dev_gpu.trig, gpu_trig, sizeof(gpu_trig));
    memcpy(s_dev_gpu.clr, gpu_clr, sizeof(gpu_clr));
    memcpy(s_dev_gpu.target, gpu_target, sizeof(gpu_target));
    finalize_device_thresholds(&s_dev_gpu, s_state.gpu_max_state);

    s_dev_brightness.num_tiers = 1;
    s_dev_brightness.dwell_up_req_ms   = 2000;
    s_dev_brightness.dwell_down_req_ms = 2000;
    s_dev_brightness.trig[0]   = 51000;
    s_dev_brightness.clr[0]    = 49000;
    s_dev_brightness.target[0] = 160;
    finalize_device_thresholds(&s_dev_brightness, 4095);
}

static astra_profile_t s_current_loaded_profile = (astra_profile_t)-1;

static void switch_profile_gradient_table(astra_profile_t profile) {
    if (s_current_loaded_profile == profile && !s_has_parsed_policy) return;

    s_current_loaded_profile    = profile;
    s_state.using_custom_policy = false;

    switch (profile) {
        case PROFILE_BEAST_UNLOCKED:
            init_beast_unlocked_profile();
            strncpy(s_state.active_profile_name, "BEAST_UNLOCKED", sizeof(s_state.active_profile_name) - 1);
            break;
        case PROFILE_ESPORTS:
            init_esports_aggressive_profile();
            strncpy(s_state.active_profile_name, "ESPORTS_AGGRESSIVE", sizeof(s_state.active_profile_name) - 1);
            break;
        case PROFILE_BALANCED:
            init_gentle_daily_profile();
            strncpy(s_state.active_profile_name, "GENTLE_DAILY", sizeof(s_state.active_profile_name) - 1);
            break;
        case PROFILE_STOCK_REPLICA:
        default:
            init_stock_replica_profile();
            strncpy(s_state.active_profile_name, "STOCK_REPLICA", sizeof(s_state.active_profile_name) - 1);
            break;
    }

    ASTRA_LOG_INFO("[ARBITER] Active Profile Gradient Tables Switched -> %s", s_state.active_profile_name);
}

void arbiter_clear_custom_policy(void) {
    if (s_has_parsed_policy) {
        s_has_parsed_policy         = false;
        s_state.using_custom_policy = false;
        s_current_loaded_profile    = (astra_profile_t)-1;
        virtual_synth_reset_default();
        safety_guard_clear_sic_policy();
        ASTRA_LOG_INFO("[ARBITER] Released Custom Policy lock; restored built-in profile switching.");
    }
}

bool arbiter_has_active_limits(void) {
    if (s_applied_cpu_khz[0] < 0 || s_applied_gpu_level < 0) return true;
    if (s_dev_c0.current_tier > 0 || s_dev_c1.current_tier > 0 ||
        s_dev_c2.current_tier > 0 || s_dev_c3.current_tier > 0 ||
        s_dev_gpu.current_tier > 0 || s_dev_brightness.current_tier > 0) {
        return true;
    }
    return false;
}

int arbiter_voting_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_has_parsed_policy        = false;
    s_last_dispatch_ms         = 0;
    s_cpu_online_modified_mask = 0;
    s_cpu_limits_modified      = false;
    s_gpu_cdev_modified        = false;
    s_brightness_modified      = false;

    for (int i = 0; i < 4; i++) {
        s_applied_cpu_khz[i] = -1;
    }
    s_applied_gpu_level  = -1;
    s_applied_brightness = -1;

    probe_hardware_topology_and_initial_state();

    s_fd_cpu_limits = open(PATH_CPU_LIMITS, O_RDWR);
    if (s_fd_cpu_limits < 0) {
        ASTRA_LOG_WARN("Failed to open %s: %s", PATH_CPU_LIMITS, strerror(errno));
    } else {
        ASTRA_LOG_INFO("Bound to CPU Limits Arbiter Node (fd=%d)", s_fd_cpu_limits);
    }

    s_fd_gpu_cdev   = find_gpu_cdev_fd();
    s_fd_brightness = open(PATH_THERMAL_BRIGHTNESS, O_RDWR);

    switch_profile_gradient_table(PROFILE_BALANCED);
    arbiter_force_cores_online();
    return 0;
}

int arbiter_load_parsed_policy(const thermal_policy_t *policy) {
    if (!policy || !policy->loaded || !policy->validated) {
        return -EINVAL;
    }

    int bound_devices = 0;

    for (int i = 0; i < policy->num_ss_rules; i++) {
        const char *d = policy->ss_rules[i].device;
        if (strcmp(d, "cpu7") == 0 || strcmp(d, "cluster3") == 0 ||
            strcmp(d, "cpu5") == 0 || strcmp(d, "cluster2") == 0 ||
            strcmp(d, "cpu2") == 0 || strcmp(d, "cluster1") == 0 ||
            strcmp(d, "cpu0") == 0 || strcmp(d, "cluster0") == 0 ||
            strcmp(d, "gpu") == 0) {
            bound_devices++;
        }
    }
    for (int i = 0; i < policy->num_mon_rules; i++) {
        const char *d = policy->mon_rules[i].device;
        if (strcmp(d, "gpu") == 0 || strcmp(d, "thermal_max_brightness") == 0) {
            bound_devices++;
        }
    }

    if (bound_devices == 0 && policy->num_sic_rules == 0) {
        ASTRA_LOG_WARN("[ARBITER] Rejected policy [%s]: 0 supported target devices matched (preserving built-in profile)",
                       policy->profile_name);
        return -EINVAL;
    }

    reset_all_devices();

    for (int i = 0; i < policy->num_ss_rules; i++) {
        const parsed_ss_rule_t *r = &policy->ss_rules[i];
        arbiter_stepwise_device_t *target_dev = NULL;
        int32_t max_cap = 0;

        if (strcmp(r->device, "cpu7") == 0 || strcmp(r->device, "cluster3") == 0) {
            target_dev = &s_dev_c3;
            max_cap    = s_full_cpu_khz[3];
        } else if (strcmp(r->device, "cpu5") == 0 || strcmp(r->device, "cluster2") == 0) {
            target_dev = &s_dev_c2;
            max_cap    = s_full_cpu_khz[2];
        } else if (strcmp(r->device, "cpu2") == 0 || strcmp(r->device, "cluster1") == 0) {
            target_dev = &s_dev_c1;
            max_cap    = s_full_cpu_khz[1];
        } else if (strcmp(r->device, "cpu0") == 0 || strcmp(r->device, "cluster0") == 0) {
            target_dev = &s_dev_c0;
            max_cap    = s_full_cpu_khz[0];
        } else if (strcmp(r->device, "gpu") == 0) {
            target_dev = &s_dev_gpu;
            max_cap    = s_state.gpu_max_state;
        }

        if (target_dev) {
            target_dev->num_tiers         = r->num_tiers;
            target_dev->current_tier      = 0;
            target_dev->hot_dwell_ms      = 0;
            target_dev->cool_dwell_ms     = 0;
            target_dev->dwell_up_req_ms   = (r->polling_ms > 0) ? (uint32_t)(r->polling_ms * 2) : 3000;
            target_dev->dwell_down_req_ms = 2000;
            memcpy(target_dev->trig, r->trig, sizeof(target_dev->trig));
            memcpy(target_dev->clr, r->clr, sizeof(target_dev->clr));
            memcpy(target_dev->target, r->target, sizeof(target_dev->target));
            finalize_device_thresholds(target_dev, max_cap);
        }
    }

    for (int i = 0; i < policy->num_mon_rules; i++) {
        const parsed_monitor_rule_t *r = &policy->mon_rules[i];
        if (r->reverse != 0) continue;

        arbiter_stepwise_device_t *target_dev = NULL;
        int32_t max_cap = 0;

        if (strcmp(r->device, "gpu") == 0) {
            target_dev = &s_dev_gpu;
            max_cap    = s_state.gpu_max_state;
        } else if (strcmp(r->device, "thermal_max_brightness") == 0) {
            target_dev = &s_dev_brightness;
            max_cap    = 4095;
        }

        if (target_dev) {
            target_dev->num_tiers         = r->num_tiers;
            target_dev->current_tier      = 0;
            target_dev->hot_dwell_ms      = 0;
            target_dev->cool_dwell_ms     = 0;
            target_dev->dwell_up_req_ms   = (r->polling_ms > 0) ? (uint32_t)r->polling_ms : 2000;
            target_dev->dwell_down_req_ms = 2000;
            memcpy(target_dev->trig, r->trig, sizeof(target_dev->trig));
            memcpy(target_dev->clr, r->clr, sizeof(target_dev->clr));
            memcpy(target_dev->target, r->target, sizeof(target_dev->target));
            finalize_device_thresholds(target_dev, max_cap);
        }
    }

    if (policy->has_virtual_sensor) {
        virtual_synth_load_config(&policy->v_sensor);
    }

    if (policy->num_sic_rules > 0) {
        safety_guard_load_sic_policy(&policy->sic_rules[0]);
    }

    memcpy(&s_parsed_policy, policy, sizeof(s_parsed_policy));
    s_has_parsed_policy         = true;
    s_state.using_custom_policy = true;
    strncpy(s_state.active_profile_name, policy->profile_name, sizeof(s_state.active_profile_name) - 1);
    s_state.active_profile_name[sizeof(s_state.active_profile_name) - 1] = '\0';

    ASTRA_LOG_INFO("[ARBITER] Activated Custom Policy [%s] (bound_devices=%d, sic=%d, virtual=%d)",
                   s_state.active_profile_name, bound_devices,
                   policy->num_sic_rules, policy->has_virtual_sensor ? 1 : 0);
    return 0;
}

int arbiter_voting_dispatch(astra_profile_t profile, int32_t virtual_temp) {
    if (!s_has_parsed_policy) {
        switch_profile_gradient_table(profile);
    }

    uint64_t now_ms = get_monotonic_ms();
    uint32_t dt_ms  = 1000;
    if (s_last_dispatch_ms > 0 && now_ms > s_last_dispatch_ms) {
        uint64_t diff = now_ms - s_last_dispatch_ms;
        if (diff < 50) diff = 50;
        if (diff > 5000) diff = 5000;
        dt_ms = (uint32_t)diff;
    }
    s_last_dispatch_ms = now_ms;

    int32_t desired_c[4];
    desired_c[0] = evaluate_stepwise(&s_dev_c0, virtual_temp, dt_ms, s_full_cpu_khz[0]);
    desired_c[1] = evaluate_stepwise(&s_dev_c1, virtual_temp, dt_ms, s_full_cpu_khz[1]);
    desired_c[2] = evaluate_stepwise(&s_dev_c2, virtual_temp, dt_ms, s_full_cpu_khz[2]);
    desired_c[3] = evaluate_stepwise(&s_dev_c3, virtual_temp, dt_ms, s_full_cpu_khz[3]);

    int32_t desired_gpu = evaluate_stepwise(&s_dev_gpu, virtual_temp, dt_ms, 0);
    if (desired_gpu < 0) desired_gpu = 0;
    if (s_state.gpu_max_state > 0 && desired_gpu > s_state.gpu_max_state) {
        desired_gpu = s_state.gpu_max_state;
    }

    int32_t desired_bright = evaluate_stepwise(&s_dev_brightness, virtual_temp, dt_ms, 0);

    const char *c_names[4] = { "cpu0", "cpu2", "cpu5", "cpu7" };
    bool any_cpu_updated = false;

    for (int i = 0; i < 4; i++) {
        if (desired_c[i] != s_applied_cpu_khz[i]) {
            if (apply_cpu_cluster_limit(c_names[i], desired_c[i]) == 0) {
                s_applied_cpu_khz[i] = desired_c[i];
                any_cpu_updated      = true;
            }
        }
    }

    s_state.cpu_cluster0_limit_khz = (s_applied_cpu_khz[0] > 0) ? s_applied_cpu_khz[0] : desired_c[0];
    s_state.cpu_cluster1_limit_khz = (s_applied_cpu_khz[1] > 0) ? s_applied_cpu_khz[1] : desired_c[1];
    s_state.cpu_cluster2_limit_khz = (s_applied_cpu_khz[2] > 0) ? s_applied_cpu_khz[2] : desired_c[2];
    s_state.cpu_cluster3_limit_khz = (s_applied_cpu_khz[3] > 0) ? s_applied_cpu_khz[3] : desired_c[3];

    if (any_cpu_updated) {
        ASTRA_LOG_INFO("[ARBITER-DISPATCH] Applied Clocks -> c0:%d c2:%d c5:%d c7:%d kHz | Virt: %.1f°C | Profile: %s",
                       s_state.cpu_cluster0_limit_khz,
                       s_state.cpu_cluster1_limit_khz,
                       s_state.cpu_cluster2_limit_khz,
                       s_state.cpu_cluster3_limit_khz,
                       virtual_temp / 1000.0f,
                       s_state.active_profile_name);
    }

    if (desired_gpu != s_applied_gpu_level) {
        if (apply_gpu_level(desired_gpu) == 0) {
            s_applied_gpu_level    = desired_gpu;
            s_state.gpu_cool_level = desired_gpu;
            ASTRA_LOG_INFO("[ARBITER-DISPATCH] Applied GPU Cooling Level -> %d (max=%d) | Virt: %.1f°C",
                           desired_gpu, s_state.gpu_max_state, virtual_temp / 1000.0f);
        }
    }

    if (s_dev_brightness.num_tiers > 0 && desired_bright != s_applied_brightness) {
        if (apply_brightness_limit(desired_bright) == 0) {
            s_applied_brightness         = desired_bright;
            s_state.brightness_max_limit = desired_bright;
            ASTRA_LOG_INFO("[ARBITER-DISPATCH] Applied Thermal Brightness Limit -> %d | Virt: %.1f°C",
                           desired_bright, virtual_temp / 1000.0f);
        }
    }

    arbiter_force_cores_online();
    return 0;
}

const arbiter_state_t* arbiter_get_state(void) {
    return &s_state;
}

void arbiter_voting_restore(void) {

    if (s_cpu_limits_modified) {
        apply_cpu_cluster_limit("cpu0", s_full_cpu_khz[0]);
        apply_cpu_cluster_limit("cpu2", s_full_cpu_khz[1]);
        apply_cpu_cluster_limit("cpu5", s_full_cpu_khz[2]);
        apply_cpu_cluster_limit("cpu7", s_full_cpu_khz[3]);
        s_cpu_limits_modified = false;
        ASTRA_LOG_INFO("[ARBITER-RESTORE] Restored CPU clusters to hardware max clocks.");
    }

    if (s_gpu_cdev_modified) {
        int32_t restore_gpu = (!g_astra_ctx.takeover_mi_thermald && s_initial_gpu_captured) ? s_initial_gpu_level : 0;
        apply_gpu_level(restore_gpu);
        s_gpu_cdev_modified = false;
        ASTRA_LOG_INFO("[ARBITER-RESTORE] Restored GPU cooling level to %d.", restore_gpu);
    }

    if (s_brightness_modified) {
        int32_t restore_br = s_initial_bright_captured ? s_initial_brightness : 0;
        apply_brightness_limit(restore_br);
        s_brightness_modified = false;
    }

    if (s_cpu_online_modified_mask != 0) {
        char path[64];
        for (int i = 1; i < 8; i++) {
            if (s_cpu_online_modified_mask & (1u << i)) {
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
                int fd = open(path, O_WRONLY);
                if (fd >= 0) {
                    const char *v = (s_initial_cpu_online[i] != 0) ? "1\n" : "0\n";
                    (void)write(fd, v, 2);
                    close(fd);
                }
            }
        }
        s_cpu_online_modified_mask = 0;
    }

    if (s_fd_cpu_limits >= 0) { close(s_fd_cpu_limits); s_fd_cpu_limits = -1; }
    if (s_fd_gpu_cdev >= 0)   { close(s_fd_gpu_cdev);   s_fd_gpu_cdev   = -1; }
    if (s_fd_brightness >= 0) { close(s_fd_brightness); s_fd_brightness = -1; }
}
