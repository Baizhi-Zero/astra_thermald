/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "takeover_ctrl.h"
#include <sys/system_properties.h>

static takeover_status_t s_status = { false, false, false, false, -1 };
static int s_fd_sconfig    = -1;
static int s_fd_screen     = -1;
static int s_fd_board_temp = -1;

static svc_state_t query_mi_thermald_state(void) {
    char val[PROP_VALUE_MAX] = "";
    int len = __system_property_get("init.svc.mi_thermald", val);
    if (len <= 0) {
        return SVC_STATE_UNKNOWN;
    }
    if (strcmp(val, "stopped") == 0) {
        return SVC_STATE_STOPPED;
    }
    if (strcmp(val, "running") == 0) {
        return SVC_STATE_RUNNING;
    }
    return SVC_STATE_TRANSIT;
}

static int control_mi_thermald_and_wait(bool stop_req, uint32_t timeout_ms) {
    const char *ctl_key = stop_req ? "ctl.stop" : "ctl.start";
    svc_state_t desired = stop_req ? SVC_STATE_STOPPED : SVC_STATE_RUNNING;

    if (__system_property_set(ctl_key, "mi_thermald") != 0) {
        return -EPERM;
    }

    uint32_t elapsed = 0;
    const uint32_t step_ms = 20;
    while (elapsed < timeout_ms) {
        svc_state_t cur = query_mi_thermald_state();
        if (cur == desired) {
            return 0;
        }
        usleep(step_ms * 1000);
        elapsed += step_ms;
    }

    return (query_mi_thermald_state() == desired) ? 0 : -ETIMEDOUT;
}

int takeover_ctrl_init(void) {
    memset(&s_status, 0, sizeof(s_status));
    s_status.last_sconfig = -1;

    svc_state_t init_st = query_mi_thermald_state();
    if (init_st != SVC_STATE_UNKNOWN) {
        s_status.initial_state_captured = true;
        s_status.initial_was_running    = (init_st == SVC_STATE_RUNNING);
        s_status.mi_thermald_stopped    = (init_st == SVC_STATE_STOPPED);
    }

    s_fd_sconfig = open("/sys/class/thermal/thermal_message/sconfig", O_RDONLY);
    if (s_fd_sconfig >= 0) {
        ASTRA_LOG_INFO("Pre-opened sconfig monitor node (fd=%d)", s_fd_sconfig);
    }
    s_fd_board_temp = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_WRONLY);
    if (s_fd_board_temp >= 0) {
        ASTRA_LOG_INFO("Pre-opened board_sensor_temp publisher node (fd=%d)", s_fd_board_temp);
    }
    return 0;
}

int takeover_engage(void) {
    svc_state_t cur = query_mi_thermald_state();
    if (!s_status.initial_state_captured && cur != SVC_STATE_UNKNOWN) {
        s_status.initial_state_captured = true;
        s_status.initial_was_running    = (cur == SVC_STATE_RUNNING);
    }

    if (cur == SVC_STATE_STOPPED) {
        s_status.mi_thermald_stopped = true;
        ASTRA_LOG_INFO("Official mi_thermald is already stopped prior to takeover.");
        return 0;
    }

    ASTRA_LOG_INFO("Engaging Astra Takeover Sequence: Requesting init ctl.stop mi_thermald...");
    int ret = control_mi_thermald_and_wait(true, 600);
    if (ret == 0) {
        s_status.mi_thermald_stopped = true;
        s_status.we_stopped_service  = true;
        ASTRA_LOG_INFO("Verified init.svc.mi_thermald == stopped. AstraThermald now in full command.");
        return 0;
    }

    s_status.mi_thermald_stopped = false;
    ASTRA_LOG_ERROR("Takeover verification failed: mi_thermald did not reach stopped state (err=%d)!", ret);
    return ret;
}

void takeover_release(void) {

    if (g_astra_ctx.takeover_mi_thermald && s_status.we_stopped_service) {
        if (!s_status.initial_state_captured || s_status.initial_was_running) {
            ASTRA_LOG_INFO("Releasing Astra Takeover: Restoring official mi_thermald service...");
            if (control_mi_thermald_and_wait(false, 600) == 0) {
                ASTRA_LOG_INFO("Verified official mi_thermald service is running again.");
            } else {
                ASTRA_LOG_WARN("Requested ctl.start mi_thermald, waiting for init confirmation.");
            }
        }
        s_status.we_stopped_service = false;
    }
    s_status.mi_thermald_stopped = (query_mi_thermald_state() == SVC_STATE_STOPPED);

    if (s_fd_sconfig >= 0) {
        close(s_fd_sconfig);
        s_fd_sconfig = -1;
    }
    if (s_fd_screen >= 0) {
        close(s_fd_screen);
        s_fd_screen = -1;
    }
    if (s_fd_board_temp >= 0) {
        close(s_fd_board_temp);
        s_fd_board_temp = -1;
    }
}

bool takeover_is_active(void) {
    return (query_mi_thermald_state() == SVC_STATE_STOPPED);
}

