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
 *  - LF_MS_OS_ENABLE=1|true : enable Phase 4 OS policy application
 *  - LF_MS_OS_LAG_NS=...    : lag threshold for OS policy decisions
 *  - LF_MS_OS_READY_Q_LEN=... : ready queue threshold for OS policy decisions
 *  - LF_MS_OS_NICE_DELTA=... : nice delta to apply for low-criticality workers
 */

#include <stdbool.h>
#include <stdint.h>

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

typedef enum {
  MS_SCHED_KEEP = 0,
  MS_SCHED_OTHER = 1,
  MS_SCHED_FIFO = 2,
  MS_SCHED_RR = 3
} ms_sched_policy_t;

typedef struct {
  int nice_delta;                 // Positive lowers priority (nice increases).
  ms_sched_policy_t sched_policy; // KEEP unless explicitly requested.
  int rt_priority;                // FIFO/RR only; ignored otherwise.
  uint64_t affinity_mask;         // Optional; 0 means KEEP.
} ms_os_policy_t;

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

// Phase 3: explicit degradation guard for optional LC reactions.
// Returns true only when it is safe to skip the specified reaction in the
// current ready set under overload pressure.
bool ms_should_skip_reaction(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long logical_time_ns
);

// Phase 4: Report metrics for OS-level policy decisions.
void ms_on_metrics(
    int env_id,
    int worker_id,
    uint64_t now_ns,
    int64_t lag_ns,
    int ready_q_len,
    int64_t ptdv_ns
);

// Phase 4: Retrieve a pending OS policy for the given worker, if any.
bool ms_take_os_policy(int worker_id, ms_os_policy_t* out_policy);

// Phase 4: Log OS policy application results.
void ms_log_os_policy_apply(int worker_id, const ms_os_policy_t* policy, int nice_applied);
void ms_log_os_policy_fail(int worker_id, const ms_os_policy_t* policy, const char* operation, int err);
void ms_log_os_policy_skip(int worker_id, const ms_os_policy_t* policy, const char* reason);

#ifdef __cplusplus
}
#endif

#endif // MASTER_SCHEDULER_H
