/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "conf_parser.h"
#include <ctype.h>

#define MAX_CONF_SIZE (64 * 1024)
static char s_parse_buf[MAX_CONF_SIZE];
static uint8_t s_raw_buf[MAX_CONF_SIZE];
static uint8_t s_pt_buf[MAX_CONF_SIZE];

static char *trim_whitespace(char *str) {
    while (*str && isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static int parse_int32_array(const char *line, int32_t *out_arr, int max_count) {
    if (!line || !out_arr || max_count <= 0) return -EINVAL;
    int count = 0;
    const char *p = line;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (count >= max_count) {
            return -EOVERFLOW;
        }

        errno = 0;
        char *endptr = NULL;
        long val = strtol(p, &endptr, 10);
        if (p == endptr || errno == ERANGE || val < INT32_MIN || val > INT32_MAX) {
            return -EINVAL;
        }

        while (*endptr == '+') {
            const char *sub = endptr + 1;
            if (!isdigit((unsigned char)*sub) && *sub != '-') {
                return -EINVAL;
            }
            errno = 0;
            char *sub_end = NULL;
            long sub_val = strtol(sub, &sub_end, 10);
            if (sub == sub_end || errno == ERANGE || sub_val < INT32_MIN || sub_val > INT32_MAX) {
                return -EINVAL;
            }
            endptr = sub_end;
        }

        if (*endptr != '\0' && !isspace((unsigned char)*endptr)) {
            return -EINVAL;
        }

        out_arr[count++] = (int32_t)val;
        p = endptr;
    }
    return count;
}

static int parse_string_array(const char *line, char out_arr[][32], int max_count) {
    if (!line || !out_arr || max_count <= 0) return -EINVAL;
    int count = 0;
    const char *p = line;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        int token_len = 0;
        while (p[token_len] && !isspace((unsigned char)p[token_len])) token_len++;

        if (token_len >= 32) {
            return -ENAMETOOLONG;
        }
        if (count >= max_count) {
            return -EOVERFLOW;
        }

        memcpy(out_arr[count], p, (size_t)token_len);
        out_arr[count][token_len] = '\0';
        count++;
        p += token_len;
    }
    return count;
}

typedef struct {
    bool active;
    bool has_error;
    char section[64];
    char algo_type[32];
    char sensor[64];
    char device[64];
    int polling_ms;
    int proportion;
    int reverse;

    int num_trig;
    int num_clr;
    int num_target;
    int num_ks;
    int num_ki;
    int num_kc;
    int num_max;
    int num_min;

    int32_t trig[MAX_TIERS];
    int32_t clr[MAX_TIERS];
    int32_t target[MAX_TIERS];
    int32_t ks[MAX_TIERS];
    int32_t ki[MAX_TIERS];
    int32_t kc[MAX_TIERS];
    int32_t max[MAX_TIERS];
    int32_t min[MAX_TIERS];

    char v_sensors[8][32];
    int32_t v_weights[8];
    int num_v_sensors;
    int num_v_weights;
    int weight_sum;
    int compensation;
    bool has_weight_sum;
} staged_section_t;

