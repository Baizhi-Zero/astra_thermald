/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "takeover_ctrl.h"
#include <sys/system_properties.h>

static takeover_status_t s_status = { false, false, -1 };
static int s_fd_sconfig = -1;
static int s_fd_screen = -1;

int takeover_ctrl_init(void) {
    s_status.mi_thermald_stopped = false;
    s_status.sysfs_nodes_locked  = false;
    s_status.last_sconfig        = -1;

    s_fd_sconfig = open("/sys/class/thermal/thermal_message/sconfig", O_RDONLY);
    if (s_fd_sconfig >= 0) {
        ASTRA_LOG_INFO("Pre-opened sconfig monitor node (fd=%d)", s_fd_sconfig);
    }
    return 0;
}

static int set_android_prop(const char *key, const char *val) {
    int ret = __system_property_set(key, val);
    if (ret != 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "setprop %s %s 2>/dev/null", key, val);
        ret = system(cmd);
    }
    return ret;
}

int takeover_engage(void) {
    ASTRA_LOG_INFO("Engaging Astra Takeover Sequence: Halting official mi_thermald...");

    set_android_prop("ctl.stop", "mi_thermald");
    system("killall -SIGTERM mi_thermald 2>/dev/null");
    s_status.mi_thermald_stopped = true;

    char val[PROP_VALUE_MAX] = "";
    if (__system_property_get("init.svc.mi_thermald", val) > 0) {
        ASTRA_LOG_INFO("Confirmed mi_thermald state: %s", val);
    }

    ASTRA_LOG_INFO("Official mi_thermald successfully suspended. Astra Core now in full command.");
    return 0;
}

void takeover_release(void) {
    ASTRA_LOG_INFO("Releasing Takeover Control: Restoring mi_thermald...");

    if (g_astra_ctx.takeover_mi_thermald) {
        set_android_prop("ctl.start", "mi_thermald");
        ASTRA_LOG_INFO("Official mi_thermald service re-awakened via init.");
    }
    s_status.mi_thermald_stopped = false;

    if (s_fd_sconfig >= 0) {
        close(s_fd_sconfig);
        s_fd_sconfig = -1;
    }
    if (s_fd_screen >= 0) {
        close(s_fd_screen);
        s_fd_screen = -1;
    }
}

bool takeover_is_active(void) {
    return s_status.mi_thermald_stopped;
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

    int scfg = atoi(buf);
    s_status.last_sconfig = scfg;
    return scfg;
}

bool takeover_ensure_suppressed(void) {
    if (!g_astra_ctx.takeover_mi_thermald) return true;

    char val[PROP_VALUE_MAX] = "";
    if (__system_property_get("init.svc.mi_thermald", val) > 0) {
        if (strcmp(val, "running") == 0) {
            ASTRA_LOG_WARN("[TAKEOVER-GUARD] mi_thermald revived in background! Suppressing immediately...");
            set_android_prop("ctl.stop", "mi_thermald");
            system("killall -SIGTERM mi_thermald 2>/dev/null");
            s_status.mi_thermald_stopped = true;
            return false;
        }
    }
    return true;
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
            return "NoLimits Geek (Genshin Global / Overseas Heavy Games)";
        case 20:
        case 120:
        case 220:
        case 320:
            return "Genshin Impact (Genshin CN)";
        case 25:
            return "Honkai: Star Rail";
        case 18:
        case 118:
        case 218:
        case 318:
            return "Tencent Esports (Honor of Kings / Peacekeeper Elite)";
        case 19:
        case 119:
        case 219:
        case 319:
            return "Mainstream 3D Gaming";
        case 26:
        case 76:
            return "High FPS Esports Mode";
        case 501:
        case 502:
        case 503:
        case 601:
            return "Game Turbo Beast / WildBoost Engine";
        case 700:
        case 707:
        case 800:
            return "Heavy Load Gaming";
        case 11:
            return "Video Playback";
        case 14:
            return "Video Call / Meeting";
        case 10:
            return "GPS Navigation";
        case 16:
            return "Camera HDR / 4K Recording";
        case 0:
            return "Default System Launcher / UI";
        default:
            return "Other System Scene";
    }
}
