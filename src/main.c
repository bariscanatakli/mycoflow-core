#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_UBUS
#include <libubox/blobmsg_json.h>
#include <libubox/uloop.h>
#include <libubus.h>
#endif

#ifdef HAVE_LIBBPF
#include <bpf/libbpf.h>
#endif

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3
} log_level_t;

typedef struct {
    int enabled;
    char egress_iface[32];
    double sample_hz;
    double max_cpu_pct;
    int log_level;
    int dummy_metrics;
    int baseline_samples;
    double action_cooldown_s;
    double action_rate_limit;
    int bandwidth_kbit;
    int bandwidth_step_kbit;
    int min_bandwidth_kbit;
    int max_bandwidth_kbit;
    int no_tc;
    char metric_file[128];
    char probe_host[64];
    int force_act_fail;
    int ebpf_enabled;
    char ebpf_obj[128];
    int ebpf_attach;
    char ebpf_tc_dir[16];
} myco_config_t;

typedef struct {
    double rtt_ms;
    double jitter_ms;
    double rx_bps;
    double tx_bps;
    double cpu_pct;
} metrics_t;

typedef enum {
    PERSONA_UNKNOWN = 0,
    PERSONA_INTERACTIVE = 1,
    PERSONA_BULK = 2
} persona_t;

typedef struct {
    persona_t current;
    persona_t history[5];
    int history_len;
} persona_state_t;

typedef struct {
    int bandwidth_kbit;
    int boosted;
} policy_t;

typedef struct {
    policy_t current;
    policy_t last_stable;
    int safe_mode;
    int stable_cycles;
} control_state_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;

static int g_log_level = LOG_INFO;
static unsigned long long g_prev_rx = 0;
static unsigned long long g_prev_tx = 0;
static double g_prev_rtt = 10.0;
static unsigned long long g_prev_cpu_total = 0;
static unsigned long long g_prev_cpu_idle = 0;
static int g_seeded = 0;
static persona_t g_persona_override = PERSONA_UNKNOWN;
static int g_persona_override_active = 0;
static metrics_t g_last_metrics;
static metrics_t g_last_baseline;
static policy_t g_last_policy;
static persona_t g_last_persona = PERSONA_UNKNOWN;
static char g_last_reason[128];

#ifdef HAVE_LIBBPF
static struct bpf_object *g_bpf_obj = NULL;
#endif
static int g_ebpf_attached = 0;
static char g_ebpf_iface[32];
static char g_ebpf_dir[16];

#ifdef HAVE_UBUS
static struct ubus_context *g_ubus_ctx = NULL;
static struct ubus_object g_ubus_obj;
static pthread_t g_ubus_thread;
static int g_ubus_running = 0;
static int g_ubus_thread_started = 0;
static myco_config_t *g_ubus_cfg = NULL;
static control_state_t *g_ubus_control = NULL;
#endif

static double now_monotonic_s(void) {
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

static double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static const char *level_name(int level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN: return "WARN";
        case LOG_INFO: return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default: return "UNK";
    }
}

static void log_init(int level) {
    g_log_level = level;
}

static void log_set_level(int level) {
    g_log_level = level;
}

static void log_msg(int level, const char *source, const char *fmt, ...) {
    if (level > g_log_level) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);

    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);

    fprintf(stdout, "%s.%03ld [%s] %s: ",
            time_buf, ts.tv_nsec / 1000000L, level_name(level), source);

    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "\n");
    fflush(stdout);
}

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

static int config_load(myco_config_t *cfg) {
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

static int config_reload(myco_config_t *cfg) {
    return config_load(cfg);
}

static int read_netdev(const char *iface, unsigned long long *rx_bytes, unsigned long long *tx_bytes) {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return -1;
    }

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        if (line_no <= 2) {
            continue;
        }
        char name[64];
        unsigned long long rx = 0;
        unsigned long long tx = 0;
        if (sscanf(line, " %63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                   name, &rx, &tx) == 3) {
            if (strcmp(name, iface) == 0) {
                *rx_bytes = rx;
                *tx_bytes = tx;
                fclose(fp);
                return 0;
            }
        }
    }
    fclose(fp);
    return -1;
}

