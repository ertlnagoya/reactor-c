/**
 * @file
 * @brief Local RTI implementation.
 * @author Erling Jellum
 * @author Edward A. Lee
 * @author Chadlia Jerad
 * @author Soroush Bateni
 *
 * This file implements the enclave coordination logic.
 * Here we are dealing with multiple mutexes. To avoid deadlocking we follow the
 * following rules:
 * 1) Mutexes are always locked in the following order:
 *  Enclave mutexes followed by RTI mutex.
 *  This means that we never lock an enclave mutex while holding the RTI mutex.
 * 2) Mutexes are always unlocked in the following order:
 *  RTI mutex followed by Enclave mutex.
 * 3) If the coordination logic might block. We unlock the enclave mutex while
 *  blocking, using a condition variable to unblock.
 * 4) When blocking on the coordination logic, never hold the RTI mutex.
 */

#ifdef LF_ENCLAVES
#include "rti_local.h"
#include "rti_common.h"
#include "util.h"
#include "platform.h"
#include "environment.h"
#include "tracepoint.h"
#include "reactor.h"

// Static global pointer to the RTI object.
static rti_local_t* rti_local;

// The RTI mutex. A pointer to this mutex will be put on the rti_local struct
lf_mutex_t rti_mutex;

#if !defined(PLATFORM_ZEPHYR) && !defined(PLATFORM_FLEXPRET) && !defined(PLATFORM_ARDUINO) && !defined(PLATFORM_RP2040)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
// LF_MS_RTI_PI=1 (experiment A4p of docs/PREREG.ja.md of lf-ms-orchestration): make rti_mutex, which every enclave
// takes to advance its tag and to report a completed tag, a priority-inheritance mutex (still recursive), so that a
// worker at normal priority (the LC enclave) that holds it while an HC worker at real-time priority waits for it
// runs at the waiter's priority. LF_MS_RTI_WAITLOG=<us>: record every wait for rti_mutex of at least <us>
// microseconds (site 1: tag advance, 2: tag complete, 3: NET update), and every hold of at least <us> at sites 2
// and 3, in a ring of 8192, written to stderr at exit as
// "RTI_WAIT tid=<tid> site=<s> kind=<wait|hold> start_ns=<CLOCK_MONOTONIC> dur_ns=<ns>".
typedef struct {
  int tid, site, hold;
  int64_t start, dur;
} _rti_wrec_t;
static _rti_wrec_t _rti_wring[8192];
static long _rti_wn = 0;
static int64_t _rti_wthr = -1;  // ns; -1 = not read yet, 0 = off
static int64_t _rti_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void _rti_wdump(void) {
  long n = _rti_wn < 8192 ? _rti_wn : 8192;
  for (long i = 0; i < n; i++)
    fprintf(stderr, "RTI_WAIT tid=%d site=%d kind=%s start_ns=%lld dur_ns=%lld\n", _rti_wring[i].tid, _rti_wring[i].site,
            _rti_wring[i].hold ? "hold" : "wait", (long long)_rti_wring[i].start, (long long)_rti_wring[i].dur);
  fprintf(stderr, "RTI_WAIT_TOTAL n=%ld thr_ns=%lld\n", _rti_wn, (long long)_rti_wthr);
}
static void _rti_wput(int site, int hold, int64_t start, int64_t dur) {
  long i = __atomic_fetch_add(&_rti_wn, 1, __ATOMIC_RELAXED);
  _rti_wring[i % 8192] = (_rti_wrec_t){(int)syscall(SYS_gettid), site, hold, start, dur};
}
// Take rti_mutex; returns the time it was taken (0 when the log is off).
static int64_t _rti_lock(int site) {
  if (_rti_wthr <= 0) {
    LF_MUTEX_LOCK(&rti_mutex);
    return 0;
  }
  int64_t a = _rti_now();
  if (pthread_mutex_trylock((pthread_mutex_t*)&rti_mutex) != 0) {
    LF_MUTEX_LOCK(&rti_mutex);
  }
  int64_t b = _rti_now();
  if (b - a >= _rti_wthr) _rti_wput(site, 0, a, b - a);
  return b;
}
static void _rti_unlock(int site, int64_t got) {
  LF_MUTEX_UNLOCK(&rti_mutex);
  if (got > 0) {
    int64_t e = _rti_now();
    if (e - got >= _rti_wthr) _rti_wput(site, 1, got, e - got);
  }
}
static void _rti_options(void) {
  const char* e = getenv("LF_MS_RTI_PI");
  if (e != NULL && e[0] == '1') {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    if (rc == 0) {
      pthread_mutex_destroy((pthread_mutex_t*)&rti_mutex);
      rc = pthread_mutex_init((pthread_mutex_t*)&rti_mutex, &a);
    }
    pthread_mutexattr_destroy(&a);
    fprintf(stderr, "RTI_PI rc=%d\n", rc);
  }
  const char* w = getenv("LF_MS_RTI_WAITLOG");
  _rti_wthr = (w != NULL && atol(w) > 0) ? atol(w) * 1000LL : 0;
  if (_rti_wthr > 0) atexit(_rti_wdump);
}
#else
static int64_t _rti_lock(int site) {
  (void)site;
  LF_MUTEX_LOCK(&rti_mutex);
  return 0;
}
static void _rti_unlock(int site, int64_t got) {
  (void)site;
  (void)got;
  LF_MUTEX_UNLOCK(&rti_mutex);
}
static void _rti_options(void) {}
#endif

