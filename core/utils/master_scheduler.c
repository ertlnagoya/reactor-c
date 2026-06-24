#include "master_scheduler.h"

#include <stdio.h>
#include <stdlib.h>   // atexit
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdarg.h>

#include <sys/syscall.h>
#include <sys/types.h>

// ---------------- Internal state ----------------

static pthread_mutex_t _ms_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE* _ms_log = NULL;
static bool _ms_enabled = true;
static ms_log_level_t _ms_level = MS_LEVEL_INFO;
static bool _ms_observe_only = false;
static bool _ms_minimal_log = false;
static int _ms_partition_enabled = 0;
static int _ms_partition_hc_workers = 0;

typedef enum {
  MS_CRIT_HIGH = 0,
  MS_CRIT_LOW = 1
} ms_criticality_t;

typedef enum {
  MS_DEGRADE_DEFER = 0,
  MS_DEGRADE_SKIP = 1
} ms_degrade_action_t;

typedef enum {
  MS_BUDGET_REACTION_COUNT = 0,
  MS_BUDGET_CPU_TIME = 1
} ms_budget_type_t;

typedef struct {
  ms_degrade_action_t degrade_action;
  ms_budget_type_t budget_type;
  int64_t budget_window_ns;
  int64_t default_budget;
} ms_policy_config_t;

typedef struct {
  bool enabled;
  bool rt_enabled;
  bool affinity_enabled;
  bool allow_mixed_workers;
  int64_t lag_threshold_ns;
  int ready_q_len_threshold;
  int lc_base_nice_delta;
  int nice_delta_low;
  int hc_nice_delta;
  int rt_priority_hc;
  int rt_priority_lc;
  bool rt_group_enable;
  int64_t min_switch_interval_ns;
  bool hc_guard_enabled;
  int64_t hc_guard_lag_ns;
  int hc_guard_ready_q_len;
} ms_os_config_t;

typedef struct {
  int env_id;
  uint64_t reaction_index;
  ms_criticality_t criticality;
  bool degradable;
  int64_t budget;
  int64_t window_start_mono_ns;
  int64_t used_in_window;
} ms_reaction_policy_t;

static ms_policy_config_t _ms_policy = {
  .degrade_action = MS_DEGRADE_DEFER,
  .budget_type = MS_BUDGET_REACTION_COUNT,
  .budget_window_ns = 1000000000LL,
  .default_budget = -1
};

static ms_os_config_t _ms_os_config = {
  .enabled = false,
  .rt_enabled = false,
  .affinity_enabled = false,
  .allow_mixed_workers = false,
  .lag_threshold_ns = -1,
  .ready_q_len_threshold = -1,
  .lc_base_nice_delta = 0,
  .nice_delta_low = 5,
  .hc_nice_delta = 0,
  .rt_priority_hc = 10,
  .rt_priority_lc = 2,
  .rt_group_enable = false,
  .min_switch_interval_ns = 5LL * 1000LL * 1000LL,
  .hc_guard_enabled = false,
  .hc_guard_lag_ns = -1,
  .hc_guard_ready_q_len = -1
};

#define MS_MAX_REACTION_POLICIES 2048
static ms_reaction_policy_t _ms_reaction_policies[MS_MAX_REACTION_POLICIES];
static int _ms_reaction_policy_count = 0;
static bool _ms_config_loaded = false;

// Ensure shutdown runs even if the LF program calls exit(0).
static bool _ms_atexit_registered = false;
// Prevent double shutdown (e.g., runtime hook + atexit).
static bool _ms_shutdown_called = false;

// Rate limit for ms_report() (default: 100ms)
static int64_t _ms_report_min_interval_ns = 100LL * 1000LL * 1000LL;

#define MS_MAX_WORKERS 256
static int64_t _ms_last_report_mono_ns[MS_MAX_WORKERS] = {0};
static ms_criticality_t _ms_worker_last_crit[MS_MAX_WORKERS];
static int _ms_worker_crit_known[MS_MAX_WORKERS] = {0};
static ms_os_policy_t _ms_worker_pending_policy[MS_MAX_WORKERS];
static int _ms_worker_policy_pending[MS_MAX_WORKERS] = {0};
static int _ms_worker_last_nice_delta[MS_MAX_WORKERS] = {0};
static int _ms_worker_last_sched_policy[MS_MAX_WORKERS] = {0}; // ms_sched_policy_t
static int _ms_worker_last_rt_priority[MS_MAX_WORKERS] = {0};
static int64_t _ms_worker_last_nice_switch_ns[MS_MAX_WORKERS] = {0};
static uint8_t _ms_worker_crit_mask[MS_MAX_WORKERS] = {0}; // bit0: high, bit1: low

#define MS_MAX_ENVS 32
#define MS_MAX_READY_PER_ENV 1024

static void _ms_logf(ms_log_level_t lvl, const char* fmt, ...);

typedef enum {
  MS_READY = 0,
  MS_RUNNING = 1,
  MS_DEFERRED = 2
} ms_ready_state_t;

typedef struct {
  uint64_t reaction_index;
  int64_t deadline_ns;
  int64_t ready_time_ns;
  int is_input;
  ms_ready_state_t state;
} ms_ready_entry_t;

typedef struct {
  ms_ready_entry_t entries[MS_MAX_READY_PER_ENV];
  int count;
  long long last_pick_reaction_index;
} ms_ready_set_t;

static ms_ready_set_t _ms_ready_sets[MS_MAX_ENVS];
static int _ms_env_pressure[MS_MAX_ENVS] = {0};
static int _ms_env_degrade_pressure[MS_MAX_ENVS] = {0};
static bool _ms_degrade_enabled = false;
static int64_t _ms_degrade_lag_threshold_ns = -1;
static int _ms_degrade_ready_q_len_threshold = -1;
// When set, HC reactions are always picked before LC (criticality-monotonic),
// not only during transient overload pressure. Opt-in via LF_MS_HC_STRICT_PRIORITY.
static bool _ms_hc_strict_priority = false;
// Diagnostic: when LF_MS_DEGRADE_DEBUG=1, ms_should_skip_reaction logs the gating
// state (pressure / has_hc_ready / criticality) for every evaluation, to root-
// cause why degradation does/doesn't fire. Off by default (no normal-run impact).
static bool _ms_degrade_debug = false;
// Env-level LC budget accounting for graceful (partial) shedding. Active when
// _ms_policy.default_budget >= 0: at most default_budget LC reactions are kept
// PER LOGICAL TAG while degrade pressure is active; the rest are shed. The first
// array holds the current tag's logical time, the second the count used in it.
static int64_t _ms_env_lc_window_start_ns[MS_MAX_ENVS] = {0};
static int64_t _ms_env_lc_used_in_window[MS_MAX_ENVS] = {0};

