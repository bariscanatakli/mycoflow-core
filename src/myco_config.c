/*
 * MycoFlow — Bio-Inspired Reflexive QoS System
 * myco_config.c — Configuration loading (UCI + env + defaults)
 */
#include "myco_config.h"
#include "myco_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Utility helpers ────────────────────────────────────────── */

double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

double now_monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static int parse_env_int(const char *key, int default_value) {
    const char *val = getenv(key);
    if (!val || !*val) {
        return default_value;
    }
    return atoi(val);
}

static double parse_env_double(const char *key, double default_value) {
    const char *val = getenv(key);
    if (!val || !*val) {
        return default_value;
    }
    return atof(val);
}

/* ── Defaults ───────────────────────────────────────────────── */

static void apply_defaults(myco_config_t *cfg) {
    cfg->enabled = 1;
    strncpy(cfg->egress_iface, "eth0", sizeof(cfg->egress_iface) - 1);
    cfg->egress_iface[sizeof(cfg->egress_iface) - 1] = '\0';
    cfg->sample_hz = 1.0;
    cfg->max_cpu_pct = 40.0;
    cfg->log_level = 2;
    cfg->dummy_metrics = 1;
    cfg->baseline_samples = 5;
    cfg->action_cooldown_s = 3.0;
    cfg->action_rate_limit = 0.5;
    cfg->bandwidth_kbit = 20000;
    cfg->bandwidth_step_kbit = 2000;
    cfg->min_bandwidth_kbit = 2000;
    cfg->max_bandwidth_kbit = 100000;
    cfg->no_tc = 1;
    cfg->metric_file[0] = '\0';
    strncpy(cfg->probe_host, "1.1.1.1", sizeof(cfg->probe_host) - 1);
    cfg->probe_host[sizeof(cfg->probe_host) - 1] = '\0';
    cfg->force_act_fail = 0;
    cfg->ebpf_enabled = 0;
    strncpy(cfg->ebpf_obj, "/usr/lib/mycoflow/mycoflow.bpf.o", sizeof(cfg->ebpf_obj) - 1);
    cfg->ebpf_obj[sizeof(cfg->ebpf_obj) - 1] = '\0';
    cfg->ebpf_attach = 0;
    strncpy(cfg->ebpf_tc_dir, "ingress", sizeof(cfg->ebpf_tc_dir) - 1);
    cfg->ebpf_tc_dir[sizeof(cfg->ebpf_tc_dir) - 1] = '\0';
}

/* ── UCI helpers ────────────────────────────────────────────── */

static void trim_whitespace(char *value) {
    if (!value) {
        return;
    }
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[len - 1] = '\0';
        len--;
    }
    size_t start = 0;
    while (value[start] && isspace((unsigned char)value[start])) {
        start++;
    }
    if (start > 0) {
        memmove(value, value + start, len - start + 1);
    }
}

static int uci_get_option(const char *option, char *out, size_t out_len) {
    if (!option || !out || out_len == 0) {
        return 0;
    }
    const char *sections[] = {
        "mycoflow.mycoflow",
        "mycoflow.@mycoflow[0]"
    };
    char cmd[256];
    char buf[256];
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        snprintf(cmd, sizeof(cmd), "uci -q get %s.%s 2>/dev/null", sections[i], option);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            continue;
        }
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            trim_whitespace(buf);
            if (buf[0] != '\0') {
                strncpy(out, buf, out_len - 1);
                out[out_len - 1] = '\0';
                return 1;
            }
        } else {
            pclose(fp);
        }
    }
    return 0;
}

/* ── UCI overrides ──────────────────────────────────────────── */

