#include "master_scheduler.h"

#include <stdio.h>
#include <stdlib.h>   // atexit
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

typedef enum {
  MS_READY = 0,
  MS_RUNNING = 1
} ms_ready_state_t;

typedef struct {
  int reaction_id;
  int64_t deadline_ns;
  int64_t ready_time_ns;
  int is_input;
  ms_ready_state_t state;
} ms_ready_entry_t;

typedef struct {
  ms_ready_entry_t entries[MS_MAX_READY_PER_ENV];
  int count;
  int last_pick_reaction_id;
} ms_ready_set_t;

static ms_ready_set_t _ms_ready_sets[MS_MAX_ENVS];

static int64_t _ms_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static ms_ready_set_t* _ms_get_ready_set(int env_id) {
  if (env_id < 0 || env_id >= MS_MAX_ENVS) return NULL;
  return &_ms_ready_sets[env_id];
}

static int _ms_ready_find(ms_ready_set_t* set, int reaction_id) {
  for (int i = 0; i < set->count; i++) {
    if (set->entries[i].reaction_id == reaction_id) return i;
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
  (void)config_path; // Unused in Phase 0.

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

  fprintf(_ms_log, "# phase1 master_scheduler started pid=%d\n", (int)getpid());
  fflush(_ms_log);

  for (int i = 0; i < MS_MAX_ENVS; i++) {
    _ms_ready_sets[i].count = 0;
    _ms_ready_sets[i].last_pick_reaction_id = -1;
  }

  pthread_mutex_unlock(&_ms_lock);
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
    fprintf(_ms_log, "# phase1 master_scheduler shutdown pid=%d\n", (int)getpid());
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
    int reaction_id,
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
        int idx = _ms_ready_find(set, reaction_id);
        if (idx >= 0) {
          ms_ready_entry_t* entry = &set->entries[idx];
          entry->deadline_ns = deadline_ns;
          entry->ready_time_ns = ready_time;
          entry->is_input = is_input;
          entry->state = MS_READY;
          updated = 1;
        } else if (set->count < MS_MAX_READY_PER_ENV) {
          ms_ready_entry_t* entry = &set->entries[set->count++];
          entry->reaction_id = reaction_id;
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
          "event=ready_drop env=%d reaction_id=%d logical=%lld deadline=%lld is_input=%d",
          env_id, reaction_id, logical_time_ns, deadline_ns, is_input
      );
      return;
    }

    _ms_logf(
        MS_LEVEL_DEBUG,
        "event=ready env=%d reaction_id=%d logical=%lld deadline=%lld is_input=%d updated=%d",
        env_id, reaction_id, logical_time_ns, deadline_ns, is_input, updated
    );
}

int ms_pick_next(
    int env_id,
    int worker_id,
    long long logical_time_ns
) {
    (void)worker_id;

    if (!_ms_enabled) return -1;

    int candidate = -1;
    int64_t candidate_deadline = 0;
    int64_t candidate_ready = 0;

    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set != NULL) {
        for (int i = 0; i < set->count; i++) {
          const ms_ready_entry_t* entry = &set->entries[i];
          if (entry->state != MS_READY) continue;
          if (candidate < 0 ||
              entry->deadline_ns < candidate_deadline ||
              (entry->deadline_ns == candidate_deadline &&
               entry->ready_time_ns < candidate_ready)) {
            candidate = entry->reaction_id;
            candidate_deadline = entry->deadline_ns;
            candidate_ready = entry->ready_time_ns;
          }
        }
        set->last_pick_reaction_id = candidate;
      }
    }
    pthread_mutex_unlock(&_ms_lock);

    if (candidate >= 0) {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=pick_next env=%d candidate=%d reason=earliest_deadline deadline=%lld ready_time=%lld logical=%lld",
          env_id, candidate, (long long)candidate_deadline, (long long)candidate_ready, logical_time_ns
      );
    } else {
      _ms_logf(
          MS_LEVEL_DEBUG,
          "event=pick_next env=%d candidate=-1 reason=empty logical=%lld",
          env_id, logical_time_ns
      );
    }

    // Phase 1-B: log only (no control yet)
    // Returning -1 means "follow the existing scheduler"
    return -1;
}

void ms_on_reaction_start(
    int env_id,
    int worker_id,
    int reaction_id,
    long long physical_time_ns
) {
    (void)worker_id;
    if (!_ms_enabled) return;

    int found = 0;
    int last_pick = -1;
    pthread_mutex_lock(&_ms_lock);
    if (_ms_enabled && _ms_log != NULL) {
      ms_ready_set_t* set = _ms_get_ready_set(env_id);
      if (set != NULL) {
        last_pick = set->last_pick_reaction_id;
        int idx = _ms_ready_find(set, reaction_id);
        if (idx >= 0) {
          set->entries[idx].state = MS_RUNNING;
          found = 1;
        }
      }
    }
    pthread_mutex_unlock(&_ms_lock);

    _ms_logf(
        MS_LEVEL_DEBUG,
        "event=runtime_selected env=%d reaction_id=%d physical=%lld ready_found=%d",
        env_id, reaction_id, physical_time_ns, found
    );

    if (!found) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=runtime_selected_missing env=%d reaction_id=%d physical=%lld",
          env_id, reaction_id, physical_time_ns
      );
    }

    if (last_pick >= 0 && last_pick != reaction_id) {
      _ms_logf(
          MS_LEVEL_WARN,
          "event=mismatch env=%d picked=%d runtime=%d",
          env_id, last_pick, reaction_id
      );
    }
}

void ms_on_reaction_end(
    int env_id,
    int worker_id,
    int reaction_id,
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
        int idx = _ms_ready_find(set, reaction_id);
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
          "event=ready_missing_on_end env=%d reaction_id=%d",
          env_id, reaction_id
      );
    }
}