static double read_cpu_pct(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0.0;
    }
    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0.0;
    }
    fclose(fp);

    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(line, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return 0.0;
    }

    unsigned long long idle_all = idle + iowait;
    unsigned long long non_idle = user + nice + system + irq + softirq + steal;
    unsigned long long total = idle_all + non_idle;

    if (g_prev_cpu_total == 0) {
        g_prev_cpu_total = total;
        g_prev_cpu_idle = idle_all;
        return 0.0;
    }

    unsigned long long totald = total - g_prev_cpu_total;
    unsigned long long idled = idle_all - g_prev_cpu_idle;

    g_prev_cpu_total = total;
    g_prev_cpu_idle = idle_all;

    if (totald == 0) {
        return 0.0;
    }

    double cpu_pct = (double)(totald - idled) * 100.0 / (double)totald;
    if (cpu_pct < 0.0) {
        cpu_pct = 0.0;
    }
    return cpu_pct;
}

static double dummy_rtt(void) {
    if (!g_seeded) {
        srand((unsigned int)time(NULL));
        g_seeded = 1;
    }
    double base = 10.0 + (rand() % 10);
    double spike = (rand() % 100 < 5) ? (double)(rand() % 60) : 0.0;
    return base + spike;
}

static double probe_rtt_ms(const char *iface, const char *host) {
    if (!iface || !host) {
        return -1.0;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 -I %s %s 2>/dev/null", iface, host);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1.0;
    }
    char line[256];
    double rtt = -1.0;
    while (fgets(line, sizeof(line), fp)) {
        char *pos = strstr(line, "time=");
        if (pos) {
            double value = 0.0;
            if (sscanf(pos, "time=%lf", &value) == 1) {
                rtt = value;
                break;
            }
        }
    }
    pclose(fp);
    return rtt;
}

static int sense_init(const char *iface, int dummy_metrics) {
    (void)iface;
    (void)dummy_metrics;
    g_prev_rx = 0;
    g_prev_tx = 0;
    g_prev_rtt = 10.0;
    g_prev_cpu_total = 0;
    g_prev_cpu_idle = 0;
    return 0;
}

static int sense_sample(const char *iface, const char *probe_host, double interval_s, int dummy_metrics, metrics_t *out) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    unsigned long long rx = 0;
    unsigned long long tx = 0;
    if (read_netdev(iface, &rx, &tx) != 0) {
        log_msg(LOG_WARN, "sense", "netdev read failed for %s: %s", iface, strerror(errno));
    } else {
        if (g_prev_rx != 0 || g_prev_tx != 0) {
            out->rx_bps = ((double)(rx - g_prev_rx) * 8.0) / interval_s;
            out->tx_bps = ((double)(tx - g_prev_tx) * 8.0) / interval_s;
        }
        g_prev_rx = rx;
        g_prev_tx = tx;
    }

    if (dummy_metrics) {
        out->rtt_ms = dummy_rtt();
    } else {
        double rtt = probe_rtt_ms(iface, probe_host ? probe_host : "1.1.1.1");
        if (rtt < 0.0) {
            log_msg(LOG_WARN, "sense", "icmp probe failed, using fallback");
            out->rtt_ms = dummy_rtt();
        } else {
            out->rtt_ms = rtt;
        }
    }

    out->jitter_ms = fabs(out->rtt_ms - g_prev_rtt);
    g_prev_rtt = out->rtt_ms;

    out->cpu_pct = read_cpu_pct();

    return 0;
}

static int sense_get_idle_baseline(const char *iface,
                                  const char *probe_host,
                                  int samples,
                                  double interval_s,
                                  int dummy_metrics,
                                  metrics_t *baseline) {
    if (!baseline || samples <= 0) {
        return -1;
    }

    memset(baseline, 0, sizeof(*baseline));
    metrics_t m = {0};
    for (int i = 0; i < samples; i++) {
        if (sense_sample(iface, probe_host, interval_s, dummy_metrics, &m) == 0) {
            baseline->rtt_ms += m.rtt_ms;
            baseline->jitter_ms += m.jitter_ms;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)interval_s;
        ts.tv_nsec = (long)((interval_s - ts.tv_sec) * 1000000000.0);
        nanosleep(&ts, NULL);
    }
    baseline->rtt_ms /= (double)samples;
    baseline->jitter_ms /= (double)samples;
    return 0;
}