static void apply_uci_overrides(myco_config_t *cfg) {
    char val[128];
    if (uci_get_option("enabled", val, sizeof(val))) {
        cfg->enabled = atoi(val);
    }
    if (uci_get_option("egress_iface", val, sizeof(val))) {
        strncpy(cfg->egress_iface, val, sizeof(cfg->egress_iface) - 1);
        cfg->egress_iface[sizeof(cfg->egress_iface) - 1] = '\0';
    }
    if (uci_get_option("sample_hz", val, sizeof(val))) {
        cfg->sample_hz = atof(val);
    }
    if (uci_get_option("max_cpu", val, sizeof(val))) {
        cfg->max_cpu_pct = atof(val);
    }
    if (uci_get_option("log_level", val, sizeof(val))) {
        cfg->log_level = atoi(val);
    }
    if (uci_get_option("dummy_metrics", val, sizeof(val))) {
        cfg->dummy_metrics = atoi(val);
    }
    if (uci_get_option("baseline_samples", val, sizeof(val))) {
        cfg->baseline_samples = atoi(val);
    }
    if (uci_get_option("action_cooldown", val, sizeof(val))) {
        cfg->action_cooldown_s = atof(val);
    }
    if (uci_get_option("action_rate", val, sizeof(val))) {
        cfg->action_rate_limit = atof(val);
    }
    if (uci_get_option("bandwidth_kbit", val, sizeof(val))) {
        cfg->bandwidth_kbit = atoi(val);
    }
    if (uci_get_option("bandwidth_step_kbit", val, sizeof(val))) {
        cfg->bandwidth_step_kbit = atoi(val);
    }
    if (uci_get_option("min_bandwidth_kbit", val, sizeof(val))) {
        cfg->min_bandwidth_kbit = atoi(val);
    }
    if (uci_get_option("max_bandwidth_kbit", val, sizeof(val))) {
        cfg->max_bandwidth_kbit = atoi(val);
    }
    if (uci_get_option("no_tc", val, sizeof(val))) {
        cfg->no_tc = atoi(val);
    }
    if (uci_get_option("metric_file", val, sizeof(val))) {
        strncpy(cfg->metric_file, val, sizeof(cfg->metric_file) - 1);
        cfg->metric_file[sizeof(cfg->metric_file) - 1] = '\0';
    }
    if (uci_get_option("probe_host", val, sizeof(val))) {
        strncpy(cfg->probe_host, val, sizeof(cfg->probe_host) - 1);
        cfg->probe_host[sizeof(cfg->probe_host) - 1] = '\0';
    }
    if (uci_get_option("force_act_fail", val, sizeof(val))) {
        cfg->force_act_fail = atoi(val);
    }
    if (uci_get_option("ebpf_enabled", val, sizeof(val))) {
        cfg->ebpf_enabled = atoi(val);
    }
    if (uci_get_option("ebpf_obj", val, sizeof(val))) {
        strncpy(cfg->ebpf_obj, val, sizeof(cfg->ebpf_obj) - 1);
        cfg->ebpf_obj[sizeof(cfg->ebpf_obj) - 1] = '\0';
    }
    if (uci_get_option("ebpf_attach", val, sizeof(val))) {
        cfg->ebpf_attach = atoi(val);
    }
    if (uci_get_option("ebpf_tc_dir", val, sizeof(val))) {
        strncpy(cfg->ebpf_tc_dir, val, sizeof(cfg->ebpf_tc_dir) - 1);
        cfg->ebpf_tc_dir[sizeof(cfg->ebpf_tc_dir) - 1] = '\0';
    }
}

/* ── Environment overrides ──────────────────────────────────── */

