# Master Scheduler for Lingua Franca (reactor-c)

This repository is a fork of [`lf-lang/reactor-c`](https://github.com/lf-lang/reactor-c)
that adds a **user-space Master Scheduler (MS)** to the Lingua Franca (LF) C
runtime. It contains the MS *implementation* only; the evaluation harness lives
in a separate repository (see [Evaluation](#evaluation)).

## What the MS adds

The MS is a user-space semantic control plane layered on the LF C runtime. It
introduces staged control capabilities — observation, intervention, controlled
degradation, and OS-level coordination — while **preserving LF logical-time
(deterministic reactive) semantics**:

- The MS acts only at the LF runtime **ready-set boundary**, so it never alters
  the deterministic order of reactions for a given tag.
- It is deployable entirely in **user space**, with no kernel, RTOS, or
  hypervisor modifications.
- Overload handling (LC budget shedding / degradation and OS policy actions) is
  explicitly **observable via structured logs**.

A phase-by-phase implementation summary (phases 0–4) is in
[`README_ms.md`](README_ms.md).

## Where the implementation lives

- `core/utils/master_scheduler.c`, `include/core/utils/master_scheduler.h` — the MS itself
- `core/threaded/reactor_threaded.c`, `core/threaded/scheduler_NP.c`,
  `core/threaded/scheduler_GEDF_NP.c`, `core/threaded/scheduler_adaptive.c`,
  `core/reactor.c` — the `LF_MS_*` integration hooks
- `core/utils/CMakeLists.txt` — builds `master_scheduler.c` into the runtime

The MS is controlled at run time through `LF_MS_*` environment variables and an
optional `LF_MS_CONFIG` policy file (see `README_ms.md`).

## Evaluation

The evaluation harness (experiments E1–E6, workload generators, plotting) is
maintained separately and pins this runtime as a submodule:

- **[ertlnagoya/lf-ms-evaluation](https://github.com/ertlnagoya/lf-ms-evaluation)**
  — pins reactor-c at tag [`ms-eval-v1.0`](https://github.com/ertlnagoya/reactor-c/tree/ms-eval-v1.0).

## Tags

- [`ms-v1.0`](https://github.com/ertlnagoya/reactor-c/tree/ms-v1.0) — MS implementation
- [`ms-eval-v1.0`](https://github.com/ertlnagoya/reactor-c/tree/ms-eval-v1.0) — runtime evaluated by `lf-ms-evaluation`

## Upstream reactor-c

`reactor-c` is the C/CCpp runtime for Lingua Franca. Upstream documentation is at
[lf-lang.org/reactor-c](https://lf-lang.org/reactor-c) and the
[target language details](https://lf-lang.org/docs/next/reference/target-language-details?target-languages=c).