static persona_t decide_persona(const metrics_t *metrics) {
    if (!metrics) {
        return PERSONA_UNKNOWN;
    }
    if (metrics->rtt_ms > 40.0 || metrics->jitter_ms > 15.0) {
        return PERSONA_INTERACTIVE;
    }
    if (metrics->tx_bps > metrics->rx_bps * 1.5) {
        return PERSONA_BULK;
    }
    return PERSONA_UNKNOWN;
}

static void persona_init(persona_state_t *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->current = PERSONA_UNKNOWN;
    state->history_len = 0;
}

static const char *persona_name(persona_t persona) {
    switch (persona) {
        case PERSONA_INTERACTIVE: return "interactive";
        case PERSONA_BULK: return "bulk";
        default: return "unknown";
    }
}

static persona_t persona_update(persona_state_t *state, const metrics_t *metrics) {
    if (!state || !metrics) {
        return PERSONA_UNKNOWN;
    }

    persona_t candidate = decide_persona(metrics);
    if (state->history_len < (int)(sizeof(state->history) / sizeof(state->history[0]))) {
        state->history[state->history_len++] = candidate;
    } else {
        memmove(&state->history[0], &state->history[1], sizeof(state->history) - sizeof(state->history[0]));
        state->history[state->history_len - 1] = candidate;
    }

    int count_interactive = 0;
    int count_bulk = 0;
    for (int i = 0; i < state->history_len; i++) {
        if (state->history[i] == PERSONA_INTERACTIVE) {
            count_interactive++;
        } else if (state->history[i] == PERSONA_BULK) {
            count_bulk++;
        }
    }

    persona_t next = state->current;
    if (count_interactive >= 3) {
        next = PERSONA_INTERACTIVE;
    } else if (count_bulk >= 3) {
        next = PERSONA_BULK;
    } else if (state->history_len >= 5 && count_interactive == 0 && count_bulk == 0) {
        next = PERSONA_UNKNOWN;
    }

    if (next != state->current) {
        log_msg(LOG_INFO, "persona", "persona changed: %s -> %s", persona_name(state->current), persona_name(next));
        state->current = next;
    }

    return state->current;
}

static int is_outlier(const metrics_t *metrics, const metrics_t *baseline, const myco_config_t *cfg) {
    if (!metrics || !baseline || !cfg) {
        return 0;
    }
    if (metrics->cpu_pct > cfg->max_cpu_pct) {
        return 1;
    }
    if (metrics->rtt_ms > baseline->rtt_ms * 5.0 && baseline->rtt_ms > 0.1) {
        return 1;
    }
    if (metrics->jitter_ms > baseline->jitter_ms * 5.0 && baseline->jitter_ms > 0.1) {
        return 1;
    }
    return 0;
}

static void control_init(control_state_t *state, int initial_bw) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->current.bandwidth_kbit = initial_bw;
    state->last_stable = state->current;
    state->safe_mode = 0;
    state->stable_cycles = 0;
}

static int control_decide(control_state_t *state,
                          const myco_config_t *cfg,
                          const metrics_t *metrics,
                          const metrics_t *baseline,
                          persona_t persona,
                          policy_t *desired,
                          char *reason,
                          size_t reason_len) {
    if (!state || !cfg || !metrics || !baseline || !desired || !reason) {
        return 0;
    }

    *desired = state->current;
    snprintf(reason, reason_len, "no-change");

    if (is_outlier(metrics, baseline, cfg)) {
        state->safe_mode = 1;
        *desired = state->last_stable;
        snprintf(reason, reason_len, "safe-mode: outlier");
        return (state->current.bandwidth_kbit != desired->bandwidth_kbit);
    }

    double rtt_delta = metrics->rtt_ms - baseline->rtt_ms;
    double jitter_delta = metrics->jitter_ms - baseline->jitter_ms;
    int congested = (rtt_delta > 20.0) || (jitter_delta > 10.0);

    if (congested && persona == PERSONA_BULK) {
        desired->bandwidth_kbit -= cfg->bandwidth_step_kbit;
        desired->boosted = 0;
        snprintf(reason, reason_len, "bulk-congested: throttle");
    } else if (!congested && persona == PERSONA_INTERACTIVE) {
        desired->bandwidth_kbit += cfg->bandwidth_step_kbit;
        desired->boosted = 1;
        snprintf(reason, reason_len, "interactive-clear: boost");
    } else if (congested && persona == PERSONA_INTERACTIVE) {
        desired->bandwidth_kbit -= cfg->bandwidth_step_kbit / 2;
        desired->boosted = 0;
        snprintf(reason, reason_len, "interactive-congested: soften");
    }

    desired->bandwidth_kbit = (int)clamp_double((double)desired->bandwidth_kbit,
                                                (double)cfg->min_bandwidth_kbit,
                                                (double)cfg->max_bandwidth_kbit);

    if (desired->bandwidth_kbit == state->current.bandwidth_kbit) {
        state->stable_cycles++;
        if (state->stable_cycles >= 3) {
            state->last_stable = state->current;
            state->stable_cycles = 0;
        }
        return 0;
    }

    state->stable_cycles = 0;
    return 1;
}