void initialize_local_rti(environment_t* envs, int num_envs) {
  rti_local = (rti_local_t*)calloc(1, sizeof(rti_local_t));
  LF_ASSERT_NON_NULL(rti_local);

  initialize_rti_common(&rti_local->base);
  LF_MUTEX_INIT(&rti_mutex);
  _rti_options();
  rti_local->base.mutex = &rti_mutex;
  rti_local->base.number_of_scheduling_nodes = num_envs;
#ifdef LF_TRACE
  rti_local->base.tracing_enabled = true;
#else
  rti_local->base.tracing_enabled = false;
#endif

  // Allocate memory for the enclave_info objects
  rti_local->base.scheduling_nodes = (scheduling_node_t**)calloc(num_envs, sizeof(scheduling_node_t*));
  for (int i = 0; i < num_envs; i++) {
    enclave_info_t* enclave_info = (enclave_info_t*)calloc(1, sizeof(enclave_info_t));
    initialize_enclave_info(enclave_info, i, &envs[i]);
    rti_local->base.scheduling_nodes[i] = (scheduling_node_t*)enclave_info;

    // Encode the connection topology into the enclave_info object.
    enclave_info->base.num_immediate_downstreams = lf_get_downstream_of(i, &enclave_info->base.immediate_downstreams);
    enclave_info->base.num_immediate_upstreams = lf_get_upstream_of(i, &enclave_info->base.immediate_upstreams);
    lf_get_upstream_delay_of(i, &enclave_info->base.immediate_upstream_delays);

    enclave_info->base.state = GRANTED;
  }
}

void free_local_rti() {
  free_scheduling_nodes(rti_local->base.scheduling_nodes, rti_local->base.number_of_scheduling_nodes);
  free(rti_local);
}

void initialize_enclave_info(enclave_info_t* enclave, int idx, environment_t* env) {
  initialize_scheduling_node(&enclave->base, idx);

  env->enclave_info = enclave;
  enclave->env = env;

  // Initialize the next event condition variable.
  LF_COND_INIT(&enclave->next_event_condition, &rti_mutex);
}

tag_t rti_next_event_tag_locked(enclave_info_t* e, tag_t next_event_tag) {
  LF_PRINT_LOG("RTI: enclave %u sends NET of " PRINTF_TAG " ", e->base.id, next_event_tag.time - lf_time_start(),
               next_event_tag.microstep);

  // Return early if there are only a single enclave in the program.
  if (rti_local->base.number_of_scheduling_nodes == 1) {
    return next_event_tag;
  }
  // This is called from a critical section within the source enclave. Leave
  // this critical section and acquire the RTI mutex.
  LF_MUTEX_UNLOCK(&e->env->mutex);
  _rti_lock(1);
  tracepoint_federate_to_rti(send_NET, e->base.id, &next_event_tag);
  // First, update the enclave data structure to record this next_event_tag,
  // and notify any downstream scheduling_nodes, and unblock them if appropriate.
  tag_advance_grant_t result;

  tag_t previous_tag = e->base.last_granted;

  update_scheduling_node_next_event_tag_locked(&e->base, next_event_tag);

  // If this enclave has no upstream, then we give a TAG until forever straight away.
  if (e->base.num_immediate_upstreams == 0) {
    LF_PRINT_LOG("RTI: enclave %u has no upstream. Giving it a TAG to FOREVER", e->base.id);
    e->base.last_granted = FOREVER_TAG;
  }

  // Return early if we already have been granted past the NET.
  if (lf_tag_compare(e->base.last_granted, next_event_tag) >= 0) {
    LF_PRINT_LOG("RTI: enclave %u has already been granted a TAG to " PRINTF_TAG ". Returning with a TAG to " PRINTF_TAG
                 " ",
                 e->base.id, e->base.last_granted.time - lf_time_start(), e->base.last_granted.microstep,
                 e->base.last_granted.time - lf_time_start(), e->base.last_granted.microstep);
    tracepoint_federate_from_rti(receive_TAG, e->base.id, &e->base.last_granted);
    // Release RTI mutex and re-enter the critical section of the source enclave before returning.
    LF_MUTEX_UNLOCK(rti_local->base.mutex);
    LF_MUTEX_LOCK(&e->env->mutex);
    return e->base.last_granted;
  }

  while (true) {
    // Determine whether the above call notified a TAG.
    // If so, return that value. Note that we dont care about PTAGs as we
    // have disallowed zero-delay enclave loops.
    if (lf_tag_compare(previous_tag, e->base.last_granted) < 0) {
      result.tag = e->base.last_granted;
      result.is_provisional = false;
      break;
    }
    // If not, block.
    LF_PRINT_LOG("RTI: enclave %u sleeps waiting for TAG to " PRINTF_TAG " ", e->base.id,
                 e->base.next_event.time - lf_time_start(), e->base.next_event.microstep);
    LF_ASSERT(lf_cond_wait(&e->next_event_condition) == 0, "Could not wait for cond var");
  }

  // At this point we have gotten a new TAG.
  LF_PRINT_LOG("RTI: enclave %u returns with TAG to" PRINTF_TAG " ", e->base.id,
               e->base.next_event.time - lf_time_start(), e->base.next_event.microstep);
  tracepoint_federate_from_rti(receive_TAG, e->base.id, &result.tag);
  // Release RTI mutex and re-enter the critical section of the source enclave.
  LF_MUTEX_UNLOCK(rti_local->base.mutex);
  LF_MUTEX_LOCK(&e->env->mutex);
  return result.tag;
}

