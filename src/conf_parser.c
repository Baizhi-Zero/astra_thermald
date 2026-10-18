/*
 * Copyright (C) 2026 Baizhi, Gemini. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * by Baizhi, Gemini
 */

#include "conf_parser.h"
#include <ctype.h>

static char *trim_whitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static int parse_int32_array(const char *line, int32_t *out_arr, int max_count) {
    int count = 0;
    const char *p = line;
    while (*p && count < max_count) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *endptr;
        long val = strtol(p, &endptr, 10);
        if (p == endptr) break;
        out_arr[count++] = (int32_t)val;
        p = endptr;
    }
    return count;
}

static int parse_string_array(const char *line, char out_arr[][32], int max_count) {
    int count = 0;
    const char *p = line;
    while (*p && count < max_count) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        int len = 0;
        while (p[len] && !isspace((unsigned char)p[len]) && len < 31) len++;
        strncpy(out_arr[count], p, len);
        out_arr[count][len] = '\0';
        count++;
        p += len;
    }
    return count;
}

int conf_parser_parse_string(const char *text, size_t len, thermal_policy_t *out_policy) {
    if (!text || len == 0 || !out_policy) return -EINVAL;

    memset(out_policy, 0, sizeof(*out_policy));

    char *buf = (char *)malloc(len + 1);
    if (!buf) return -ENOMEM;
    memcpy(buf, text, len);
    buf[len] = '\0';

    char current_section[64] = "";
    char current_algo[32] = "";

    enum { SEC_NONE, SEC_VIRTUAL, SEC_SS, SEC_MONITOR, SEC_SIC } sec_type = SEC_NONE;
    int cur_idx = -1;

    char *saveptr = NULL;
    char *line = strtok_r(buf, "\r\n", &saveptr);

    while (line != NULL) {
        line = trim_whitespace(line);

        if (line[0] == '\0' || line[0] == '#') {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(current_section, line + 1, sizeof(current_section) - 1);
                current_algo[0] = '\0';
                sec_type = SEC_NONE;
                cur_idx = -1;
            }
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        char *sep = strpbrk(line, " \t");
        if (!sep) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }
        *sep = '\0';
        char *key = line;
        char *val = trim_whitespace(sep + 1);

        if (strcmp(key, "algo_type") == 0) {
            strncpy(current_algo, val, sizeof(current_algo) - 1);
            if (strcasecmp(val, "Virtual") == 0) {
                sec_type = SEC_VIRTUAL;
                strncpy(out_policy->v_sensor.section, current_section, 63);
            } else if (strcmp(val, "ss") == 0) {
                sec_type = SEC_SS;
                if (out_policy->num_ss_rules < MAX_RULES) {
                    cur_idx = out_policy->num_ss_rules++;
                    strncpy(out_policy->ss_rules[cur_idx].section, current_section, 63);
                }
            } else if (strcmp(val, "monitor") == 0) {
                sec_type = SEC_MONITOR;
                if (out_policy->num_mon_rules < MAX_RULES) {
                    cur_idx = out_policy->num_mon_rules++;
                    strncpy(out_policy->mon_rules[cur_idx].section, current_section, 63);
                }
            } else if (strcmp(val, "sic") == 0) {
                sec_type = SEC_SIC;
                if (out_policy->num_sic_rules < 4) {
                    cur_idx = out_policy->num_sic_rules++;
                    strncpy(out_policy->sic_rules[cur_idx].section, current_section, 63);
                }
            }
        } else if (sec_type == SEC_VIRTUAL) {
            if (strcmp(key, "sensors") == 0) {
                out_policy->v_sensor.num_sensors = parse_string_array(val, out_policy->v_sensor.sensor_names, 8);
            } else if (strcmp(key, "weight") == 0) {
                parse_int32_array(val, out_policy->v_sensor.weights, 8);
            } else if (strcmp(key, "weight_sum") == 0) {
                out_policy->v_sensor.weight_sum = atoi(val);
            } else if (strcmp(key, "compensation") == 0) {
                out_policy->v_sensor.compensation = atoi(val);
            } else if (strcmp(key, "polling") == 0) {
                out_policy->v_sensor.polling_ms = atoi(val);
            }
        } else if (sec_type == SEC_SS && cur_idx >= 0) {
            parsed_ss_rule_t *r = &out_policy->ss_rules[cur_idx];
            if (strcmp(key, "sensor") == 0) {
                strncpy(r->sensor, val, 63);
            } else if (strcmp(key, "device") == 0) {
                strncpy(r->device, val, 63);
            } else if (strcmp(key, "polling") == 0) {
                r->polling_ms = atoi(val);
            } else if (strcmp(key, "trig") == 0) {
                r->num_tiers = parse_int32_array(val, r->trig, MAX_TIERS);
            } else if (strcmp(key, "clr") == 0) {
                parse_int32_array(val, r->clr, MAX_TIERS);
            } else if (strcmp(key, "target") == 0) {
                parse_int32_array(val, r->target, MAX_TIERS);
            }
        } else if (sec_type == SEC_MONITOR && cur_idx >= 0) {
            parsed_monitor_rule_t *r = &out_policy->mon_rules[cur_idx];
            if (strcmp(key, "sensor") == 0) {
                strncpy(r->sensor, val, 63);
            } else if (strcmp(key, "device") == 0) {
                strncpy(r->device, val, 63);
            } else if (strcmp(key, "polling") == 0) {
                r->polling_ms = atoi(val);
            } else if (strcmp(key, "trig") == 0) {
                r->num_tiers = parse_int32_array(val, r->trig, MAX_TIERS);
            } else if (strcmp(key, "clr") == 0) {
                parse_int32_array(val, r->clr, MAX_TIERS);
            } else if (strcmp(key, "target") == 0) {
                parse_int32_array(val, r->target, MAX_TIERS);
            }
        } else if (sec_type == SEC_SIC && cur_idx >= 0) {
            parsed_sic_rule_t *r = &out_policy->sic_rules[cur_idx];
            if (strcmp(key, "sensor") == 0) {
                strncpy(r->sensor, val, 63);
            } else if (strcmp(key, "device") == 0) {
                strncpy(r->device, val, 63);
            } else if (strcmp(key, "polling") == 0) {
                r->polling_ms = atoi(val);
            } else if (strcmp(key, "proportion") == 0) {
                r->proportion = atoi(val);
            } else if (strcmp(key, "trig") == 0) {
                r->num_tiers = parse_int32_array(val, r->trig, MAX_TIERS);
            } else if (strcmp(key, "clr") == 0) {
                parse_int32_array(val, r->clr, MAX_TIERS);
            } else if (strcmp(key, "target") == 0) {
                parse_int32_array(val, r->target, MAX_TIERS);
            } else if (strcmp(key, "ks") == 0) {
                parse_int32_array(val, r->ks, MAX_TIERS);
            } else if (strcmp(key, "ki") == 0) {
                parse_int32_array(val, r->ki, MAX_TIERS);
            } else if (strcmp(key, "kc") == 0) {
                parse_int32_array(val, r->kc, MAX_TIERS);
            } else if (strcmp(key, "max") == 0) {
                parse_int32_array(val, r->max, MAX_TIERS);
            } else if (strcmp(key, "min") == 0) {
                parse_int32_array(val, r->min, MAX_TIERS);
            }
        }

        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    free(buf);
    out_policy->loaded = true;
    return 0;
}