static void control_on_action_result(control_state_t *state, int success) {
    if (!state) {
        return;
    }
    if (!success) {
        log_msg(LOG_WARN, "control", "actuation failed, entering safe mode");
        state->safe_mode = 1;
        state->current = state->last_stable;
    }
}

static int act_apply_policy(const char *iface, const policy_t *policy, int no_tc, int force_fail) {
    if (!iface || !policy) {
        return 0;
    }

    if (force_fail) {
        log_msg(LOG_WARN, "act", "forced actuation failure");
        return 0;
    }

    if (no_tc) {
        log_msg(LOG_INFO, "act", "tc disabled, would set %s to %d kbit", iface, policy->bandwidth_kbit);
        return 1;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "tc qdisc replace dev %s root cake bandwidth %dkbit", iface, policy->bandwidth_kbit);
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_WARN, "act", "tc call failed (rc=%d)", rc);
        return 0;
    }

    log_msg(LOG_INFO, "act", "applied cake bandwidth %d kbit on %s", policy->bandwidth_kbit, iface);
    return 1;
}

static void dump_metrics(const myco_config_t *cfg,
                         const metrics_t *metrics,
                         persona_t persona,
                         const char *reason) {
    if (!cfg || !metrics || !reason) {
        return;
    }
    if (cfg->metric_file[0] == '\0') {
        return;
    }
    FILE *fp = fopen(cfg->metric_file, "a");
    if (!fp) {
        log_msg(LOG_WARN, "metrics", "metric file open failed: %s", cfg->metric_file);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(fp,
            "{\"ts\":%ld.%03ld,\"rtt_ms\":%.2f,\"jitter_ms\":%.2f,\"tx_bps\":%.0f,\"rx_bps\":%.0f,\"cpu_pct\":%.1f,\"persona\":\"%s\",\"reason\":\"%s\"}\n",
            ts.tv_sec, ts.tv_nsec / 1000000L,
            metrics->rtt_ms, metrics->jitter_ms,
            metrics->tx_bps, metrics->rx_bps,
            metrics->cpu_pct, persona_name(persona), reason);
    fclose(fp);
}

static int ebpf_init(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled) {
        return 0;
    }
#ifdef HAVE_LIBBPF
    if (access(cfg->ebpf_obj, R_OK) != 0) {
        log_msg(LOG_WARN, "ebpf", "ebpf obj not found: %s", cfg->ebpf_obj);
        return -1;
    }

    struct bpf_object_open_opts opts = {0};
    g_bpf_obj = bpf_object__open_file(cfg->ebpf_obj, &opts);
    if (!g_bpf_obj) {
        log_msg(LOG_WARN, "ebpf", "failed to open bpf obj: %s", cfg->ebpf_obj);
        return -1;
    }

    if (bpf_object__load(g_bpf_obj) != 0) {
        log_msg(LOG_WARN, "ebpf", "failed to load bpf obj: %s", cfg->ebpf_obj);
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
        return -1;
    }

    log_msg(LOG_INFO, "ebpf", "bpf object loaded (no attach yet): %s", cfg->ebpf_obj);
    return 0;
#else
    if (cfg->ebpf_attach) {
        log_msg(LOG_INFO, "ebpf", "libbpf not available, using tc attach only");
        return 0;
    }
    log_msg(LOG_WARN, "ebpf", "ebpf enabled but libbpf not available");
    return -1;
#endif
}