static int64_t _ms_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static const char* _ms_criticality_str(ms_criticality_t crit) {
  switch (crit) {
    case MS_CRIT_HIGH: return "high";
    case MS_CRIT_LOW:  return "low";
    default:           return "unknown";
  }
}

static const char* _ms_degrade_str(ms_degrade_action_t action) {
  switch (action) {
    case MS_DEGRADE_DEFER: return "defer";
    case MS_DEGRADE_SKIP:  return "skip";
    default:               return "unknown";
  }
}

static const char* _ms_sched_policy_str(ms_sched_policy_t policy) {
  switch (policy) {
    case MS_SCHED_KEEP:  return "keep";
    case MS_SCHED_OTHER: return "other";
    case MS_SCHED_FIFO:  return "fifo";
    case MS_SCHED_RR:    return "rr";
    default:             return "unknown";
  }
}

static char* _ms_trim(char* s) {
  if (s == NULL) return NULL;
  while (*s && isspace((unsigned char)*s)) s++;
  if (*s == '\0') return s;
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)*(end - 1))) end--;
  *end = '\0';
  return s;
}

static ms_reaction_policy_t* _ms_find_policy(int env_id, uint64_t reaction_index) {
  for (int i = 0; i < _ms_reaction_policy_count; i++) {
    ms_reaction_policy_t* p = &_ms_reaction_policies[i];
    if (p->env_id == env_id && p->reaction_index == reaction_index) {
      return p;
    }
  }
  return NULL;
}

static ms_reaction_policy_t* _ms_get_policy(int env_id, uint64_t reaction_index, bool create_if_missing) {
  ms_reaction_policy_t* pol = _ms_find_policy(env_id, reaction_index);
  if (pol != NULL || !create_if_missing) return pol;
  if (_ms_reaction_policy_count >= MS_MAX_REACTION_POLICIES) return NULL;

  pol = &_ms_reaction_policies[_ms_reaction_policy_count++];
  pol->env_id = env_id;
  pol->reaction_index = reaction_index;
  pol->criticality = MS_CRIT_HIGH;
  pol->degradable = false;
  pol->budget = _ms_policy.default_budget;
  pol->window_start_mono_ns = 0;
  pol->used_in_window = 0;
  return pol;
}

static ms_criticality_t _ms_parse_criticality(const char* s, int* ok) {
  if (s == NULL) {
    if (ok) *ok = 0;
    return MS_CRIT_HIGH;
  }
  if (strcasecmp(s, "high") == 0) {
    if (ok) *ok = 1;
    return MS_CRIT_HIGH;
  }
  if (strcasecmp(s, "low") == 0) {
    if (ok) *ok = 1;
    return MS_CRIT_LOW;
  }
  if (ok) *ok = 0;
  return MS_CRIT_HIGH;
}

static int _ms_parse_bool(const char* s, int* ok) {
  if (s == NULL) {
    if (ok) *ok = 0;
    return 0;
  }
  if (strcmp(s, "1") == 0 ||
      strcasecmp(s, "true") == 0 ||
      strcasecmp(s, "yes") == 0 ||
      strcasecmp(s, "on") == 0) {
    if (ok) *ok = 1;
    return 1;
  }
  if (strcmp(s, "0") == 0 ||
      strcasecmp(s, "false") == 0 ||
      strcasecmp(s, "no") == 0 ||
      strcasecmp(s, "off") == 0) {
    if (ok) *ok = 1;
    return 0;
  }
  if (ok) *ok = 0;
  return 0;
}

static void _ms_set_policy_kv(const char* key, const char* val) {
  if (key == NULL || val == NULL) return;

  if (strcasecmp(key, "degrade_action") == 0) {
    if (strcasecmp(val, "defer") == 0) _ms_policy.degrade_action = MS_DEGRADE_DEFER;
    else if (strcasecmp(val, "skip") == 0) _ms_policy.degrade_action = MS_DEGRADE_SKIP;
    else _ms_logf(MS_LEVEL_WARN, "event=config_invalid key=%s value=%s", key, val);
    return;
  }

  if (strcasecmp(key, "budget_type") == 0) {
    if (strcasecmp(val, "reaction_count") == 0) _ms_policy.budget_type = MS_BUDGET_REACTION_COUNT;
    else if (strcasecmp(val, "cpu_time") == 0) _ms_policy.budget_type = MS_BUDGET_CPU_TIME;
    else _ms_logf(MS_LEVEL_WARN, "event=config_invalid key=%s value=%s", key, val);
    return;
  }

  if (strcasecmp(key, "budget_window_ns") == 0) {
    _ms_policy.budget_window_ns = (int64_t)strtoll(val, NULL, 10);
    return;
  }

  if (strcasecmp(key, "default_budget") == 0) {
    _ms_policy.default_budget = (int64_t)strtoll(val, NULL, 10);
    return;
  }

  _ms_logf(MS_LEVEL_WARN, "event=config_unknown_key key=%s", key);
}

static void _ms_parse_reaction_line(char* line) {
  // Expected format:
  // reaction,env_id,reaction_index,criticality,budget[,degradable]
  char* rest = line + strlen("reaction");
  while (*rest && (isspace((unsigned char)*rest) || *rest == ',')) rest++;
  if (*rest == '\0') {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_missing_fields line=%s", line);
    return;
  }

  char* tokens[5] = {0};
  int count = 0;
  char* tok = strtok(rest, ", \t");
  while (tok != NULL && count < 5) {
    tokens[count++] = tok;
    tok = strtok(NULL, ", \t");
  }

  if (count < 4) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_missing_fields line=%s", line);
    return;
  }

  int env_id = atoi(tokens[0]);
  uint64_t reaction_index = (uint64_t)strtoull(tokens[1], NULL, 10);
  int crit_ok = 0;
  ms_criticality_t crit = _ms_parse_criticality(tokens[2], &crit_ok);
  int64_t budget = (int64_t)strtoll(tokens[3], NULL, 10);
  int degradable_ok = 1;
  int degradable = 0;
  if (count >= 5) {
    degradable = _ms_parse_bool(tokens[4], &degradable_ok);
  }

  if (!crit_ok) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_bad_crit value=%s", tokens[2]);
    return;
  }
  if (!degradable_ok) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_bad_degradable value=%s", tokens[4]);
    return;
  }

  ms_reaction_policy_t* pol = _ms_get_policy(env_id, reaction_index, true);
  if (pol == NULL) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_overflow env=%d reaction_index=%llu",
             env_id, (unsigned long long)reaction_index);
    return;
  }
  pol->criticality = crit;
  pol->degradable = (degradable != 0);
  pol->budget = budget;

  _ms_logf(MS_LEVEL_INFO,
           "event=config_reaction env=%d reaction_index=%llu criticality=%s degradable=%d budget=%lld",
           env_id, (unsigned long long)reaction_index, _ms_criticality_str(crit), pol->degradable ? 1 : 0,
           (long long)budget);
}

