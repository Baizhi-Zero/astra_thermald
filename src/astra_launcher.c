/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "astra_thermald.h"
#include <sys/wait.h>
#include <sys/system_properties.h>

#define DAEMON_BIN_PATH   "/data/local/tmp/astra_thermald"
#define LAUNCHER_PID_PATH "/data/local/tmp/astra_launcher.pid"
#define PATH_FCC_NODE     "/sys/class/power_supply/battery/constant_charge_current"
#define PATH_TEMP_ST_NODE "/sys/class/thermal/thermal_message/temp_state"
#define PATH_CPU_LIM_NODE "/sys/class/thermal/thermal_message/cpu_limits"

typedef struct {
    char fcc_orig[32];
    char gpu_cdev_path[64];
    char gpu_orig[16];
    bool mi_was_running;
} hw_snapshot_t;

static void read_one_line(const char *path, char *out, size_t max_len) {
    if (!out || max_len == 0) return;
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, out, max_len - 1);
    close(fd);
    if (n > 0) {
        out[n] = '\0';
        for (ssize_t i = 0; i < n; i++) {
            if (out[i] == '\r' || out[i] == '\n') {
                out[i] = '\0';
                break;
            }
        }
    }
}

static void write_one_line(const char *path, const char *str) {
    if (!path || !str || str[0] == '\0') return;
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, str, strlen(str));
    close(fd);
}

static void capture_pre_start_snapshot(hw_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));
    read_one_line(PATH_FCC_NODE, snap->fcc_orig, sizeof(snap->fcc_orig));
    if (snap->fcc_orig[0] == '\0') {
        strncpy(snap->fcc_orig, "22000000", sizeof(snap->fcc_orig) - 1);
    }

    char type_path[64], type_buf[32];
    for (int i = 0; i < 128; i++) {
        snprintf(type_path, sizeof(type_path), "/sys/class/thermal/cooling_device%d/type", i);
        read_one_line(type_path, type_buf, sizeof(type_buf));
        if (strcasecmp(type_buf, "gpu") == 0) {
            snprintf(snap->gpu_cdev_path, sizeof(snap->gpu_cdev_path),
                     "/sys/class/thermal/cooling_device%d/cur_state", i);
            read_one_line(snap->gpu_cdev_path, snap->gpu_orig, sizeof(snap->gpu_orig));
            break;
        }
    }

    char prop_val[PROP_VALUE_MAX] = "";
    if (__system_property_get("init.svc.mi_thermald", prop_val) > 0 &&
        strcmp(prop_val, "running") == 0) {
        snap->mi_was_running = true;
    } else {
        snap->mi_was_running = true;
    }
}

static void external_supervisor_emergency_restore(const hw_snapshot_t *snap, int termsig) {
    FILE *lf = fopen(ASTRA_DEFAULT_LOG_PATH, "a");
    if (lf) {
        fprintf(lf, "[ASTRA-LAUNCHER] [SUPERVISOR-RECOVERY] Daemon crashed with signal %d! Executing external safe hardware recovery...\n",
                termsig);
        fflush(lf);
    }

    write_one_line(PATH_FCC_NODE, snap->fcc_orig[0] ? snap->fcc_orig : "22000000\n");
    write_one_line(PATH_TEMP_ST_NODE, "0\n");
    if (snap->gpu_cdev_path[0] != '\0') {
        write_one_line(snap->gpu_cdev_path, "0\n");
    }

    write_one_line(PATH_CPU_LIM_NODE, "cpu0 2265600\n");
    write_one_line(PATH_CPU_LIM_NODE, "cpu2 3148800\n");
    write_one_line(PATH_CPU_LIM_NODE, "cpu5 2956800\n");
    write_one_line(PATH_CPU_LIM_NODE, "cpu7 3302400\n");

    if (snap->mi_was_running) {
        __system_property_set("ctl.start", "mi_thermald");
    }

    if (lf) {
        fprintf(lf, "[ASTRA-LAUNCHER] [SUPERVISOR-RECOVERY] Hardware limits cleared & mi_thermald restored.\n");
        fclose(lf);
    }
}

