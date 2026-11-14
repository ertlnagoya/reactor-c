#include "master_scheduler.h"

#include <stdio.h>
#include <stdlib.h>   // atexit
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>

#include <dlfcn.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>

// ---------------- Internal state ----------------

static pthread_mutex_t _ms_lock = PTHREAD_MUTEX_INITIALIZER;
// Time this thread spent blocked on _ms_lock (for LF_MS_HCTRACE): a trylock first, so the uncontended case costs nothing.
static __thread int64_t _ms_tl_lockwait_ns = 0;
static void _ms_lock_take(void) {
  if (pthread_mutex_trylock(&_ms_lock) == 0) return;
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  pthread_mutex_lock(&_ms_lock);
  clock_gettime(CLOCK_MONOTONIC, &b);
  _ms_tl_lockwait_ns += (int64_t)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}
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

static void _ms_logf(ms_log_level_t lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

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

// Orchestrator link. All of this is opt-in via environment variables; when they
// are unset no thread is created and no file is opened.
//  - LF_MS_CTRL_FIFO: FIFO of "set_budget <seq> <b>" commands (b >= 0) from the
//    orchestrator. A command is applied at the first decision of the next logical
//    time, so the budget never changes within a logical time.
//  - LF_MS_CTRL_SOURCE=<symbol> (A4b): an in-process command source instead of the FIFO. The MS
//    finds int <symbol>(void (*submit)(long long seq, long long budget)) with dlsym and calls it once
//    at start-up; each submit(seq, b) is handled as the FIFO line "set_budget <seq> <b>". The source
//    returns 0 when it started (e.g. a thread that receives the commands over LoLa).
//  - LF_MS_SUMMARY_SINK=<symbol> (A4b): each summary line (below) goes to void <symbol>(const char*),
//    without its newline, instead of to LF_MS_SUMMARY_FIFO. No line reaches it after ms_shutdown.
//  - LF_MS_DECISION_LOG: record of a run, one record per line:
//      D,<env>,<elapsed_ns>,<microstep>,<reaction>   shed reaction
//      X,<env>,<elapsed_ns>,<microstep>,<reaction>   deadline or STP violation
//      C,<seq>,<b>,<recv_mono_ns>                    command received
//      R,<recv_mono_ns>                              command rejected
//      M,<seq>,<b>,<elapsed_ns>,<microstep>,<recv_mono_ns>,<apply_mono_ns>   applied
//    A command received before the previous one was applied replaces it: the
//    replaced one has a C record and no M record.
//      E,<count of D>,<count of X>,<seq of a command never applied, or -1>
//    A record without the final E line is incomplete and replay refuses it.
//  - LF_MS_REPLAY_D: shed exactly the D records, ignoring pressure and budget. A
//    violation cannot be reproduced, only detected: the summary has ok=0 when the
//    violations or the sheds of the replay differ from the record.
//  - LF_MS_REPLAY_X=1 (with LF_MS_REPLAY_D): also reproduce the deadline violations. For a
//    reaction with a deadline, the runtime takes the violation as given by the X records
//    (violated iff the reaction and tag are recorded) instead of comparing physical time
//    with the deadline, and runs the deadline handler or the body accordingly. STP
//    violations are not forced (federated programs are not supported by the link).
//  - LF_MS_SUMMARY_FIFO: FIFO that receives one line per period (non-blocking, a
//    line that does not fit is dropped and counted). The thread only reads the
//    counters, so the summary cannot change a decision of this run:
//      summary <seq> <emit_mono_ns> <commanded budget> <shed_total> <kept_total> <samples>
//              <samples_over> <lag_max_ns> <violations_total> <applied_cmd_seq> <dropped_total>
//    samples, samples_over and lag_max_ns cover the period; samples_over counts the
//    lag samples at or above LF_MS_SUMMARY_LAG_NS (default 5 ms). A lag sample is taken at the
//    start of every reaction (HC and LC), so samples_over is not the HC miss rate of a program
//    that measures its own HC lag at another point. With LF_MS_SUMMARY_HC_ONLY=1 only the starts
//    of reactions that the policy does not mark as LC are sampled: when HC runs first, the LC
//    starts lag more, and an L2 controller reading all starts would see more pressure than the HC
//    does. kept_total counts the LC reactions kept by
//    the budget while shedding applies; LC reactions that run with no shedding are not counted,
//    so kept_total / (kept_total + shed_total) is not the LC completion rate. The period is
//    LF_MS_SUMMARY_PERIOD_MS (default 1000). Two fields follow at the end of the line:
//    <guard_tags> <tags>, the logical times of the period with the guard on, and all.
//    With LF_MS_SUMMARY_EXT=1 one more field follows: <ext_violations_total>, the violations the
//    program reported with ms_count_external_violation() (B1b: late accelerator completions).
//  - LF_MS_GUARD_LAG_NS, LF_MS_GUARD_RELEASE_NS, LF_MS_GUARD_BUDGET: tag-scale guard.
//    At the first decision of a logical time, the guard turns on when the latest lag
//    sample is at or above GUARD_LAG_NS and off when it is below GUARD_RELEASE_NS.
//    While on, the budget of that logical time is min(commanded budget, GUARD_BUDGET). The budget
//    acts only under degrade pressure, so GUARD_LAG_NS below LF_MS_DEGRADE_LAG_NS (or no lag
//    threshold at all) would turn the guard on without any effect; such a setting is refused.
//  - Reaction keys: while the link is active, every reaction is checked when it is first
//    dispatched; two different reactions with the same 64-bit key stop the program, since a
//    D record could not tell them apart (docs/DETERMINISM.ja.md, P5). Only reactions that are
//    dispatched are seen, which covers every collision that could mislead a replay: both
//    reactions would have to be triggered. LF_MS_KEY_CHECK_TEST_BITS=<n> (testing only) makes the
//    check compare the low n bits of the keys, to provoke a collision.
//  - LF_MS_BUDGET_CAP=1: the budget b caps the LC reactions of every logical time, with or
//    without degrade pressure (default: only under pressure). Needs LF_MS_DEGRADE_ENABLE,
//    degrade_action=skip and default_budget >= 0.
//    The orchestrator's budget is thus an upper bound. RELEASE_NS must be > 0, and
//    LF_MS_REPORT_EVERY > 1 is refused because it thins the lag samples the guard
//    and the summary read. Each change is recorded:
//      G,<elapsed_ns>,<microstep>,<1 on or 0 off>,<lag_ns>
//    Replay sheds by D only and ignores G.
//  - LF_MS_LC_LAG_NS: reaction-scale shedding. A degradable LC reaction is shed when, at
//    the moment it would start, the lag (physical time - its logical time) exceeds this
//    value, the same test as an LF deadline handler. Unlike a deadline handler, the shed
//    is recorded as a D record, so a replay reproduces it. It acts with or without
//    degrade pressure, before the budget. Needs LF_MS_DEGRADE_ENABLE and degrade_action=skip.
//  - LF_MS_SLACK_PERIOD_NS, LF_MS_SLACK_DEADLINE_NS (and optionally LF_MS_SLACK_MARGIN_NS, default
//    1 ms, LF_MS_SLACK_WORKERS, default 2): the slack budget. At the first decision of each logical
//    time t, the MS predicts the lag at which the HC of the next period (t + PERIOD) will start if
//    k LC reactions of t run: lag(k) = lag0 + (H + k C) / W - PERIOD + E, where lag0 is the lag now,
//    C the mean execution time of an LC reaction, H the mean execution time of the HC reactions of
//    a logical time (both moving averages of what the MS measured, wall time), W the workers, and E
//    the mean error of this prediction at the earlier logical times. The budget of t is the largest
//    k with lag(k) <= DEADLINE - MARGIN, capped by the commanded budget, and applies with or without
//    degrade pressure. The decisions take physical time into account like every other shed, and
//    each shed is a D record, so a replay reproduces them. Each logical time is recorded:
//      S,<elapsed_ns>,<microstep>,<budget>,<lag0_ns>,<C_ns>,<H_ns>,<E_ns>
//    Replay sheds by D only and ignores S. Cannot be combined with LF_MS_REPLAY_D.
//  - LF_MS_LAG_CLOCK=monotonic: measure the lag that the MS decisions read (the degrade pressure,
//    the guard, the summary and LF_MS_LC_LAG_NS) on CLOCK_MONOTONIC, anchored to CLOCK_REALTIME at
//    the first measurement, instead of on CLOCK_REALTIME. A step of the system clock (time sync)
//    then does not move the lag. The runtime itself still waits on CLOCK_REALTIME.
//  - LF_MS_DECISION_SINK=<symbol>: also hand every line of the decision record (without the
//    newline) to the C function `void <symbol>(const char*)`, looked up with dlsym in the
//    running program, e.g. score_lf_log_record, which writes it through S-CORE mw::log. The
//    runtime does not link S-CORE. Needs LF_MS_DECISION_LOG; the file and the sink get the
//    same lines, in the same order, under the same lock.
// Times are relative to the start time, so they compare across runs. The link
// supports one environment of the threaded runtime and refuses anything else, unless
// LF_MS_LINK_ENV=<n> names it: then the link (shedding, records, replay, guard, summary, commands) and the lag
// samples act on environment n only (an enclave, e.g. the LC of a program), and the reactions of the other
// environments run unshed and do not feed the lag the MS reads.
typedef struct {
  int env_id;
  int64_t elapsed_ns;
  uint32_t microstep;
  uint64_t reaction_index;
  bool seen;
} ms_tag_key_t;

typedef struct {
  ms_tag_key_t* keys;
  size_t count;
  size_t seen;
  size_t unexpected;
} ms_replay_set_t;

static bool _ms_link_active = false;
static bool _ms_link_failed = false;
static bool _ms_ctrl_active = false;
static bool _ms_replay_active = false;
static bool _ms_replay_force_x = false;  // LF_MS_REPLAY_X=1
static FILE* _ms_decision_log = NULL;
typedef void (*ms_record_sink_t)(const char* line);
static ms_record_sink_t _ms_record_sink = NULL;
// One line of the decision record, to the file and the sink. Caller holds _ms_lock (or runs alone).
static void _ms_rec(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void _ms_rec(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n >= sizeof(line)) line[sizeof(line) - 2] = '\n';  // truncated: keep one record per line
  if (_ms_decision_log != NULL) fputs(line, _ms_decision_log);
  if (_ms_record_sink != NULL) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    _ms_record_sink(line);
  }
}
static long long _ms_decision_d_count = 0;
static long long _ms_decision_x_count = 0;
static bool _ms_ctrl_pending = false;
static int64_t _ms_ctrl_pending_budget = 0;
static long long _ms_ctrl_pending_seq = -1;
static int64_t _ms_ctrl_pending_recv_ns = 0;
static ms_replay_set_t _ms_replay_d = {NULL, 0, 0, 0};
static ms_replay_set_t _ms_replay_x = {NULL, 0, 0, 0};
static bool _ms_guard_active = false;
static int64_t _ms_guard_on_ns = 0;
static int64_t _ms_guard_off_ns = 0;
static int64_t _ms_guard_budget = 0;
static bool _ms_guard_on = false;
static bool _ms_lc_lag_active = false;
static bool _ms_budget_cap = false;
// Slack budget (LF_MS_SLACK_*)
static bool _ms_slack_active = false;
static int64_t _ms_slack_period_ns = 0, _ms_slack_deadline_ns = 0, _ms_slack_margin_ns = 1000000;
static int _ms_slack_workers = 2;
static double _ms_slack_c_ns = -1;       // moving average of one LC reaction (wall time), -1 before the first
static double _ms_slack_h_ns = 0;        // moving average of the HC work of one logical time
static double _ms_slack_e_ns = 0;        // moving average of the prediction error
static int64_t _ms_slack_h_acc_ns = 0;   // HC work of the current logical time so far
static int64_t _ms_slack_budget = -1;    // budget of the current logical time, -1 = none
static int64_t _ms_slack_pred_ns = 0;    // predicted lag of the next period (for the error)
static int64_t _ms_slack_pred_for = -1;  // logical time the prediction is for
static int64_t _ms_slack_wstart[MS_MAX_WORKERS];
static int8_t _ms_slack_wcrit[MS_MAX_WORKERS];  // 0 none, 1 HC, 2 LC
#define MS_KEY_TABLE 4096
static uint64_t _ms_key_tab_key[MS_KEY_TABLE];
static const void* _ms_key_tab_ptr[MS_KEY_TABLE];
static const char* _ms_key_tab_name[MS_KEY_TABLE];
static int _ms_key_tab_number[MS_KEY_TABLE];
static int _ms_key_tab_count = 0;
static uint64_t _ms_key_check_mask = ~0ULL;
static bool _ms_lag_mono = false;
static int64_t _ms_lag_anchor_real = 0;
static int64_t _ms_lag_anchor_mono = -1;
static int64_t _ms_lc_lag_ns = 0;
static int64_t _ms_guard_last_lag_ns = 0;
static int64_t _ms_tag_last_logical_ns = 0;
static bool _ms_tag_seen_logical = false;
static long long _ms_sum_guard_tags = 0;
static long long _ms_sum_tags = 0;
static bool _ms_sum_active = false;
static int _ms_sum_fd = -1;
// A4b: in-process summary sink (LF_MS_SUMMARY_SINK); _ms_sum_sink_lock lets the exit drop it safely
typedef void (*ms_summary_sink_t)(const char* line);
static ms_summary_sink_t _ms_sum_sink = NULL;
static pthread_mutex_t _ms_sum_sink_lock = PTHREAD_MUTEX_INITIALIZER;
static long long _ms_ctrl_last_seq = -1;
// B1b: violations reported by the program (e.g. late accelerator completions), appended to the summary
static long long _ms_sum_ext = 0;   // updated with __atomic builtins: any thread may report
static bool _ms_sum_ext_on = false;  // LF_MS_SUMMARY_EXT=1
static long _ms_sum_period_ms = 1000;
static int64_t _ms_sum_lag_ns = 5000000;
static long long _ms_sum_shed = 0;
static long long _ms_sum_kept = 0;
static long long _ms_sum_samples = 0;
static long long _ms_sum_over = 0;
static int64_t _ms_sum_lag_max = 0;
static bool _ms_sum_hc_only = false;                  // LF_MS_SUMMARY_HC_ONLY=1
static bool _ms_guard_hc_only = false;                // LF_MS_GUARD_HC_ONLY=1
// LF_MS_LINK_ENV: -2 not read yet, -1 not set (the link refuses environments other than 0), n the link's environment
static int _ms_link_env = -2;
static int _ms_link_env_get(void) {
  if (_ms_link_env == -2) {
    const char* v = getenv("LF_MS_LINK_ENV");
    _ms_link_env = (v != NULL && v[0] != '\0') ? atoi(v) : -1;
  }
  return _ms_link_env;
}
// true when this environment is outside the link: the caller returns without a decision
static bool _ms_env_outside_link(int env_id) {
  const int le = _ms_link_env_get();
  return le >= 0 && env_id != le;
}
// LF_MS_RT_HC_PRIO=<p> (1-99): a worker thread runs every reaction that the policy does not mark as LC in SCHED_FIFO
// at priority p, and every LC reaction in SCHED_OTHER, switching at the start of a reaction when the class changes.
// LC reactions thus never run at a real-time priority, whichever worker takes them (the worker partition of
// LF_MS_WORKER_PARTITION_ENABLE only steers the pick and lets an HC worker run LC). Needs CAP_SYS_NICE (sudo).
static int _ms_rt_hc_prio = -1;                       // -1: not read yet
static __thread int _ms_rt_cur = -1;                  // this worker's class: -1 unknown, 0 SCHED_OTHER, 1 SCHED_FIFO
static int _ms_is_true(const char* s);
static int64_t _ms_sum_pend_lag[MS_MAX_WORKERS];      // lag of the reaction about to start on each worker
static bool _ms_sum_pend[MS_MAX_WORKERS];
static long long _ms_sum_x = 0;
static long long _ms_sum_cmd_seq = -1;

