#ifndef MASTER_SCHEDULER_H
#define MASTER_SCHEDULER_H

/**
 * Phase 0: Skeleton user-space master scheduler hooks.
 *
 * Design goals for Phase 0:
 *  - MUST NOT change runtime scheduling behavior.
 *  - MUST remain safe if never initialized (silent no-op).
 *  - Provide lightweight, rate-limited logging for later analysis.
 *
 * Environment variables:
 *  - LF_MS_DISABLE=1|true  : disable all logging (near-zero overhead)
 *  - LF_MS_LOG=/path/file  : log file path (default: /tmp/lf_master_scheduler_phase0.log)
 *  - LF_MS_CONFIG=/path/file : optional master scheduler config file
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MS_LEVEL_ERROR = 0,
  MS_LEVEL_WARN  = 1,
  MS_LEVEL_INFO  = 2,
  MS_LEVEL_DEBUG = 3
} ms_log_level_t;

typedef struct {
  int32_t worker_id;        // Stable worker identifier assigned by runtime.
  int32_t os_tid;           // OS thread id (tid) if available; -1 otherwise.
  int32_t os_pid;           // Process id; -1 if unknown.
  const char* name;         // Optional name; may be NULL.
  uint32_t flags;           // Reserved for future use.
} ms_worker_info_t;

typedef struct {
  // Timing
  int64_t logical_time_ns;   // Logical time of the executing reaction/tag.
  int64_t physical_time_ns;  // Physical time sampled at report point.
  int64_t lag_ns;            // physical - logical

  // Context
  int32_t worker_id;         // Worker issuing the report.
  int32_t reaction_id;       // Reaction id if available; -1 otherwise.
  int32_t reactor_id;        // Reactor id if available; -1 otherwise.

  // Optional counters (use -1 if unavailable)
  int32_t ready_q_len;
  int64_t deadline_misses;
} ms_report_t;

bool ms_init(const char* config_path);
void ms_register_worker(const ms_worker_info_t* info);
void ms_report(const ms_report_t* report);
void ms_shutdown(void);

void ms_set_log_level(ms_log_level_t level);
void ms_set_enabled(bool enabled);
int ms_gettid(void);

// Phase 1: Notify that a reaction has become ready
void ms_on_reaction_ready(
    int env_id,
    uint64_t reaction_index,
    long long logical_time_ns,
    long long deadline_ns,
    int is_input
);

// Phase 1: Ask the master scheduler which reaction should be executed next
// Return value:
//   >= 0 : the scheduler explicitly selects a reaction_index
//   -1   : no intervention; follow the existing runtime scheduler
long long ms_pick_next(
    int env_id,
    int worker_id,
    long long logical_time_ns
);

// Phase 1: Notify execution lifecycle (for future control policies)
void ms_on_reaction_start(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long physical_time_ns
);

void ms_on_reaction_end(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long physical_time_ns,
    int status
);

#ifdef __cplusplus
}
#endif

#endif // MASTER_SCHEDULER_H