static void _ms_load_config(const char* path) {
  if (_ms_config_loaded) return;
  _ms_config_loaded = true;

  if (path == NULL || path[0] == '\0') return;

  FILE* f = fopen(path, "r");
  if (f == NULL) {
    _ms_logf(MS_LEVEL_WARN, "event=config_open_failed path=%s", path);
    return;
  }

  char line[512];
  while (fgets(line, sizeof(line), f) != NULL) {
    char* t = _ms_trim(line);
    if (t[0] == '\0' || t[0] == '#') continue;

    char* eq = strchr(t, '=');
    if (eq != NULL) {
      *eq = '\0';
      _ms_set_policy_kv(_ms_trim(t), _ms_trim(eq + 1));
      continue;
    }

    if (strncasecmp(t, "reaction", strlen("reaction")) == 0) {
      _ms_parse_reaction_line(t);
      continue;
    }

    _ms_logf(MS_LEVEL_WARN, "event=config_unknown_line line=%s", t);
  }

  fclose(f);
  _ms_logf(MS_LEVEL_INFO,
           "event=config_loaded path=%s degrade_action=%s budget_type=%d window_ns=%lld default_budget=%lld",
           path, _ms_degrade_str(_ms_policy.degrade_action),
           (int)_ms_policy.budget_type, (long long)_ms_policy.budget_window_ns,
           (long long)_ms_policy.default_budget);
}

static ms_ready_set_t* _ms_get_ready_set(int env_id) {
  if (env_id < 0 || env_id >= MS_MAX_ENVS) return NULL;
  return &_ms_ready_sets[env_id];
}

static int _ms_ready_find(ms_ready_set_t* set, uint64_t reaction_index) {
  for (int i = 0; i < set->count; i++) {
    if (set->entries[i].reaction_index == reaction_index) return i;
  }
  return -1;
}

static void _ms_ready_remove_at(ms_ready_set_t* set, int index) {
  if (index < 0 || index >= set->count) return;
  set->count--;
  if (index != set->count) {
    set->entries[index] = set->entries[set->count];
  }
}

static const char* _ms_level_str(ms_log_level_t lvl) {
  switch (lvl) {
    case MS_LEVEL_ERROR: return "ERROR";
    case MS_LEVEL_WARN:  return "WARN";
    case MS_LEVEL_INFO:  return "INFO";
    case MS_LEVEL_DEBUG: return "DEBUG";
    default:             return "UNKNOWN";
  }
}

static int _ms_is_true(const char* s) {
  if (s == NULL) return 0;
  return (strcmp(s, "1") == 0 ||
          strcasecmp(s, "true") == 0 ||
          strcasecmp(s, "yes") == 0 ||
          strcasecmp(s, "on") == 0);
}

static ms_criticality_t _ms_worker_target_crit(int worker_id) {
  if (!_ms_partition_enabled || worker_id < 0) {
    return MS_CRIT_HIGH;
  }
  if (worker_id < _ms_partition_hc_workers) {
    return MS_CRIT_HIGH;
  }
  return MS_CRIT_LOW;
}