static void apply_env_overrides(myco_config_t *cfg) {
    cfg->enabled = parse_env_int("MYCOFLOW_ENABLED", cfg->enabled);
    const char *iface = getenv("MYCOFLOW_EGRESS_IFACE");
    if (iface && *iface) {
        strncpy(cfg->egress_iface, iface, sizeof(cfg->egress_iface) - 1);
        cfg->egress_iface[sizeof(cfg->egress_iface) - 1] = '\0';
    }
    cfg->sample_hz = parse_env_double("MYCOFLOW_SAMPLE_HZ", cfg->sample_hz);
    cfg->max_cpu_pct = parse_env_double("MYCOFLOW_MAX_CPU", cfg->max_cpu_pct);
    cfg->log_level = parse_env_int("MYCOFLOW_LOG_LEVEL", cfg->log_level);
    cfg->dummy_metrics = parse_env_int("MYCOFLOW_DUMMY", cfg->dummy_metrics);
    cfg->baseline_samples = parse_env_int("MYCOFLOW_BASELINE_SAMPLES", cfg->baseline_samples);
    cfg->action_cooldown_s = parse_env_double("MYCOFLOW_ACTION_COOLDOWN", cfg->action_cooldown_s);
    cfg->action_rate_limit = parse_env_double("MYCOFLOW_ACTION_RATE", cfg->action_rate_limit);
    cfg->bandwidth_kbit = parse_env_int("MYCOFLOW_BW_KBIT", cfg->bandwidth_kbit);
    cfg->bandwidth_step_kbit = parse_env_int("MYCOFLOW_BW_STEP", cfg->bandwidth_step_kbit);
    cfg->min_bandwidth_kbit = parse_env_int("MYCOFLOW_BW_MIN", cfg->min_bandwidth_kbit);
    cfg->max_bandwidth_kbit = parse_env_int("MYCOFLOW_BW_MAX", cfg->max_bandwidth_kbit);
    cfg->no_tc = parse_env_int("MYCOFLOW_NO_TC", cfg->no_tc);
    const char *metric_file = getenv("MYCOFLOW_METRIC_FILE");
    if (metric_file && *metric_file) {
        strncpy(cfg->metric_file, metric_file, sizeof(cfg->metric_file) - 1);
        cfg->metric_file[sizeof(cfg->metric_file) - 1] = '\0';
    }
    const char *probe_host = getenv("MYCOFLOW_PROBE_HOST");
    if (probe_host && *probe_host) {
        strncpy(cfg->probe_host, probe_host, sizeof(cfg->probe_host) - 1);
        cfg->probe_host[sizeof(cfg->probe_host) - 1] = '\0';
    }
    cfg->force_act_fail = parse_env_int("MYCOFLOW_FORCE_ACT_FAIL", cfg->force_act_fail);
    cfg->ebpf_enabled = parse_env_int("MYCOFLOW_EBPF", cfg->ebpf_enabled);
    const char *ebpf_obj = getenv("MYCOFLOW_EBPF_OBJ");
    if (ebpf_obj && *ebpf_obj) {
        strncpy(cfg->ebpf_obj, ebpf_obj, sizeof(cfg->ebpf_obj) - 1);
        cfg->ebpf_obj[sizeof(cfg->ebpf_obj) - 1] = '\0';
    }
    cfg->ebpf_attach = parse_env_int("MYCOFLOW_EBPF_ATTACH", cfg->ebpf_attach);
    const char *ebpf_tc_dir = getenv("MYCOFLOW_EBPF_TC_DIR");
    if (ebpf_tc_dir && *ebpf_tc_dir) {
        strncpy(cfg->ebpf_tc_dir, ebpf_tc_dir, sizeof(cfg->ebpf_tc_dir) - 1);
        cfg->ebpf_tc_dir[sizeof(cfg->ebpf_tc_dir) - 1] = '\0';
    }
}

/* ── Public API ─────────────────────────────────────────────── */

int config_load(myco_config_t *cfg) {
    if (!cfg) {
        return -1;
    }
    apply_defaults(cfg);
    apply_uci_overrides(cfg);
    apply_env_overrides(cfg);
    if (cfg->sample_hz <= 0.1) {
        cfg->sample_hz = 0.1;
    }
    if (cfg->action_cooldown_s < 0.0) {
        cfg->action_cooldown_s = 0.0;
    }
    if (cfg->action_rate_limit <= 0.0) {
        cfg->action_rate_limit = 0.1;
    }
    if (cfg->min_bandwidth_kbit < 100) {
        cfg->min_bandwidth_kbit = 100;
    }
    if (cfg->max_bandwidth_kbit < cfg->min_bandwidth_kbit) {
        cfg->max_bandwidth_kbit = cfg->min_bandwidth_kbit;
    }
    if (cfg->bandwidth_kbit < cfg->min_bandwidth_kbit) {
        cfg->bandwidth_kbit = cfg->min_bandwidth_kbit;
    }
    if (cfg->bandwidth_kbit > cfg->max_bandwidth_kbit) {
        cfg->bandwidth_kbit = cfg->max_bandwidth_kbit;
    }
    if (strcmp(cfg->ebpf_tc_dir, "ingress") != 0 && strcmp(cfg->ebpf_tc_dir, "egress") != 0) {
        strncpy(cfg->ebpf_tc_dir, "ingress", sizeof(cfg->ebpf_tc_dir) - 1);
        cfg->ebpf_tc_dir[sizeof(cfg->ebpf_tc_dir) - 1] = '\0';
    }
    return 0;
}

int config_reload(myco_config_t *cfg) {
    return config_load(cfg);
}