static pid_t get_running_daemon_pid(void) {
    char buf[32];
    read_one_line(ASTRA_DEFAULT_PID_PATH, buf, sizeof(buf));
    int32_t pid_val = 0;
    if (!astra_parse_int32_strict(buf, &pid_val) || pid_val <= 1) {
        return -1;
    }
    if (kill((pid_t)pid_val, 0) == 0) {
        return (pid_t)pid_val;
    }
    return -1;
}

static int write_pid_file(const char *path, pid_t pid) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)pid);
    (void)write(fd, buf, (size_t)len);
    close(fd);
    return 0;
}

static int cmd_stop(void) {
    pid_t pid = get_running_daemon_pid();
    if (pid <= 0) {
        unlink(ASTRA_DEFAULT_PID_PATH);
        unlink(LAUNCHER_PID_PATH);
        printf("[AstraLauncher] astra_thermald is not running.\n");
        return 0;
    }

    printf("[AstraLauncher] Sending SIGTERM to astra_thermald (PID %d)...\n", (int)pid);
    kill(pid, SIGTERM);

    for (int i = 0; i < 40; i++) {
        if (kill(pid, 0) != 0) {
            unlink(ASTRA_DEFAULT_PID_PATH);
            unlink(LAUNCHER_PID_PATH);
            printf("[AstraLauncher] astra_thermald stopped cleanly.\n");
            return 0;
        }
        usleep(25000);
    }

    printf("[AstraLauncher] Warning: daemon did not exit in 1s, sending SIGKILL...\n");
    kill(pid, SIGKILL);
    unlink(ASTRA_DEFAULT_PID_PATH);
    unlink(LAUNCHER_PID_PATH);
    __system_property_set("ctl.start", "mi_thermald");
    return 0;
}

static int cmd_start(const char *profile, bool takeover, const char *custom_conf) {
    mkdir(ASTRA_MOD_DIR, 0755);

    pid_t existing = get_running_daemon_pid();
    if (existing > 0) {
        printf("[AstraLauncher] Stopping existing instance (PID %d)...\n", (int)existing);
        cmd_stop();
        usleep(100000);
    }

    const char *bin_path = DAEMON_BIN_PATH;
    if (access(bin_path, X_OK) != 0) {
        if (access("/data/local/tmp/astra_thermald", X_OK) == 0) {
            bin_path = "/data/local/tmp/astra_thermald";
        } else {
            fprintf(stderr, "[AstraLauncher] Error: Binary not found at %s\n", DAEMON_BIN_PATH);
            return 1;
        }
    }

    hw_snapshot_t snap;
    capture_pre_start_snapshot(&snap);

    pid_t sup_pid = fork();
    if (sup_pid < 0) {
        perror("fork supervisor");
        return 1;
    }
    if (sup_pid > 0) {

        for (int i = 0; i < 30; i++) {
            usleep(20000);
            pid_t dpid = get_running_daemon_pid();
            if (dpid > 0) {
                printf("[AstraLauncher] AstraThermald started (PID: %d, Profile: %s, Takeover: %s)\n",
                       (int)dpid, profile, takeover ? "ON" : "OFF");
                return 0;
            }
        }
        printf("[AstraLauncher] Supervisor launched (PID: %d).\n", (int)sup_pid);
        return 0;
    }

    setsid();
    write_pid_file(LAUNCHER_PID_PATH, getpid());

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }

    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        unlink(LAUNCHER_PID_PATH);
        _exit(1);
    }

    if (worker_pid == 0) {

        const char *args[12];
        int idx = 0;
        args[idx++] = bin_path;
        args[idx++] = "-p";
        args[idx++] = profile;
        if (takeover) {
            args[idx++] = "-t";
        }
        if (custom_conf && custom_conf[0] != '\0') {
            args[idx++] = "-c";
            args[idx++] = custom_conf;
        }
        args[idx++] = "-l";
        args[idx++] = ASTRA_DEFAULT_LOG_PATH;
        args[idx]   = NULL;

        execv(bin_path, (char *const *)args);
        _exit(127);
    }

    write_pid_file(ASTRA_DEFAULT_PID_PATH, worker_pid);

    int status = 0;
    while (waitpid(worker_pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }

    unlink(ASTRA_DEFAULT_PID_PATH);
    unlink(LAUNCHER_PID_PATH);

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig != SIGTERM && sig != SIGINT && sig != SIGHUP) {
            external_supervisor_emergency_restore(&snap, sig);
        }
    }

    _exit(0);
}