static void _ms_os_load_env(void) {
  static int loaded = 0;
  if (loaded) return;
  loaded = 1;

  _ms_os_config.enabled = _ms_is_true(getenv("LF_MS_OS_ENABLE"));
  _ms_os_config.rt_enabled = _ms_is_true(getenv("LF_MS_OS_RT_ENABLE"));
  _ms_os_config.affinity_enabled = _ms_is_true(getenv("LF_MS_OS_AFFINITY_ENABLE"));
  _ms_os_config.allow_mixed_workers = _ms_is_true(getenv("LF_MS_OS_ALLOW_MIXED"));

  const char* lag = getenv("LF_MS_OS_LAG_NS");
  if (lag != NULL && lag[0] != '\0') {
    _ms_os_config.lag_threshold_ns = (int64_t)strtoll(lag, NULL, 10);
  }

  const char* rq = getenv("LF_MS_OS_READY_Q_LEN");
  if (rq != NULL && rq[0] != '\0') {
    _ms_os_config.ready_q_len_threshold = atoi(rq);
  }

  const char* nd = getenv("LF_MS_OS_NICE_DELTA");
  if (nd != NULL && nd[0] != '\0') {
    _ms_os_config.nice_delta_low = atoi(nd);
  }

  const char* lbd = getenv("LF_MS_OS_LC_BASE_NICE_DELTA");
  if (lbd != NULL && lbd[0] != '\0') {
    _ms_os_config.lc_base_nice_delta = atoi(lbd);
  }

  const char* hnd = getenv("LF_MS_OS_HC_NICE_DELTA");
  if (hnd != NULL && hnd[0] != '\0') {
    _ms_os_config.hc_nice_delta = atoi(hnd);
  }

  const char* msw = getenv("LF_MS_OS_MIN_SWITCH_NS");
  if (msw != NULL && msw[0] != '\0') {
    _ms_os_config.min_switch_interval_ns = (int64_t)strtoll(msw, NULL, 10);
  }
  const char* rtp = getenv("LF_MS_OS_RT_PRIO_HC");
  if (rtp != NULL && rtp[0] != '\0') {
    _ms_os_config.rt_priority_hc = atoi(rtp);
    if (_ms_os_config.rt_priority_hc < 1) {
      _ms_os_config.rt_priority_hc = 1;
    }
  }
  const char* rtpl = getenv("LF_MS_OS_RT_PRIO_LC");
  if (rtpl != NULL && rtpl[0] != '\0') {
    _ms_os_config.rt_priority_lc = atoi(rtpl);
    if (_ms_os_config.rt_priority_lc < 1) {
      _ms_os_config.rt_priority_lc = 1;
    }
  }
  _ms_os_config.rt_group_enable = _ms_is_true(getenv("LF_MS_OS_RT_GROUP_ENABLE"));

  _ms_os_config.hc_guard_enabled = _ms_is_true(getenv("LF_MS_HC_GUARD_ENABLE"));
  const char* hgl = getenv("LF_MS_HC_GUARD_LAG_NS");
  if (hgl != NULL && hgl[0] != '\0') {
    _ms_os_config.hc_guard_lag_ns = (int64_t)strtoll(hgl, NULL, 10);
  }
  const char* hgrq = getenv("LF_MS_HC_GUARD_READY_Q_LEN");
  if (hgrq != NULL && hgrq[0] != '\0') {
    _ms_os_config.hc_guard_ready_q_len = atoi(hgrq);
  }

  _ms_partition_enabled = _ms_is_true(getenv("LF_MS_WORKER_PARTITION_ENABLE"));
  _ms_minimal_log = _ms_is_true(getenv("LF_MS_MINIMAL_LOG"));
  _ms_degrade_enabled = _ms_is_true(getenv("LF_MS_DEGRADE_ENABLE"));
  const char* dlag = getenv("LF_MS_DEGRADE_LAG_NS");
  if (dlag != NULL && dlag[0] != '\0') {
    _ms_degrade_lag_threshold_ns = (int64_t)strtoll(dlag, NULL, 10);
  }
  const char* drq = getenv("LF_MS_DEGRADE_READY_Q_LEN");
  if (drq != NULL && drq[0] != '\0') {
    _ms_degrade_ready_q_len_threshold = atoi(drq);
  }
  _ms_hc_strict_priority = _ms_is_true(getenv("LF_MS_HC_STRICT_PRIORITY"));
  _ms_degrade_debug = _ms_is_true(getenv("LF_MS_DEGRADE_DEBUG"));
  _ms_partition_hc_workers = 0;
  const char* phc = getenv("LF_MS_HC_WORKERS");
  if (phc != NULL && phc[0] != '\0') {
    _ms_partition_hc_workers = atoi(phc);
    if (_ms_partition_hc_workers < 0) {
      _ms_partition_hc_workers = 0;
    }
  }

  _ms_logf(
      MS_LEVEL_INFO,
      "event=os_config enabled=%d rt_enabled=%d rt_group=%d lag_ns=%lld ready_q_len=%d lc_base_nice=%d nice_delta=%d hc_nice=%d rt_prio_hc=%d rt_prio_lc=%d min_switch_ns=%lld partition=%d hc_workers=%d allow_mixed=%d hc_guard=%d hc_guard_lag=%lld hc_guard_ready_q=%d",
      _ms_os_config.enabled ? 1 : 0,
      _ms_os_config.rt_enabled ? 1 : 0,
      _ms_os_config.rt_group_enable ? 1 : 0,
      (long long)_ms_os_config.lag_threshold_ns,
      _ms_os_config.ready_q_len_threshold,
      _ms_os_config.lc_base_nice_delta,
      _ms_os_config.nice_delta_low,
      _ms_os_config.hc_nice_delta,
      _ms_os_config.rt_priority_hc,
      _ms_os_config.rt_priority_lc,
      (long long)_ms_os_config.min_switch_interval_ns,
      _ms_partition_enabled,
      _ms_partition_hc_workers,
      _ms_os_config.allow_mixed_workers ? 1 : 0,
      _ms_os_config.hc_guard_enabled ? 1 : 0,
      (long long)_ms_os_config.hc_guard_lag_ns,
      _ms_os_config.hc_guard_ready_q_len
  );
  _ms_logf(
      MS_LEVEL_INFO,
      "event=degrade_config enabled=%d lag_ns=%lld ready_q_len=%d action=%s",
      _ms_degrade_enabled ? 1 : 0,
      (long long)_ms_degrade_lag_threshold_ns,
      _ms_degrade_ready_q_len_threshold,
      _ms_degrade_str(_ms_policy.degrade_action)
  );
}

static void _ms_logf(ms_log_level_t lvl, const char* fmt, ...) {
  if (!_ms_enabled) return;
  if (lvl > _ms_level) return;

  pthread_mutex_lock(&_ms_lock);
  FILE* out = _ms_log;
  if (out == NULL) {
    pthread_mutex_unlock(&_ms_lock);
    return; // silent no-op if not initialized
  }

  // Monotonic timestamp for stable ordering
  const int64_t mono = _ms_now_mono_ns();
  fprintf(out, "%lld,%s,", (long long)mono, _ms_level_str(lvl));

  va_list ap;
  va_start(ap, fmt);
  vfprintf(out, fmt, ap);
  va_end(ap);

  fputc('\n', out);
  fflush(out);
  pthread_mutex_unlock(&_ms_lock);
}

// atexit() handler must be a function with signature void(void).
static void _ms_shutdown_atexit(void) {
  ms_shutdown();
}

// ---------------- Public API ----------------

bool ms_init(const char* config_path) {
  // Phase 3: allow config_path override for policy configuration.

  const char* lv = getenv("LF_MS_LOG_LEVEL");
  if (lv != NULL) {
    if (strcasecmp(lv, "DEBUG") == 0) ms_set_log_level(MS_LEVEL_DEBUG);
    else if (strcasecmp(lv, "INFO") == 0) ms_set_log_level(MS_LEVEL_INFO);
    else if (strcasecmp(lv, "WARN") == 0) ms_set_log_level(MS_LEVEL_WARN);
    else if (strcasecmp(lv, "ERROR") == 0) ms_set_log_level(MS_LEVEL_ERROR);    
  }
  _ms_logf(MS_LEVEL_INFO, "event=ms_init level=%s", _ms_level_str(_ms_level));

  // Register shutdown at process exit so it runs even if generated code calls exit(0).
  pthread_mutex_lock(&_ms_lock);
  if (!_ms_atexit_registered) {
    atexit(_ms_shutdown_atexit);
    _ms_atexit_registered = true;
  }
  pthread_mutex_unlock(&_ms_lock);

  // Allow hard disable via env var for near-zero overhead.
  if (_ms_is_true(getenv("LF_MS_DISABLE"))) {
    pthread_mutex_lock(&_ms_lock);
    _ms_enabled = false;
    pthread_mutex_unlock(&_ms_lock);
    return true;
  }
  if (_ms_is_true(getenv("LF_MS_OBSERVE_ONLY"))) {
    pthread_mutex_lock(&_ms_lock);
    _ms_observe_only = true;
    pthread_mutex_unlock(&_ms_lock);
  }
  const char* report_iv = getenv("LF_MS_REPORT_MIN_INTERVAL_NS");
  if (report_iv != NULL && report_iv[0] != '\0') {
    long long v = atoll(report_iv);
    if (v >= 0) {
      _ms_report_min_interval_ns = v;
    }
  }

  const char* path = getenv("LF_MS_LOG");
  if (path == NULL || path[0] == '\0') {
    path = "/tmp/lf_master_scheduler_phase0.log";
  }

  pthread_mutex_lock(&_ms_lock);
  if (_ms_log != NULL) {
    pthread_mutex_unlock(&_ms_lock);
    return true; // already initialized
  }

  _ms_log = fopen(path, "a");
  if (_ms_log == NULL) {
    // Non-fatal: disable to avoid affecting runtime behavior.
    _ms_enabled = false;
    pthread_mutex_unlock(&_ms_lock);
    return true;
  }

  fprintf(_ms_log, "# phase3 master_scheduler started pid=%d\n", (int)getpid());
  fflush(_ms_log);

  for (int i = 0; i < MS_MAX_ENVS; i++) {
    _ms_ready_sets[i].count = 0;
    _ms_ready_sets[i].last_pick_reaction_index = -1;
  }

  pthread_mutex_unlock(&_ms_lock);

  const char* cfg_path = config_path;
  if (cfg_path == NULL || cfg_path[0] == '\0') {
    cfg_path = getenv("LF_MS_CONFIG");
  }
  _ms_load_config(cfg_path);
  _ms_os_load_env();

  return true;
}