int takeover_read_sconfig(void) {
    if (s_fd_sconfig < 0) {
        s_fd_sconfig = open("/sys/class/thermal/thermal_message/sconfig", O_RDONLY);
        if (s_fd_sconfig < 0) return -1;
    }

    if (lseek(s_fd_sconfig, 0, SEEK_SET) < 0) {
        close(s_fd_sconfig);
        s_fd_sconfig = open("/sys/class/thermal/thermal_message/sconfig", O_RDONLY);
        if (s_fd_sconfig < 0) return -1;
    }

    char buf[32];
    ssize_t n = read(s_fd_sconfig, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(s_fd_sconfig);
        s_fd_sconfig = open("/sys/class/thermal/thermal_message/sconfig", O_RDONLY);
        if (s_fd_sconfig < 0) return -1;
        n = read(s_fd_sconfig, buf, sizeof(buf) - 1);
        if (n <= 0) return -1;
    }
    buf[n] = '\0';

    int32_t scfg = -1;
    if (!astra_parse_int32_strict(buf, &scfg)) {
        return -1;
    }
    s_status.last_sconfig = scfg;
    return (int)scfg;
}

bool takeover_ensure_suppressed(void) {
    if (!g_astra_ctx.takeover_mi_thermald) return true;

    svc_state_t st = query_mi_thermald_state();
    if (st == SVC_STATE_STOPPED) {
        s_status.mi_thermald_stopped = true;
        return true;
    }
    if (st == SVC_STATE_UNKNOWN) {

        s_status.mi_thermald_stopped = false;
        return false;
    }

    ASTRA_LOG_WARN("[TAKEOVER-GUARD] mi_thermald detected in state=%d! Suppressing via init ctl.stop...", (int)st);
    if (control_mi_thermald_and_wait(true, 400) == 0) {
        s_status.mi_thermald_stopped = true;
        s_status.we_stopped_service  = true;
        return true;
    }

    s_status.mi_thermald_stopped = false;
    return false;
}

int takeover_read_screen_state(void) {
    if (s_fd_screen < 0) {
        s_fd_screen = open("/sys/class/thermal/thermal_message/screen_state", O_RDONLY);
        if (s_fd_screen < 0) return 1;
    }

    if (lseek(s_fd_screen, 0, SEEK_SET) < 0) {
        close(s_fd_screen);
        s_fd_screen = open("/sys/class/thermal/thermal_message/screen_state", O_RDONLY);
        if (s_fd_screen < 0) return 1;
    }

    char buf[16];
    ssize_t n = read(s_fd_screen, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(s_fd_screen);
        s_fd_screen = -1;
        return 1;
    }
    buf[n] = '\0';
    return (buf[0] == '0') ? 0 : 1;
}

bool takeover_is_game_scene(int sconfig) {
    if (sconfig == 6 || sconfig == 106 || sconfig == 206 || sconfig == 306) return true;
    if (sconfig == 18 || sconfig == 118 || sconfig == 218 || sconfig == 318) return true;
    if (sconfig == 19 || sconfig == 119 || sconfig == 219 || sconfig == 319) return true;
    if (sconfig == 20 || sconfig == 120 || sconfig == 220 || sconfig == 320) return true;
    if (sconfig == 25 || sconfig == 26 || sconfig == 76) return true;
    if (sconfig == 501 || sconfig == 502 || sconfig == 503 || sconfig == 601) return true;
    if (sconfig == 700 || sconfig == 707 || sconfig == 800) return true;

    return false;
}

const char *takeover_get_scene_desc(int sconfig) {
    switch (sconfig) {
        case 6:
        case 106:
        case 206:
        case 306:
            return "NoLimits 全解极客 (国际服原神/出海手游)";
        case 20:
        case 120:
        case 220:
        case 320:
            return "Yuanshen (原神国服)";
        case 25:
            return "Xingtie (崩坏：星穹铁道)";
        case 18:
        case 118:
        case 218:
        case 318:
            return "TGame (王者/吃鸡等腾讯电竞)";
        case 19:
        case 119:
        case 219:
        case 319:
            return "MGame (主流 3D 手游)";
        case 26:
        case 76:
            return "HighFPS (高刷竞技电竞)";
        case 501:
        case 601:
            return "GameTurbo (狂暴引擎性能档)";
        case 502:
        case 503:
            return "GameTurbo (超频/插帧极客画质)";
        case 700:
        case 800:
            return "CGame (竞技云游戏)";
        case 707:
            return "WGame (重载大型手游)";
        case 0:
        case 50:
        case 100:
        case 200:
        case 300:
        case 500:
            return "Default System (日常桌面系统)";
        case 11:
        case 61:
        case 111:
        case 161:
        case 211:
        case 261:
        case 311:
        case 361:
            return "Video Playback (视频播放)";
        case 10:
        case 60:
        case 110:
        case 160:
        case 210:
        case 260:
        case 310:
        case 360:
            return "Navigation (地图导航)";
        case 14:
        case 114:
        case 214:
        case 314:
            return "VideoChat (视频通话)";
        default:
            return "Other System Scene";
    }
}

int takeover_publish_board_temp(int32_t temp_mc) {
    if (s_fd_board_temp < 0) {
        s_fd_board_temp = open("/sys/class/thermal/thermal_message/board_sensor_temp", O_WRONLY);
        if (s_fd_board_temp < 0) return -errno;
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", temp_mc);
    if (lseek(s_fd_board_temp, 0, SEEK_SET) < 0 && errno != ESPIPE && errno != EINVAL) {
        return -errno;
    }
    ssize_t n = write(s_fd_board_temp, buf, (size_t)len);
    if (n != (ssize_t)len) {
        close(s_fd_board_temp);
        s_fd_board_temp = -1;
        return -EIO;
    }
    return 0;
}
