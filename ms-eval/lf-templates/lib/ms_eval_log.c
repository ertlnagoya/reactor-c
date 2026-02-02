#include "ms_eval_log.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE* ms_eval_log_fp = NULL;
static int ms_eval_log_opened = 0;

static int64_t ms_eval_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void ms_eval_log_printf(const char* fmt, ...) {
  if (ms_eval_log_fp == NULL) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(ms_eval_log_fp, fmt, args);
  va_end(args);
  fputc('\n', ms_eval_log_fp);
  fflush(ms_eval_log_fp);
}

void ms_eval_log_init(const char* log_path,
                      const char* experiment,
                      const char* mode,
                      int reactions,
                      int steps,
                      int workload_us,
                      int deadline_us,
                      double load_factor,
                      int seed) {
  if (ms_eval_log_opened) return;
  ms_eval_log_opened = 1;
  if (log_path == NULL || log_path[0] == '\0') return;
  ms_eval_log_fp = fopen(log_path, "a");
  if (ms_eval_log_fp == NULL) return;
  ms_eval_log_printf(
      "{\"type\":\"run_start\",\"ts_mono_ns\":%" PRId64
      ",\"experiment\":\"%s\",\"mode\":\"%s\",\"reactions\":%d,"
      "\"steps\":%d,\"workload_us\":%d,\"deadline_us\":%d,"
      "\"load_factor\":%.3f,\"seed\":%d}",
      ms_eval_now_mono_ns(),
      (experiment != NULL ? experiment : ""),
      (mode != NULL ? mode : ""),
      reactions,
      steps,
      workload_us,
      deadline_us,
      load_factor,
      seed);
}

void ms_eval_log_close(void) {
  if (ms_eval_log_fp == NULL) return;
  ms_eval_log_printf("{\"type\":\"run_end\",\"ts_mono_ns\":%" PRId64 "}",
                     ms_eval_now_mono_ns());
  fclose(ms_eval_log_fp);
  ms_eval_log_fp = NULL;
}

void ms_eval_log_reaction_start(int reaction_id,
                                int64_t logical_time_ns,
                                int64_t physical_time_ns) {
  ms_eval_log_printf(
      "{\"type\":\"reaction_start\",\"ts_mono_ns\":%" PRId64
      ",\"logical_time_ns\":%" PRId64 ",\"reaction_id\":%d}",
      physical_time_ns,
      logical_time_ns,
      reaction_id);
}

void ms_eval_log_reaction_end(int reaction_id,
                              int64_t logical_time_ns,
                              int64_t physical_time_ns,
                              int64_t deadline_us,
                              int missed_deadline) {
  ms_eval_log_printf(
      "{\"type\":\"reaction_end\",\"ts_mono_ns\":%" PRId64
      ",\"logical_time_ns\":%" PRId64 ",\"reaction_id\":%d,"
      "\"deadline_us\":%" PRId64 ",\"missed_deadline\":%d}",
      physical_time_ns,
      logical_time_ns,
      reaction_id,
      deadline_us,
      missed_deadline);
}

void ms_eval_log_injection(int reaction_id,
                           int64_t logical_time_ns,
                           int injected_delay_us,
                           int deadline_us,
                           int expected_miss) {
  ms_eval_log_printf(
      "{\"type\":\"injection\",\"ts_mono_ns\":%" PRId64
      ",\"logical_time_ns\":%" PRId64 ",\"reaction_id\":%d,"
      "\"injected_delay_us\":%d,\"deadline_us\":%d,\"expected_miss\":%d}",
      ms_eval_now_mono_ns(),
      logical_time_ns,
      reaction_id,
      injected_delay_us,
      deadline_us,
      expected_miss);
}

void ms_eval_busy_wait_us(int work_us) {
  if (work_us <= 0) return;
  int64_t start = ms_eval_now_mono_ns();
  int64_t target = start + (int64_t)work_us * 1000LL;
  while (ms_eval_now_mono_ns() < target) {
    // busy wait
  }
}

static uint64_t ms_eval_hash_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

int ms_eval_should_inject(int reaction_id,
                          int64_t logical_time_ns,
                          int inject_period,
                          int inject_rate_pct) {
  if (inject_period <= 0 || inject_rate_pct <= 0) return 0;
  if ((logical_time_ns / 1000LL) % inject_period != 0) return 0;
  uint64_t seed = ((uint64_t)reaction_id << 32) ^ (uint64_t)logical_time_ns;
  uint64_t h = ms_eval_hash_u64(seed);
  int bucket = (int)(h % 100ULL);
  return bucket < inject_rate_pct;
}
