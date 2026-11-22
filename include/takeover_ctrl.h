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

typedef enum {
    SVC_STATE_UNKNOWN = 0,
    SVC_STATE_RUNNING = 1,
    SVC_STATE_STOPPED = 2,
    SVC_STATE_TRANSIT = 3
} svc_state_t;

typedef struct {
    bool mi_thermald_stopped;
    bool initial_state_captured;
    bool initial_was_running;
    bool we_stopped_service;
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

int takeover_publish_board_temp(int32_t temp_mc);

#endif
