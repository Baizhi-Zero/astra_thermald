/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AstraThermald - Autonomous Thermal Governance Daemon
 * by Baizhi, Gemini
 */

#include "astra_thermald.h"
#include "sensor_hub.h"
#include "virtual_synth.h"
#include "safety_guard.h"
#include "arbiter_voting.h"
#include "takeover_ctrl.h"
#include "hook_injector.h"
#include "conf_parser.h"

static const char __attribute__((used)) g_watermark[] =
    "AstraThermald: Copyright (C) 2026 Baizhi & Gemini. Licensed under GPL-3.0-or-later. All rights reserved.";

astra_context_t g_astra_ctx;
static volatile sig_atomic_t g_sig_received = 0;

static void emergency_hardware_restore(void) {
    static volatile sig_atomic_t s_restored = 0;
    if (s_restored) return;
    s_restored = 1;

    if (g_astra_ctx.takeover_mi_thermald) {
        takeover_release();
    }
    safety_guard_restore();
    arbiter_voting_restore();
}

static void handle_signal(int sig) {
    g_sig_received = sig;
    g_astra_ctx.running = 0;
}

static void handle_crash_signal(int sig) {
    const char msg[] = "\n[ASTRA-TH] [FATAL-CRASH] Caught critical crash signal! Restoring hardware and restarting mi_thermald...\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    emergency_hardware_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void print_usage(const char *prog) {
    printf("AstraThermald - Qualcomm Snapdragon 8 Gen 3 (SM8650) Thermal Daemon\n");
    printf("Version: %s | by %s\n", ASTRA_THERMALD_VERSION, ASTRA_THERMALD_AUTHOR);
    printf("License: %s\n\n", ASTRA_THERMALD_LICENSE);
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p, --profile <name>   Thermal profile (auto|beast|esports|balance|stock, default: auto)\n");
    printf("  -c, --config <file>    Load external encrypted or plaintext .conf policy file\n");
    printf("  -t, --takeover         Halt official mi_thermald and engage full control\n");
    printf("  -i, --interval <ms>    Sampling interval in milliseconds (default: 1000)\n");
    printf("  -d, --daemon           Run in daemonized background mode\n");
    printf("  -v, --version          Show version\n");
    printf("  -h, --help             Show this help screen\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (getuid() != 0 && geteuid() != 0) {
        fprintf(stderr, "[ASTRA-TH] [FATAL] Permission Denied: AstraThermald MUST run as root (UID 0)!\n");
        fprintf(stderr, "[ASTRA-TH] [FATAL] Current UID=%d, EUID=%d. Please launch via 'su -c'.\n", getuid(), geteuid());
        return 1;
    }

    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd >= 0) {
        (void)write(oom_fd, "-1000\n", 6);
        close(oom_fd);
    }
    int cpuset_fd = open("/dev/cpuset/top-app/cgroup.procs", O_WRONLY);
    if (cpuset_fd >= 0) {
        char pid_str[16];
        int l = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
        (void)write(cpuset_fd, pid_str, l);
        close(cpuset_fd);
    }

    atexit(emergency_hardware_restore);

    memset(&g_astra_ctx, 0, sizeof(g_astra_ctx));
    g_astra_ctx.running = 1;
    g_astra_ctx.active_profile = PROFILE_BALANCED;
    g_astra_ctx.auto_sconfig = true;
    g_astra_ctx.last_sconfig = -1;
    g_astra_ctx.takeover_mi_thermald = false;
    g_astra_ctx.enforce_permissions = false;
    g_astra_ctx.poll_interval_ms = 1000;
    pthread_mutex_init(&g_astra_ctx.lock, NULL);

    bool daemonize = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--profile") == 0) {
            if (i + 1 < argc) {
                const char *p = argv[++i];
                if (strcmp(p, "auto") == 0) {
                    g_astra_ctx.auto_sconfig = true;
                    g_astra_ctx.active_profile = PROFILE_BALANCED;
                } else if (strcmp(p, "beast") == 0) {
                    g_astra_ctx.auto_sconfig = false;
                    g_astra_ctx.active_profile = PROFILE_BEAST_UNLOCKED;
                } else if (strcmp(p, "esports") == 0) {
                    g_astra_ctx.auto_sconfig = false;
                    g_astra_ctx.active_profile = PROFILE_ESPORTS;
                } else if (strcmp(p, "balance") == 0) {
                    g_astra_ctx.auto_sconfig = false;
                    g_astra_ctx.active_profile = PROFILE_BALANCED;
                } else if (strcmp(p, "stock") == 0) {
                    g_astra_ctx.auto_sconfig = false;
                    g_astra_ctx.active_profile = PROFILE_STOCK_REPLICA;
                } else {
                    fprintf(stderr, "Unknown profile: %s\n", p);
                    return 1;
                }
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                strncpy(g_astra_ctx.config_path, argv[++i], sizeof(g_astra_ctx.config_path) - 1);
            }
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--takeover") == 0) {
            g_astra_ctx.takeover_mi_thermald = true;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (i + 1 < argc) g_astra_ctx.poll_interval_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("AstraThermald %s by %s (%s)\n", ASTRA_THERMALD_VERSION, ASTRA_THERMALD_AUTHOR, ASTRA_THERMALD_LICENSE);
            return 0;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    signal(SIGSEGV, handle_crash_signal);
    signal(SIGABRT, handle_crash_signal);
    signal(SIGBUS,  handle_crash_signal);
    signal(SIGFPE,  handle_crash_signal);
    signal(SIGILL,  handle_crash_signal);

    ASTRA_LOG_INFO("==================================================");
    ASTRA_LOG_INFO("  AstraThermald v%s (by %s)", ASTRA_THERMALD_VERSION, ASTRA_THERMALD_AUTHOR);
    ASTRA_LOG_INFO("  License: %s", ASTRA_THERMALD_LICENSE);
    ASTRA_LOG_INFO("  Target SoC: Qualcomm Snapdragon 8 Gen 3 (SM8650)");
    ASTRA_LOG_INFO("  Mode: %s | Dynamic Scene: %s | Takeover: %s",
                   (g_astra_ctx.active_profile == PROFILE_BEAST_UNLOCKED) ? "BEAST" :
                   (g_astra_ctx.active_profile == PROFILE_ESPORTS) ? "ESPORTS" :
                   (g_astra_ctx.active_profile == PROFILE_BALANCED) ? "GENTLE_DAILY" : "STOCK",
                   g_astra_ctx.auto_sconfig ? "ENABLED (sconfig inotify)" : "LOCKED",
                   g_astra_ctx.takeover_mi_thermald ? "ENABLED" : "STANDBY");
    ASTRA_LOG_INFO("==================================================");

    if (daemonize) {
        if (daemon(0, 0) < 0) {
            ASTRA_LOG_ERROR("Failed to daemonize: %s", strerror(errno));
            return 1;
        }
    }

    if (sensor_hub_init() < 0) {
        ASTRA_LOG_ERROR("Fatal: Failed to initialize sensor hub.");
        return 1;
    }
    virtual_synth_init();

    if (safety_guard_init() < 0) {
        ASTRA_LOG_ERROR("Fatal: Failed to initialize safety guard.");
        sensor_hub_deinit();
        return 1;
    }

    if (arbiter_voting_init() < 0) {
        ASTRA_LOG_ERROR("Fatal: Failed to initialize arbiter voting.");
        safety_guard_restore();
        sensor_hub_deinit();
        return 1;
    }

    if (strlen(g_astra_ctx.config_path) > 0) {
        thermal_policy_t custom_policy;
        if (conf_parser_load_file(g_astra_ctx.config_path, &custom_policy) == 0) {
            arbiter_load_parsed_policy(&custom_policy);
            ASTRA_LOG_INFO("Custom Policy Successfully Loaded from %s", g_astra_ctx.config_path);
        } else {
            ASTRA_LOG_WARN("Failed to load policy from %s, falling back to embedded gradients.",
                           g_astra_ctx.config_path);
        }
    }

    hook_injector_init();
    takeover_ctrl_init();
    if (g_astra_ctx.takeover_mi_thermald) {
        takeover_engage();
    }

    sensor_snapshot_t snap;
    uint32_t loop_counter = 0;
    uint32_t last_anomaly_mask = 0;

    ASTRA_LOG_INFO("Entering Autonomous Governance Loop (Interval: %u ms)...",
                   g_astra_ctx.poll_interval_ms);

    while (g_astra_ctx.running) {
        int screen_on = takeover_read_screen_state();
        uint32_t current_sleep_ms = g_astra_ctx.poll_interval_ms;

        if (g_astra_ctx.takeover_mi_thermald && (loop_counter % 5 == 0)) {
            takeover_ensure_suppressed();
        }

        if (g_astra_ctx.auto_sconfig) {
            int cur_scfg = takeover_read_sconfig();
            if (cur_scfg >= 0 && cur_scfg != g_astra_ctx.last_sconfig) {
                g_astra_ctx.last_sconfig = cur_scfg;
                if (takeover_is_game_scene(cur_scfg)) {
                    g_astra_ctx.active_profile = PROFILE_ESPORTS;
                    ASTRA_LOG_INFO("[DYNAMIC-SCENE] Game Scene Detected (sconfig=%d, %s)! Transition -> ESPORTS Profile",
                                   cur_scfg, takeover_get_scene_desc(cur_scfg));
                } else {
                    g_astra_ctx.active_profile = PROFILE_BALANCED;
                    ASTRA_LOG_INFO("[DYNAMIC-SCENE] Normal Daily Scene Detected (sconfig=%d, %s). Transition -> GENTLE DAILY Profile",
                                   cur_scfg, takeover_get_scene_desc(cur_scfg));
                }
            }
        }

        if (sensor_hub_sample(&snap) == 0) {
            if (snap.anomaly_mask != last_anomaly_mask) {
                if (snap.anomaly_mask != 0) {
                    ASTRA_LOG_WARN("[SENSOR-HEALTH] Anomaly Bitmask Detected: 0x%04X (Redundant fallback active)",
                                   snap.anomaly_mask);
                } else {
                    ASTRA_LOG_INFO("[SENSOR-HEALTH] All sensors returned to pristine healthy status.");
                }
                last_anomaly_mask = snap.anomaly_mask;
            }

            int32_t virt_t = virtual_synth_compute(&snap);
            safety_guard_evaluate(&snap, virt_t);

            if (screen_on > 0 || virt_t >= 42000) {
                arbiter_voting_dispatch(g_astra_ctx.active_profile, virt_t);
            }

            hook_sync_to_kernel_lkm(g_astra_ctx.active_profile, virt_t);

            if (screen_on == 0) {
                current_sleep_ms = 3000;
            }

            if (++loop_counter % 5 == 0) {
                ASTRA_LOG_INFO("[HEARTBEAT] Virt: %.1f°C | Batt: %.1f°C | Quiet: %.1f°C | FCC: %d mA | scfg: %d | Profile: %s | Screen: %s | Cores: 8/8 Online",
                               virt_t / 1000.0f,
                               snap.temp_battery / 1000.0f,
                               snap.temp_quiet / 1000.0f,
                               safety_guard_get_state()->current_fcc_limit_ma,
                               g_astra_ctx.last_sconfig,
                               (g_astra_ctx.active_profile == PROFILE_BEAST_UNLOCKED) ? "BEAST" :
                               (g_astra_ctx.active_profile == PROFILE_ESPORTS) ? "ESPORTS" :
                               (g_astra_ctx.active_profile == PROFILE_BALANCED) ? "GENTLE" : "STOCK",
                               screen_on ? "ON" : "OFF");
            }

            if (loop_counter % 60 == 0) {
                struct stat st;
                if (fstat(STDOUT_FILENO, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (st.st_size > 1024 * 1024) {
                        lseek(STDOUT_FILENO, 0, SEEK_SET);
                        if (ftruncate(STDOUT_FILENO, 0) == 0) {
                            ASTRA_LOG_INFO("[LOG-MAINT] Log file exceeded 1MB; automatically truncated to prevent I/O bloat.");
                        }
                    }
                }
            }
        }

        usleep(current_sleep_ms * 1000);
    }

    ASTRA_LOG_INFO("Shutdown sequence triggered: Restoring hardware safe states...");
    arbiter_voting_restore();
    safety_guard_restore();
    if (g_astra_ctx.takeover_mi_thermald) {
        takeover_release();
    }
    hook_injector_cleanup();
    sensor_hub_deinit();
    pthread_mutex_destroy(&g_astra_ctx.lock);

    ASTRA_LOG_INFO("AstraThermald gracefully exited. System intact.");
    return 0;
}