static int cmd_profile(const char *prof) {
    if (!prof || prof[0] == '\0') return 1;

    int fd = open(ASTRA_CTRL_NODE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "[AstraLauncher] Failed to write %s: %s\n", ASTRA_CTRL_NODE_PATH, strerror(errno));
        return 1;
    }
    (void)write(fd, prof, strlen(prof));
    (void)write(fd, "\n", 1);
    close(fd);
    printf("[AstraLauncher] Dispatched runtime profile switch -> %s\n", prof);
    return 0;
}

static int cmd_status(void) {
    pid_t pid = get_running_daemon_pid();
    char mi_prop[PROP_VALUE_MAX] = "unknown";
    __system_property_get("init.svc.mi_thermald", mi_prop);

    bool lkm_loaded = (access("/proc/k70perf", F_OK) == 0);
    char sconfig_str[16] = "-1";
    read_one_line("/sys/class/thermal/thermal_message/sconfig", sconfig_str, sizeof(sconfig_str));

    char fcc_str[32] = "0";
    read_one_line(PATH_FCC_NODE, fcc_str, sizeof(fcc_str));
    int32_t fcc_ua = 0;
    astra_parse_int32_strict(fcc_str, &fcc_ua);

    printf("=== AstraThermald Standalone Launcher Status ===\n");
    printf("AstraThermald : %s (PID: %d)\n", (pid > 0) ? "RUNNING" : "STOPPED", (int)pid);
    printf("mi_thermald   : %s\n", mi_prop);
    printf("Optional LKM  : %s\n", lkm_loaded ? "ONLINE (/proc/k70perf)" : "NOT LOADED (Standalone Mode)");
    printf("Scene sconfig : %s\n", sconfig_str);
    printf("FCC Limit     : %d mA\n", fcc_ua / 1000);
    return (pid > 0) ? 0 : 1;
}

static void usage(const char *prog) {
    printf("AstraLauncher - Dedicated Standalone Launcher & Supervisor for AstraThermald\n");
    printf("Usage:\n");
    printf("  %s start [auto|beast|esports|balance|stock] [--standby] [-c <conf>]\n", prog);
    printf("  %s stop\n", prog);
    printf("  %s restart [profile]\n", prog);
    printf("  %s profile <auto|beast|esports|balance|stock|/path/to/conf>\n", prog);
    printf("  %s status\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (getuid() != 0 && geteuid() != 0) {
        fprintf(stderr, "[AstraLauncher] Must run as root (UID 0).\n");
        return 1;
    }

    const char *sub = argv[1];
    if (strcmp(sub, "start") == 0 || strcmp(sub, "restart") == 0) {
        const char *prof = "auto";
        const char *conf = NULL;
        bool takeover    = true;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--standby") == 0 || strcmp(argv[i], "--no-takeover") == 0) {
                takeover = false;
            } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                conf = argv[++i];
            } else if (argv[i][0] != '-') {
                prof = argv[i];
            }
        }
        return cmd_start(prof, takeover, conf);
    } else if (strcmp(sub, "stop") == 0) {
        return cmd_stop();
    } else if (strcmp(sub, "profile") == 0 && argc >= 3) {
        return cmd_profile(argv[2]);
    } else if (strcmp(sub, "status") == 0) {
        return cmd_status();
    }

    usage(argv[0]);
    return 1;
}