void ms_shutdown(void) {
  pthread_mutex_lock(&_ms_lock);

  // Avoid double shutdown (runtime hook + atexit, etc.)
  if (_ms_shutdown_called) {
    pthread_mutex_unlock(&_ms_lock);
    return;
  }
  _ms_shutdown_called = true;

  if (_ms_log != NULL) {
    fprintf(_ms_log, "# phase3 master_scheduler shutdown pid=%d\n", (int)getpid());
    fflush(_ms_log);
    fclose(_ms_log);
    _ms_log = NULL;
  }

  pthread_mutex_unlock(&_ms_lock);
}

void ms_set_log_level(ms_log_level_t level) {
  pthread_mutex_lock(&_ms_lock);
  _ms_level = level;
  pthread_mutex_unlock(&_ms_lock);
}

void ms_set_enabled(bool enabled) {
  pthread_mutex_lock(&_ms_lock);
  _ms_enabled = enabled;
  pthread_mutex_unlock(&_ms_lock);
}

void ms_register_worker(const ms_worker_info_t* info) {
  if (info == NULL) return;

  // Phase 0: log only
  _ms_logf(MS_LEVEL_INFO,
           "event=register_worker worker_id=%d os_pid=%d os_tid=%d name=%s flags=0x%x",
           (int)info->worker_id,
           (int)info->os_pid,
           (int)info->os_tid,
           (info->name != NULL ? info->name : "(null)"),
           (unsigned)info->flags);
}

void ms_report(const ms_report_t* report) {
  if (report == NULL) return;
  if (_ms_minimal_log) return;

  // Rate limit to avoid perturbing runtime for each worker.
  const int64_t now = _ms_now_mono_ns();

  int wid = report->worker_id;
  if (wid < 0 || wid >= MS_MAX_WORKERS) {
    wid = 0; // fallback
  }

  if (_ms_report_min_interval_ns > 0 &&
      (now - _ms_last_report_mono_ns[wid]) < _ms_report_min_interval_ns) {
    return;
  }
  _ms_last_report_mono_ns[wid] = now;
  
  _ms_logf(MS_LEVEL_INFO,
           "event=report worker_id=%d reactor_id=%d reaction_id=%d logical=%lld physical=%lld lag=%lld ready_q=%d miss=%lld",
           (int)report->worker_id,
           (int)report->reactor_id,
           (int)report->reaction_id,
           (long long)report->logical_time_ns,
           (long long)report->physical_time_ns,
           (long long)report->lag_ns,
           (int)report->ready_q_len,
           (long long)report->deadline_misses);
}

int ms_gettid(void) {
#ifdef SYS_gettid
  return (int)syscall(SYS_gettid);
#else
  // fallback
  return (int)(uintptr_t)pthread_self();
#endif
}

void ms_on_reaction_ready(    
    int env_id,
    uint64_t reaction_index,
    long long logical_time_ns,
    long long deadline_ns,
    int is_input
) {
    if (!_ms_enabled) return;

    int updated = 0;
    int dropped = 0;
    int64_t ready_time = _ms_now_mono_ns();

    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set == NULL) {
        dropped = 1;
      } else {
        int idx = _ms_ready_find(set, reaction_index);
        if (idx >= 0) {
          ms_ready_entry_t* entry = &set->entries[idx];
          entry->deadline_ns = deadline_ns;
          entry->ready_time_ns = ready_time;
          entry->is_input = is_input;
          entry->state = MS_READY;
          updated = 1;
        } else if (set->count < MS_MAX_READY_PER_ENV) {
          ms_ready_entry_t* entry = &set->entries[set->count++];
          entry->reaction_index = reaction_index;
          entry->deadline_ns = deadline_ns;
          entry->ready_time_ns = ready_time;
          entry->is_input = is_input;
          entry->state = MS_READY;
        } else {
          dropped = 1;
        }

      }
    }
    pthread_mutex_unlock(&_ms_lock);

    if (dropped) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=ready_drop env=%d reaction_index=%llu logical=%lld deadline=%lld is_input=%d",
          env_id, (unsigned long long)reaction_index, logical_time_ns, deadline_ns, is_input
      );
      return;
    }

    if (!_ms_minimal_log) {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=ready env=%d reaction_index=%llu logical=%lld deadline=%lld is_input=%d updated=%d",
          env_id, (unsigned long long)reaction_index, logical_time_ns, deadline_ns, is_input, updated
      );
    }

}