static int ebpf_attach_tc(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled || !cfg->ebpf_attach) {
        return 0;
    }
    if (g_ebpf_attached) {
        return 0;
    }

    if (access(cfg->ebpf_obj, R_OK) != 0) {
        log_msg(LOG_WARN, "ebpf", "ebpf obj not found: %s", cfg->ebpf_obj);
        return -1;
    }

    const char *dir = cfg->ebpf_tc_dir[0] ? cfg->ebpf_tc_dir : "ingress";
    strncpy(g_ebpf_iface, cfg->egress_iface, sizeof(g_ebpf_iface) - 1);
    g_ebpf_iface[sizeof(g_ebpf_iface) - 1] = '\0';
    strncpy(g_ebpf_dir, dir, sizeof(g_ebpf_dir) - 1);
    g_ebpf_dir[sizeof(g_ebpf_dir) - 1] = '\0';
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null", cfg->egress_iface);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "tc filter replace dev %s %s bpf da obj %s sec tc",
             cfg->egress_iface, dir, cfg->ebpf_obj);
    int rc = system(cmd);
    if (rc != 0) {
        log_msg(LOG_WARN, "ebpf", "tc attach failed (rc=%d)", rc);
        return -1;
    }
    g_ebpf_attached = 1;
    log_msg(LOG_INFO, "ebpf", "tc attach ok (%s)", dir);
    return 0;
}

static void ebpf_tick(const myco_config_t *cfg) {
    if (!cfg || !cfg->ebpf_enabled || !cfg->ebpf_attach) {
        return;
    }
    if (!g_ebpf_attached) {
        ebpf_attach_tc(cfg);
    }
}

static void ebpf_shutdown(void) {
    if (g_ebpf_attached && g_ebpf_iface[0]) {
        char cmd[512];
        const char *dir = g_ebpf_dir[0] ? g_ebpf_dir : "ingress";
        snprintf(cmd, sizeof(cmd), "tc filter del dev %s %s 2>/dev/null", g_ebpf_iface, dir);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s clsact 2>/dev/null", g_ebpf_iface);
        system(cmd);
    }
#ifdef HAVE_LIBBPF
    if (g_bpf_obj) {
        bpf_object__close(g_bpf_obj);
        g_bpf_obj = NULL;
    }
#endif
    g_ebpf_attached = 0;
}

#ifdef HAVE_UBUS
enum {
    POLICY_BW,
    POLICY_STEP,
    __POLICY_MAX
};

enum {
    PERSONA_NAME,
    __PERSONA_MAX
};

static const struct blobmsg_policy policy_policy[__POLICY_MAX] = {
    [POLICY_BW] = { .name = "bandwidth_kbit", .type = BLOBMSG_TYPE_INT32 },
    [POLICY_STEP] = { .name = "step_kbit", .type = BLOBMSG_TYPE_INT32 }
};

static const struct blobmsg_policy persona_policy[__PERSONA_MAX] = {
    [PERSONA_NAME] = { .name = "persona", .type = BLOBMSG_TYPE_STRING }
};

static void ubus_fill_status(struct blob_buf *buf) {
    void *metrics = blobmsg_open_table(buf, "metrics");
    blobmsg_add_double(buf, "rtt_ms", g_last_metrics.rtt_ms);
    blobmsg_add_double(buf, "jitter_ms", g_last_metrics.jitter_ms);
    blobmsg_add_double(buf, "tx_bps", g_last_metrics.tx_bps);
    blobmsg_add_double(buf, "rx_bps", g_last_metrics.rx_bps);
    blobmsg_add_double(buf, "cpu_pct", g_last_metrics.cpu_pct);
    blobmsg_close_table(buf, metrics);

    void *baseline = blobmsg_open_table(buf, "baseline");
    blobmsg_add_double(buf, "rtt_ms", g_last_baseline.rtt_ms);
    blobmsg_add_double(buf, "jitter_ms", g_last_baseline.jitter_ms);
    blobmsg_close_table(buf, baseline);

    void *policy = blobmsg_open_table(buf, "policy");
    blobmsg_add_u32(buf, "bandwidth_kbit", g_last_policy.bandwidth_kbit);
    blobmsg_add_u32(buf, "boosted", g_last_policy.boosted);
    blobmsg_close_table(buf, policy);

    blobmsg_add_string(buf, "persona", persona_name(g_last_persona));
    blobmsg_add_string(buf, "reason", g_last_reason);
    blobmsg_add_u32(buf, "safe_mode", g_ubus_control ? g_ubus_control->safe_mode : 0);
    blobmsg_add_u32(buf, "persona_override", g_persona_override_active);
    if (g_persona_override_active) {
        blobmsg_add_string(buf, "persona_override_value", persona_name(g_persona_override));
    }
}

