# Master Scheduler Phases 0–1 (Implementation Summary)

## Overview

Phase 0 introduces a **skeleton user-space master scheduler** into the
Lingua Franca (LF) C runtime (`reactor-c`).

The goal of Phase 0 is **not** to control scheduling, but to establish
stable integration points that later phases can safely build upon.

Phase 1 builds on that foundation by adding **lightweight observation hooks**
and **candidate selection logic** while keeping the runtime scheduler in charge.

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

## Role in the Overall Roadmap

- **Phase 0.5** — Worker and environment registration
- **Phase 1** — Metric collection (lag, PTDV, queue length)
- **Phase 2** — Active control (priority and policy adjustment)
- **Phase 3** — Mixed-criticality orchestration

Each phase incrementally extends functionality without breaking
the guarantees established in Phase 0.