long long ms_pick_next(
    int env_id,
    int worker_id,
    long long logical_time_ns
) {
    if (!_ms_enabled) return -1;
    if (_ms_observe_only) {
      if (!_ms_minimal_log) {
        _ms_logf(
            MS_LEVEL_INFO,
            "event=pick_next env=%d candidate=-1 reason=observe_only logical=%lld",
            env_id, logical_time_ns);
      }
      return -1;
    }

    uint64_t candidate = 0;
    int has_candidate = 0;
    int64_t candidate_deadline = 0;
    int64_t candidate_ready = 0;
    uint64_t fallback_candidate = 0;
    int has_fallback_candidate = 0;
    int64_t fallback_deadline = 0;
    int64_t fallback_ready = 0;
    uint64_t hc_candidate = 0;
    int has_hc_candidate = 0;
    int64_t hc_deadline = 0;
    int64_t hc_ready = 0;
    int force_hc_guard = 0;
    int degrade_pressure = 0;
    int ready_count = 0;
    char ready_buf[1024];
    ready_buf[0] = '\0';
    const ms_criticality_t target_crit = _ms_worker_target_crit(worker_id);
    const int partition_enabled = _ms_partition_enabled;

    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (_ms_os_config.hc_guard_enabled &&
          env_id >= 0 && env_id < MS_MAX_ENVS &&
          _ms_env_pressure[env_id]) {
        force_hc_guard = 1;
      }
      if (_ms_degrade_enabled &&
          env_id >= 0 && env_id < MS_MAX_ENVS &&
          _ms_env_degrade_pressure[env_id]) {
        degrade_pressure = 1;
      }
      if (set != NULL) {
        int i = 0;
        while (i < set->count) {
          ms_ready_entry_t* entry = &set->entries[i];
          if (entry->state == MS_RUNNING) {
            i++;
            continue;
          }
          ready_count++;
          size_t used = strlen(ready_buf);
          if (used < sizeof(ready_buf) - 32) {
            snprintf(ready_buf + used, sizeof(ready_buf) - used, "%s%llu",
                     used == 0 ? "" : "|",
                     (unsigned long long)entry->reaction_index);
          }

          ms_reaction_policy_t* pol = _ms_find_policy(env_id, entry->reaction_index);

          ms_criticality_t entry_crit = MS_CRIT_HIGH;
          if (pol != NULL) {
            entry_crit = pol->criticality;
          }

          if (!has_fallback_candidate ||
              entry->deadline_ns < fallback_deadline ||
              (entry->deadline_ns == fallback_deadline &&
               entry->ready_time_ns < fallback_ready)) {
            fallback_candidate = entry->reaction_index;
            fallback_deadline = entry->deadline_ns;
            fallback_ready = entry->ready_time_ns;
            has_fallback_candidate = 1;
          }

          if (entry_crit == MS_CRIT_HIGH) {
            if (!has_hc_candidate ||
                entry->deadline_ns < hc_deadline ||
                (entry->deadline_ns == hc_deadline &&
                 entry->ready_time_ns < hc_ready)) {
              hc_candidate = entry->reaction_index;
              hc_deadline = entry->deadline_ns;
              hc_ready = entry->ready_time_ns;
              has_hc_candidate = 1;
            }
          }

          if (!partition_enabled || entry_crit == target_crit) {
            if (!has_candidate ||
                entry->deadline_ns < candidate_deadline ||
                (entry->deadline_ns == candidate_deadline &&
                 entry->ready_time_ns < candidate_ready)) {
              candidate = entry->reaction_index;
              candidate_deadline = entry->deadline_ns;
              candidate_ready = entry->ready_time_ns;
              has_candidate = 1;
            }
          }
          i++;
        }
        if ((force_hc_guard || degrade_pressure || _ms_hc_strict_priority) && has_hc_candidate) {
          candidate = hc_candidate;
          candidate_deadline = hc_deadline;
          candidate_ready = hc_ready;
          has_candidate = 1;
        }
        if (!has_candidate && has_fallback_candidate) {
          candidate = fallback_candidate;
          candidate_deadline = fallback_deadline;
          candidate_ready = fallback_ready;
          has_candidate = 1;
        }
        set->last_pick_reaction_index = has_candidate ? (long long)candidate : -1;
      }
    }
    pthread_mutex_unlock(&_ms_lock);

    if (!_ms_minimal_log) {
      if (has_candidate) {
        _ms_logf(
            MS_LEVEL_DEBUG,
            "event=pick_next env=%d candidate=%llu reason=%s deadline=%lld ready_time=%lld logical=%lld ready_count=%d ready_set=%s",
            env_id, (unsigned long long)candidate, (long long)candidate_deadline,
            (force_hc_guard || degrade_pressure || _ms_hc_strict_priority) ? "hc_first" : "earliest_deadline",
            (long long)candidate_ready, logical_time_ns, ready_count,
            ready_buf[0] != '\0' ? ready_buf : "-"
        );
      } else {
        _ms_logf(
            MS_LEVEL_INFO,
            "event=fallback env=%d reason=no_candidate logical=%lld ready_count=%d ready_set=%s",
            env_id, logical_time_ns, ready_count,
            ready_buf[0] != '\0' ? ready_buf : "-"
        );
      }
    }

    // Phase 3: return explicit candidate when available to avoid runtime stall.
    if (has_candidate) {
      return (long long)candidate;
    }
    return -1;
}

void ms_on_reaction_start(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long physical_time_ns
) {
    if (!_ms_enabled) return;

    int found = 0;
    long long last_pick = -1;
    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set != NULL) {
        last_pick = set->last_pick_reaction_index;
        int idx = _ms_ready_find(set, reaction_index);
        if (idx >= 0) {
          set->entries[idx].state = MS_RUNNING;
          found = 1;
        }
      }

      ms_reaction_policy_t* pol = _ms_find_policy(env_id, reaction_index);
      if (pol == NULL && _ms_policy.default_budget >= 0) {
        pol = _ms_get_policy(env_id, reaction_index, true);
      }
      if (pol != NULL && worker_id >= 0 && worker_id < MS_MAX_WORKERS) {
        _ms_worker_last_crit[worker_id] = pol->criticality;
        _ms_worker_crit_known[worker_id] = 1;
        if (pol->criticality == MS_CRIT_HIGH) {
          _ms_worker_crit_mask[worker_id] |= 0x1;
        } else if (pol->criticality == MS_CRIT_LOW) {
          _ms_worker_crit_mask[worker_id] |= 0x2;
        }
      }
    }
    pthread_mutex_unlock(&_ms_lock);

    if (!_ms_minimal_log) {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=runtime_selected env=%d reaction_index=%llu physical=%lld ready_found=%d",
          env_id, (unsigned long long)reaction_index, physical_time_ns, found
      );

      if (!found) {
        _ms_logf(
            MS_LEVEL_WARN,
            "event=runtime_selected_missing env=%d reaction_index=%llu physical=%lld",
            env_id, (unsigned long long)reaction_index, physical_time_ns
        );
      }
    }

    if (last_pick >= 0 && (uint64_t)last_pick != reaction_index) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=mismatch env=%d picked=%lld runtime=%llu",
          env_id, last_pick, (unsigned long long)reaction_index
      );
    }

}

void ms_on_reaction_end(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long physical_time_ns,
    int status
) {
    (void)worker_id;
    (void)physical_time_ns;
    (void)status;

    if (!_ms_enabled) return;

    int removed = 0;
    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set != NULL) {
        int idx = _ms_ready_find(set, reaction_index);
        if (idx >= 0) {
          _ms_ready_remove_at(set, idx);
          removed = 1;
        }
      }

    }
    pthread_mutex_unlock(&_ms_lock);

    if (!removed && !_ms_minimal_log) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=ready_missing_on_end env=%d reaction_index=%llu",
          env_id, (unsigned long long)reaction_index
      );
    }
}

