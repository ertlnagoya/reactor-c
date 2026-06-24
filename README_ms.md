# Master Scheduler Phases 0–4 (Implementation Summary)

## Overview

Phase 0 introduces a **skeleton user-space master scheduler** into the
Lingua Franca (LF) C runtime (`reactor-c`).

The goal of Phase 0 is **not** to control scheduling, but to establish
stable integration points that later phases can safely build upon.

Phase 1 builds on that foundation by adding **lightweight observation hooks**
and **candidate selection logic** while keeping the runtime scheduler in charge.

Phase 2 introduces **active control** by enforcing selected reactions across
runtime schedulers.

Phase 3 extends this to **mixed-criticality orchestration** across environments.

Phase 4 introduces **OS-level basic control** using runtime metrics.

---

## Phase 0

### Summary (Implemented)

- Master scheduler module (`master_scheduler.c/.h`) with init/shutdown lifecycle
- Safe, no-op behavior if not initialized (no runtime disruption)
- Optional logging to a configurable file (`LF_MS_LOG`) with rate limits
- Deterministic baseline: no threads, no scheduling changes

Example log output:
```
# phase0 master_scheduler started pid=...
# phase0 master_scheduler shutdown pid=...
```

### What Phase 0 Does NOT Do

- Create additional threads
- Change OS scheduling policies or priorities
- Modify LF reaction scheduling semantics
- Collect runtime metrics
- Make control decisions

### Design Philosophy (Phase 0)

- Minimalism: add only what is strictly necessary
- Observability before control
- Determinism over optimization
- Compatibility with existing LF runtimes

### Intended Audience (Phase 0)

- Researchers extending the LF runtime
- Developers experimenting with user-space scheduling frameworks
- Artifact evaluation and reproducibility studies

---

## Phase 1

### Summary (Implemented)

- Worker registration and periodic reporting (`ms_register_worker`, `ms_report`)
- Ready notifications when reactions become eligible (`ms_on_reaction_ready`)
- Execution lifecycle hooks (`ms_on_reaction_start`, `ms_on_reaction_end`)
- Candidate selection (`ms_pick_next`) using earliest-deadline policy
  - Returns `-1` in Phase 1 to preserve existing scheduler behavior
- Ready-set tracking keyed by `reaction_index` to avoid collisions
- Consistency checks between predicted candidate and runtime-selected reaction
- Logging for readiness, picks, runtime selections, and mismatches

### What Phase 1 Does NOT Do

- Override or reorder the runtime scheduler’s execution decisions
- Change OS thread priorities or scheduling policies
- Introduce preemption or new execution threads
- Enforce master scheduler picks (Phase 1 is log-only)

### Design Philosophy (Phase 1)

- Observe first: measure and log before taking control
- Keep runtime behavior unchanged while validating candidates
- Prefer safe, deterministic instrumentation over optimization

### Intended Audience (Phase 1)

- Researchers validating scheduling policies with real traces
- Developers preparing for Phase 2 control experiments

---

## Phase 2

### Summary (Implemented)

- Enforce master scheduler picks (`ms_pick_next`) across GEDF/NP/Adaptive schedulers
- Requeue current reaction to run the selected `reaction_index` when available
- Keep observation logs from Phase 1 while applying control

### What Phase 2 Does NOT Do

- Introduce preemption (still non-preemptive execution)
- Modify OS scheduling policies or thread priorities
- Guarantee optimality across tags or global time horizons

### Design Philosophy (Phase 2)

- Apply control cautiously using validated candidates
- Preserve safety by falling back to runtime behavior when picks are unavailable
- Keep instrumentation to verify policy effects

### Intended Audience (Phase 2)

- Developers running controlled scheduling experiments
- Researchers evaluating intervention policies under real workloads

---

## Phase 3

### Summary (Implemented)

- Mixed-criticality orchestration hooks with policy-driven degradation
- Configurable degradation action (defer or skip) via external config file
- Reaction-count budgets with per-reaction criticality settings

### What Phase 3 Does NOT Do

- Replace the underlying LF runtime scheduler implementation
- Eliminate the need for application-level criticality annotations
- Guarantee hard real-time bounds for all workloads

### Design Philosophy (Phase 3)

- Preserve safety by prioritizing high-criticality reactions under pressure
- Keep control decisions explainable and auditable via logs
- Prefer graceful degradation over global failure

### Intended Audience (Phase 3)