int conf_parser_load_file(const char *filepath, thermal_policy_t *out_policy) {
    if (!filepath || !out_policy) return -EINVAL;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ASTRA_LOG_ERROR("Failed to open thermal config: %s (%s)", filepath, strerror(errno));
        return -errno;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return -EINVAL;
    }

    uint8_t *raw_buf = (uint8_t *)malloc(file_size + 1);
    if (!raw_buf) {
        fclose(f);
        return -ENOMEM;
    }

    size_t read_bytes = fread(raw_buf, 1, file_size, f);
    fclose(f);

    if ((long)read_bytes != file_size) {
        free(raw_buf);
        return -EIO;
    }
    raw_buf[file_size] = '\0';

    bool is_encrypted = false;
    if (file_size >= 16 && (file_size % 16 == 0)) {
        if ((unsigned char)raw_buf[0] > 127 || raw_buf[0] < 32) {
            is_encrypted = true;
        }
    }

    uint8_t *plaintext = NULL;
    size_t pt_len = 0;

    if (is_encrypted) {
        ASTRA_LOG_INFO("Detected Encrypted Vendor Config: %s (size: %ld)", filepath, file_size);

        uint8_t vendor_key[16];
        uint8_t vendor_iv[16];
        if (aes128_extract_vendor_keys(vendor_key, vendor_iv) != 0) {
            ASTRA_LOG_ERROR("Failed to dynamically extract vendor thermal keys from local device /vendor/bin/mi_thermald");
            free(raw_buf);
            return -EKEYREVOKED;
        }

        plaintext = (uint8_t *)malloc(file_size + 1);
        if (!plaintext) {
            free(raw_buf);
            return -ENOMEM;
        }

        int dec_ret = aes128_cbc_decrypt(raw_buf, file_size,
                                         vendor_key,
                                         vendor_iv,
                                         plaintext, &pt_len);
        free(raw_buf);

        if (dec_ret != 0) {
            ASTRA_LOG_ERROR("Decryption failed for %s (code: %d)", filepath, dec_ret);
            free(plaintext);
            return -EACCES;
        }

        plaintext[pt_len] = '\0';
        ASTRA_LOG_INFO("Decryption succeeded via local binary extraction! Plaintext length: %zu bytes", pt_len);
    } else {
        plaintext = raw_buf;
        pt_len = file_size;
    }

    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    strncpy(out_policy->profile_name, base, sizeof(out_policy->profile_name) - 1);

    int ret = conf_parser_parse_string((char *)plaintext, pt_len, out_policy);
    free(plaintext);

    if (ret == 0) {
        ASTRA_LOG_INFO("Loaded Policy [%s]: SS Rules: %d, Monitor Rules: %d, SIC Rules: %d",
                       out_policy->profile_name, out_policy->num_ss_rules,
                       out_policy->num_mon_rules, out_policy->num_sic_rules);
    }

    return ret;
}

void conf_parser_dump_summary(const thermal_policy_t *policy) {
    if (!policy || !policy->loaded) return;

    printf("=== Thermal Policy Summary: [%s] ===\n", policy->profile_name);
    printf("Virtual Sensor [%s]: %d sensors, sum=%d, comp=%d\n",
           policy->v_sensor.section, policy->v_sensor.num_sensors,
           policy->v_sensor.weight_sum, policy->v_sensor.compensation);

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