static int commit_staged_section(const staged_section_t *st, thermal_policy_t *out_policy) {
    if (!st || !st->active) return 0;
    if (st->has_error) {
        ASTRA_LOG_WARN("[CONF-PARSER] Section [%s] rejected due to malformed field values", st->section);
        return -EINVAL;
    }
    if (st->algo_type[0] == '\0') {
        return 0;
    }

    if (strcasecmp(st->algo_type, "Virtual") == 0) {
        if (st->num_v_sensors <= 0 || st->num_v_sensors != st->num_v_weights ||
            !st->has_weight_sum || st->weight_sum == 0) {
            ASTRA_LOG_WARN("[CONF-PARSER] Virtual section [%s] invalid: sensors=%d, weights=%d, sum=%d",
                           st->section, st->num_v_sensors, st->num_v_weights, st->weight_sum);
            return -EINVAL;
        }

        if (!out_policy->has_virtual_sensor || strcmp(st->section, "VIRTUAL-SENSOR0") == 0) {
            parsed_virtual_sensor_t *vs = &out_policy->v_sensor;
            memset(vs, 0, sizeof(*vs));
            strncpy(vs->section, st->section, sizeof(vs->section) - 1);
            memcpy(vs->sensor_names, st->v_sensors, sizeof(vs->sensor_names));
            memcpy(vs->weights, st->v_weights, sizeof(vs->weights));
            vs->num_sensors  = st->num_v_sensors;
            vs->num_weights  = st->num_v_weights;
            vs->weight_sum   = st->weight_sum;
            vs->compensation = st->compensation;
            vs->polling_ms   = st->polling_ms;
            vs->valid        = true;
            out_policy->has_virtual_sensor = true;
        }
        return 0;
    }

    if (strcmp(st->algo_type, "ss") == 0) {

        if (st->sensor[0] == '\0' || st->device[0] == '\0' ||
            st->num_trig <= 0 || st->num_trig != st->num_clr || st->num_trig != st->num_target) {
            ASTRA_LOG_WARN("[CONF-PARSER] SS section [%s] rejected: array length mismatch (trig=%d, clr=%d, target=%d)",
                           st->section, st->num_trig, st->num_clr, st->num_target);
            return -EINVAL;
        }
        if (out_policy->num_ss_rules >= MAX_RULES) return -EOVERFLOW;
        parsed_ss_rule_t *r = &out_policy->ss_rules[out_policy->num_ss_rules++];
        memset(r, 0, sizeof(*r));
        strncpy(r->section, st->section, sizeof(r->section) - 1);
        strncpy(r->sensor, st->sensor, sizeof(r->sensor) - 1);
        strncpy(r->device, st->device, sizeof(r->device) - 1);
        r->polling_ms = st->polling_ms;
        r->num_tiers  = st->num_trig;
        memcpy(r->trig, st->trig, sizeof(r->trig));
        memcpy(r->clr, st->clr, sizeof(r->clr));
        memcpy(r->target, st->target, sizeof(r->target));
        return 0;
    }

    if (strcmp(st->algo_type, "monitor") == 0) {
        if (st->sensor[0] == '\0' || st->device[0] == '\0' ||
            st->num_trig <= 0 || st->num_trig != st->num_clr || st->num_trig != st->num_target) {
            ASTRA_LOG_WARN("[CONF-PARSER] Monitor section [%s] rejected: array length mismatch (trig=%d, clr=%d, target=%d)",
                           st->section, st->num_trig, st->num_clr, st->num_target);
            return -EINVAL;
        }
        if (out_policy->num_mon_rules >= MAX_RULES) return -EOVERFLOW;
        parsed_monitor_rule_t *r = &out_policy->mon_rules[out_policy->num_mon_rules++];
        memset(r, 0, sizeof(*r));
        strncpy(r->section, st->section, sizeof(r->section) - 1);
        strncpy(r->sensor, st->sensor, sizeof(r->sensor) - 1);
        strncpy(r->device, st->device, sizeof(r->device) - 1);
        r->polling_ms = st->polling_ms;
        r->reverse    = st->reverse;
        r->num_tiers  = st->num_trig;
        memcpy(r->trig, st->trig, sizeof(r->trig));
        memcpy(r->clr, st->clr, sizeof(r->clr));
        memcpy(r->target, st->target, sizeof(r->target));
        return 0;
    }

    if (strcmp(st->algo_type, "sic") == 0) {
        int n = st->num_trig;
        if (st->sensor[0] == '\0' || st->device[0] == '\0' || n <= 0 ||
            st->num_clr != n || st->num_target != n ||
            st->num_ks != n || st->num_ki != n || st->num_kc != n ||
            st->num_max != n || st->num_min != n) {
            ASTRA_LOG_WARN("[CONF-PARSER] SIC section [%s] rejected: inconsistent array counts", st->section);
            return -EINVAL;
        }
        if (out_policy->num_sic_rules >= MAX_SIC_RULES) return -EOVERFLOW;
        parsed_sic_rule_t *r = &out_policy->sic_rules[out_policy->num_sic_rules++];
        memset(r, 0, sizeof(*r));
        strncpy(r->section, st->section, sizeof(r->section) - 1);
        strncpy(r->sensor, st->sensor, sizeof(r->sensor) - 1);
        strncpy(r->device, st->device, sizeof(r->device) - 1);
        r->polling_ms = st->polling_ms;
        r->proportion = st->proportion;
        r->num_tiers  = n;
        memcpy(r->trig, st->trig, sizeof(r->trig));
        memcpy(r->clr, st->clr, sizeof(r->clr));
        memcpy(r->target, st->target, sizeof(r->target));
        memcpy(r->ks, st->ks, sizeof(r->ks));
        memcpy(r->ki, st->ki, sizeof(r->ki));
        memcpy(r->kc, st->kc, sizeof(r->kc));
        memcpy(r->max, st->max, sizeof(r->max));
        memcpy(r->min, st->min, sizeof(r->min));
        return 0;
    }

    return 0;
}

