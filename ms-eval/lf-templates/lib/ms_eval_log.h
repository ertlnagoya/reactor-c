#ifndef MS_EVAL_LOG_H
#define MS_EVAL_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ms_eval_log_init(const char* log_path,
                      const char* experiment,
                      const char* mode,
                      int reactions,
                      int steps,
                      int workload_us,
                      int deadline_us,
                      double load_factor,
                      int seed);

void ms_eval_log_close(void);

void ms_eval_log_reaction_start(int reaction_id,
                                int64_t logical_time_ns,
                                int64_t physical_time_ns);

void ms_eval_log_reaction_end(int reaction_id,
                              int64_t logical_time_ns,
                              int64_t physical_time_ns,
                              int64_t deadline_us,
                              int missed_deadline);

void ms_eval_log_injection(int reaction_id,
                           int64_t logical_time_ns,
                           int injected_delay_us,
                           int deadline_us,
                           int expected_miss);

void ms_eval_busy_wait_us(int work_us);

int ms_eval_should_inject(int reaction_id,
                          int64_t logical_time_ns,
                          int inject_period,
                          int inject_rate_pct);

#ifdef __cplusplus
}
#endif

#endif // MS_EVAL_LOG_H