extern int64_t lf_time_start(void);
extern int64_t lf_time_physical(void);

static int64_t _ms_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Physical time for the lag the MS reads: CLOCK_REALTIME, or (LF_MS_LAG_CLOCK=monotonic) the elapsed
// CLOCK_MONOTONIC time since the anchor taken when the link starts, added to CLOCK_REALTIME then.
static int64_t _ms_lag_physical_ns(void) {
  if (!_ms_lag_mono) return lf_time_physical();
  return _ms_lag_anchor_real + (_ms_mono_ns() - _ms_lag_anchor_mono);
}

static int64_t _ms_now_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int _ms_tag_key_cmp(const void* a, const void* b) {
  const ms_tag_key_t* x = (const ms_tag_key_t*)a;
  const ms_tag_key_t* y = (const ms_tag_key_t*)b;
  if (x->elapsed_ns != y->elapsed_ns) return x->elapsed_ns < y->elapsed_ns ? -1 : 1;
  if (x->microstep != y->microstep) return x->microstep < y->microstep ? -1 : 1;
  if (x->reaction_index != y->reaction_index) return x->reaction_index < y->reaction_index ? -1 : 1;
  if (x->env_id != y->env_id) return x->env_id < y->env_id ? -1 : 1;
  return 0;
}

// Must be called without _ms_lock: exit() runs ms_shutdown.
static void _ms_link_fatal(const char* what, const char* detail) {
  fprintf(stderr, "master_scheduler: %s: %s\n", what, detail != NULL ? detail : "");
  _ms_link_failed = true;
  exit(EXIT_FAILURE);
}

static bool _ms_env_set(const char* name) {
  const char* v = getenv(name);
  return v != NULL && v[0] != '\0';
}

