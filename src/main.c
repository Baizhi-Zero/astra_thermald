/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
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
#include <ctype.h>

astra_context_t g_astra_ctx;
static volatile sig_atomic_t g_sig_received   = 0;
static volatile sig_atomic_t s_cleanup_done   = 0;
static bool                  s_hw_initialized = false;

static void astra_graceful_cleanup(void) {
    if (!s_hw_initialized || s_cleanup_done) return;
    s_cleanup_done = 1;

    ASTRA_LOG_INFO("Shutdown cleanup triggered: Restoring modified hardware & service states...");
    arbiter_voting_restore();
    safety_guard_restore();
    if (g_astra_ctx.takeover_mi_thermald) {
        takeover_release();
    }
    hook_injector_cleanup();
    sensor_hub_deinit();
    ASTRA_LOG_INFO("AstraThermald gracefully exited. System intact.");
}

static void handle_termination_signal(int sig) {
    g_sig_received = sig;
    g_astra_ctx.running = 0;
}

static unsigned long get_process_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) == 2) {
        fclose(f);
        long page_kb = sysconf(_SC_PAGESIZE) / 1024;
        return resident * (unsigned long)(page_kb > 0 ? page_kb : 4);
    }
    fclose(f);
    return 0;
}

static void consume_control_file(const char *node_path) {
    int fd = open(node_path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= 256) {
        close(fd);
        unlink(node_path);
        return;
    }

    char cmd[256];
    ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
    close(fd);
    unlink(node_path);

    if (n <= 0) return;
    cmd[n] = '\0';

    char *p = cmd;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return;
    char *end = p + strlen(p) - 1;
    while (end > p && isspace((unsigned char)*end)) *end-- = '\0';

    if (strcmp(p, "auto") == 0) {
        arbiter_clear_custom_policy();
        g_astra_ctx.auto_sconfig       = true;
        g_astra_ctx.manual_mode_locked = false;
        g_astra_ctx.last_sconfig       = -1;
        ASTRA_LOG_INFO("[RUNTIME-CTRL] Switched to Dynamic Auto Scene Mode (manual lock released)");
    } else if (strcmp(p, "beast") == 0) {
        arbiter_clear_custom_policy();
        g_astra_ctx.auto_sconfig       = false;
        g_astra_ctx.manual_mode_locked = true;
        g_astra_ctx.active_profile     = PROFILE_BEAST_UNLOCKED;
        ASTRA_LOG_INFO("[RUNTIME-CTRL] Manually Locked Profile -> BEAST_UNLOCKED");
    } else if (strcmp(p, "esports") == 0) {
        arbiter_clear_custom_policy();
        g_astra_ctx.auto_sconfig       = false;
        g_astra_ctx.manual_mode_locked = true;
        g_astra_ctx.active_profile     = PROFILE_ESPORTS;
        ASTRA_LOG_INFO("[RUNTIME-CTRL] Manually Locked Profile -> ESPORTS");
    } else if (strcmp(p, "balance") == 0 || strcmp(p, "gentle") == 0) {
        arbiter_clear_custom_policy();
        g_astra_ctx.auto_sconfig       = false;
        g_astra_ctx.manual_mode_locked = true;
        g_astra_ctx.active_profile     = PROFILE_BALANCED;
        ASTRA_LOG_INFO("[RUNTIME-CTRL] Manually Locked Profile -> GENTLE_DAILY");
    } else if (strcmp(p, "stock") == 0) {
        arbiter_clear_custom_policy();
        g_astra_ctx.auto_sconfig       = false;
        g_astra_ctx.manual_mode_locked = true;
        g_astra_ctx.active_profile     = PROFILE_STOCK_REPLICA;
        ASTRA_LOG_INFO("[RUNTIME-CTRL] Manually Locked Profile -> STOCK_REPLICA");
    } else if (p[0] == '/') {
        struct stat cst;
        if (stat(p, &cst) == 0 && S_ISREG(cst.st_mode)) {
            thermal_policy_t custom_policy;
            if (conf_parser_load_file(p, &custom_policy) == 0 &&
                arbiter_load_parsed_policy(&custom_policy) == 0) {
                g_astra_ctx.auto_sconfig       = false;
                g_astra_ctx.manual_mode_locked = true;
                ASTRA_LOG_INFO("[RUNTIME-CTRL] Hot-reloaded & activated custom policy from %s", p);
            } else {
                ASTRA_LOG_WARN("[RUNTIME-CTRL] Custom policy %s rejected; keeping current active policy", p);
            }
        } else {
            ASTRA_LOG_WARN("[RUNTIME-CTRL] Rejected invalid config path: %s", p);
        }
    }
}

