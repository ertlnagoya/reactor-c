# Master Scheduler for Lingua Franca (Paper-Oriented README)

## Repository Positioning

- This repository contains the implementation and evaluation code for a **user-space Master Scheduler (MS)** on the Lingua Franca (LF) C runtime.
- The goal is to introduce staged control capabilities (observation, intervention, degradation, OS-level coordination) while **preserving LF logical-time semantics**.
- It serves as the implementation/evaluation artifact base for the current paper draft (`TechnicalPaper-MS-v1.1.pdf`).

## Paper Positioning (Key Points)

- The MS intervenes only at the LF runtime ready-set boundary to avoid violating deterministic reactive semantics.
- The design is deployable in user space without kernel/RTOS/hypervisor modifications.
- Overload handling (degradation and OS policy actions) is explicitly observable via structured logs.
- The evaluation focuses on:
  - E1: MS runtime overhead
  - E2: high-criticality deadline miss-rate comparison across baseline and MS+RT conditions

## Current Status

- We prepared this work for submission to **Reactive CPS (ReCPS) 2026**, but **missed the submission deadline**.
- The implementation and evaluation framework are actively maintained for the next submission cycle.
- ReCPS 2026 event page: [https://www.lf-lang.org/events/recps-2026/](https://www.lf-lang.org/events/recps-2026/)

## Source Code and Tags

- Repository: [https://github.com/ertlnagoya/reactor-c](https://github.com/ertlnagoya/reactor-c)
- Current consolidated release tag: [v1.0](https://github.com/ertlnagoya/reactor-c/tree/v1.0)
- MS implementation tag: [ms-v1.0](https://github.com/ertlnagoya/reactor-c/tree/ms-v1.0)
- Evaluation code and artifacts tag: [ms-eval-v1.0](https://github.com/ertlnagoya/reactor-c/tree/ms-eval-v1.0)

## Related Documents

- Phase-by-phase implementation summary: `README_ms.md`
- Reproducible performance evaluation procedure: `README_ms_performance_test.md`
- Main evaluation scripts and artifacts live under `ms-eval/`.

## Versioning Roadmap

- We will continue iterative version upgrades with synchronized updates to code, experiments, and manuscript text.
- Planned improvements include:
  - stronger reproducibility of experiment settings and scripts
  - improved statistical robustness of E1/E2
  - tighter consistency among narrative, figures, and artifacts
- We will keep publishing fixed tags so paper URLs remain stable and citable.

## Repository Notes

- Generated LF outputs such as `src-gen/`, `bin/`, and temporary evaluation logs are not part of the maintained source history.
- The maintained evaluation record is the combination of:
  - scripts under `ms-eval/scripts/`
  - selected result CSV files under `ms-eval/results/`
  - selected manuscript figures under `ms-eval/figures/`
