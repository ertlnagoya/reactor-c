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
/** Stops the program when two different reactions have the same stable key (link active only). */
void ms_check_reaction_key(uint64_t key, const void* reaction, const char* instance, int number);
/** LF_MS_HCTRACE (master_scheduler.c): call ms_hctrace_got when the scheduler hands a worker a reaction, and
 * ms_hctrace_start just before ms_on_reaction_start, and
 * ms_hctrace_reaction after the reaction ran (t_start and t_end as given to ms_on_reaction_start/end). */
void ms_hctrace_got(void);
void ms_hctrace_start(void);
void ms_hctrace_reaction(int env_id, int worker_id, uint64_t key, const char* (*name_of)(const void*, int),
                         const void* reaction, long long tag, long long t_start, long long t_end);
/** ms_shutdown from atexit: the record sink is dropped first, since the library behind it may be torn down. */
void ms_shutdown_at_exit(void);

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
// The key of the reaction the MS picks, if any (use this rather than the sign of ms_pick_next's result).
bool ms_pick_next_key(int env_id, int worker_id, long long logical_time_ns, uint64_t* key);

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
    long long logical_time_ns,
    uint32_t microstep
);

// Orchestrator link: a deadline or STP violation replaced the body of a reaction.
void ms_on_reaction_violation(
    int env_id,
    uint64_t reaction_index,
    long long logical_time_ns,
    uint32_t microstep
);

/** B1b: counts one violation the program detected itself (e.g. an accelerator completion later than its
 * contract); the total is the last field of the summary with LF_MS_SUMMARY_EXT=1. Any thread may call it. */
void ms_count_external_violation(void);

/** Whether the orchestrator link is active (one of its variables is set and the MS is enabled). */
bool ms_link_is_active(void);

/**
 * Replay with LF_MS_REPLAY_X=1: whether the deadline of a reaction is to be taken as violated at
 * this tag. Returns -1 when violations are not forced (compare physical time as usual), 1 when
 * the record has an X for the reaction and tag, and 0 otherwise.
 */
int ms_replay_deadline_decision(int env_id, uint64_t reaction_index, long long logical_time_ns, uint32_t microstep);

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