static bool _ms_link_requested(void) {
  return _ms_env_set("LF_MS_CTRL_FIFO") || _ms_env_set("LF_MS_DECISION_LOG") || _ms_env_set("LF_MS_REPLAY_D") ||
         _ms_env_set("LF_MS_SUMMARY_FIFO") || _ms_env_set("LF_MS_GUARD_LAG_NS") ||
         _ms_env_set("LF_MS_GUARD_RELEASE_NS") || _ms_env_set("LF_MS_GUARD_BUDGET") ||
         _ms_env_set("LF_MS_SUMMARY_PERIOD_MS") || _ms_env_set("LF_MS_SUMMARY_LAG_NS") ||
         _ms_env_set("LF_MS_LC_LAG_NS") || _ms_env_set("LF_MS_DECISION_SINK") || _ms_env_set("LF_MS_LAG_CLOCK") ||
         _ms_env_set("LF_MS_BUDGET_CAP") || _ms_env_set("LF_MS_KEY_CHECK_TEST_BITS") || _ms_env_set("LF_MS_REPLAY_X") ||
         _ms_env_set("LF_MS_CTRL_SOURCE") || _ms_env_set("LF_MS_SUMMARY_SINK") || _ms_env_set("LF_MS_SUMMARY_EXT") ||
         _ms_env_set("LF_MS_SUMMARY_HC_ONLY") || _ms_env_set("LF_MS_GUARD_HC_ONLY") ||
         _ms_env_set("LF_MS_SLACK_PERIOD_NS") || _ms_env_set("LF_MS_SLACK_DEADLINE_NS");
}

// Caller holds _ms_lock.
// Runs once per logical time: applies a pending command, then updates the guard.
static void _ms_tag_begin_locked(long long logical_time_ns, uint32_t microstep) {
  if (_ms_tag_seen_logical && _ms_tag_last_logical_ns == (int64_t)logical_time_ns) return;
  _ms_tag_seen_logical = true;
  _ms_tag_last_logical_ns = (int64_t)logical_time_ns;
  _ms_sum_tags++;
  if (_ms_slack_active) {
    const int64_t lag0 = _ms_lag_physical_ns() - (int64_t)logical_time_ns;
    if (_ms_slack_pred_for == (int64_t)logical_time_ns) {
      _ms_slack_e_ns += ((double)(lag0 - _ms_slack_pred_ns) - _ms_slack_e_ns) / 8.0;
    }
    _ms_slack_h_ns += ((double)_ms_slack_h_acc_ns - _ms_slack_h_ns) / 8.0;
    _ms_slack_h_acc_ns = 0;
    int64_t b = _ms_policy.default_budget;
    if (_ms_slack_c_ns > 0) {
      const double room = ((double)(_ms_slack_period_ns + _ms_slack_deadline_ns - _ms_slack_margin_ns - lag0) -
                           _ms_slack_e_ns) * _ms_slack_workers - _ms_slack_h_ns;
      int64_t k = room <= 0 ? 0 : (int64_t)(room / _ms_slack_c_ns);
      if (k < b) b = k;
    }
    if (b < 0) b = 0;
    _ms_slack_budget = b;
    const double c = _ms_slack_c_ns > 0 ? _ms_slack_c_ns : 0;
    const double pred = (double)lag0 + (_ms_slack_h_ns + (double)b * c) / _ms_slack_workers -
                        (double)_ms_slack_period_ns;
    _ms_slack_pred_ns = pred > 0 ? (int64_t)pred : 0;
    _ms_slack_pred_for = (int64_t)logical_time_ns + _ms_slack_period_ns;
    if (_ms_decision_log != NULL) {
      _ms_rec("S,%lld,%u,%lld,%lld,%lld,%lld,%lld\n", (long long)((int64_t)logical_time_ns - lf_time_start()),
              (unsigned)microstep, (long long)b, (long long)lag0, (long long)c, (long long)_ms_slack_h_ns,
              (long long)_ms_slack_e_ns);
    }
  }
  if (_ms_guard_active) {
    const bool on = _ms_guard_on ? _ms_guard_last_lag_ns >= _ms_guard_off_ns : _ms_guard_last_lag_ns >= _ms_guard_on_ns;
    if (on != _ms_guard_on && _ms_decision_log != NULL) {
      _ms_rec("G,%lld,%u,%d,%lld\n", (long long)((int64_t)logical_time_ns - lf_time_start()),
              (unsigned)microstep, on ? 1 : 0, (long long)_ms_guard_last_lag_ns);
    }
    _ms_guard_on = on;
    if (on) _ms_sum_guard_tags++;
  }
  if (!_ms_ctrl_active || !_ms_ctrl_pending) return;
  _ms_ctrl_pending = false;
  _ms_policy.default_budget = _ms_ctrl_pending_budget;
  _ms_sum_cmd_seq = _ms_ctrl_pending_seq;
  if (_ms_decision_log != NULL) {
    _ms_rec("M,%lld,%lld,%lld,%u,%lld,%lld\n",
            _ms_ctrl_pending_seq, (long long)_ms_ctrl_pending_budget,
            (long long)((int64_t)logical_time_ns - lf_time_start()), (unsigned)microstep,
            (long long)_ms_ctrl_pending_recv_ns, (long long)_ms_now_mono_ns());
  }
}

// Caller holds _ms_lock.
static void _ms_decision_log_tag_locked(char kind, int env_id, uint64_t reaction_index,
                                        long long logical_time_ns, uint32_t microstep) {
  if (_ms_decision_log == NULL) return;
  _ms_rec("%c,%d,%lld,%u,%llu\n", kind, env_id,
          (long long)((int64_t)logical_time_ns - lf_time_start()), (unsigned)microstep,
          (unsigned long long)reaction_index);
  if (kind == 'D') _ms_decision_d_count++;
  else _ms_decision_x_count++;
}

// Caller holds _ms_lock. Returns whether the key is in the recorded set.
static bool _ms_replay_mark_locked(ms_replay_set_t* set, int env_id, uint64_t reaction_index,
                                   long long logical_time_ns, uint32_t microstep) {
  ms_tag_key_t key = {env_id, (int64_t)logical_time_ns - lf_time_start(), microstep, reaction_index, false};
  ms_tag_key_t* hit = NULL;
  if (set->count > 0) {
    hit = (ms_tag_key_t*)bsearch(&key, set->keys, set->count, sizeof(ms_tag_key_t), _ms_tag_key_cmp);
  }
  if (hit == NULL || hit->seen) {
    set->unexpected++;
    return hit != NULL;
  }
  hit->seen = true;
  set->seen++;
  return true;
}

static bool _ms_ctrl_parse(const char* line, long long* seq, long long* budget) {
  static const char prefix[] = "set_budget ";
  if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return false;
  const char* p = line + sizeof(prefix) - 1;
  char* end = NULL;
  errno = 0;
  *seq = strtoll(p, &end, 10);
  if (end == p || *end != ' ') return false;
  p = end + 1;
  *budget = strtoll(p, &end, 10);
  return end != p && *end == '\0' && errno == 0;
}

// One command from the FIFO or the in-process source: accepted (C record, applied at the next logical
// time) when it parsed, its seq increases and the budget is >= 0; refused (R record) otherwise.
static bool _ms_ctrl_accept(bool parsed, long long seq, long long budget) {
  const int64_t recv_ns = _ms_now_mono_ns();
  _ms_lock_take();
  const bool valid = parsed && seq > _ms_ctrl_last_seq && budget >= 0;
  if (valid) {
    _ms_ctrl_last_seq = seq;
    _ms_ctrl_pending_seq = seq;
    _ms_ctrl_pending_budget = (int64_t)budget;
    _ms_ctrl_pending_recv_ns = recv_ns;
    _ms_ctrl_pending = true;
    if (_ms_decision_log != NULL) {
      _ms_rec("C,%lld,%lld,%lld\n", seq, budget, (long long)recv_ns);
    }
  } else if (_ms_decision_log != NULL) {
    _ms_rec("R,%lld\n", (long long)recv_ns);
  }
  pthread_mutex_unlock(&_ms_lock);
  return valid;
}

// A4b: the function handed to the in-process command source (LF_MS_CTRL_SOURCE)
static void _ms_ctrl_submit(long long seq, long long budget) {
  if (!_ms_ctrl_accept(true, seq, budget)) {
    fprintf(stderr, "master_scheduler: rejected control command: set_budget %lld %lld\n", seq, budget);
  }
}

static void* _ms_ctrl_thread(void* arg) {
  // Raw read(), not stdio: a thread blocked in fgets holds the FILE lock, and
  // exit() then waits for that lock when it flushes all streams.
  int fd = (int)(intptr_t)arg;
  char line[128];
  size_t len = 0;
  bool too_long = false;
  for (;;) {
    char c;
    ssize_t got = read(fd, &c, 1);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    if (c != '\n') {
      if (len + 1 < sizeof(line)) line[len++] = c;
      else too_long = true;
      continue;
    }
    line[len] = '\0';
    len = 0;
    long long seq = -1;
    long long budget = -1;
    const bool parsed = !too_long && _ms_ctrl_parse(line, &seq, &budget);
    too_long = false;
    if (!_ms_ctrl_accept(parsed, seq, budget)) fprintf(stderr, "master_scheduler: rejected control command: %s\n", line);
  }
  close(fd);
  return NULL;
}

