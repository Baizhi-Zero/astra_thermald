# AstraThermald Makefile (解决 P2-07 & P2-08)
NDK_DIR ?= $(or $(ANDROID_NDK_HOME),$(ANDROID_NDK_ROOT),C:/Users/yuand/AppData/Local/Android/Sdk/ndk/26.0.10792818)

ifeq ($(origin CC),default)
    CC = $(NDK_DIR)/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android34-clang.cmd
endif
CC ?= $(NDK_DIR)/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android34-clang.cmd

CFLAGS ?= -O2 -Wall -Wextra -Iinclude
DAEMON_SRC = src/main.c \
             src/sensor_hub.c \
             src/virtual_synth.c \
             src/safety_guard.c \
             src/arbiter_voting.c \
             src/takeover_ctrl.c \
             src/hook_injector.c \
             src/conf_parser.c \
             crypto/aes128.c

LAUNCHER_SRC = src/astra_launcher.c

OUT_DAEMON   = bin/astra_thermald
OUT_LAUNCHER = bin/astra_launcher

.PHONY: all clean

all: $(OUT_DAEMON) $(OUT_LAUNCHER)

$(OUT_DAEMON): $(DAEMON_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(DAEMON_SRC) -o $(OUT_DAEMON)
	@echo "[BUILD] AstraThermald binary created at $(OUT_DAEMON)"

$(OUT_LAUNCHER): $(LAUNCHER_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LAUNCHER_SRC) -o $(OUT_LAUNCHER)
	@echo "[BUILD] AstraLauncher binary created at $(OUT_LAUNCHER)"

clean:
	rm -rf bin/