int conf_parser_parse_string(const char *text, size_t len, thermal_policy_t *out_policy) {
    if (!text || len == 0 || !out_policy) return -EINVAL;
    if (len >= MAX_CONF_SIZE) return -EFBIG;

    char saved_name[64];
    strncpy(saved_name, out_policy->profile_name, sizeof(saved_name) - 1);
    saved_name[sizeof(saved_name) - 1] = '\0';

    memset(out_policy, 0, sizeof(*out_policy));
    strncpy(out_policy->profile_name, saved_name, sizeof(out_policy->profile_name) - 1);

    memcpy(s_parse_buf, text, len);
    s_parse_buf[len] = '\0';

    staged_section_t staged;
    memset(&staged, 0, sizeof(staged));

    char *saveptr = NULL;
    char *line = strtok_r(s_parse_buf, "\r\n", &saveptr);

    while (line != NULL) {
        line = trim_whitespace(line);

        if (line[0] == '\0' || line[0] == '#') {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (line[0] == '[') {

            if (staged.active) {
                int c_ret = commit_staged_section(&staged, out_policy);
                if (c_ret < 0) return c_ret;
            }
            memset(&staged, 0, sizeof(staged));

            char *end = strchr(line, ']');
            if (!end || end == line + 1) {
                return -EINVAL;
            }
            *end = '\0';
            if (strlen(line + 1) >= sizeof(staged.section)) {
                return -ENAMETOOLONG;
            }
            strncpy(staged.section, line + 1, sizeof(staged.section) - 1);
            staged.active = true;

            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (!staged.active) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char *sep = strpbrk(line, " \t=");
        if (!sep) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }
        *sep = '\0';
        char *key = trim_whitespace(line);
        char *val = trim_whitespace(sep + 1);

        int32_t scalar_val = 0;

        if (strcmp(key, "algo_type") == 0) {
            if (strlen(val) >= sizeof(staged.algo_type)) staged.has_error = true;
            strncpy(staged.algo_type, val, sizeof(staged.algo_type) - 1);
        } else if (strcmp(key, "sensor") == 0) {
            if (strlen(val) >= sizeof(staged.sensor)) staged.has_error = true;
            strncpy(staged.sensor, val, sizeof(staged.sensor) - 1);
        } else if (strcmp(key, "device") == 0) {
            if (strlen(val) >= sizeof(staged.device)) staged.has_error = true;
            strncpy(staged.device, val, sizeof(staged.device) - 1);
        } else if (strcmp(key, "polling") == 0) {
            if (astra_parse_int32_strict(val, &scalar_val) && scalar_val >= 0) {
                staged.polling_ms = scalar_val;
            } else {
                staged.has_error = true;
            }
        } else if (strcmp(key, "proportion") == 0) {
            if (astra_parse_int32_strict(val, &scalar_val)) {
                staged.proportion = scalar_val;
            } else {
                staged.has_error = true;
            }
        } else if (strcmp(key, "reverse") == 0) {
            if (astra_parse_int32_strict(val, &scalar_val)) {
                staged.reverse = scalar_val;
            } else {
                staged.has_error = true;
            }
        } else if (strcmp(key, "sensors") == 0) {
            int n = parse_string_array(val, staged.v_sensors, 8);
            if (n < 0) staged.has_error = true;
            else staged.num_v_sensors = n;
        } else if (strcmp(key, "weight") == 0) {
            int n = parse_int32_array(val, staged.v_weights, 8);
            if (n < 0) staged.has_error = true;
            else staged.num_v_weights = n;
        } else if (strcmp(key, "weight_sum") == 0) {
            if (astra_parse_int32_strict(val, &scalar_val) && scalar_val != 0) {
                staged.weight_sum = scalar_val;
                staged.has_weight_sum = true;
            } else {
                staged.has_error = true;
            }
        } else if (strcmp(key, "compensation") == 0) {
            if (astra_parse_int32_strict(val, &scalar_val)) {
                staged.compensation = scalar_val;
            } else {
                staged.has_error = true;
            }
        } else if (strcmp(key, "trig") == 0) {
            int n = parse_int32_array(val, staged.trig, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_trig = n;
        } else if (strcmp(key, "clr") == 0) {
            int n = parse_int32_array(val, staged.clr, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_clr = n;
        } else if (strcmp(key, "target") == 0) {
            int n = parse_int32_array(val, staged.target, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_target = n;
        } else if (strcmp(key, "ks") == 0) {
            int n = parse_int32_array(val, staged.ks, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_ks = n;
        } else if (strcmp(key, "ki") == 0) {
            int n = parse_int32_array(val, staged.ki, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_ki = n;
        } else if (strcmp(key, "kc") == 0) {
            int n = parse_int32_array(val, staged.kc, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_kc = n;
        } else if (strcmp(key, "max") == 0) {
            int n = parse_int32_array(val, staged.max, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_max = n;
        } else if (strcmp(key, "min") == 0) {
            int n = parse_int32_array(val, staged.min, MAX_TIERS);
            if (n <= 0) staged.has_error = true;
            else staged.num_min = n;
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    if (staged.active) {
        int c_ret = commit_staged_section(&staged, out_policy);
        if (c_ret < 0) return c_ret;
    }

    out_policy->parsed = true;

    int total_rules = out_policy->num_ss_rules + out_policy->num_mon_rules + out_policy->num_sic_rules;
    if (total_rules <= 0) {
        ASTRA_LOG_ERROR("[CONF-PARSER] Policy rejected: 0 valid executable thermal rules found");
        return -EINVAL;
    }

    out_policy->validated = true;
    out_policy->loaded    = true;
    return 0;
}

static bool is_valid_plaintext_config(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return false;
    bool has_open_bracket = false;
    bool has_close_bracket = false;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (c < 0x09 || (c > 0x0D && c < 0x20) || c == 0x7F) {
            return false;
        }
        if (c == '[') has_open_bracket = true;
        if (c == ']' && has_open_bracket) has_close_bracket = true;
    }
    return (has_open_bracket && has_close_bracket);
}

int conf_parser_load_file(const char *filepath, thermal_policy_t *out_policy) {
    if (!filepath || !out_policy) return -EINVAL;

    memset(out_policy, 0, sizeof(*out_policy));

    struct stat st;
    if (stat(filepath, &st) < 0) {
        ASTRA_LOG_ERROR("Failed to stat thermal config: %s (%s)", filepath, strerror(errno));
        return -errno;
    }
    if (!S_ISREG(st.st_mode)) {
        ASTRA_LOG_ERROR("Rejected non-regular config file: %s", filepath);
        return -EINVAL;
    }
    if (st.st_size <= 0) {
        return -EINVAL;
    }
    if (st.st_size >= MAX_CONF_SIZE) {
        ASTRA_LOG_ERROR("Thermal config exceeds maximum buffer size (%lld >= %d): %s",
                        (long long)st.st_size, MAX_CONF_SIZE, filepath);
        return -EFBIG;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ASTRA_LOG_ERROR("Failed to open thermal config: %s (%s)", filepath, strerror(errno));
        return -errno;
    }

    size_t file_size = (size_t)st.st_size;
    size_t read_bytes = fread(s_raw_buf, 1, file_size, f);
    fclose(f);

    if (read_bytes != file_size) {
        return -EIO;
    }
    s_raw_buf[file_size] = '\0';

    const uint8_t *plaintext = NULL;
    size_t pt_len = 0;

    if (is_valid_plaintext_config(s_raw_buf, file_size)) {
        plaintext = s_raw_buf;
        pt_len = file_size;
    } else if (file_size >= 16 && (file_size % 16 == 0)) {
        ASTRA_LOG_INFO("Detected Encrypted Official Xiaomi Config: %s (size: %zu)", filepath, file_size);

        uint8_t vendor_key[16];
        uint8_t vendor_iv[16];
        if (aes128_extract_and_verify_vendor_keys(s_raw_buf, file_size, vendor_key, vendor_iv) != 0) {
            ASTRA_LOG_ERROR("Failed to dynamically extract vendor thermal keys from local /vendor/bin/mi_thermald");
            return -EKEYREVOKED;
        }
        int dec_ret = aes128_cbc_decrypt(s_raw_buf, file_size,
                                         vendor_key,
                                         vendor_iv,
                                         s_pt_buf, &pt_len);

        if (dec_ret != 0 || !is_valid_plaintext_config(s_pt_buf, pt_len)) {
            ASTRA_LOG_ERROR("Decryption or plaintext verification failed for %s (code: %d)", filepath, dec_ret);
            return -EACCES;
        }

        s_pt_buf[pt_len] = '\0';
        plaintext = s_pt_buf;
        ASTRA_LOG_INFO("Decryption & verification succeeded! Plaintext length: %zu bytes", pt_len);
    } else {
        ASTRA_LOG_ERROR("File is neither valid plaintext config nor 16-byte aligned AES ciphertext: %s", filepath);
        return -EINVAL;
    }

    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    strncpy(out_policy->profile_name, base, sizeof(out_policy->profile_name) - 1);
    out_policy->profile_name[sizeof(out_policy->profile_name) - 1] = '\0';

    int ret = conf_parser_parse_string((const char *)plaintext, pt_len, out_policy);
    if (ret == 0) {
        ASTRA_LOG_INFO("Loaded & Validated Policy [%s]: SS=%d, Monitor=%d, SIC=%d, Virtual=%s",
                       out_policy->profile_name, out_policy->num_ss_rules,
                       out_policy->num_mon_rules, out_policy->num_sic_rules,
                       out_policy->has_virtual_sensor ? out_policy->v_sensor.section : "default");
    }

    return ret;
}

void conf_parser_dump_summary(const thermal_policy_t *policy) {
    if (!policy || !policy->loaded) return;

    printf("=== Thermal Policy Summary: [%s] ===\n", policy->profile_name);
    if (policy->has_virtual_sensor) {
        printf("Virtual Sensor [%s]: %d sensors, sum=%d, comp=%d\n",
               policy->v_sensor.section, policy->v_sensor.num_sensors,
               policy->v_sensor.weight_sum, policy->v_sensor.compensation);
    }

    printf("Step-Wise Rules (%d):\n", policy->num_ss_rules);
    for (int i = 0; i < policy->num_ss_rules; i++) {
        const parsed_ss_rule_t *r = &policy->ss_rules[i];
        printf("  [%s] dev: %s, tiers: %d (trig0: %d, target0: %d)\n",
               r->section, r->device, r->num_tiers,
               r->num_tiers > 0 ? r->trig[0] : -1,
               r->num_tiers > 0 ? r->target[0] : -1);
    }

    printf("Monitor Rules (%d):\n", policy->num_mon_rules);
    for (int i = 0; i < policy->num_mon_rules; i++) {
        const parsed_monitor_rule_t *r = &policy->mon_rules[i];
        printf("  [%s] dev: %s, tiers: %d\n", r->section, r->device, r->num_tiers);
    }

    printf("SIC Fast-Charge PID Rules (%d):\n", policy->num_sic_rules);
    for (int i = 0; i < policy->num_sic_rules; i++) {
        const parsed_sic_rule_t *r = &policy->sic_rules[i];
        printf("  [%s] dev: %s, max0: %d mA, min0: %d mA\n",
               r->section, r->device,
               r->num_tiers > 0 ? r->max[0] : -1,
               r->num_tiers > 0 ? r->min[0] : -1);
    }
}