static void* _ms_summary_thread(void* arg) {
  (void)arg;
  long long seq = 0;
  long long dropped = 0;
  const struct timespec period = {_ms_sum_period_ms / 1000, (_ms_sum_period_ms % 1000) * 1000000L};
  for (;;) {
    nanosleep(&period, NULL);
    seq++;
    _ms_lock_take();
    const long long budget = (long long)_ms_policy.default_budget;
    const long long shed = _ms_sum_shed;
    const long long kept = _ms_sum_kept;
    const long long samples = _ms_sum_samples;
    const long long over = _ms_sum_over;
    const long long lag_max = (long long)_ms_sum_lag_max;
    const long long violations = _ms_sum_x;
    const long long cmd_seq = _ms_sum_cmd_seq;
    const long long guard_tags = _ms_sum_guard_tags;
    const long long tags = _ms_sum_tags;
    _ms_sum_guard_tags = 0;
    _ms_sum_tags = 0;
    _ms_sum_samples = 0;
    _ms_sum_over = 0;
    _ms_sum_lag_max = 0;
    pthread_mutex_unlock(&_ms_lock);
    char line[256];
    const int len = snprintf(line, sizeof(line), "summary %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n",
                             seq, (long long)_ms_now_mono_ns(), budget, shed, kept, samples, over, lag_max,
                             violations, cmd_seq, dropped, guard_tags, tags);
    int n = len;
    if (_ms_sum_ext_on && n > 0 && n < (int)sizeof(line)) {
      // B1b: one more field, the total of the violations the program reported (LF_MS_SUMMARY_EXT=1)
      n += snprintf(line + n - 1, sizeof(line) - (size_t)(n - 1), " %lld\n",
                    __atomic_load_n(&_ms_sum_ext, __ATOMIC_RELAXED)) - 1;
    }
    if (_ms_sum_fd >= 0) {
      if (write(_ms_sum_fd, line, (size_t)n) != n) dropped++;
    } else {
      line[n > 0 ? n - 1 : 0] = '\0';  // the sink takes the line without its newline
      pthread_mutex_lock(&_ms_sum_sink_lock);
      if (_ms_sum_sink != NULL) _ms_sum_sink(line);
      pthread_mutex_unlock(&_ms_sum_sink_lock);
    }
  }
  return NULL;
}

static void _ms_replay_append(ms_replay_set_t* set, size_t* cap, const ms_tag_key_t* key) {
  if (set->count == *cap) {
    *cap = (*cap == 0) ? 1024 : *cap * 2;
    set->keys = (ms_tag_key_t*)realloc(set->keys, *cap * sizeof(ms_tag_key_t));
    if (set->keys == NULL) _ms_link_fatal("out of memory reading LF_MS_REPLAY_D", NULL);
  }
  set->keys[set->count++] = *key;
}

static void _ms_replay_finish(ms_replay_set_t* set) {
  if (set->count == 0) return;
  qsort(set->keys, set->count, sizeof(ms_tag_key_t), _ms_tag_key_cmp);
  for (size_t i = 1; i < set->count; i++) {
    if (_ms_tag_key_cmp(&set->keys[i - 1], &set->keys[i]) == 0) {
      _ms_link_fatal("duplicate record in LF_MS_REPLAY_D", NULL);
    }
  }
}

static void _ms_load_replay(const char* path) {
  FILE* in = fopen(path, "r");
  if (in == NULL) _ms_link_fatal("cannot open LF_MS_REPLAY_D", path);
  size_t cap_d = 0;
  size_t cap_x = 0;
  bool complete = false;
  long long want_d = -1;
  long long want_x = -1;
  char line[256];
  while (fgets(line, sizeof(line), in) != NULL) {
    if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;  // e.g. a record taken from a log
    if (line[0] == 'C' || line[0] == 'M' || line[0] == 'R' || line[0] == 'G') continue;
    if (line[0] == 'E') {
      long long pending;
      if (sscanf(line, "E,%lld,%lld,%lld", &want_d, &want_x, &pending) != 3) {
        _ms_link_fatal("malformed end record in LF_MS_REPLAY_D", line);
      }
      complete = true;
      continue;
    }
    char kind;
    int env_id;
    long long elapsed;
    unsigned micro;
    unsigned long long rid;
    if (sscanf(line, "%c,%d,%lld,%u,%llu", &kind, &env_id, &elapsed, &micro, &rid) != 5 ||
        (kind != 'D' && kind != 'X')) {
      _ms_link_fatal("malformed record in LF_MS_REPLAY_D", line);
    }
    ms_tag_key_t key = {env_id, (int64_t)elapsed, (uint32_t)micro, (uint64_t)rid, false};
    if (kind == 'D') _ms_replay_append(&_ms_replay_d, &cap_d, &key);
    else _ms_replay_append(&_ms_replay_x, &cap_x, &key);
  }
  fclose(in);
  if (!complete || want_d != (long long)_ms_replay_d.count || want_x != (long long)_ms_replay_x.count) {
    _ms_link_fatal("incomplete record in LF_MS_REPLAY_D", path);
  }
  _ms_replay_finish(&_ms_replay_d);
  _ms_replay_finish(&_ms_replay_x);
  _ms_replay_active = true;
}

bool ms_link_is_active(void) { return _ms_enabled && _ms_link_active; }

void ms_count_external_violation(void) { __atomic_fetch_add(&_ms_sum_ext, 1, __ATOMIC_RELAXED); }

int ms_replay_deadline_decision(int env_id, uint64_t reaction_index, long long logical_time_ns, uint32_t microstep) {
  if (!_ms_replay_force_x) return -1;
  // The X set is read-only after loading, so no lock is needed for the lookup.
  ms_tag_key_t key = {env_id, (int64_t)logical_time_ns - lf_time_start(), microstep, reaction_index, false};
  if (_ms_replay_x.count == 0) return 0;
  return bsearch(&key, _ms_replay_x.keys, _ms_replay_x.count, sizeof(ms_tag_key_t), _ms_tag_key_cmp) != NULL ? 1 : 0;
}