void rti_logical_tag_complete_locked(enclave_info_t* enclave, tag_t completed) {
  if (rti_local->base.number_of_scheduling_nodes == 1) {
    return;
  }
  // Release the enclave mutex while doing the local RTI work.
  LF_MUTEX_UNLOCK(&enclave->env->mutex);
  tracepoint_federate_to_rti(send_LTC, enclave->base.id, &completed);
  int64_t got = _rti_lock(2);
  _logical_tag_complete(&enclave->base, completed);
  _rti_unlock(2, got);
  // Acquire the enclave mutex again before returning.
  LF_MUTEX_LOCK(&enclave->env->mutex);
}

void rti_update_other_net_locked(enclave_info_t* src, enclave_info_t* target, tag_t net) {
  // Here we do NOT leave the critical section of the target enclave before we
  // acquire the RTI mutex. This means that we cannot block within this function.
  int64_t got = _rti_lock(3);
  tracepoint_federate_to_federate(send_TAGGED_MSG, src->base.id, target->base.id, &net);

  // If our proposed NET is less than the current NET, update it.
  if (lf_tag_compare(net, target->base.next_event) < 0) {
    target->base.next_event = net;
  }
  _rti_unlock(3, got);
}

///////////////////////////////////////////////////////////////////////////////
// The local RTIs implementation of the notify functions
///////////////////////////////////////////////////////////////////////////////

void notify_tag_advance_grant(scheduling_node_t* e, tag_t tag) {
  if (e->state == NOT_CONNECTED || lf_tag_compare(tag, e->last_granted) <= 0 ||
      lf_tag_compare(tag, e->last_provisionally_granted) < 0) {
    return;
  }
  if (rti_local->base.tracing_enabled) {
    tracepoint_rti_to_federate(send_TAG, e->id, &tag);
  }
  e->last_granted = tag;
  // TODO: Here we can consider adding a flag to the RTI struct and only signal the cond var if we have
  // sleeping enclaves.
  LF_ASSERT(lf_cond_signal(&((enclave_info_t*)e)->next_event_condition) == 0, "Could not signal cond var");
}

// We currently ignore the PTAGs, because they are only relevant with zero
// delay enclave loops.
void notify_provisional_tag_advance_grant(scheduling_node_t* e, tag_t tag) {
  LF_PRINT_LOG("RTI: enclave %u callback with PTAG " PRINTF_TAG " ", e->id, tag.time - lf_time_start(), tag.microstep);
}

void notify_downstream_next_event_tag(scheduling_node_t* e, tag_t tag) {
  // Nothing to do here.
  SUPPRESS_UNUSED_WARNING(e);
  SUPPRESS_UNUSED_WARNING(tag);
}

void free_scheduling_nodes(scheduling_node_t** scheduling_nodes, uint16_t number_of_scheduling_nodes) {
  // Nothing to do here.
  SUPPRESS_UNUSED_WARNING(scheduling_nodes);
  SUPPRESS_UNUSED_WARNING(number_of_scheduling_nodes);
}

#endif // LF_ENCLAVES