bool ms_should_skip_reaction(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long logical_time_ns
) {
  (void)worker_id;
  if (!_ms_enabled || !_ms_degrade_enabled) return false;
  if (_ms_policy.degrade_action != MS_DEGRADE_SKIP) return false;

  int should_skip = 0;
  int ready_count = 0;
  int missing_metadata = 0;
  char ready_buf[1024];
  ready_buf[0] = '\0';

  pthread_mutex_lock(&_ms_lock);
  ms_ready_set_t* set = _ms_get_ready_set(env_id);

  if (_ms_degrade_debug) {
    int dbg_pressure = (set != NULL && env_id >= 0 && env_id < MS_MAX_ENVS)
                           ? _ms_env_degrade_pressure[env_id] : -1;
    ms_reaction_policy_t* dbg_pol = _ms_find_policy(env_id, reaction_index);
    int dbg_crit = dbg_pol ? (int)dbg_pol->criticality : -1;
    int dbg_degr = dbg_pol ? (dbg_pol->degradable ? 1 : 0) : -1;
    int dbg_hcr = 0, dbg_rc = 0;
    if (set != NULL) {
      for (int i = 0; i < set->count; i++) {
        if (set->entries[i].state == MS_RUNNING) continue;
        dbg_rc++;
        ms_reaction_policy_t* ep = _ms_find_policy(env_id, set->entries[i].reaction_index);
        if (ep != NULL && ep->criticality == MS_CRIT_HIGH) dbg_hcr = 1;
      }
    }
    pthread_mutex_unlock(&_ms_lock);
    _ms_logf(MS_LEVEL_WARN,
             "event=degrade_dbg env=%d rid=%llu crit=%d degradable=%d pressure=%d has_hc_ready=%d ready_count=%d",
             env_id, (unsigned long long)reaction_index, dbg_crit, dbg_degr,
             dbg_pressure, dbg_hcr, dbg_rc);
    pthread_mutex_lock(&_ms_lock);
  }

  if (set == NULL ||
      env_id < 0 || env_id >= MS_MAX_ENVS ||
      !_ms_env_degrade_pressure[env_id]) {
    pthread_mutex_unlock(&_ms_lock);
    return false;
  }

  ms_reaction_policy_t* pol = _ms_find_policy(env_id, reaction_index);
  if (pol == NULL) {
    missing_metadata = 1;
  }

  for (int i = 0; i < set->count; i++) {
    ms_ready_entry_t* entry = &set->entries[i];
    if (entry->state == MS_RUNNING) continue;
    ready_count++;
    size_t used = strlen(ready_buf);
    if (used < sizeof(ready_buf) - 32) {
      snprintf(ready_buf + used, sizeof(ready_buf) - used, "%s%llu",
               used == 0 ? "" : "|",
               (unsigned long long)entry->reaction_index);
    }
  }

  // Pressure-driven shedding: under degrade pressure, shed LC reactions beyond
  // the per-tag budget regardless of whether an HC is concurrently ready. (The
  // earlier has_hc_ready gate only shed LC that overlapped a ready HC, which is
  // unsatisfiable under sequential/low-worker execution where HC runs first, so
  // shedding under-fired and backlog was not contained at w1/w2.)
  if (!missing_metadata &&
      pol != NULL &&
      pol->criticality == MS_CRIT_LOW &&
      pol->degradable) {
    // Graceful budget: when default_budget >= 0, keep up to that many LC
    // reactions PER LOGICAL TAG and shed only the surplus. A per-tag budget is
    // independent of period, worker count and reaction duration; an earlier
    // time-window budget (budget_window_ns) failed because, when the window was
    // shorter than the per-tag span, it reset between sequentially executed LC
    // reactions and never limited anything (so shedding only fired at high
    // worker counts where LC ran in parallel within one window). When
    // default_budget < 0 we fall back to all-or-nothing shedding under pressure.
    int allow_within_budget = 0;
    if (_ms_policy.default_budget >= 0 && env_id >= 0 && env_id < MS_MAX_ENVS) {
      if (_ms_env_lc_window_start_ns[env_id] != (int64_t)logical_time_ns) {
        _ms_env_lc_window_start_ns[env_id] = (int64_t)logical_time_ns;
        _ms_env_lc_used_in_window[env_id] = 0;
      }
      if (_ms_env_lc_used_in_window[env_id] < _ms_policy.default_budget) {
        _ms_env_lc_used_in_window[env_id]++;
        allow_within_budget = 1;
      }
    }
    if (!allow_within_budget) {
      int idx = _ms_ready_find(set, reaction_index);
      if (idx >= 0) {
        _ms_ready_remove_at(set, idx);
      }
      should_skip = 1;
    }
  }
  pthread_mutex_unlock(&_ms_lock);

  if (missing_metadata) {
    _ms_logf(
        MS_LEVEL_WARN,
        "event=fallback env=%d reason=missing_metadata logical=%lld reaction_index=%llu ready_count=%d ready_set=%s",
        env_id, logical_time_ns, (unsigned long long)reaction_index, ready_count,
        ready_buf[0] != '\0' ? ready_buf : "-"
    );
    return false;
  }

  if (should_skip) {
    _ms_logf(
        MS_LEVEL_WARN,
        "event=degrade action=skip reason=pressure env=%d logical=%lld reaction_index=%llu ready_count=%d ready_set=%s",
        env_id, logical_time_ns, (unsigned long long)reaction_index, ready_count,
        ready_buf[0] != '\0' ? ready_buf : "-"
    );
    return true;
  }

  return false;
}