static int ubus_status(struct ubus_context *ctx,
                       struct ubus_object *obj,
                       struct ubus_request_data *req,
                       const char *method,
                       struct blob_attr *msg) {
    (void)obj;
    (void)method;
    (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    ubus_fill_status(&buf);
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_policy_get(struct ubus_context *ctx,
                           struct ubus_object *obj,
                           struct ubus_request_data *req,
                           const char *method,
                           struct blob_attr *msg) {
    (void)obj;
    (void)method;
    (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    blobmsg_add_u32(&buf, "bandwidth_kbit", g_last_policy.bandwidth_kbit);
    blobmsg_add_u32(&buf, "boosted", g_last_policy.boosted);
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_policy_set(struct ubus_context *ctx,
                           struct ubus_object *obj,
                           struct ubus_request_data *req,
                           const char *method,
                           struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!tb[POLICY_BW] || !g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int bw = blobmsg_get_u32(tb[POLICY_BW]);
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);

    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 0;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-set");
    }
    return 0;
}

static int ubus_policy_boost(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int step = g_ubus_cfg->bandwidth_step_kbit;
    if (tb[POLICY_STEP]) {
        step = blobmsg_get_u32(tb[POLICY_STEP]);
    }
    int bw = g_ubus_control->current.bandwidth_kbit + step;
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);
    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 1;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-boost");
    }
    return 0;
}

static int ubus_policy_throttle(struct ubus_context *ctx,
                                struct ubus_object *obj,
                                struct ubus_request_data *req,
                                const char *method,
                                struct blob_attr *msg) {
    (void)obj;
    (void)method;
    struct blob_attr *tb[__POLICY_MAX];
    blobmsg_parse(policy_policy, __POLICY_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!g_ubus_cfg || !g_ubus_control) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    int step = g_ubus_cfg->bandwidth_step_kbit;
    if (tb[POLICY_STEP]) {
        step = blobmsg_get_u32(tb[POLICY_STEP]);
    }
    int bw = g_ubus_control->current.bandwidth_kbit - step;
    bw = (int)clamp_double((double)bw,
                           (double)g_ubus_cfg->min_bandwidth_kbit,
                           (double)g_ubus_cfg->max_bandwidth_kbit);
    policy_t desired = g_ubus_control->current;
    desired.bandwidth_kbit = bw;
    desired.boosted = 0;
    int ok = act_apply_policy(g_ubus_cfg->egress_iface, &desired, g_ubus_cfg->no_tc, g_ubus_cfg->force_act_fail);
    control_on_action_result(g_ubus_control, ok);
    if (ok) {
        g_ubus_control->current = desired;
        g_last_policy = desired;
        snprintf(g_last_reason, sizeof(g_last_reason), "ubus-throttle");
    }
    return 0;
}

static int ubus_persona_list(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg) {
    (void)obj;
    (void)method;
    (void)msg;
    struct blob_buf buf;
    blob_buf_init(&buf, 0);
    blobmsg_add_string(&buf, "current", persona_name(g_last_persona));
    blobmsg_add_u32(&buf, "override_active", g_persona_override_active);
    if (g_persona_override_active) {
        blobmsg_add_string(&buf, "override", persona_name(g_persona_override));
    }
    ubus_send_reply(ctx, req, buf.head);
    blob_buf_free(&buf);
    return 0;
}

static int ubus_persona_add(struct ubus_context *ctx,
                            struct ubus_object *obj,
                            struct ubus_request_data *req,
                            const char *method,
                            struct blob_attr *msg) {
    (void)obj;
    (void)method;
    if (!msg) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    struct blob_attr *tb[__PERSONA_MAX];
    blobmsg_parse(persona_policy, __PERSONA_MAX, tb, blobmsg_data(msg), blobmsg_len(msg));
    if (!tb[PERSONA_NAME]) {
        return UBUS_STATUS_INVALID_ARGUMENT;
    }
    const char *val = blobmsg_get_string(tb[PERSONA_NAME]);
    if (strcmp(val, "interactive") == 0) {
        g_persona_override = PERSONA_INTERACTIVE;
    } else if (strcmp(val, "bulk") == 0) {
        g_persona_override = PERSONA_BULK;
    } else {
        g_persona_override = PERSONA_UNKNOWN;
    }
    g_persona_override_active = 1;
    return 0;
}