static void _ms_link_init(void) {
  if (!_ms_link_requested()) return;
  _ms_link_active = true;
  const char* decision_path = getenv("LF_MS_DECISION_LOG");
  const char* replay_path = getenv("LF_MS_REPLAY_D");
  const char* fifo_path = getenv("LF_MS_CTRL_FIFO");
  if (_ms_env_set("LF_MS_REPLAY_D") && _ms_env_set("LF_MS_CTRL_FIFO")) {
    _ms_link_fatal("LF_MS_REPLAY_D and LF_MS_CTRL_FIFO cannot be combined", NULL);
  }
  if (_ms_env_set("LF_MS_CTRL_SOURCE") && (_ms_env_set("LF_MS_CTRL_FIFO") || _ms_env_set("LF_MS_REPLAY_D"))) {
    _ms_link_fatal("LF_MS_CTRL_SOURCE cannot be combined with LF_MS_CTRL_FIFO or LF_MS_REPLAY_D", NULL);
  }
  if (_ms_env_set("LF_MS_SUMMARY_SINK") && _ms_env_set("LF_MS_SUMMARY_FIFO")) {
    _ms_link_fatal("LF_MS_SUMMARY_SINK and LF_MS_SUMMARY_FIFO cannot be combined", NULL);
  }
  if (_ms_env_set("LF_MS_REPLAY_D") && _ms_env_set("LF_MS_DECISION_LOG") &&
      strcmp(replay_path, decision_path) == 0) {
    _ms_link_fatal("LF_MS_DECISION_LOG would overwrite LF_MS_REPLAY_D", replay_path);
  }
  // Load before opening the record, so a refused replay leaves no record behind.
  if (_ms_env_set("LF_MS_REPLAY_D")) _ms_load_replay(replay_path);
  if (_ms_env_set("LF_MS_DECISION_LOG")) {
    _ms_decision_log = fopen(decision_path, "w");
    if (_ms_decision_log == NULL) _ms_link_fatal("cannot open LF_MS_DECISION_LOG", decision_path);
    // One flush per run in the usual case: a write under _ms_lock stalls every worker.
    setvbuf(_ms_decision_log, NULL, _IOFBF, 1 << 22);
  }
  if (_ms_env_set("LF_MS_REPLAY_X")) {
    if (!_ms_env_set("LF_MS_REPLAY_D")) _ms_link_fatal("LF_MS_REPLAY_X needs LF_MS_REPLAY_D", NULL);
    if (strcmp(getenv("LF_MS_REPLAY_X"), "1") != 0) _ms_link_fatal("LF_MS_REPLAY_X must be 1", getenv("LF_MS_REPLAY_X"));
    _ms_replay_force_x = true;
  }
  if (_ms_env_set("LF_MS_KEY_CHECK_TEST_BITS")) {
    const int bits = atoi(getenv("LF_MS_KEY_CHECK_TEST_BITS"));
    if (bits < 1 || bits > 63) _ms_link_fatal("invalid LF_MS_KEY_CHECK_TEST_BITS", getenv("LF_MS_KEY_CHECK_TEST_BITS"));
    _ms_key_check_mask = (1ULL << bits) - 1;
  }
  if (_ms_env_set("LF_MS_BUDGET_CAP")) {
    if (_ms_env_set("LF_MS_REPLAY_D")) _ms_link_fatal("LF_MS_REPLAY_D and LF_MS_BUDGET_CAP cannot be combined", NULL);
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP || _ms_policy.default_budget < 0) {
      _ms_link_fatal("LF_MS_BUDGET_CAP needs LF_MS_DEGRADE_ENABLE, degrade_action=skip and default_budget >= 0", NULL);
    }
    _ms_budget_cap = strcmp(getenv("LF_MS_BUDGET_CAP"), "1") == 0;
  }
  if (_ms_env_set("LF_MS_LAG_CLOCK")) {
    if (strcmp(getenv("LF_MS_LAG_CLOCK"), "monotonic") == 0) {
      _ms_lag_anchor_real = lf_time_physical();
      _ms_lag_anchor_mono = _ms_mono_ns();
      _ms_lag_mono = true;
    }
    else if (strcmp(getenv("LF_MS_LAG_CLOCK"), "realtime") != 0) _ms_link_fatal("LF_MS_LAG_CLOCK must be realtime or monotonic", getenv("LF_MS_LAG_CLOCK"));
  }
  if (_ms_env_set("LF_MS_DECISION_SINK")) {
    if (!_ms_env_set("LF_MS_DECISION_LOG")) _ms_link_fatal("LF_MS_DECISION_SINK needs LF_MS_DECISION_LOG", NULL);
    // POSIX: assign through the object pointer, since ISO C forbids the cast (-Werror=pedantic)
    *(void**)(&_ms_record_sink) = dlsym(RTLD_DEFAULT, getenv("LF_MS_DECISION_SINK"));
    if (_ms_record_sink == NULL) _ms_link_fatal("LF_MS_DECISION_SINK symbol not found", getenv("LF_MS_DECISION_SINK"));
  }
  if (_ms_env_set("LF_MS_CTRL_FIFO")) {
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP) {
      _ms_link_fatal("LF_MS_CTRL_FIFO needs LF_MS_DEGRADE_ENABLE and degrade_action=skip", NULL);
    }
    // O_RDWR keeps the FIFO open between writers and does not block here.
    int fd = open(fifo_path, O_RDWR);
    pthread_t thread;
    if (fd < 0 || pthread_create(&thread, NULL, _ms_ctrl_thread, (void*)(intptr_t)fd) != 0) {
      _ms_link_fatal("control channel unavailable", fifo_path);
    }
    pthread_detach(thread);
    _ms_ctrl_active = true;
  }
  if (_ms_env_set("LF_MS_CTRL_SOURCE")) {
    // A4b: an in-process command source (e.g. a LoLa subscriber loaded with the program). The MS hands
    // it a submit function; each call is one "set_budget <seq> <b>" command, handled as from the FIFO.
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP) {
      _ms_link_fatal("LF_MS_CTRL_SOURCE needs LF_MS_DEGRADE_ENABLE and degrade_action=skip", NULL);
    }
    typedef int (*ms_ctrl_source_t)(void (*submit)(long long seq, long long budget));
    ms_ctrl_source_t source = NULL;
    *(void**)(&source) = dlsym(RTLD_DEFAULT, getenv("LF_MS_CTRL_SOURCE"));
    if (source == NULL) _ms_link_fatal("LF_MS_CTRL_SOURCE symbol not found", getenv("LF_MS_CTRL_SOURCE"));
    _ms_ctrl_active = true;  // before the source starts, so no command finds the channel inactive
    if (source(_ms_ctrl_submit) != 0) _ms_link_fatal("LF_MS_CTRL_SOURCE failed to start", getenv("LF_MS_CTRL_SOURCE"));
  }
  if (!_ms_env_set("LF_MS_GUARD_LAG_NS") &&
      (_ms_env_set("LF_MS_GUARD_RELEASE_NS") || _ms_env_set("LF_MS_GUARD_BUDGET"))) {
    _ms_link_fatal("LF_MS_GUARD_RELEASE_NS and LF_MS_GUARD_BUDGET need LF_MS_GUARD_LAG_NS", NULL);
  }
  const bool summary_wanted = _ms_env_set("LF_MS_SUMMARY_FIFO") || _ms_env_set("LF_MS_SUMMARY_SINK");
  if (!summary_wanted && (_ms_env_set("LF_MS_SUMMARY_PERIOD_MS") || _ms_env_set("LF_MS_SUMMARY_LAG_NS") ||
                          _ms_env_set("LF_MS_SUMMARY_EXT") || _ms_env_set("LF_MS_SUMMARY_HC_ONLY"))) {
    _ms_link_fatal("LF_MS_SUMMARY_PERIOD_MS, LF_MS_SUMMARY_LAG_NS, LF_MS_SUMMARY_EXT and LF_MS_SUMMARY_HC_ONLY need LF_MS_SUMMARY_FIFO or LF_MS_SUMMARY_SINK", NULL);
  }
  if ((_ms_env_set("LF_MS_GUARD_LAG_NS") || summary_wanted) &&
      _ms_env_set("LF_MS_REPORT_EVERY") && atoi(getenv("LF_MS_REPORT_EVERY")) > 1) {
    _ms_link_fatal("LF_MS_REPORT_EVERY > 1 thins the lag samples of the guard and the summary", NULL);
  }
  if (_ms_env_set("LF_MS_GUARD_LAG_NS")) {
    if (_ms_env_set("LF_MS_REPLAY_D")) {
      _ms_link_fatal("LF_MS_REPLAY_D and LF_MS_GUARD_LAG_NS cannot be combined", NULL);
    }
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP || _ms_policy.default_budget < 0) {
      _ms_link_fatal("LF_MS_GUARD_LAG_NS needs LF_MS_DEGRADE_ENABLE, degrade_action=skip and default_budget >= 0", NULL);
    }
    const char* names[3] = {"LF_MS_GUARD_LAG_NS", "LF_MS_GUARD_RELEASE_NS", "LF_MS_GUARD_BUDGET"};
    int64_t* values[3] = {&_ms_guard_on_ns, &_ms_guard_off_ns, &_ms_guard_budget};
    for (int i = 0; i < 3; i++) {
      const char* v = getenv(names[i]);
      char* end = NULL;
      if (v == NULL || v[0] == '\0') _ms_link_fatal("the guard needs all three variables", names[i]);
      *values[i] = (int64_t)strtoll(v, &end, 10);
      if (*end != '\0' || *values[i] < 0) _ms_link_fatal("invalid guard setting", names[i]);
    }
    if (_ms_guard_off_ns == 0) _ms_link_fatal("LF_MS_GUARD_RELEASE_NS must be > 0", NULL);
    if (_ms_guard_off_ns > _ms_guard_on_ns) _ms_link_fatal("LF_MS_GUARD_RELEASE_NS exceeds LF_MS_GUARD_LAG_NS", NULL);
    // The guard's budget acts only under degrade pressure: the lag threshold must exist and not exceed it
    if (!_ms_budget_cap && (_ms_degrade_lag_threshold_ns < 0 || _ms_guard_on_ns < _ms_degrade_lag_threshold_ns)) {
      _ms_link_fatal("LF_MS_GUARD_LAG_NS needs LF_MS_DEGRADE_LAG_NS set and not above it (or LF_MS_BUDGET_CAP=1)", NULL);
    }
    _ms_guard_active = true;
    // LF_MS_GUARD_HC_ONLY=1: the guard reads the lag at the start of the reactions that the policy does not mark
    // as LC. When HC runs first, the last reaction to start in a tag is often an LC that waited behind it, and its
    // lag would turn the guard on although the HC kept up.
    _ms_guard_hc_only = _ms_is_true(getenv("LF_MS_GUARD_HC_ONLY"));
  }  // without the guard, LF_MS_GUARD_HC_ONLY has no effect
  if (_ms_env_set("LF_MS_LC_LAG_NS")) {
    if (_ms_env_set("LF_MS_REPLAY_D")) {
      _ms_link_fatal("LF_MS_REPLAY_D and LF_MS_LC_LAG_NS cannot be combined", NULL);
    }
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP) {
      _ms_link_fatal("LF_MS_LC_LAG_NS needs LF_MS_DEGRADE_ENABLE and degrade_action=skip", NULL);
    }
    char* end = NULL;
    _ms_lc_lag_ns = (int64_t)strtoll(getenv("LF_MS_LC_LAG_NS"), &end, 10);
    if (*end != '\0' || _ms_lc_lag_ns <= 0) _ms_link_fatal("invalid LF_MS_LC_LAG_NS", getenv("LF_MS_LC_LAG_NS"));
    _ms_lc_lag_active = true;
  }
  if (_ms_env_set("LF_MS_SLACK_PERIOD_NS") || _ms_env_set("LF_MS_SLACK_DEADLINE_NS")) {
    if (_ms_env_set("LF_MS_REPLAY_D")) {
      _ms_link_fatal("LF_MS_REPLAY_D and the slack budget (LF_MS_SLACK_*) cannot be combined", NULL);
    }
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP || _ms_policy.default_budget < 0) {
      _ms_link_fatal("the slack budget needs LF_MS_DEGRADE_ENABLE, degrade_action=skip and default_budget >= 0", NULL);
    }
    const char* names[4] = {"LF_MS_SLACK_PERIOD_NS", "LF_MS_SLACK_DEADLINE_NS", "LF_MS_SLACK_MARGIN_NS", "LF_MS_SLACK_WORKERS"};
    int64_t vals[4] = {0, 0, _ms_slack_margin_ns, _ms_slack_workers};
    for (int i = 0; i < 4; i++) {
      const char* v = getenv(names[i]);
      if (v == NULL || v[0] == '\0') {
        if (i < 2) _ms_link_fatal("the slack budget needs both variables", names[i]);
        continue;
      }
      char* end = NULL;
      vals[i] = (int64_t)strtoll(v, &end, 10);
      if (*end != '\0' || vals[i] < 0 || (i != 2 && vals[i] == 0)) _ms_link_fatal("invalid slack setting", names[i]);
    }
    _ms_slack_period_ns = vals[0];
    _ms_slack_deadline_ns = vals[1];
    _ms_slack_margin_ns = vals[2];
    _ms_slack_workers = (int)vals[3];
    _ms_slack_active = true;
  }
  if (summary_wanted) {
    if (_ms_env_set("LF_MS_REPLAY_D")) {
      _ms_link_fatal("LF_MS_REPLAY_D and the summary (LF_MS_SUMMARY_FIFO or LF_MS_SUMMARY_SINK) cannot be combined", NULL);
    }
    if (!_ms_degrade_enabled || _ms_policy.degrade_action != MS_DEGRADE_SKIP) {
      _ms_link_fatal("the summary (LF_MS_SUMMARY_FIFO or LF_MS_SUMMARY_SINK) needs LF_MS_DEGRADE_ENABLE and degrade_action=skip", NULL);
    }
    if (_ms_env_set("LF_MS_SUMMARY_PERIOD_MS")) {
      char* end = NULL;
      _ms_sum_period_ms = strtol(getenv("LF_MS_SUMMARY_PERIOD_MS"), &end, 10);
      if (*end != '\0' || _ms_sum_period_ms <= 0) {
        _ms_link_fatal("invalid LF_MS_SUMMARY_PERIOD_MS", getenv("LF_MS_SUMMARY_PERIOD_MS"));
      }
    }
    _ms_sum_hc_only = _ms_is_true(getenv("LF_MS_SUMMARY_HC_ONLY"));
    if (_ms_env_set("LF_MS_SUMMARY_LAG_NS")) {
      char* end = NULL;
      _ms_sum_lag_ns = (int64_t)strtoll(getenv("LF_MS_SUMMARY_LAG_NS"), &end, 10);
      if (*end != '\0' || _ms_sum_lag_ns < 0) {
        _ms_link_fatal("invalid LF_MS_SUMMARY_LAG_NS", getenv("LF_MS_SUMMARY_LAG_NS"));
      }
    }
    const char* summary_path = getenv("LF_MS_SUMMARY_FIFO");
    if (_ms_env_set("LF_MS_SUMMARY_SINK")) {
      // A4b: each summary line goes to a function of the process (found with dlsym), not to a FIFO
      summary_path = getenv("LF_MS_SUMMARY_SINK");
      *(void**)(&_ms_sum_sink) = dlsym(RTLD_DEFAULT, summary_path);
      if (_ms_sum_sink == NULL) _ms_link_fatal("LF_MS_SUMMARY_SINK symbol not found", summary_path);
    } else {
      // O_RDWR: opening does not wait for a reader and a write never raises SIGPIPE.
      _ms_sum_fd = open(summary_path, O_RDWR | O_NONBLOCK);
      if (_ms_sum_fd < 0) _ms_link_fatal("summary channel unavailable", summary_path);
    }
    if (_ms_env_set("LF_MS_SUMMARY_EXT")) {
      if (strcmp(getenv("LF_MS_SUMMARY_EXT"), "1") != 0) _ms_link_fatal("LF_MS_SUMMARY_EXT must be 1", getenv("LF_MS_SUMMARY_EXT"));
      _ms_sum_ext_on = true;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, _ms_summary_thread, NULL) != 0) {
      _ms_link_fatal("summary channel unavailable", summary_path);
    }
    pthread_detach(thread);
    _ms_sum_active = true;
  }
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

  _ms_lock_take();
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
  ms_shutdown_at_exit();
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
  _ms_lock_take();
  if (!_ms_atexit_registered) {
    atexit(_ms_shutdown_atexit);
    _ms_atexit_registered = true;
  }
  pthread_mutex_unlock(&_ms_lock);