void ms_on_metrics(
    int env_id,
    int worker_id,
    uint64_t now_ns,
    int64_t lag_ns,
    int ready_q_len,
    int64_t ptdv_ns
) {
  (void)env_id;
  (void)ptdv_ns;

  if (!_ms_enabled) return;
  if (worker_id < 0 || worker_id >= MS_MAX_WORKERS) return;

  ms_criticality_t crit = MS_CRIT_HIGH;
  int desired_delta = 0;
  const int64_t now_mono_ns = (now_ns > 0) ? (int64_t)now_ns : _ms_now_mono_ns();
  ms_os_policy_t policy = {
    .nice_delta = 0,
    .sched_policy = MS_SCHED_KEEP,
    .rt_priority = 0,
    .affinity_mask = 0
  };

  pthread_mutex_lock(&_ms_lock);
  int ready_len = ready_q_len;
  if (ready_len < 0) {
    ms_ready_set_t* ready_set = _ms_get_ready_set(env_id);
    if (ready_set != NULL) {
      ready_len = ready_set->count;
    }
  }

  const bool lag_enabled = (_ms_os_config.lag_threshold_ns >= 0);
  const bool ready_enabled = (_ms_os_config.ready_q_len_threshold >= 0);
  int pressure = 0;
  if (lag_enabled && lag_ns >= _ms_os_config.lag_threshold_ns) {
    pressure = 1;
  }
  if (ready_enabled && ready_len >= 0 &&
      ready_len >= _ms_os_config.ready_q_len_threshold) {
    pressure = 1;
  }
  int hc_guard_pressure = 0;
  if (_ms_os_config.hc_guard_lag_ns >= 0 && lag_ns >= _ms_os_config.hc_guard_lag_ns) {
    hc_guard_pressure = 1;
  }
  if (_ms_os_config.hc_guard_ready_q_len >= 0 && ready_len >= 0 &&
      ready_len >= _ms_os_config.hc_guard_ready_q_len) {
    hc_guard_pressure = 1;
  }

  if (env_id >= 0 && env_id < MS_MAX_ENVS) {
    _ms_env_pressure[env_id] = hc_guard_pressure;
    _ms_env_degrade_pressure[env_id] = 0;
    if (_ms_degrade_enabled) {
      if (_ms_degrade_lag_threshold_ns >= 0 && lag_ns >= _ms_degrade_lag_threshold_ns) {
        _ms_env_degrade_pressure[env_id] = 1;
      }
      if (_ms_degrade_ready_q_len_threshold >= 0 && ready_len >= 0 &&
          ready_len >= _ms_degrade_ready_q_len_threshold) {
        _ms_env_degrade_pressure[env_id] = 1;
      }
    }
  }
  const int mixed_criticality_worker =
      !_ms_partition_enabled && ((_ms_worker_crit_mask[worker_id] & 0x3) == 0x3);
  if (_ms_worker_crit_known[worker_id]) {
    crit = _ms_worker_last_crit[worker_id];
  }
  if (_ms_partition_enabled) {
    crit = _ms_worker_target_crit(worker_id);
  }
  if (!_ms_partition_enabled && !_ms_os_config.allow_mixed_workers) {
    desired_delta = 0;
  } else if (!mixed_criticality_worker && crit == MS_CRIT_LOW) {
    desired_delta = _ms_os_config.lc_base_nice_delta;
    if (pressure) {
      desired_delta += _ms_os_config.nice_delta_low;
    }
    if (_ms_os_config.rt_enabled && _ms_os_config.rt_group_enable) {
      policy.sched_policy = MS_SCHED_FIFO;
      policy.rt_priority = _ms_os_config.rt_priority_lc;
    }
  } else if (!mixed_criticality_worker && crit == MS_CRIT_HIGH) {
    if (_ms_os_config.rt_enabled && _ms_os_config.rt_group_enable) {
      desired_delta = 0;
      policy.sched_policy = MS_SCHED_FIFO;
      policy.rt_priority = _ms_os_config.rt_priority_hc;
    } else {
      if (pressure) {
        desired_delta = _ms_os_config.hc_nice_delta;
        if (_ms_os_config.rt_enabled) {
          policy.sched_policy = MS_SCHED_FIFO;
          policy.rt_priority = _ms_os_config.rt_priority_hc;
        }
      } else {
        desired_delta = 0;
        if (_ms_os_config.rt_enabled) {
          policy.sched_policy = MS_SCHED_OTHER;
          policy.rt_priority = 0;
        }
      }
    }
  }
  if (desired_delta == _ms_worker_last_nice_delta[worker_id] &&
      (int)policy.sched_policy == _ms_worker_last_sched_policy[worker_id] &&
      policy.rt_priority == _ms_worker_last_rt_priority[worker_id]) {
    pthread_mutex_unlock(&_ms_lock);
    return;
  }
  if (_ms_os_config.min_switch_interval_ns > 0 &&
      _ms_worker_last_nice_switch_ns[worker_id] > 0 &&
      (now_mono_ns - _ms_worker_last_nice_switch_ns[worker_id]) < _ms_os_config.min_switch_interval_ns) {
    pthread_mutex_unlock(&_ms_lock);
    return;
  }
  _ms_worker_last_nice_delta[worker_id] = desired_delta;
  _ms_worker_last_sched_policy[worker_id] = (int)policy.sched_policy;
  _ms_worker_last_rt_priority[worker_id] = policy.rt_priority;
  _ms_worker_last_nice_switch_ns[worker_id] = now_mono_ns;

  policy.nice_delta = desired_delta;
  if (!_ms_os_config.enabled) {
    _ms_logf(
        MS_LEVEL_INFO,
        "event=os_policy_skip worker_id=%d reason=disabled nice_delta=%d",
        worker_id, desired_delta
    );
    pthread_mutex_unlock(&_ms_lock);
    return;
  }

  _ms_worker_pending_policy[worker_id] = policy;
  _ms_worker_policy_pending[worker_id] = 1;
  pthread_mutex_unlock(&_ms_lock);
}

bool ms_take_os_policy(int worker_id, ms_os_policy_t* out_policy) {
  if (!_ms_enabled) return false;
  if (out_policy == NULL) return false;
  if (worker_id < 0 || worker_id >= MS_MAX_WORKERS) return false;

  pthread_mutex_lock(&_ms_lock);
  if (_ms_worker_policy_pending[worker_id]) {
    *out_policy = _ms_worker_pending_policy[worker_id];
    _ms_worker_policy_pending[worker_id] = 0;
    pthread_mutex_unlock(&_ms_lock);
    return true;
  }
  pthread_mutex_unlock(&_ms_lock);
  return false;
}

void ms_log_os_policy_apply(int worker_id, const ms_os_policy_t* policy, int nice_applied) {
  if (policy == NULL) return;
  _ms_logf(
      MS_LEVEL_INFO,
      "event=os_policy_apply worker_id=%d nice_delta=%d nice_applied=%d policy=%s",
      worker_id, policy->nice_delta, nice_applied,
      _ms_sched_policy_str(policy->sched_policy)
  );
}

void ms_log_os_policy_fail(int worker_id, const ms_os_policy_t* policy, const char* operation, int err) {
  if (policy == NULL) return;
  _ms_logf(
      MS_LEVEL_WARN,
      "event=os_policy_fail worker_id=%d op=%s err=%d nice_delta=%d policy=%s",
      worker_id, (operation != NULL ? operation : "(null)"), err,
      policy->nice_delta, _ms_sched_policy_str(policy->sched_policy)
  );
}

void ms_log_os_policy_skip(int worker_id, const ms_os_policy_t* policy, const char* reason) {
  if (policy == NULL) return;
  _ms_logf(
      MS_LEVEL_INFO,
      "event=os_policy_skip worker_id=%d reason=%s nice_delta=%d policy=%s",
      worker_id, (reason != NULL ? reason : "(null)"),
      policy->nice_delta, _ms_sched_policy_str(policy->sched_policy)
  );
}
