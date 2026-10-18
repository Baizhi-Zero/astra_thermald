#
# Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
# SPDX-License-Identifier: GPL-3.0-or-later
#

CC ?= aarch64-linux-android34-clang
CFLAGS ?= -O2 -Wall -Wextra -pthread -Iinclude
SRC = src/main.c \
      src/sensor_hub.c \
      src/virtual_synth.c \
      src/safety_guard.c \
      src/arbiter_voting.c \
      src/takeover_ctrl.c \
      src/hook_injector.c \
      src/conf_parser.c \
      crypto/aes128.c

OUT = bin/astra_thermald

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SRC) -o $(OUT)
	@echo "[BUILD] AstraThermald binary created at $(OUT)"

clean:
	rm -rf bin/