- Researchers studying mixed-criticality scheduling
- Developers deploying LF in resource-constrained or safety-critical settings

### Phase 3 Config File

Provide a config file via `LF_MS_CONFIG` or the `config_path` argument to `ms_init`.
Lines beginning with `#` are ignored. Supported keys are:

```
degrade_action=defer|skip
budget_type=reaction_count
budget_window_ns=1000000000
default_budget=-1
reaction,env_id,reaction_index,criticality,budget
```

Example:
```
degrade_action=defer
budget_type=reaction_count
budget_window_ns=1000000000
default_budget=-1

reaction,0,42,low,10
reaction,0,43,high,100
```

---

## Phase 4

### Summary (Implemented)

- OS-level control path driven by runtime pressure metrics (`lag_ns`, `ready_q_len`)
- User-space policy application for worker threads:
  - `nice` control
  - optional RT policy control (`SCHED_FIFO`/`SCHED_OTHER`)
- Support for two RT styles:
  - single-worker RT
  - worker-group RT (applies RT to the runtime thread set needed for progress)
- High-criticality guard knobs to avoid harmful OS policy flips under unstable conditions
- Structured `os_policy_*` logs for auditability of apply/skip/fail outcomes

### What Phase 4 Does NOT Do

- Replace the LF runtime scheduler
- Require elevated privileges when `LF_MS_OS_ENABLE` is unset
- Assume FIFO/RR or affinity always succeed (failures are logged and the runtime keeps running)

### Phase 4 Environment Flags

| Variable | Description |
|----------|-------------|
| `LF_MS_OS_ENABLE` | Master switch that must be `1` or `true` before any OS policy is applied. |
| `LF_MS_OS_RT_ENABLE` | Allows FIFO/RR adjustments; ignored unless `LF_MS_OS_ENABLE` is true. |
| `LF_MS_OS_RT_GROUP_ENABLE` | Enables RT application for worker-group mode (instead of single-worker only). |
| `LF_MS_OS_LAG_NS` | Metric threshold for `lag_ns` to trigger OS interventions. |
| `LF_MS_OS_READY_Q_LEN` | Ready-queue length threshold for policy activation. |
| `LF_MS_OS_NICE_DELTA` | Nice delta to apply when pressure hits. |
| `LF_MS_OS_LC_BASE_NICE_DELTA` | Base nice delta for lower-criticality workers. |
| `LF_MS_OS_HC_NICE_DELTA` | Nice delta for higher-criticality workers. |
| `LF_MS_OS_RT_PRIO_HC` | RT priority for higher-criticality worker/group path. |
| `LF_MS_OS_RT_PRIO_LC` | RT priority for lower-criticality worker/group path. |
| `LF_MS_HC_GUARD_ENABLE` | Enables high-criticality guard checks for OS control activation. |
| `LF_MS_HC_GUARD_LAG_NS` | Guard threshold for lag-based activation. |
| `LF_MS_HC_GUARD_READY_Q_LEN` | Guard threshold for ready-queue-based activation. |

The runtime applies OS policy only when pressure thresholds are met and `LF_MS_OS_ENABLE` is set. RT controls are opt-in (`LF_MS_OS_RT_ENABLE`) and failure paths are logged without terminating runtime execution.

### Logs

- `event=os_policy_apply` records every successful nice/policy change.
- `event=os_policy_skip` occurs when no change is needed, policies are disabled, or the requested policy is already in effect.
- `event=os_policy_fail` records kernel failures (`errno`, operation name) so operators can triage missing privileges or unsupported targets.

### Testing guidance

The evaluation harness and reproducible procedures live in a separate
repository: [ertlnagoya/lf-ms-evaluation](https://github.com/ertlnagoya/lf-ms-evaluation).
At the implementation level, validate Phase 4 by checking that
`os_policy_apply`, `os_policy_skip`, and `os_policy_fail` events are present in
`ms.log` under the expected conditions, and verify capability-dependent behavior
(`--cap-add SYS_NICE`) for RT/nice operations.

---

## Role in the Overall Roadmap

- **Phase 0.5** — Worker and environment registration
- **Phase 1** — Metric collection (lag, PTDV, queue length)
- **Phase 2** — Active control (priority and policy adjustment)
- **Phase 3** — Mixed-criticality orchestration
- **Phase 4** — OS-level control (nice-based adjustments, Linux first)

Each phase incrementally extends functionality without breaking
the guarantees established in Phase 0.
