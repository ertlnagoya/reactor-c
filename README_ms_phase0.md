# Phase 0 Master Scheduler (Concept and Design)

## Overview

Phase 0 introduces a **skeleton user-space master scheduler** into the
Lingua Franca (LF) C runtime (`reactor-c`).

The goal of Phase 0 is **not** to control scheduling, but to establish
stable integration points that later phases can safely build upon.

---

## Purpose of Phase 0

Phase 0 is designed to:

- Validate where and how a master scheduler can be attached to the LF runtime
- Ensure zero impact on existing scheduling semantics
- Provide a deterministic baseline for later experimental phases
- Enable lifecycle observation of LF applications

---

## What Phase 0 Does

Phase 0 adds the following functionality:

- A master scheduler module (`master_scheduler.c/.h`)
- Lifecycle hooks:
  - `ms_init()` called during runtime initialization
  - `ms_shutdown()` called during runtime shutdown
- Lightweight logging for validation

Example log output:
```
# phase0 master_scheduler started pid=...
# phase0 master_scheduler shutdown pid=...
```

---

## What Phase 0 Does NOT Do

Phase 0 intentionally does **not**:

- Create additional threads
- Change OS scheduling policies or priorities
- Modify LF reaction scheduling semantics
- Collect runtime metrics
- Make control decisions

This strict limitation ensures that Phase 0 remains safe,
deterministic, and suitable as a baseline.

---

## Role in the Overall Roadmap

Phase 0 serves as the foundation for subsequent phases:

- **Phase 0.5** — Worker and environment registration
- **Phase 1** — Metric collection (lag, PTDV, queue length)
- **Phase 2** — Active control (priority and policy adjustment)
- **Phase 3** — Mixed-criticality orchestration

Each phase incrementally extends functionality without breaking
the guarantees established in Phase 0.

---

## Design Philosophy

Phase 0 follows these principles:

- Minimalism: add only what is strictly necessary
- Observability before control
- Determinism over optimization
- Compatibility with existing LF runtimes

---

## Intended Audience

Phase 0 is intended for:

- Researchers extending the LF runtime
- Developers experimenting with user-space scheduling frameworks
- Artifact evaluation and reproducibility studies

It is **not** intended for production use by itself.