static int ubus_persona_delete(struct ubus_context *ctx,
                               struct ubus_object *obj,
                               struct ubus_request_data *req,
                               const char *method,
                               struct blob_attr *msg) {
    (void)obj;
    (void)method;
    (void)msg;
    g_persona_override_active = 0;
    g_persona_override = PERSONA_UNKNOWN;
    return 0;
}

static const struct ubus_method myco_methods[] = {
    UBUS_METHOD_NOARG("status", ubus_status),
    UBUS_METHOD_NOARG("policy_get", ubus_policy_get),
    UBUS_METHOD("policy_set", ubus_policy_set, policy_policy),
    UBUS_METHOD("policy_boost", ubus_policy_boost, policy_policy),
    UBUS_METHOD("policy_throttle", ubus_policy_throttle, policy_policy),
    UBUS_METHOD_NOARG("persona_list", ubus_persona_list),
    UBUS_METHOD("persona_add", ubus_persona_add, policy_policy),
    UBUS_METHOD_NOARG("persona_delete", ubus_persona_delete)
};

static struct ubus_object_type myco_obj_type =
    UBUS_OBJECT_TYPE("mycoflow", myco_methods);

static void *ubus_loop(void *arg) {
    (void)arg;
    uloop_init();
    g_ubus_ctx = ubus_connect(NULL);
    if (!g_ubus_ctx) {
        log_msg(LOG_WARN, "ubus", "ubus connect failed");
        uloop_done();
        return NULL;
    }
    ubus_add_uloop(g_ubus_ctx);
    g_ubus_obj.name = "myco";
    g_ubus_obj.type = &myco_obj_type;
    g_ubus_obj.methods = myco_methods;
    g_ubus_obj.n_methods = sizeof(myco_methods) / sizeof(myco_methods[0]);

    if (ubus_add_object(g_ubus_ctx, &g_ubus_obj) != 0) {
        log_msg(LOG_WARN, "ubus", "failed to add ubus object");
        ubus_free(g_ubus_ctx);
        g_ubus_ctx = NULL;
        uloop_done();
        return NULL;
    }

    g_ubus_running = 1;
    uloop_run();
    g_ubus_running = 0;

    if (g_ubus_ctx) {
        ubus_free(g_ubus_ctx);
        g_ubus_ctx = NULL;
    }
    uloop_done();
    return NULL;
}

static void ubus_start(myco_config_t *cfg, control_state_t *control) {
    g_ubus_cfg = cfg;
    g_ubus_control = control;
    if (pthread_create(&g_ubus_thread, NULL, ubus_loop, NULL) == 0) {
        g_ubus_thread_started = 1;
        log_msg(LOG_INFO, "ubus", "ubus thread started");
    } else {
        log_msg(LOG_WARN, "ubus", "ubus thread start failed");
    }
}

static void ubus_stop(void) {
    if (g_ubus_running) {
        uloop_end();
    }
    if (g_ubus_thread_started) {
        pthread_join(g_ubus_thread, NULL);
    }
    g_ubus_thread_started = 0;
}
#endif

static void handle_signal(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_stop = 1;
    } else if (signo == SIGHUP) {
        g_reload = 1;
    }
}

static void sleep_interval(double seconds) {
    if (seconds <= 0.0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000.0);
    nanosleep(&ts, NULL);
}

int main(void) {
    struct utsname buffer;
    myco_config_t cfg;
    metrics_t baseline;
    metrics_t metrics;
    persona_state_t persona_state;
    control_state_t control_state;

    if (config_load(&cfg) != 0) {
        fprintf(stderr, "MycoFlow config load failed\n");
        return 1;
    }

    log_init(cfg.log_level);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);

    log_msg(LOG_INFO, "main", "MycoFlow daemon starting");
    if (uname(&buffer) == 0) {
        log_msg(LOG_INFO, "main", "system: %s %s", buffer.sysname, buffer.machine);
    }

    sense_init(cfg.egress_iface, cfg.dummy_metrics);
    persona_init(&persona_state);
    control_init(&control_state, cfg.bandwidth_kbit);
    g_last_policy = control_state.current;
    snprintf(g_last_reason, sizeof(g_last_reason), "startup");

    ebpf_init(&cfg);

