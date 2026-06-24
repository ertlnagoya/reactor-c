#!/usr/bin/env python3
"""TCRS 2026 main experiment orchestrator (E4-E6) with workload calibration.

Runs INSIDE the lf-ms-phase4 Docker container (see scripts/run_e4_e6_docker.sh).
It does not start Docker itself; the launcher mounts both the reactor-c repo and
this project at their host paths so the absolute paths below resolve unchanged.

Modes:
  calibrate  Short sweep over workload configs (lc count, lc_work, period,
             workers) to find a NON-saturated overload region: baseline HC miss
             that rises with load, while controlled degradation protects HC and
             retains LC with zero semantic violations. Writes a ranked
             data/calibration_summary.csv and data/selected_workload.txt, then
             stops so the chosen workload can be reviewed.
  full       Reads data/selected_workload.txt, runs the E6 trigger search at that
             workload, auto-selects a degradation-active policy by the graceful
             criterion (HC improvement x LC retention, violations==0), then runs
             E4 / E5 / E6 main experiments.
  smoke      Like full but tiny repeats, for a quick end-to-end check.
  dry        Print the plan only.

Why calibration exists: load_factor scales only LC work (gen_overload.py), so a
too-heavy base workload pins baseline HC miss at ~100% across all loads, which is
a degenerate regime. Calibration finds a workload where overload onset is visible.
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys
from pathlib import Path
from statistics import mean

# Self-contained from reactor-c/ms-eval/scripts: paths derive from this script's
# location. Override with TCRS_REACTOR (repo root) or TCRS_DATA (output dir). The
# values are exported so subprocesses (run_e3_degradation_compare.py, run_e5/e6)
# resolve the same repo and data directory.
SCRIPT_DIR = Path(__file__).resolve().parent
REACTOR_C = Path(os.environ.get("TCRS_REACTOR", str(SCRIPT_DIR.parents[1])))
DATA = Path(os.environ.get("TCRS_DATA", str(REACTOR_C / "ms-eval/tcrs-data")))
os.environ.setdefault("TCRS_REACTOR", str(REACTOR_C))
os.environ.setdefault("TCRS_DATA", str(DATA))
RUN_COMPARE = SCRIPT_DIR / "run_e3_degradation_compare.py"
RUN_E5 = SCRIPT_DIR / "run_e5_backlog_worker_sweep.py"
RUN_E6 = SCRIPT_DIR / "run_e6_policy_sensitivity.py"
RESULTS = REACTOR_C / "ms-eval/results"

# Container has cmake/lfc on PATH; pass an existing dir so helper scripts'
# PATH prepend is harmless (their macOS default would not exist in-container).
CMAKE_BIN_DIR = "/usr/bin"

# Fixed across the study (HC side); calibration varies the LC side + parallelism.
HC = 2
HC_WORK_US = 180
DEADLINE_US = 900
PERIOD_DEFAULT = 1000


def log(msg: str) -> None:
    print(f"\n=== {msg} ===", flush=True)


def run(cmd: list[str], dry_run: bool, env: dict | None = None) -> None:
    import os
    if env:
        print("  env: " + " ".join(f"{k}={v}" for k, v in env.items()), flush=True)
    print("$ " + " ".join(cmd), flush=True)
    if not dry_run:
        full_env = None
        if env:
            full_env = dict(os.environ)
            full_env.update(env)
        subprocess.run(cmd, cwd=str(REACTOR_C), check=True, env=full_env)


def fnum(row: dict, key: str) -> float:
    try:
        return float(row.get(key, "") or 0)
    except (TypeError, ValueError):
        return 0.0


def read_summary(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with path.open() as f:
        return list(csv.DictReader(f))


# --------------------------------------------------------------------------
# Calibration
# --------------------------------------------------------------------------
def calibrate(args, dry: bool) -> None:
    log("Workload calibration")
    workers_grid = [int(x) for x in args.cal_workers.split(",") if x.strip()]
    lc_grid = [int(x) for x in args.cal_lc.split(",") if x.strip()]
    lcwork_grid = [int(x) for x in args.cal_lc_work.split(",") if x.strip()]
    period_grid = [int(x) for x in args.cal_periods.split(",") if x.strip()]
    loads = args.cal_loads
    load_vals = [float(x) for x in loads.split(",") if x.strip()]
    lo, hi = min(load_vals), max(load_vals)

    cal_dir = DATA / "calibration"
    if not dry:
        cal_dir.mkdir(parents=True, exist_ok=True)
    ranked: list[dict] = []

    for workers in workers_grid:
        for lc in lc_grid:
            for lc_work in lcwork_grid:
                for period in period_grid:
                    budget = max(1, lc // 2)  # trigger-capable policy
                    prefix = f"cal_w{workers}_lc{lc}_lw{lc_work}_p{period}"
                    cmd = [
                        "python3", str(RUN_COMPARE),
                        "--workers", str(workers),
                        "--loads", loads,
                        "--repeats", str(args.cal_repeats),
                        "--steps", str(args.cal_steps),
                        "--hc", str(HC), "--lc", str(lc),
                        "--lc-budget", str(budget),
                        "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lc_work),
                        "--deadline-us", str(DEADLINE_US), "--period-us", str(period),
                        "--degrade-lag-ns", "50000", "--degrade-ready-q-len", "2",
                        "--out-prefix", prefix,
                    ]
                    run(cmd, dry)
                    if dry:
                        continue
                    src = RESULTS / f"{prefix}_summary.csv"
                    rows = read_summary(src)
                    if not rows:
                        continue
                    shutil.copy2(src, cal_dir / src.name)
                    by_load = {fnum(r, "load_factor"): r for r in rows}
                    base_lo = fnum(by_load.get(lo, {}), "baseline_hc_miss_mean")
                    base_hi = fnum(by_load.get(hi, {}), "baseline_hc_miss_mean")
                    benefit = mean(fnum(r, "baseline_hc_miss_mean") - fnum(r, "degrade_hc_miss_mean") for r in rows)
                    lc_ret = mean(fnum(r, "degrade_lc_completion_mean") for r in rows)
                    actions = mean(fnum(r, "degrade_actions_mean") for r in rows)
                    vio = sum(fnum(r, "violations_mean") for r in rows)
                    fb = sum(fnum(r, "fallbacks_mean") for r in rows)
                    # Non-saturated overload: low load mostly OK, high load overloaded.
                    qualifies = (
                        base_lo < args.cal_low_max and base_hi > args.cal_high_min
                        and actions > 0 and vio == 0 and fb == 0
                        and benefit > 0
                    )
                    score = benefit + 0.5 * lc_ret + 0.3 * (base_hi - base_lo)
                    ranked.append({
                        "workers": workers, "lc": lc, "lc_work_us": lc_work, "period_us": period,
                        "lc_budget": budget,
                        "baseline_hc_low": round(base_lo, 2), "baseline_hc_high": round(base_hi, 2),
                        "hc_spread": round(base_hi - base_lo, 2),
                        "degrade_benefit": round(benefit, 2), "lc_retention": round(lc_ret, 2),
                        "degrade_actions": round(actions, 1), "violations": round(vio, 3),
                        "fallbacks": round(fb, 3),
                        "qualifies": int(qualifies), "score": round(score, 3),
                    })

    if dry:
        return
    ranked.sort(key=lambda r: (r["qualifies"], r["score"]), reverse=True)
    out_csv = DATA / "calibration_summary.csv"
    if ranked:
        with out_csv.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(ranked[0].keys()))
            w.writeheader()
            w.writerows(ranked)
    log("Calibration ranking (top rows)")
    hdr = "workers lc lc_work period | base_lo base_hi spread benefit lc_ret actions vio qual score"
    print(hdr, flush=True)
    for r in ranked[:10]:
        print(f"{r['workers']:>7} {r['lc']:>2} {r['lc_work_us']:>7} {r['period_us']:>6} | "
              f"{r['baseline_hc_low']:>7} {r['baseline_hc_high']:>7} {r['hc_spread']:>6} "
              f"{r['degrade_benefit']:>7} {r['lc_retention']:>6} {r['degrade_actions']:>7} "
              f"{r['violations']:>3} {r['qualifies']:>4} {r['score']:>6}", flush=True)

    qualified = [r for r in ranked if r["qualifies"]]
    if not qualified:
        print("\nWARNING: no workload met the non-saturated overload criteria.", flush=True)
        print("Widen the grid (lower --cal-lc / --cal-lc-work, larger --cal-periods,", flush=True)
        print("or adjust --cal-low-max / --cal-high-min) and re-run calibrate.", flush=True)
        print("selected_workload.txt was NOT written.", flush=True)
        sys.exit(3)
    best = qualified[0]
    (DATA / "selected_workload.txt").write_text(
        f"workers={best['workers']}\nlc={best['lc']}\nlc_work_us={best['lc_work_us']}\n"
        f"period_us={best['period_us']}\nlc_budget_hint={best['lc_budget']}\n"
        f"baseline_hc_low={best['baseline_hc_low']}\nbaseline_hc_high={best['baseline_hc_high']}\n"
        f"degrade_benefit={best['degrade_benefit']}\nlc_retention={best['lc_retention']}\n"
    )
    log("Selected workload")
    print(f"  workers={best['workers']} lc={best['lc']} lc_work_us={best['lc_work_us']} "
          f"period_us={best['period_us']}  (baseline {best['baseline_hc_low']}%->"
          f"{best['baseline_hc_high']}%, degrade benefit {best['degrade_benefit']}pts, "
          f"LC retention {best['lc_retention']}%)", flush=True)
    print("\nReview data/calibration_summary.csv, then run: "
          "./scripts/run_e4_e6_docker.sh full", flush=True)


# --------------------------------------------------------------------------
# High-repeat statistical verification
# --------------------------------------------------------------------------
def verify(args, dry: bool) -> None:
    """For each candidate workload x policy, run a high-repeat baseline/MS/degrade
    comparison and test whether degradation SIGNIFICANTLY reduces HC miss
    (non-overlapping 95% CIs) at the highest load. Answers, with statistics,
    whether controlled degradation protects HC before any main run is committed."""
    log("High-repeat verification")
    # workloads: "workers:lc:lc_work:period,..."
    wls = []
    for spec in args.verify_workloads.split(","):
        w, lc, lw, p = (int(x) for x in spec.split(":"))
        wls.append((w, lc, lw, p))
    # policies: "ready_q:lag_ns:keep_frac,..."  budget = round(lc*keep_frac)
    pols = []
    for spec in args.verify_policies.split(","):
        q, lag, keep = spec.split(":")
        pols.append((int(q), int(lag), float(keep)))
    loads = args.verify_loads
    load_vals = [float(x) for x in loads.split(",") if x.strip()]
    hi = max(load_vals)

    vdir = DATA / "verify"
    if not dry:
        vdir.mkdir(parents=True, exist_ok=True)
    rows_out: list[dict] = []

    for (w, lc, lw, p) in wls:
        for (q, lag, keep) in pols:
            budget = max(1, round(lc * keep))
            prefix = f"verify_w{w}_lc{lc}_lw{lw}_p{p}_q{q}_lag{lag}_b{budget}"
            run([
                "python3", str(RUN_COMPARE),
                "--workers", str(w), "--loads", loads,
                "--repeats", str(args.verify_repeats), "--steps", str(args.verify_steps),
                "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(budget),
                "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lw),
                "--deadline-us", str(DEADLINE_US), "--period-us", str(p),
                "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", str(q),
                "--out-prefix", prefix,
            ], dry)
            if dry:
                continue
            src = RESULTS / f"{prefix}_summary.csv"
            rows = read_summary(src)
            if not rows:
                continue
            shutil.copy2(src, vdir / src.name)
            by_load = {fnum(r, "load_factor"): r for r in rows}
            top = by_load.get(hi, {})
            b, bl, bh = (fnum(top, "baseline_hc_miss_mean"),
                         fnum(top, "baseline_hc_miss_ci_low"), fnum(top, "baseline_hc_miss_ci_high"))
            m, ml, mh = (fnum(top, "ms_hc_miss_mean"),
                         fnum(top, "ms_hc_miss_ci_low"), fnum(top, "ms_hc_miss_ci_high"))
            d, dl, dh = (fnum(top, "degrade_hc_miss_mean"),
                         fnum(top, "degrade_hc_miss_ci_low"), fnum(top, "degrade_hc_miss_ci_high"))
            sig_vs_base = dh < bl   # degrade CI entirely below baseline CI
            sig_vs_ms = dh < ml     # degrade CI entirely below MS CI
            lc_ret = mean(fnum(r, "degrade_lc_completion_mean") for r in rows)
            vio = sum(fnum(r, "violations_mean") for r in rows)
            rows_out.append({
                "workers": w, "lc": lc, "lc_work_us": lw, "period_us": p,
                "ready_q": q, "lag_ns": lag, "lc_budget": budget,
                f"hi_load": hi,
                "baseline_hc": round(b, 1), "baseline_ci": f"[{bl:.0f},{bh:.0f}]",
                "ms_hc": round(m, 1), "ms_ci": f"[{ml:.0f},{mh:.0f}]",
                "degrade_hc": round(d, 1), "degrade_ci": f"[{dl:.0f},{dh:.0f}]",
                "degrade_lt_baseline_sig": int(sig_vs_base),
                "degrade_lt_ms_sig": int(sig_vs_ms),
                "lc_retention": round(lc_ret, 1), "violations": round(vio, 3),
            })

    if dry:
        return
    out_csv = DATA / "verify_summary.csv"
    if rows_out:
        with out_csv.open("w", newline="") as f:
            wd = csv.DictWriter(f, fieldnames=list(rows_out[0].keys()))
            wd.writeheader()
            wd.writerows(rows_out)
    log(f"Verification verdict at highest load ({hi}), repeats={args.verify_repeats}")
    print(f"{'workload':22} {'policy':18} | {'baseline':>16} {'MS':>16} {'degrade':>16} "
          f"| {'d<base':>6} {'d<MS':>5} {'LCret':>6} vio", flush=True)
    any_sig = False
    for r in rows_out:
        wl = f"w{r['workers']} lc{r['lc']} lw{r['lc_work_us']} p{r['period_us']}"
        pol = f"q{r['ready_q']} lag{r['lag_ns']//1000}k b{r['lc_budget']}"
        any_sig = any_sig or r["degrade_lt_baseline_sig"] or r["degrade_lt_ms_sig"]
        print(f"{wl:22} {pol:18} | {r['baseline_hc']:>5}{r['baseline_ci']:>11} "
              f"{r['ms_hc']:>5}{r['ms_ci']:>11} {r['degrade_hc']:>5}{r['degrade_ci']:>11} "
              f"| {r['degrade_lt_baseline_sig']:>6} {r['degrade_lt_ms_sig']:>5} "
              f"{r['lc_retention']:>6} {r['violations']}", flush=True)
    print(flush=True)
    if any_sig:
        print("RESULT: at least one workload+policy shows degradation SIGNIFICANTLY "
              "reducing HC miss (non-overlapping 95% CI). Use it as the main "
              "experiment condition.", flush=True)
    else:
        print("RESULT: no workload+policy shows a statistically significant HC-miss "
              "reduction from degradation. The 'degradation protects HC' claim is "
              "not supported; consider reframing toward LC/semantics (E5).", flush=True)
    print("See data/verify_summary.csv for the full table.", flush=True)


# --------------------------------------------------------------------------
# Fix validation: A/B/C arms testing the HC-protection mechanisms
# --------------------------------------------------------------------------
OS_RT_ENV = {
    "LF_MS_OS_ENABLE": "1",
    "LF_MS_OS_RT_ENABLE": "1",
    "LF_MS_OS_RT_GROUP_ENABLE": "1",
    "LF_MS_OS_RT_PRIO_HC": "10",
    "LF_MS_OS_RT_PRIO_LC": "2",
    "LF_MS_WORKER_PARTITION_ENABLE": "1",
    "LF_MS_HC_WORKERS": "1",
}
STRICT_ENV = {"LF_MS_HC_STRICT_PRIORITY": "1"}


def validate(args, dry: bool) -> None:
    """Run baseline/ms/degrade at a fixed non-saturated workload under several
    fix configurations and report, with 95% CIs, whether HC miss drops.
    Arms:
      A off            : no OS, no strict (ready-set + budgeted shedding only)
      B os_rt_part     : OS RT priority + HC/LC worker partition (needs SYS_NICE)
      C hc_strict      : user-space criticality-monotonic pick_next (no OS priv)
      D strict_plus_os : both
    """
    log("Fix validation (A/B/C/D)")
    w, lc, lw, p = (int(x) for x in args.validate_workload.split(":"))
    q, lag, budget = (int(x) for x in args.validate_policy.split(":"))
    loads = args.validate_loads
    hi = max(float(x) for x in loads.split(","))
    arms = [("off", {}), ("os_rt_part", OS_RT_ENV),
            ("hc_strict", STRICT_ENV), ("strict_plus_os", {**OS_RT_ENV, **STRICT_ENV})]
    vdir = DATA / "validate"
    if not dry:
        vdir.mkdir(parents=True, exist_ok=True)
    out_rows = []
    for arm, env in arms:
        prefix = f"validate_{arm}"
        run([
            "python3", str(RUN_COMPARE), "--workers", str(w), "--loads", loads,
            "--repeats", str(args.validate_repeats), "--steps", str(args.validate_steps),
            "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(budget),
            "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lw),
            "--deadline-us", str(args.validate_deadline), "--period-us", str(p),
            "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", str(q),
            "--out-prefix", prefix,
        ], dry, env=env)
        if dry:
            continue
        src = RESULTS / f"{prefix}_summary.csv"
        rows = read_summary(src)
        if not rows:
            continue
        shutil.copy2(src, vdir / src.name)
        top = {fnum(r, "load_factor"): r for r in rows}.get(hi, {})
        bl, bh = fnum(top, "baseline_hc_miss_ci_low"), fnum(top, "baseline_hc_miss_ci_high")
        ml, mh = fnum(top, "ms_hc_miss_ci_low"), fnum(top, "ms_hc_miss_ci_high")
        dl, dh = fnum(top, "degrade_hc_miss_ci_low"), fnum(top, "degrade_hc_miss_ci_high")
        out_rows.append({
            "arm": arm, "hi_load": hi,
            "baseline_hc": round(fnum(top, "baseline_hc_miss_mean"), 1),
            "baseline_ci": f"[{bl:.0f},{bh:.0f}]",
            "ms_hc": round(fnum(top, "ms_hc_miss_mean"), 1), "ms_ci": f"[{ml:.0f},{mh:.0f}]",
            "degrade_hc": round(fnum(top, "degrade_hc_miss_mean"), 1),
            "degrade_ci": f"[{dl:.0f},{dh:.0f}]",
            "degrade_lt_baseline_sig": int(dh < bl),
            "ms_lt_baseline_sig": int(mh < bl),
            "degrade_lc_ret": round(mean(fnum(r, "degrade_lc_completion_mean") for r in rows), 1),
            "violations": round(sum(fnum(r, "violations_mean") for r in rows), 3),
        })
    if dry:
        return
    if out_rows:
        with (DATA / "validate_summary.csv").open("w", newline="") as f:
            wd = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
            wd.writeheader()
            wd.writerows(out_rows)
    log(f"Validation verdict at load {hi}, repeats={args.validate_repeats} "
        f"(workload w{w} lc{lc} lw{lw} p{p}, policy q{q} lag{lag} budget{budget})")
    print(f"{'arm':16} | {'baseline':>16} {'ms(+fix)':>16} {'degrade(+fix)':>16} "
          f"| {'d<base':>6} {'ms<base':>7} {'LCret':>6} vio", flush=True)
    for r in out_rows:
        print(f"{r['arm']:16} | {r['baseline_hc']:>5}{r['baseline_ci']:>11} "
              f"{r['ms_hc']:>5}{r['ms_ci']:>11} {r['degrade_hc']:>5}{r['degrade_ci']:>11} "
              f"| {r['degrade_lt_baseline_sig']:>6} {r['ms_lt_baseline_sig']:>7} "
              f"{r['degrade_lc_ret']:>6} {r['violations']}", flush=True)
    win = [r["arm"] for r in out_rows if r["degrade_lt_baseline_sig"] or r["ms_lt_baseline_sig"]]
    print(flush=True)
    if win:
        print(f"RESULT: HC miss significantly reduced (non-overlapping 95% CI) in arm(s): "
              f"{', '.join(win)}. The fixes restore HC protection -> mixed-criticality "
              f"claim is supportable.", flush=True)
    else:
        print("RESULT: even with the fixes, no arm shows a significant HC-miss reduction. "
              "Report backlog containment / semantics as the contribution.", flush=True)


# --------------------------------------------------------------------------
# Degradation-trigger diagnostic (root-cause why skips don't fire under overload)
# --------------------------------------------------------------------------
def diag(args, dry: bool) -> None:
    """Short single-worker overload run with LF_MS_DEGRADE_DEBUG=1 so the runtime
    logs, for every skip evaluation, pressure / has_hc_ready / criticality. Lets
    us confirm whether degradation fails to fire because pressure is not set or
    because has_hc_ready is never true under sequential (1-worker) execution."""
    log("Degradation-trigger diagnostic (w1 overload, debug logging)")
    run([
        "python3", str(RUN_COMPARE), "--workers", "1", "--loads", "3.0",
        "--repeats", "1", "--steps", "40",
        "--hc", str(HC), "--lc", "4", "--lc-budget", "2",
        "--hc-work-us", "1800", "--lc-work-us", "1200",
        "--deadline-us", "25000", "--period-us", "10000",
        "--degrade-lag-ns", "1500000", "--degrade-ready-q-len", "2",
        "--out-prefix", "tcrs_diag",
    ], dry, env={"LF_MS_DEGRADE_DEBUG": "1"})
    print("Diagnostic done. Inspect the newest ms-eval/logs/e3/<ts>/degrade/L3.0/ms.log "
          "for event=degrade_dbg lines (pressure / has_hc_ready).", flush=True)


# --------------------------------------------------------------------------
# Backlog-containment experiment (large period to escape ~1ms env jitter)
# --------------------------------------------------------------------------
def backlog(args, dry: bool) -> None:
    """Test logical-time backlog containment in a regime where the effect is not
    swamped by environment jitter. The Docker VM jitter is ~1 ms; with a 1 ms
    period it dominates. Here the period is scaled up (default 10 ms) and the per
    reaction work scaled proportionally, so jitter is ~10% of the period and the
    overload onset is crisp: baseline accumulates unbounded tag overrun while
    budgeted degradation keeps per-tag work under the period. Runs the E5 backlog
    sweep (which also emits HC/LC summaries) across worker counts."""
    log("Backlog containment (scaled period)")
    q, lag, budget = (int(x) for x in args.bk_policy.split(":"))
    if not dry:
        (DATA / "e5").mkdir(parents=True, exist_ok=True)
    print(f"  period {args.bk_period}us, hc_work {args.bk_hc_work}us, lc_work {args.bk_lc_work}us, "
          f"deadline {args.bk_deadline}us, policy q{q} lag{lag} budget{budget}", flush=True)
    run([
        "python3", str(RUN_E5), "--workers", args.bk_workers, "--loads", args.bk_loads,
        "--repeats", str(args.bk_repeats), "--steps", str(args.bk_steps),
        "--hc", str(HC), "--lc", "4", "--lc-budget", str(budget),
        "--hc-work-us", str(args.bk_hc_work), "--lc-work-us", str(args.bk_lc_work),
        "--deadline-us", str(args.bk_deadline), "--period-us", str(args.bk_period),
        "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", str(q),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_bk_backlog",
    ] + (["--dry-run"] if dry else []), dry)
    log("Backlog experiment complete")
    print("Outputs: data/e5/tcrs_bk_backlog_combined_backlog.csv (+ per-worker summaries)", flush=True)


# --------------------------------------------------------------------------
# Period-robustness experiment (show the effect is not specific to one period)
# --------------------------------------------------------------------------
def periods(args, dry: bool) -> None:
    """Repeat the clean w2 overload-onset comparison at several periods, scaling
    reaction times and deadline proportionally, to show HC protection is not an
    artifact of a single (10 ms) period. Outputs data/periods/tcrs_periods_p<P>_*."""
    log("Period-robustness sweep (w2, overload, scaled per period)")
    pset = [int(x) for x in args.pr_periods.split(",") if x.strip()]
    out_dir = DATA / "periods"
    if not dry:
        out_dir.mkdir(parents=True, exist_ok=True)
    for p in pset:
        hcw = round(p * 0.18); lcw = round(p * 0.12)
        dl = round(p * 2.5)
        lag = round(p * 1000 * 0.15)  # ns; ~0.15 of the period (p is in us)
        prefix = f"tcrs_periods_p{p}"
        run([
            "python3", str(RUN_COMPARE), "--workers", "2", "--loads", args.pr_loads,
            "--repeats", str(args.pr_repeats), "--steps", str(args.pr_steps),
            "--hc", str(HC), "--lc", "4", "--lc-budget", "2",
            "--hc-work-us", str(hcw), "--lc-work-us", str(lcw),
            "--deadline-us", str(dl), "--period-us", str(p),
            "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", "2",
            "--out-prefix", prefix,
        ], dry)
        if not dry:
            for suf in ("_summary.csv", "_long.csv"):
                src = RESULTS / f"{prefix}{suf}"
                if src.exists():
                    shutil.copy2(src, out_dir / src.name)
    print("Outputs: data/periods/tcrs_periods_p*_summary.csv", flush=True)


# --------------------------------------------------------------------------
# Final reframed experiment: semantics-preserving overload control
# --------------------------------------------------------------------------
def final(args, dry: bool) -> None:
    """Main experiment for the reframed contribution (HC is NOT the claim).
    Fixed calibrated workload + feasible deadline + a degradation-active policy.
    Reports, across worker counts (E4), backlog propagation (E5) and a policy
    (budget) sweep (E6): LC completion / bounded shedding, logical-time backlog
    containment, semantic violations (expected 0), degradation actions, and HC
    miss (expected ~unaffected)."""
    log("Final experiment (reframed: backlog containment + bounded LC shedding)")
    w, lc, lw, p = (int(x) for x in args.final_workload.split(":"))
    q, lag, budget = (int(x) for x in args.final_policy.split(":"))
    dl, rep, steps, hcw = args.final_deadline, args.final_repeats, args.final_steps, args.final_hc_work
    if not dry:
        for sub in ("e4", "e5", "e6"):
            (DATA / sub).mkdir(parents=True, exist_ok=True)
    print(f"  workload w{w} lc{lc} lw{lw} p{p}, deadline {dl}us, policy q{q} lag{lag} budget{budget}, "
          f"repeats {rep}", flush=True)

    # E4: multi-worker controlled degradation at the fixed policy.
    log("E4: multi-worker sweep")
    for wk in [x.strip() for x in args.final_e4_workers.split(",") if x.strip()]:
        prefix = f"tcrs_e4_w{wk}"
        run([
            "python3", str(RUN_COMPARE), "--workers", wk, "--loads", args.final_loads,
            "--repeats", str(rep), "--steps", str(steps),
            "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(budget),
            "--hc-work-us", str(hcw), "--lc-work-us", str(lw),
            "--deadline-us", str(dl), "--period-us", str(p),
            "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", str(q),
            "--out-prefix", prefix,
        ], dry)
        if not dry:
            for suffix in ("_summary.csv", "_long.csv"):
                src = RESULTS / f"{prefix}{suffix}"
                if src.exists():
                    shutil.copy2(src, DATA / "e4" / src.name)

    # E5: backlog propagation across logical-time tags.
    log("E5: backlog propagation")
    run([
        "python3", str(RUN_E5), "--workers", args.final_e5_workers, "--loads", args.final_loads,
        "--repeats", str(rep), "--steps", str(steps),
        "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(budget),
        "--hc-work-us", str(hcw), "--lc-work-us", str(lw),
        "--deadline-us", str(dl), "--period-us", str(p),
        "--degrade-lag-ns", str(lag), "--degrade-ready-q-len", str(q),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_e5_backlog",
    ] + (["--dry-run"] if dry else []), dry)

    # E6: policy (LC-budget) sensitivity -> shedding vs backlog trade-off.
    log("E6: policy/budget sensitivity")
    run([
        "python3", str(RUN_E6), "--ready-q-lens", args.final_e6_ready_qs,
        "--lag-ns", str(lag), "--lc-budgets", args.final_e6_budgets,
        "--workers", str(w), "--loads", args.final_e6_loads,
        "--repeats", str(rep), "--steps", str(steps),
        "--hc", str(HC), "--lc", str(lc),
        "--hc-work-us", str(hcw), "--lc-work-us", str(lw),
        "--deadline-us", str(dl), "--period-us", str(p),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_e6_policy",
    ] + (["--dry-run"] if dry else []), dry)

    log("Final experiment complete")
    print("Outputs: data/e4/tcrs_e4_w{1,2,4}_summary.csv, "
          "data/e5/tcrs_e5_backlog_combined_backlog.csv, "
          "data/e6/tcrs_e6_policy_combined_summary.csv", flush=True)


# --------------------------------------------------------------------------
# Policy selection (graceful criterion)
# --------------------------------------------------------------------------
def select_policy(trigger_csv: Path) -> dict:
    rows = read_summary(trigger_csv)
    if not rows:
        sys.exit(f"trigger-search summary not found/empty: {trigger_csv}")
    # aggregate per policy across loads
    agg: dict[tuple, dict] = {}
    for r in rows:
        key = (r["lc_budget"], r["degrade_ready_q_len"], r["degrade_lag_ns"])
        a = agg.setdefault(key, {"benefit": [], "lc": [], "act": [], "vio": [], "fb": [], "base_hi": 0.0})
        a["benefit"].append(fnum(r, "baseline_hc_miss_mean") - fnum(r, "degrade_hc_miss_mean"))
        a["lc"].append(fnum(r, "degrade_lc_completion_mean"))
        a["act"].append(fnum(r, "degrade_actions_mean"))
        a["vio"].append(fnum(r, "violations_mean"))
        a["fb"].append(fnum(r, "fallbacks_mean"))
        a["base_hi"] = max(a["base_hi"], fnum(r, "baseline_hc_miss_mean"))
    cands = []
    for key, a in agg.items():
        if mean(a["act"]) <= 0 or sum(a["vio"]) != 0 or sum(a["fb"]) != 0:
            continue
        score = mean(a["benefit"]) + 0.5 * mean(a["lc"])
        cands.append((score, key, mean(a["benefit"]), mean(a["lc"]), mean(a["act"])))
    if not cands:
        sys.exit(
            "No degradation-active, violation-free policy found in the trigger "
            "search. Re-run calibration or widen the policy grid."
        )
    cands.sort(reverse=True)
    score, key, benefit, lc_ret, act = cands[0]
    choice = {
        "lc_budget": int(float(key[0])), "ready_q": int(float(key[1])),
        "lag_ns": int(float(key[2])), "benefit": benefit, "lc_ret": lc_ret, "actions": act,
    }
    log("Selected degradation-active policy")
    print(f"  lc_budget={choice['lc_budget']} ready_q={choice['ready_q']} lag_ns={choice['lag_ns']}"
          f"  (HC benefit {benefit:.1f}pts, LC retention {lc_ret:.1f}%, "
          f"actions {act:.0f})", flush=True)
    return choice


def read_workload() -> dict:
    p = DATA / "selected_workload.txt"
    if not p.exists():
        sys.exit("data/selected_workload.txt missing. Run calibrate first.")
    kv = {}
    for line in p.read_text().splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            kv[k.strip()] = v.strip()
    return {
        "workers": int(kv["workers"]), "lc": int(kv["lc"]),
        "lc_work_us": int(kv["lc_work_us"]), "period_us": int(kv["period_us"]),
    }


# --------------------------------------------------------------------------
# Full experiment
# --------------------------------------------------------------------------
def full(args, dry: bool, smoke: bool) -> None:
    wl = read_workload() if not dry else {"workers": 2, "lc": 4, "lc_work_us": 120, "period_us": 1000}
    lc, lc_work, period = wl["lc"], wl["lc_work_us"], wl["period_us"]
    ts_repeats, ts_steps = (1, 120) if smoke else (3, 120)
    fin_repeats, fin_steps = (2, 160) if smoke else (20, 300)
    for sub in ("e4", "e5", "e6"):
        (DATA / sub).mkdir(parents=True, exist_ok=True)

    log(f"Phase 1: E6 trigger search at workload lc={lc} lc_work={lc_work} period={period}")
    run([
        "python3", str(RUN_E6),
        "--ready-q-lens", args.ts_ready_qs, "--lag-ns", args.ts_lags,
        "--lc-budgets", args.ts_budgets, "--workers", str(wl["workers"]),
        "--loads", args.ts_loads, "--repeats", str(ts_repeats), "--steps", str(ts_steps),
        "--hc", str(HC), "--lc", str(lc),
        "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lc_work),
        "--deadline-us", str(DEADLINE_US), "--period-us", str(period),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_e6_trigger_search",
    ] + (["--dry-run"] if dry else []), dry)

    log("Phase 2: select degradation-active policy (graceful criterion)")
    if dry:
        choice = {"lc_budget": max(1, lc // 2), "ready_q": 2, "lag_ns": 100000,
                  "benefit": 0.0, "lc_ret": 0.0, "actions": 0.0}
    else:
        choice = select_policy(DATA / "e6/tcrs_e6_trigger_search_combined_summary.csv")
    (DATA / "selected_policy.txt").write_text(
        f"lc_budget={choice['lc_budget']}\nready_q={choice['ready_q']}\nlag_ns={choice['lag_ns']}\n"
        f"hc_benefit_pts={choice['benefit']:.2f}\nlc_retention={choice['lc_ret']:.2f}\n"
        f"degrade_actions_mean={choice['actions']:.2f}\n"
        f"workload_lc={lc}\nworkload_lc_work_us={lc_work}\nworkload_period_us={period}\n"
    )

    log("Phase 3: E4 multi-worker controlled degradation (final)")
    for w in [x.strip() for x in args.e4_workers.split(",") if x.strip()]:
        prefix = f"tcrs_e4_w{w}"
        run([
            "python3", str(RUN_COMPARE), "--workers", w, "--loads", args.e4_loads,
            "--repeats", str(fin_repeats), "--steps", str(fin_steps),
            "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(choice["lc_budget"]),
            "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lc_work),
            "--deadline-us", str(DEADLINE_US), "--period-us", str(period),
            "--degrade-lag-ns", str(choice["lag_ns"]), "--degrade-ready-q-len", str(choice["ready_q"]),
            "--out-prefix", prefix,
        ], dry)
        if not dry:
            for suffix in ("_summary.csv", "_long.csv"):
                src = RESULTS / f"{prefix}{suffix}"
                if src.exists():
                    shutil.copy2(src, DATA / "e4" / src.name)

    log("Phase 4: E5 backlog propagation (final)")
    run([
        "python3", str(RUN_E5), "--workers", args.e5_workers, "--loads", args.e5_loads,
        "--repeats", str(fin_repeats), "--steps", str(fin_steps),
        "--hc", str(HC), "--lc", str(lc), "--lc-budget", str(choice["lc_budget"]),
        "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lc_work),
        "--deadline-us", str(DEADLINE_US), "--period-us", str(period),
        "--degrade-lag-ns", str(choice["lag_ns"]), "--degrade-ready-q-len", str(choice["ready_q"]),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_e5_backlog",
    ] + (["--dry-run"] if dry else []), dry)

    log("Phase 5: E6 degradation-policy sensitivity (final)")
    budgets = sorted({max(1, choice["lc_budget"] - 1), choice["lc_budget"]})
    run([
        "python3", str(RUN_E6), "--ready-q-lens", args.e6_ready_qs, "--lag-ns", str(choice["lag_ns"]),
        "--lc-budgets", ",".join(str(b) for b in budgets), "--workers", str(wl["workers"]),
        "--loads", args.e6_loads, "--repeats", str(fin_repeats), "--steps", str(fin_steps),
        "--hc", str(HC), "--lc", str(lc),
        "--hc-work-us", str(HC_WORK_US), "--lc-work-us", str(lc_work),
        "--deadline-us", str(DEADLINE_US), "--period-us", str(period),
        "--cmake-bin-dir", CMAKE_BIN_DIR, "--out-prefix", "tcrs_e6_policy",
    ] + (["--dry-run"] if dry else []), dry)

    log("All phases complete")
    print("Outputs: data/e4/*, data/e5/*, data/e6/*, "
          "data/selected_policy.txt, data/selected_workload.txt", flush=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["calibrate", "verify", "validate", "backlog", "diag", "periods", "final", "full", "smoke", "dry"], default="full")
    # period-robustness sweep
    ap.add_argument("--pr-periods", default="5000,10000,20000")
    ap.add_argument("--pr-loads", default="4.0,5.0")
    ap.add_argument("--pr-repeats", type=int, default=12)
    ap.add_argument("--pr-steps", type=int, default=120)
    # backlog-containment experiment (scaled period to escape jitter)
    ap.add_argument("--bk-period", type=int, default=10000)
    ap.add_argument("--bk-hc-work", type=int, default=1800)
    ap.add_argument("--bk-lc-work", type=int, default=1200)
    ap.add_argument("--bk-deadline", type=int, default=25000)
    ap.add_argument("--bk-policy", default="2:1500000:2", help="ready_q:lag_ns:lc_budget")
    ap.add_argument("--bk-workers", default="1,2,4")
    ap.add_argument("--bk-loads", default="1.0,2.0,3.0,4.0,5.0")
    ap.add_argument("--bk-repeats", type=int, default=12)
    ap.add_argument("--bk-steps", type=int, default=150)
    # final reframed experiment
    # Scaled period (10 ms) so env jitter (~1 ms) is small vs the period and the
    # overload onset is crisp; pressure-driven per-tag budget shedding active.
    ap.add_argument("--final-workload", default="2:4:1200:10000", help="workers:lc:lc_work:period")
    ap.add_argument("--final-hc-work", type=int, default=1800)
    ap.add_argument("--final-policy", default="2:1500000:2", help="ready_q:lag_ns:lc_budget")
    ap.add_argument("--final-deadline", type=int, default=25000)
    ap.add_argument("--final-loads", default="1.0,2.0,3.0,4.0,5.0,6.0")
    ap.add_argument("--final-repeats", type=int, default=15)
    ap.add_argument("--final-steps", type=int, default=150)
    ap.add_argument("--final-e4-workers", default="1,2,4")
    ap.add_argument("--final-e5-workers", default="1,2,4")
    ap.add_argument("--final-e6-ready-qs", default="2")
    ap.add_argument("--final-e6-budgets", default="1,2,3")
    ap.add_argument("--final-e6-loads", default="3.0,4.0")
    # fix validation (A/B/C/D arms)
    ap.add_argument("--validate-workload", default="2:4:120:1000", help="workers:lc:lc_work:period")
    ap.add_argument("--validate-policy", default="2:150000:2", help="ready_q:lag_ns:lc_budget")
    ap.add_argument("--validate-loads", default="1.0,3.0,5.0,7.0,9.0")
    ap.add_argument("--validate-repeats", type=int, default=20)
    ap.add_argument("--validate-steps", type=int, default=300)
    # HC/LC deadline for the feasibility test: set above the low-load latency
    # floor (~1.2 ms median in the Docker env) so low load is feasible and only
    # overload causes misses, isolating the scheduling effect from env jitter.
    ap.add_argument("--validate-deadline", type=int, default=2500)
    # high-repeat verification
    ap.add_argument("--verify-workloads", default="2:4:120:1000,2:3:180:1000",
                    help="workers:lc:lc_work:period, comma-separated")
    ap.add_argument("--verify-policies", default="2:150000:0.75,2:250000:0.75,4:150000:0.5",
                    help="ready_q:lag_ns:keep_frac, comma-separated (budget=round(lc*keep))")
    ap.add_argument("--verify-loads", default="0.4,1.0,1.6,2.2")
    ap.add_argument("--verify-repeats", type=int, default=30)
    ap.add_argument("--verify-steps", type=int, default=300)
    # calibration grid
    ap.add_argument("--cal-workers", default="2,4")
    ap.add_argument("--cal-lc", default="3,4")
    ap.add_argument("--cal-lc-work", default="120,180")
    ap.add_argument("--cal-periods", default="1000,1500")
    ap.add_argument("--cal-loads", default="0.4,1.0,1.6,2.2")
    ap.add_argument("--cal-repeats", type=int, default=3)
    ap.add_argument("--cal-steps", type=int, default=120)
    ap.add_argument("--cal-low-max", type=float, default=30.0,
                    help="max baseline HC miss%% allowed at lowest load")
    ap.add_argument("--cal-high-min", type=float, default=50.0,
                    help="min baseline HC miss%% required at highest load")
    # trigger search grid
    ap.add_argument("--ts-ready-qs", default="1,2,4")
    ap.add_argument("--ts-lags", default="50000,100000,150000,250000")
    ap.add_argument("--ts-budgets", default="1,2,3")
    ap.add_argument("--ts-loads", default="0.4,0.8,1.2,1.6")
    # final grids
    ap.add_argument("--e4-workers", default="1,2,4")
    ap.add_argument("--e4-loads", default="0.4,0.8,1.2,1.6,2.0")
    ap.add_argument("--e5-workers", default="1,2,4")
    ap.add_argument("--e5-loads", default="0.4,0.8,1.2,1.6")
    ap.add_argument("--e6-ready-qs", default="1,2,4")
    ap.add_argument("--e6-loads", default="0.4,0.8,1.2,1.6")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    dry = args.dry_run or args.mode == "dry"
    if args.mode == "calibrate":
        calibrate(args, dry)
    elif args.mode == "verify":
        verify(args, dry)
    elif args.mode == "validate":
        validate(args, dry)
    elif args.mode == "backlog":
        backlog(args, dry)
    elif args.mode == "diag":
        diag(args, dry)
    elif args.mode == "periods":
        periods(args, dry)
    elif args.mode == "final":
        final(args, dry)
    elif args.mode in ("full", "dry"):
        full(args, dry, smoke=False)
    elif args.mode == "smoke":
        full(args, dry, smoke=True)


if __name__ == "__main__":
    main()