static void check_runtime_control_node(void) {
    if (access(ASTRA_CTRL_NODE_PATH, F_OK) == 0) {
        consume_control_file(ASTRA_CTRL_NODE_PATH);
    }
}

static int setup_logfile_redirection(const char *log_filepath) {
    if (!log_filepath || log_filepath[0] == '\0') return 0;

    int lfd = open(log_filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (lfd < 0) {
        fprintf(stderr, "[ASTRA-TH] [W] Failed to open log file %s: %s\n", log_filepath, strerror(errno));
        return -errno;
    }

    fflush(stdout);
    fflush(stderr);
    (void)dup2(lfd, STDOUT_FILENO);
    (void)dup2(lfd, STDERR_FILENO);
    if (lfd > STDERR_FILENO) {
        close(lfd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    return 0;
}

static void print_usage(const char *prog) {
    printf("AstraThermald - Redmi K70 Pro (SM8650) Autonomous Thermal Daemon\n");
    printf("Version: %s\n\n", ASTRA_THERMALD_VERSION);
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -p, --profile <name>   Thermal profile (auto|beast|esports|balance|stock, default: auto)\n");
    printf("  -c, --config <file>    Load external encrypted or plaintext .conf policy file\n");
    printf("  -t, --takeover         Halt official mi_thermald and engage full control\n");
    printf("  -i, --interval <ms>    Sampling interval in milliseconds (100..10000, default: 1000)\n");
    printf("  -l, --log <file>       Redirect log output to file (auto-rotated at 1MB)\n");
    printf("  -d, --daemon           Run in daemonized background mode\n");
    printf("  -v, --version          Show version\n");
    printf("  -h, --help             Show this help screen\n");
}

int main(int argc, char **argv) {

    memset(&g_astra_ctx, 0, sizeof(g_astra_ctx));
    g_astra_ctx.running             = 1;
    g_astra_ctx.active_profile      = PROFILE_BALANCED;
    g_astra_ctx.auto_sconfig        = true;
    g_astra_ctx.manual_mode_locked  = false;
    g_astra_ctx.last_sconfig        = -1;
    g_astra_ctx.takeover_mi_thermald = false;
    g_astra_ctx.poll_interval_ms    = 1000;

    bool daemonize = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", argv[i]);
                return 1;
            }
            const char *p = argv[++i];
            if (strcmp(p, "auto") == 0) {
                g_astra_ctx.auto_sconfig       = true;
                g_astra_ctx.manual_mode_locked = false;
                g_astra_ctx.active_profile     = PROFILE_BALANCED;
            } else if (strcmp(p, "beast") == 0) {
                g_astra_ctx.auto_sconfig       = false;
                g_astra_ctx.manual_mode_locked = true;
                g_astra_ctx.active_profile     = PROFILE_BEAST_UNLOCKED;
            } else if (strcmp(p, "esports") == 0) {
                g_astra_ctx.auto_sconfig       = false;
                g_astra_ctx.manual_mode_locked = true;
                g_astra_ctx.active_profile     = PROFILE_ESPORTS;
            } else if (strcmp(p, "balance") == 0 || strcmp(p, "gentle") == 0) {
                g_astra_ctx.auto_sconfig       = false;
                g_astra_ctx.manual_mode_locked = true;
                g_astra_ctx.active_profile     = PROFILE_BALANCED;
            } else if (strcmp(p, "stock") == 0) {
                g_astra_ctx.auto_sconfig       = false;
                g_astra_ctx.manual_mode_locked = true;
                g_astra_ctx.active_profile     = PROFILE_STOCK_REPLICA;
            } else {
                fprintf(stderr, "Unknown profile: %s\n", p);
                return 1;
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", argv[i]);
                return 1;
            }
            strncpy(g_astra_ctx.config_path, argv[++i], sizeof(g_astra_ctx.config_path) - 1);
            g_astra_ctx.config_path[sizeof(g_astra_ctx.config_path) - 1] = '\0';
            g_astra_ctx.manual_mode_locked = true;
            g_astra_ctx.auto_sconfig       = false;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", argv[i]);
                return 1;
            }
            strncpy(g_astra_ctx.log_path, argv[++i], sizeof(g_astra_ctx.log_path) - 1);
            g_astra_ctx.log_path[sizeof(g_astra_ctx.log_path) - 1] = '\0';
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--takeover") == 0) {
            g_astra_ctx.takeover_mi_thermald = true;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing argument for %s\n", argv[i]);
                return 1;
            }
            int32_t iv = 0;
            if (!astra_parse_int32_strict(argv[++i], &iv) || iv < 100 || iv > 10000) {
                fprintf(stderr, "Invalid interval (must be 100..10000 ms)\n");
                return 1;
            }
            g_astra_ctx.poll_interval_ms = (uint32_t)iv;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemonize = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("AstraThermald %s\n", ASTRA_THERMALD_VERSION);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s (use --help for usage)\n", argv[i]);
            return 1;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (getuid() != 0 && geteuid() != 0) {
        fprintf(stderr, "[ASTRA-TH] [FATAL] Permission Denied: AstraThermald MUST run as root (UID 0)!\n");
        return 1;
    }

    if (daemonize && g_astra_ctx.log_path[0] == '\0') {
        strncpy(g_astra_ctx.log_path, ASTRA_DEFAULT_LOG_PATH, sizeof(g_astra_ctx.log_path) - 1);
    }
    if (g_astra_ctx.log_path[0] != '\0') {
        setup_logfile_redirection(g_astra_ctx.log_path);
    }
    if (daemonize) {

        if (daemon(0, 1) < 0) {
            ASTRA_LOG_ERROR("Failed to daemonize: %s", strerror(errno));
            return 1;
        }
    }

    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd >= 0) {
        (void)write(oom_fd, "-500\n", 5);
        close(oom_fd);
    }
    int cpuset_fd = open("/dev/cpuset/system-background/cgroup.procs", O_WRONLY);
    if (cpuset_fd >= 0) {
        char pid_str[16];
        int l = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
        (void)write(cpuset_fd, pid_str, (size_t)l);
        close(cpuset_fd);
    }

    signal(SIGINT,  handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);
    signal(SIGHUP,  handle_termination_signal);
    atexit(astra_graceful_cleanup);

    ASTRA_LOG_INFO("==================================================");
    ASTRA_LOG_INFO("  AstraThermald v%s Starting...", ASTRA_THERMALD_VERSION);
    ASTRA_LOG_INFO("  Target SoC: Qualcomm Snapdragon 8 Gen 3 (SM8650)");
    ASTRA_LOG_INFO("  Mode: %s | Dynamic Scene: %s | Takeover: %s",
                   (g_astra_ctx.active_profile == PROFILE_BEAST_UNLOCKED) ? "BEAST" :
                   (g_astra_ctx.active_profile == PROFILE_ESPORTS) ? "ESPORTS" :
                   (g_astra_ctx.active_profile == PROFILE_BALANCED) ? "GENTLE_DAILY" : "STOCK_REPLICA",
                   g_astra_ctx.auto_sconfig ? "ENABLED (sconfig polling)" : "LOCKED",
                   g_astra_ctx.takeover_mi_thermald ? "ENABLED" : "STANDBY");
    ASTRA_LOG_INFO("==================================================");

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

    s_hw_initialized = true;

    if (g_astra_ctx.config_path[0] != '\0') {
        thermal_policy_t custom_policy;
        if (conf_parser_load_file(g_astra_ctx.config_path, &custom_policy) == 0 &&
            arbiter_load_parsed_policy(&custom_policy) == 0) {
            ASTRA_LOG_INFO("Custom Policy Successfully Activated from %s", g_astra_ctx.config_path);
        } else {
            ASTRA_LOG_WARN("Failed to activate policy from %s; falling back to built-in gradients.",
                           g_astra_ctx.config_path);
        }
    }

    hook_injector_init();
    takeover_ctrl_init();
    if (g_astra_ctx.takeover_mi_thermald) {
        takeover_engage();
    }

    sensor_snapshot_t snap;
    uint32_t loop_counter       = 0;
    uint32_t last_anomaly_mask  = 0;
    int      prev_screen_on     = -1;

    ASTRA_LOG_INFO("Entering Autonomous Governance Loop (Interval: %u ms)...",
                   g_astra_ctx.poll_interval_ms);

    while (g_astra_ctx.running) {

        check_runtime_control_node();

        int screen_on = takeover_read_screen_state();
        bool screen_changed = (prev_screen_on != -1 && screen_on != prev_screen_on);
        prev_screen_on = screen_on;
        uint32_t current_sleep_ms = g_astra_ctx.poll_interval_ms;

        if (g_astra_ctx.takeover_mi_thermald && (loop_counter % 5 == 0)) {
            takeover_ensure_suppressed();
        }

        if (g_astra_ctx.auto_sconfig && !g_astra_ctx.manual_mode_locked &&
            !arbiter_get_state()->using_custom_policy) {
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
                    ASTRA_LOG_WARN("[SENSOR-HEALTH] Anomaly Bitmask: 0x%04X | ValidCoreNTC: %d/7 | HealthOK: %s",
                                   snap.anomaly_mask, snap.valid_core_ntc_count,
                                   snap.sensor_health_ok ? "YES" : "DEGRADED");
                } else {
                    ASTRA_LOG_INFO("[SENSOR-HEALTH] All 7-NTC sensors healthy.");
                }
                last_anomaly_mask = snap.anomaly_mask;
            }

            int32_t virt_t = virtual_synth_compute(&snap);
            virt_t = sensor_hub_cross_validate(&snap, virt_t);

            if (g_astra_ctx.takeover_mi_thermald) {
                takeover_publish_board_temp(virt_t);
            }

            safety_guard_evaluate(&snap, virt_t);

            if (screen_on > 0 || virt_t >= 40000 || screen_changed || arbiter_has_active_limits()) {
                arbiter_voting_dispatch(g_astra_ctx.active_profile, virt_t);
            }

            hook_sync_to_kernel_lkm(g_astra_ctx.active_profile);

            if (screen_on == 0 && !arbiter_has_active_limits()) {
                current_sleep_ms = 3000;
            }

            if (++loop_counter % 5 == 0) {
                unsigned long rss_kb = get_process_rss_kb();
                const arbiter_state_t *ast = arbiter_get_state();
                ASTRA_LOG_INFO("[HEARTBEAT] Virt: %.1f°C | Batt: %.1f°C | Quiet: %.1f°C | FCC: %d mA | scfg: %d | Profile: %s (%s) | Screen: %s | Glitches: %u | Cores: %s | MemRSS: %lu KB",
                               virt_t / 1000.0f,
                               snap.temp_battery / 1000.0f,
                               snap.temp_quiet / 1000.0f,
                               safety_guard_get_state()->current_fcc_limit_ma,
                               g_astra_ctx.last_sconfig,
                               ast->active_profile_name,
                               g_astra_ctx.manual_mode_locked ? "LOCKED" : "AUTO",
                               screen_on ? "ON" : "OFF",
                               virtual_synth_get_glitch_count(),
                               ast->hotplug_locked_online ? "8/8 Online" : "PARTIAL",
                               rss_kb);
            }

            if (loop_counter % 60 == 0) {
                struct stat st;
                if (fstat(STDOUT_FILENO, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (st.st_size > 1024 * 1024) {
                        fflush(stdout);
                        fflush(stderr);
                        if (ftruncate(STDOUT_FILENO, 0) == 0) {
                            (void)lseek(STDOUT_FILENO, 0, SEEK_SET);
                            ASTRA_LOG_INFO("[LOG-MAINT] Log file exceeded 1MB; cleanly truncated and reset to offset 0.");
                        }
                    }
                }
            }
        }

        usleep(current_sleep_ms * 1000);
    }

    astra_graceful_cleanup();
    return 0;
}