#ifdef HAVE_UBUS
    ubus_start(&cfg, &control_state);
#endif

    double interval_s = 1.0 / cfg.sample_hz;
    log_msg(LOG_INFO, "main", "baseline capture: %d samples", cfg.baseline_samples);
    sense_get_idle_baseline(cfg.egress_iface, cfg.probe_host, cfg.baseline_samples, interval_s, cfg.dummy_metrics, &baseline);
    log_msg(LOG_INFO, "main", "baseline rtt=%.2fms jitter=%.2fms", baseline.rtt_ms, baseline.jitter_ms);

    double last_action_ts = 0.0;
    double min_action_interval = cfg.action_cooldown_s;
    if (cfg.action_rate_limit > 0.0) {
        double rate_interval = 1.0 / cfg.action_rate_limit;
        if (rate_interval > min_action_interval) {
            min_action_interval = rate_interval;
        }
    }

    while (!g_stop) {
        if (g_reload) {
            g_reload = 0;
            if (config_reload(&cfg) == 0) {
                log_set_level(cfg.log_level);
                interval_s = 1.0 / cfg.sample_hz;
                min_action_interval = cfg.action_cooldown_s;
                if (cfg.action_rate_limit > 0.0) {
                    double rate_interval = 1.0 / cfg.action_rate_limit;
                    if (rate_interval > min_action_interval) {
                        min_action_interval = rate_interval;
                    }
                }
                log_msg(LOG_INFO, "main", "baseline capture: %d samples", cfg.baseline_samples);
                sense_get_idle_baseline(cfg.egress_iface, cfg.probe_host, cfg.baseline_samples, interval_s, cfg.dummy_metrics, &baseline);
                log_msg(LOG_INFO, "main", "config reloaded");
            }
        }

        if (!cfg.enabled) {
            log_msg(LOG_INFO, "main", "disabled, sleeping");
            sleep_interval(interval_s);
            continue;
        }

        if (sense_sample(cfg.egress_iface, cfg.probe_host, interval_s, cfg.dummy_metrics, &metrics) != 0) {
            log_msg(LOG_WARN, "main", "sense sample failed");
        }

        ebpf_tick(&cfg);

        persona_t persona = persona_update(&persona_state, &metrics);
        if (g_persona_override_active) {
            persona = g_persona_override;
        }
        policy_t desired;
        char reason[128];
        int change = control_decide(&control_state, &cfg, &metrics, &baseline, persona, &desired, reason, sizeof(reason));

        g_last_metrics = metrics;
        g_last_baseline = baseline;
        g_last_persona = persona;
        g_last_policy = control_state.current;
        strncpy(g_last_reason, reason, sizeof(g_last_reason) - 1);
        g_last_reason[sizeof(g_last_reason) - 1] = '\0';

        log_msg(LOG_INFO, "loop",
                "rtt=%.2fms jitter=%.2fms tx=%.0fbps rx=%.0fbps cpu=%.1f%% persona=%s bw=%dkbit reason=%s",
                metrics.rtt_ms, metrics.jitter_ms, metrics.tx_bps, metrics.rx_bps, metrics.cpu_pct,
                persona_name(persona), control_state.current.bandwidth_kbit, reason);

        dump_metrics(&cfg, &metrics, persona, reason);

        if (control_state.safe_mode) {
            log_msg(LOG_WARN, "loop", "safe-mode active, skipping actuation");
        } else if (change) {
            double now = now_monotonic_s();
            if ((now - last_action_ts) >= min_action_interval) {
                int ok = act_apply_policy(cfg.egress_iface, &desired, cfg.no_tc, cfg.force_act_fail);
                control_on_action_result(&control_state, ok);
                if (ok) {
                    control_state.current = desired;
                    last_action_ts = now;
                }
            } else {
                log_msg(LOG_DEBUG, "loop", "action skipped (cooldown)");
            }
        }

        sleep_interval(interval_s);
    }

    log_msg(LOG_INFO, "main", "shutdown complete");
#ifdef HAVE_UBUS
    ubus_stop();
#endif
    ebpf_shutdown();
    return 0;
}
