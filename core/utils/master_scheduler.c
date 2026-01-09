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
  int env_id;
  uint64_t reaction_index;
  ms_criticality_t criticality;
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
  pol->budget = _ms_policy.default_budget;
  pol->window_start_mono_ns = 0;
  pol->used_in_window = 0;
  return pol;
}

static void _ms_refresh_budget_window(ms_reaction_policy_t* pol, int64_t now_mono_ns) {
  if (pol == NULL) return;
  if (_ms_policy.budget_window_ns <= 0) return;
  if (pol->window_start_mono_ns == 0 ||
      (now_mono_ns - pol->window_start_mono_ns) >= _ms_policy.budget_window_ns) {
    pol->window_start_mono_ns = now_mono_ns;
    pol->used_in_window = 0;
  }
}

static int _ms_budget_allows(ms_reaction_policy_t* pol, int64_t now_mono_ns) {
  if (pol == NULL) return 1;
  if (_ms_policy.budget_type != MS_BUDGET_REACTION_COUNT) return 1;
  if (pol->budget < 0) return 1;
  _ms_refresh_budget_window(pol, now_mono_ns);
  return (pol->used_in_window < pol->budget);
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
  // Expected format: reaction,env_id,reaction_index,criticality,budget
  char* rest = line + strlen("reaction");
  while (*rest && (isspace((unsigned char)*rest) || *rest == ',')) rest++;
  if (*rest == '\0') {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_missing_fields line=%s", line);
    return;
  }

  char* tokens[4] = {0};
  int count = 0;
  char* tok = strtok(rest, ", \t");
  while (tok != NULL && count < 4) {
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

  if (!crit_ok) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_bad_crit value=%s", tokens[2]);
    return;
  }

  ms_reaction_policy_t* pol = _ms_get_policy(env_id, reaction_index, true);
  if (pol == NULL) {
    _ms_logf(MS_LEVEL_WARN, "event=config_reaction_overflow env=%d reaction_index=%llu",
             env_id, (unsigned long long)reaction_index);
    return;
  }
  pol->criticality = crit;
  pol->budget = budget;

  _ms_logf(MS_LEVEL_INFO,
           "event=config_reaction env=%d reaction_index=%llu criticality=%s budget=%lld",
           env_id, (unsigned long long)reaction_index, _ms_criticality_str(crit), (long long)budget);
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

    _ms_logf(
        MS_LEVEL_DEBUG,
        "event=ready env=%d reaction_index=%llu logical=%lld deadline=%lld is_input=%d updated=%d",
        env_id, (unsigned long long)reaction_index, logical_time_ns, deadline_ns, is_input, updated
    );
}

long long ms_pick_next(
    int env_id,
    int worker_id,
    long long logical_time_ns
) {
    (void)worker_id;

    if (!_ms_enabled) return -1;

    uint64_t candidate = 0;
    int has_candidate = 0;
    int64_t candidate_deadline = 0;
    int64_t candidate_ready = 0;
    const int64_t now_mono_ns = _ms_now_mono_ns();

    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set != NULL) {
        int i = 0;
        while (i < set->count) {
          ms_ready_entry_t* entry = &set->entries[i];
          if (entry->state == MS_RUNNING) {
            i++;
            continue;
          }

          ms_reaction_policy_t* pol = _ms_find_policy(env_id, entry->reaction_index);
          if (pol == NULL && _ms_policy.default_budget >= 0) {
            pol = _ms_get_policy(env_id, entry->reaction_index, true);
          }
          int budget_allows = _ms_budget_allows(pol, now_mono_ns);
          if (entry->state == MS_DEFERRED) {
            if (!budget_allows) {
              i++;
              continue;
            }
            entry->state = MS_READY;
          }

          if (entry->state == MS_READY &&
              pol != NULL &&
              pol->criticality == MS_CRIT_LOW &&
              _ms_policy.budget_type == MS_BUDGET_REACTION_COUNT &&
              pol->budget >= 0 &&
              !budget_allows) {
            if (_ms_policy.degrade_action == MS_DEGRADE_SKIP) {
              _ms_logf(
                  MS_LEVEL_WARN,
                  "event=degrade action=skip env=%d reaction_index=%llu criticality=%s budget=%lld used=%lld",
                  env_id, (unsigned long long)entry->reaction_index,
                  _ms_criticality_str(pol->criticality),
                  (long long)pol->budget, (long long)pol->used_in_window
              );
              _ms_ready_remove_at(set, i);
              continue;
            } else {
              entry->state = MS_DEFERRED;
              _ms_logf(
                  MS_LEVEL_WARN,
                  "event=degrade action=defer env=%d reaction_index=%llu criticality=%s budget=%lld used=%lld",
                  env_id, (unsigned long long)entry->reaction_index,
                  _ms_criticality_str(pol->criticality),
                  (long long)pol->budget, (long long)pol->used_in_window
              );
              i++;
              continue;
            }
          }

          if (entry->state != MS_READY) {
            i++;
            continue;
          }

          if (!has_candidate ||
              entry->deadline_ns < candidate_deadline ||
              (entry->deadline_ns == candidate_deadline &&
               entry->ready_time_ns < candidate_ready)) {
            candidate = entry->reaction_index;
            candidate_deadline = entry->deadline_ns;
            candidate_ready = entry->ready_time_ns;
            has_candidate = 1;
          }
          i++;
        }
        set->last_pick_reaction_index = has_candidate ? (long long)candidate : -1;
      }
    }
    pthread_mutex_unlock(&_ms_lock);

    if (has_candidate) {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=pick_next env=%d candidate=%llu reason=earliest_deadline deadline=%lld ready_time=%lld logical=%lld",
          env_id, (unsigned long long)candidate, (long long)candidate_deadline,
          (long long)candidate_ready, logical_time_ns
      );
    } else {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=pick_next env=%d candidate=-1 reason=empty logical=%lld",
          env_id, logical_time_ns
      );
    }

    // Phase 3: returning -1 means "follow the existing scheduler"
    return -1;
}

void ms_on_reaction_start(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long physical_time_ns
) {
    (void)worker_id;
    if (!_ms_enabled) return;

    int found = 0;
    long long last_pick = -1;
    int budget_exceeded = 0;
    long long budget_used = 0;
    long long budget_limit = 0;
    ms_criticality_t crit = MS_CRIT_HIGH;
    const int64_t now_mono_ns = _ms_now_mono_ns();
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
      if (pol != NULL && _ms_policy.budget_type == MS_BUDGET_REACTION_COUNT && pol->budget >= 0) {
        _ms_refresh_budget_window(pol, now_mono_ns);
        pol->used_in_window++;
        if (pol->used_in_window > pol->budget) {
          budget_exceeded = 1;
          budget_used = pol->used_in_window;
          budget_limit = pol->budget;
          crit = pol->criticality;
        }
      }
    }
    pthread_mutex_unlock(&_ms_lock);

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

    if (last_pick >= 0 && (uint64_t)last_pick != reaction_index) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=mismatch env=%d picked=%lld runtime=%llu",
          env_id, last_pick, (unsigned long long)reaction_index
      );
    }

    if (budget_exceeded) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=budget_exceeded env=%d reaction_index=%llu criticality=%s used=%lld budget=%lld",
          env_id, (unsigned long long)reaction_index, _ms_criticality_str(crit),
          budget_used, budget_limit
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

    if (!removed) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=ready_missing_on_end env=%d reaction_index=%llu",
          env_id, (unsigned long long)reaction_index
      );
    }
}