#if defined(LF_SINGLE_THREADED)
  if (_ms_link_requested()) _ms_link_fatal("the orchestrator link needs the threaded runtime", NULL);
#endif
  // Allow hard disable via env var for near-zero overhead.
  if (_ms_is_true(getenv("LF_MS_DISABLE"))) {
    if (_ms_link_requested()) _ms_link_fatal("the orchestrator link cannot run with LF_MS_DISABLE", NULL);
    _ms_lock_take();
    _ms_enabled = false;
    pthread_mutex_unlock(&_ms_lock);
    return true;
  }
  if (_ms_is_true(getenv("LF_MS_OBSERVE_ONLY"))) {
    _ms_lock_take();
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

  _ms_lock_take();
  if (_ms_log != NULL) {
    pthread_mutex_unlock(&_ms_lock);
    return true; // already initialized
  }

  _ms_log = fopen(path, "a");
  if (_ms_log == NULL) {
    // Non-fatal: disable to avoid affecting runtime behavior.
    _ms_enabled = false;
    pthread_mutex_unlock(&_ms_lock);
    if (_ms_link_requested()) _ms_link_fatal("the orchestrator link needs LF_MS_LOG", path);
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
  _ms_link_init();

  return true;
}

void ms_shutdown_at_exit(void) {
  _ms_lock_take();
  _ms_record_sink = NULL;
  pthread_mutex_unlock(&_ms_lock);
  pthread_mutex_lock(&_ms_sum_sink_lock);
  _ms_sum_sink = NULL;
  pthread_mutex_unlock(&_ms_sum_sink_lock);
  ms_shutdown();
}

void ms_shutdown(void) {
  // No summary line to the in-process sink after the program ends: its library may be torn down at exit.
  pthread_mutex_lock(&_ms_sum_sink_lock);
  _ms_sum_sink = NULL;
  pthread_mutex_unlock(&_ms_sum_sink_lock);
  _ms_lock_take();

  // Avoid double shutdown (runtime hook + atexit, etc.)
  if (_ms_shutdown_called) {
    pthread_mutex_unlock(&_ms_lock);
    return;
  }
  _ms_shutdown_called = true;

  if (_ms_replay_active && !_ms_link_failed) {
    const bool ok = _ms_replay_d.seen == _ms_replay_d.count && _ms_replay_d.unexpected == 0 &&
                    _ms_replay_x.seen == _ms_replay_x.count && _ms_replay_x.unexpected == 0;
    fprintf(stderr, "event=replay_summary ok=%d d_seen=%zu d_total=%zu d_unexpected=%zu x_seen=%zu x_total=%zu x_unexpected=%zu\n",
            ok ? 1 : 0, _ms_replay_d.seen, _ms_replay_d.count, _ms_replay_d.unexpected,
            _ms_replay_x.seen, _ms_replay_x.count, _ms_replay_x.unexpected);
  }
  if (_ms_decision_log != NULL) {
    if (!_ms_link_failed) {
      _ms_rec("E,%lld,%lld,%lld\n", _ms_decision_d_count, _ms_decision_x_count,
              _ms_ctrl_pending ? _ms_ctrl_pending_seq : -1LL);
    }
    fclose(_ms_decision_log);
    _ms_decision_log = NULL;
  }

  if (_ms_log != NULL) {
    fprintf(_ms_log, "# phase3 master_scheduler shutdown pid=%d\n", (int)getpid());
    fflush(_ms_log);
    fclose(_ms_log);
    _ms_log = NULL;
  }

  pthread_mutex_unlock(&_ms_lock);
}

void ms_set_log_level(ms_log_level_t level) {
  _ms_lock_take();
  _ms_level = level;
  pthread_mutex_unlock(&_ms_lock);
}

void ms_set_enabled(bool enabled) {
  _ms_lock_take();
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

    _ms_lock_take();
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

// Whether the last ms_pick_next of this thread returned a candidate. The key is a 64-bit hash returned as
// long long, so about half of the keys are negative: a caller cannot use the sign (or -1) to tell "no
// candidate" (fixed 2026-11-12, K3 of demo/sdv: the worker loop tested pick >= 0 and never reordered a
// reaction with a negative key, so the HC-first order did not reach about half of the HC reactions).
static __thread int _ms_pick_has = 0;

bool ms_pick_next_key(int env_id, int worker_id, long long logical_time_ns, uint64_t* key) {
  const long long p = ms_pick_next(env_id, worker_id, logical_time_ns);
  if (!_ms_pick_has) return false;
  *key = (uint64_t)p;
  return true;
}

long long ms_pick_next(
    int env_id,
    int worker_id,
    long long logical_time_ns
) {
    _ms_pick_has = 0;
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

    _ms_lock_take();
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
            env_id, (unsigned long long)candidate,
            (force_hc_guard || degrade_pressure || _ms_hc_strict_priority) ? "hc_first" : "earliest_deadline",
            (long long)candidate_deadline, (long long)candidate_ready, logical_time_ns, ready_count,
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
      _ms_pick_has = 1;
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
    if (_ms_rt_hc_prio < 0) {
      const char* rp = getenv("LF_MS_RT_HC_PRIO");
      int p = (rp != NULL && rp[0] != '\0') ? atoi(rp) : 0;
      _ms_rt_hc_prio = (p >= 1 && p <= 99) ? p : 0;
    }
    int rt_want = -1;
    _ms_lock_take();
    if (_ms_rt_hc_prio > 0) {
      ms_reaction_policy_t* rpol = _ms_find_policy(env_id, reaction_index);
      rt_want = (rpol != NULL && rpol->criticality == MS_CRIT_LOW) ? 0 : 1;
    }
    if (worker_id >= 0 && worker_id < MS_MAX_WORKERS && _ms_sum_pend[worker_id]) {
      _ms_sum_pend[worker_id] = false;
      ms_reaction_policy_t* sp = _ms_find_policy(env_id, reaction_index);
      if (sp == NULL || sp->criticality != MS_CRIT_LOW) {
        const int64_t lag_ns = _ms_sum_pend_lag[worker_id];
        if (_ms_guard_hc_only) _ms_guard_last_lag_ns = lag_ns;
        if (_ms_sum_active && _ms_sum_hc_only) {
          _ms_sum_samples++;
          if (lag_ns >= _ms_sum_lag_ns) _ms_sum_over++;
          if (lag_ns > _ms_sum_lag_max) _ms_sum_lag_max = lag_ns;
        }
      }
    }
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
      if (_ms_slack_active && worker_id >= 0 && worker_id < MS_MAX_WORKERS) {
        _ms_slack_wstart[worker_id] = (int64_t)physical_time_ns;
        _ms_slack_wcrit[worker_id] = (pol != NULL && pol->criticality == MS_CRIT_LOW) ? 2 : 1;
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
    if (rt_want >= 0 && rt_want != _ms_rt_cur && _ms_rt_cur != -2) {
      struct sched_param sp;
      memset(&sp, 0, sizeof sp);
      sp.sched_priority = rt_want ? _ms_rt_hc_prio : 0;
      if (pthread_setschedparam(pthread_self(), rt_want ? SCHED_FIFO : SCHED_OTHER, &sp) == 0) {
        _ms_rt_cur = rt_want;
      } else {
        _ms_rt_cur = -2;  // warn once per thread and stop trying
        fprintf(stderr, "master_scheduler: LF_MS_RT_HC_PRIO: pthread_setschedparam failed (needs CAP_SYS_NICE)\n");
      }
    }

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
    (void)status;

    if (!_ms_enabled) return;

    int removed = 0;
    _ms_lock_take();
    if (_ms_slack_active && worker_id >= 0 && worker_id < MS_MAX_WORKERS && _ms_slack_wcrit[worker_id] != 0) {
      const int64_t d = (int64_t)physical_time_ns - _ms_slack_wstart[worker_id];
      if (_ms_slack_wcrit[worker_id] == 2) {
        _ms_slack_c_ns = _ms_slack_c_ns < 0 ? (double)d : _ms_slack_c_ns + ((double)d - _ms_slack_c_ns) / 8.0;
      } else {
        _ms_slack_h_acc_ns += d;
      }
      _ms_slack_wcrit[worker_id] = 0;
    }
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

void ms_check_reaction_key(uint64_t key, const void* reaction, const char* instance, int number) {
  const char* name = instance;
  if (!_ms_enabled || !_ms_link_active || reaction == NULL) return;
  const uint64_t k = key & _ms_key_check_mask;
  const char* other = NULL;
  int other_number = -1;
  bool full = false;
  _ms_lock_take();
  size_t i = (size_t)((k ^ (k >> 29) ^ (k >> 47)) & (MS_KEY_TABLE - 1));
  for (int n = 0; n < MS_KEY_TABLE; n++, i = (i + 1) & (MS_KEY_TABLE - 1)) {
    if (_ms_key_tab_ptr[i] == NULL) {
      if (_ms_key_tab_count >= MS_KEY_TABLE / 2) {
        full = true;
        break;
      }
      _ms_key_tab_key[i] = k;
      _ms_key_tab_ptr[i] = reaction;
      _ms_key_tab_name[i] = name;
      _ms_key_tab_number[i] = number;
      _ms_key_tab_count++;
      break;
    }
    if (_ms_key_tab_ptr[i] == reaction) break;
    if (_ms_key_tab_key[i] == k) {
      other = _ms_key_tab_name[i] != NULL ? _ms_key_tab_name[i] : "(unnamed)";
      other_number = _ms_key_tab_number[i];
      break;
    }
  }
  pthread_mutex_unlock(&_ms_lock);
  if (full) _ms_link_fatal("more reactions than the key check can hold", name);
  if (other != NULL) {
    fprintf(stderr, "master_scheduler: reactions %s/%d and %s/%d have the same key %llu\n", other, other_number,
            name != NULL ? name : "(unnamed)", number, (unsigned long long)key);
    _ms_link_fatal("reaction key collision: a D record could not tell the two apart", NULL);
  }
}

void ms_on_reaction_violation(
    int env_id,
    uint64_t reaction_index,
    long long logical_time_ns,
    uint32_t microstep
) {
  if (!_ms_enabled || !_ms_link_active) return;
  if (_ms_env_outside_link(env_id)) return;
  _ms_lock_take();
  if (_ms_replay_active) {
    _ms_replay_mark_locked(&_ms_replay_x, env_id, reaction_index, logical_time_ns, microstep);
  } else if (_ms_ctrl_active || _ms_guard_active || _ms_sum_active) {
    // A logical time whose first reaction violates its deadline still begins here (pending command, guard)
    _ms_tag_begin_locked(logical_time_ns, microstep);
  }
  _ms_sum_x++;
  _ms_decision_log_tag_locked('X', env_id, reaction_index, logical_time_ns, microstep);
  pthread_mutex_unlock(&_ms_lock);
}

bool ms_should_skip_reaction(
    int env_id,
    int worker_id,
    uint64_t reaction_index,
    long long logical_time_ns,
    uint32_t microstep
) {
  (void)worker_id;
  if (!_ms_enabled) return false;
  if (_ms_env_outside_link(env_id)) return false;
  if (_ms_link_active && env_id != 0 && _ms_link_env_get() < 0) {
    _ms_link_fatal("the orchestrator link supports one environment (set LF_MS_LINK_ENV to choose one)", NULL);
  }
  if (_ms_replay_active) {
    _ms_lock_take();
    const bool hit = _ms_replay_mark_locked(&_ms_replay_d, env_id, reaction_index, logical_time_ns, microstep);
    if (hit) {
      ms_ready_set_t* replay_set = _ms_get_ready_set(env_id);
      if (replay_set != NULL) {
        int idx = _ms_ready_find(replay_set, reaction_index);
        if (idx >= 0) _ms_ready_remove_at(replay_set, idx);
      }
      _ms_decision_log_tag_locked('D', env_id, reaction_index, logical_time_ns, microstep);
    } else {
      // A reaction outside D is not an error of the record by itself: undo the count.
      _ms_replay_d.unexpected--;
    }
    pthread_mutex_unlock(&_ms_lock);
    return hit;
  }
  if (_ms_ctrl_active || _ms_guard_active || _ms_sum_active || _ms_slack_active) {
    _ms_lock_take();
    if (_ms_guard_active && !_ms_guard_hc_only && _ms_link_env_get() >= 0) {
      // With LF_MS_LINK_ENV the link's environment may hold only LC reactions: once the guard sheds them all, no
      // reaction starts and ms_on_metrics takes no lag sample, so the guard would stay on. Take one at the decision.
      _ms_guard_last_lag_ns = _ms_lag_physical_ns() - (int64_t)logical_time_ns;
    }
    _ms_tag_begin_locked(logical_time_ns, microstep);
    pthread_mutex_unlock(&_ms_lock);
  }
  if (!_ms_degrade_enabled) return false;
  if (_ms_policy.degrade_action != MS_DEGRADE_SKIP) return false;

  int should_skip = 0;
  int ready_count = 0;
  int missing_metadata = 0;
  char ready_buf[1024];
  ready_buf[0] = '\0';

  _ms_lock_take();
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
    _ms_lock_take();
  }

  // Reaction-scale shedding (LF_MS_LC_LAG_NS): the test of a deadline handler, recorded in D.
  if (_ms_lc_lag_active && set != NULL) {
    ms_reaction_policy_t* lp = _ms_find_policy(env_id, reaction_index);
    if (lp != NULL && lp->criticality == MS_CRIT_LOW && lp->degradable &&
        _ms_lag_physical_ns() - (int64_t)logical_time_ns > _ms_lc_lag_ns) {
      int idx = _ms_ready_find(set, reaction_index);
      if (idx >= 0) _ms_ready_remove_at(set, idx);
      _ms_sum_shed++;
      _ms_decision_log_tag_locked('D', env_id, reaction_index, logical_time_ns, microstep);
      pthread_mutex_unlock(&_ms_lock);
      return true;
    }
  }

  // Shedding applies under degrade pressure, or always with LF_MS_BUDGET_CAP=1
  if (set == NULL ||
      env_id < 0 || env_id >= MS_MAX_ENVS ||
      !(_ms_env_degrade_pressure[env_id] || _ms_budget_cap || _ms_slack_active)) {
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
    int64_t budget = _ms_policy.default_budget;
    if (_ms_guard_on && _ms_guard_budget < budget) budget = _ms_guard_budget;
    if (_ms_slack_active && _ms_slack_budget >= 0 && _ms_slack_budget < budget) budget = _ms_slack_budget;
    if (budget >= 0 && env_id >= 0 && env_id < MS_MAX_ENVS) {
      if (_ms_env_lc_window_start_ns[env_id] != (int64_t)logical_time_ns) {
        _ms_env_lc_window_start_ns[env_id] = (int64_t)logical_time_ns;
        _ms_env_lc_used_in_window[env_id] = 0;
      }
      if (_ms_env_lc_used_in_window[env_id] < budget) {
        _ms_env_lc_used_in_window[env_id]++;
        allow_within_budget = 1;
        _ms_sum_kept++;
      }
    }
    if (!allow_within_budget) {
      int idx = _ms_ready_find(set, reaction_index);
      if (idx >= 0) {
        _ms_ready_remove_at(set, idx);
      }
      should_skip = 1;
      _ms_sum_shed++;
      _ms_decision_log_tag_locked('D', env_id, reaction_index, logical_time_ns, microstep);
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
  (void)ptdv_ns;

  if (!_ms_enabled) return;
  if (_ms_env_outside_link(env_id)) return;  // the lag of another environment does not press this one
  if (worker_id < 0 || worker_id >= MS_MAX_WORKERS) return;
  if (_ms_lag_mono) {
    // Same logical time, physical time from the monotonic clock (LF_MS_LAG_CLOCK=monotonic)
    const int64_t logical = (int64_t)now_ns - lag_ns;
    lag_ns = _ms_lag_physical_ns() - logical;
  }

  ms_criticality_t crit = MS_CRIT_HIGH;
  int desired_delta = 0;
  const int64_t now_mono_ns = (now_ns > 0) ? (int64_t)now_ns : _ms_now_mono_ns();
  ms_os_policy_t policy = {
    .nice_delta = 0,
    .sched_policy = MS_SCHED_KEEP,
    .rt_priority = 0,
    .affinity_mask = 0
  };

  _ms_lock_take();
  if (!_ms_guard_hc_only) _ms_guard_last_lag_ns = lag_ns;
  if (_ms_guard_hc_only || (_ms_sum_active && _ms_sum_hc_only)) {
    // used in ms_on_reaction_start, once the criticality of the reaction is known
    _ms_sum_pend_lag[worker_id] = lag_ns;
    _ms_sum_pend[worker_id] = true;
  }
  if (_ms_sum_active && !_ms_sum_hc_only) {
    _ms_sum_samples++;
    if (lag_ns >= _ms_sum_lag_ns) _ms_sum_over++;
    if (lag_ns > _ms_sum_lag_max) _ms_sum_lag_max = lag_ns;
  }
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

  _ms_lock_take();
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

// ---------------- LF_MS_HCTRACE: where the time before a reaction goes ----------------
// LF_MS_HCTRACE=<file>: every reaction of every environment appends one record of 120 bytes to <file>, a file mapped
// with MAP_SHARED, so the records survive a kill. LF_MS_HCTRACE_MAX bounds the records (default 600000); later
// reactions are not recorded. <file>.keys gets "key<TAB>name" the first time a key is seen.
// The times are lf_time_physical() (the clock of the tags); rq and cpu are the run_delay and the sum_exec_runtime of
// the worker thread from /proc/thread-self/schedstat (ns), lockw the time it spent blocked on _ms_lock (cumulative).
// "got" is when lf_sched_get_ready_reaction returned this reaction to the worker, before the MS pick and checks,
// "start" just before ms_on_reaction_start, "end" just after the reaction and the scheduling of its outputs.
typedef struct {
  int64_t tag;
  int64_t t_got, rq_got, cpu_got, lockw_got;
  int64_t t_start, rq_start, cpu_start, lockw_start;
  int64_t t_end, rq_end, cpu_end, lockw_end;
  uint64_t key;
  int32_t tid;
  int8_t env, worker, rt_got, rt_start;
} ms_hctrace_rec_t;  // 120 bytes
static int _ms_hct_state = -1;  // -1: not read, 0: off, 1: on
static ms_hctrace_rec_t* _ms_hct_recs = NULL;
static long _ms_hct_max = 0;
static long _ms_hct_next = 0;
static FILE* _ms_hct_keys = NULL;
static pthread_mutex_t _ms_hct_klock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t _ms_hct_kseen[512];
static int _ms_hct_nk = 0;
static __thread int _ms_hct_fd = -1;
static __thread int64_t _ms_hct_tgot, _ms_hct_rqgot, _ms_hct_cpugot, _ms_hct_lwgot;
static __thread int64_t _ms_hct_rqst, _ms_hct_cpust, _ms_hct_lwst;
static __thread int _ms_hct_rtgot;
static __thread uint64_t _ms_hct_tlk[64];
static __thread int _ms_hct_tlnk = 0;

static void _ms_hct_init(void) {
  pthread_mutex_lock(&_ms_hct_klock);
  if (_ms_hct_state < 0) {
    const char* p = getenv("LF_MS_HCTRACE");
    _ms_hct_state = 0;
    if (p != NULL && p[0] != '\0') {
      const char* m = getenv("LF_MS_HCTRACE_MAX");
      long max = (m != NULL && atol(m) > 0) ? atol(m) : 600000;
      int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0 && ftruncate(fd, (off_t)(max * (long)sizeof(ms_hctrace_rec_t))) == 0) {
        void* a = mmap(NULL, (size_t)max * sizeof(ms_hctrace_rec_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (a != MAP_FAILED) {
          char kp[1024];
          snprintf(kp, sizeof kp, "%s.keys", p);
          _ms_hct_keys = fopen(kp, "w");
          _ms_hct_recs = (ms_hctrace_rec_t*)a;
          _ms_hct_max = max;
          _ms_hct_state = 1;
        }
      }
      if (fd >= 0) close(fd);
      if (_ms_hct_state != 1) fprintf(stderr, "master_scheduler: LF_MS_HCTRACE: cannot map %s\n", p);
    }
  }
  pthread_mutex_unlock(&_ms_hct_klock);
}

static void _ms_hct_sched(int64_t* rq, int64_t* cpu) {
  char b[128];
  *rq = *cpu = -1;
  if (_ms_hct_fd < 0) _ms_hct_fd = open("/proc/thread-self/schedstat", O_RDONLY);
  if (_ms_hct_fd < 0) return;
  ssize_t n = pread(_ms_hct_fd, b, sizeof b - 1, 0);
  if (n <= 0) return;
  b[n] = '\0';
  long long c = -1, r = -1;
  if (sscanf(b, "%lld %lld", &c, &r) == 2) {
    *cpu = c;
    *rq = r;
  }
}

void ms_hctrace_got(void) {
  if (_ms_hct_state < 0) _ms_hct_init();
  if (_ms_hct_state != 1) return;
  _ms_hct_tgot = (int64_t)lf_time_physical();
  _ms_hct_sched(&_ms_hct_rqgot, &_ms_hct_cpugot);
  _ms_hct_lwgot = _ms_tl_lockwait_ns;
  _ms_hct_rtgot = _ms_rt_cur;
}

void ms_hctrace_start(void) {
  if (_ms_hct_state != 1) return;
  _ms_hct_sched(&_ms_hct_rqst, &_ms_hct_cpust);
  _ms_hct_lwst = _ms_tl_lockwait_ns;
}

void ms_hctrace_reaction(int env_id, int worker_id, uint64_t key, const char* (*name_of)(const void*, int),
                         const void* reaction, long long tag, long long t_start, long long t_end) {
  if (_ms_hct_state != 1) return;
  ms_hctrace_rec_t r;
  r.tag = tag;
  r.t_got = _ms_hct_tgot;
  r.t_start = t_start;
  r.t_end = t_end;
  r.rq_got = _ms_hct_rqgot;
  r.cpu_got = _ms_hct_cpugot;
  r.lockw_got = _ms_hct_lwgot;
  r.rq_start = _ms_hct_rqst;
  r.cpu_start = _ms_hct_cpust;
  r.lockw_start = _ms_hct_lwst;
  _ms_hct_sched(&r.rq_end, &r.cpu_end);
  r.lockw_end = _ms_tl_lockwait_ns;
  r.key = key;
  r.tid = (int32_t)syscall(SYS_gettid);
  r.env = (int8_t)env_id;
  r.worker = (int8_t)worker_id;
  r.rt_got = (int8_t)_ms_hct_rtgot;
  r.rt_start = (int8_t)_ms_rt_cur;
  long i = __atomic_fetch_add(&_ms_hct_next, 1, __ATOMIC_RELAXED);
  if (i < _ms_hct_max) _ms_hct_recs[i] = r;
  int seen = 0;
  for (int j = 0; j < _ms_hct_tlnk; j++) {
    if (_ms_hct_tlk[j] == key) {
      seen = 1;
      break;
    }
  }
  if (!seen) {
    if (_ms_hct_tlnk < 64) _ms_hct_tlk[_ms_hct_tlnk++] = key;
    pthread_mutex_lock(&_ms_hct_klock);
    int g = 0;
    for (int j = 0; j < _ms_hct_nk; j++) {
      if (_ms_hct_kseen[j] == key) {
        g = 1;
        break;
      }
    }
    if (!g && _ms_hct_nk < 512) {
      _ms_hct_kseen[_ms_hct_nk++] = key;
      if (_ms_hct_keys != NULL) {
        const char* name = name_of != NULL ? name_of(reaction, 0) : NULL;
        fprintf(_ms_hct_keys, "%llu\t%d\t%s\n", (unsigned long long)key, env_id, name != NULL ? name : "?");
        fflush(_ms_hct_keys);
      }
    }
    pthread_mutex_unlock(&_ms_hct_klock);
  }
}
