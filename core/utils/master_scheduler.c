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
static int64_t _ms_last_report_mono_ns = 0;

static int64_t _ms_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
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

  fprintf(_ms_log, "# phase0 master_scheduler started pid=%d\n", (int)getpid());
  fflush(_ms_log);
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
    fprintf(_ms_log, "# phase0 master_scheduler shutdown pid=%d\n", (int)getpid());
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

  // Rate limit to avoid perturbing runtime.
  const int64_t now = _ms_now_mono_ns();
  if (_ms_report_min_interval_ns > 0 &&
      (now - _ms_last_report_mono_ns) < _ms_report_min_interval_ns) {
    return;
  }
  _ms_last_report_mono_ns = now;

  _ms_logf(MS_LEVEL_DEBUG,
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

static int _ms_gettid(void) {
  return (int)syscall(SYS_gettid);
}
