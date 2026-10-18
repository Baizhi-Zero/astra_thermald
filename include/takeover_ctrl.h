/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#ifndef TAKEOVER_CTRL_H
#define TAKEOVER_CTRL_H

#include "astra_thermald.h"

typedef struct {
    bool mi_thermald_stopped;
    bool sysfs_nodes_locked;
    int32_t last_sconfig;
} takeover_status_t;

int takeover_ctrl_init(void);
int takeover_engage(void);
void takeover_release(void);
bool takeover_is_active(void);
int takeover_read_sconfig(void);
bool takeover_ensure_suppressed(void);
int takeover_read_screen_state(void);
bool takeover_is_game_scene(int sconfig);
const char *takeover_get_scene_desc(int sconfig);

#endif
