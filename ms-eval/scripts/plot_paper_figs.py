#!/usr/bin/env python3
"""Generate the TCRS paper figures from the experiment CSVs.

Prefers the final-run prefixes (tcrs_e4_*, tcrs_e5_backlog_*) and falls back to
the backlog-run prefixes (tcrs_bk_backlog_*) so it can be run before/after the
final experiment. Outputs figures/fig_hc_miss.pdf and figures/fig_backlog.pdf.
"""
import csv
import os
import statistics as st
from collections import defaultdict

import subprocess

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def _make_xbb(pdf_path):
    # Generate a .xbb bounding-box file so the figure also works under an
    # upLaTeX + dvipdfmx (DVI) build, not only pdflatex. Run inside the figure
    # directory with a relative name to satisfy TeX's openout_any policy.
    d, base = os.path.split(pdf_path)
    try:
        subprocess.run(["extractbb", base], cwd=d or ".", check=False)
    except FileNotFoundError:
        pass

# reactor-c/ms-eval/scripts/ -> repo root is three levels up. Data defaults to
# ms-eval/tcrs-data; figures are written next to the data. Override with TCRS_DATA.
REACTOR_C = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.environ.get("TCRS_DATA", os.path.join(REACTOR_C, "ms-eval", "tcrs-data"))
FIG = os.path.join(DATA, "figures")
os.makedirs(FIG, exist_ok=True)


def f(row, k):
    try:
        return float(row[k])
    except (KeyError, ValueError, TypeError):
        return float("nan")


def worker_summary(w):
    for pref in ("tcrs_e4_w", "tcrs_bk_backlog_w"):
        p = os.path.join(DATA, "e5" if "bk" in pref else "e4", f"{pref}{w}_summary.csv")
        # e4 summaries live in data/e4 (final) ; backlog per-worker in data/e5
        if not os.path.exists(p):
            p = os.path.join(DATA, "e5", f"{pref}{w}_summary.csv")
        if os.path.exists(p):
            return list(csv.DictReader(open(p))), pref
    return None, None


def fig_hc_miss():
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    styles = {1: ("o", "1 worker"), 2: ("s", "2 workers"), 4: ("^", "4 workers")}
    for w in (1, 2, 4):
        rows, _ = worker_summary(w)
        if not rows:
            continue
        rows.sort(key=lambda r: f(r, "load_factor"))
        x = [f(r, "load_factor") for r in rows]
        base = [f(r, "baseline_hc_miss_mean") for r in rows]
        deg = [f(r, "degrade_hc_miss_mean") for r in rows]
        m, lab = styles[w]
        ax.plot(x, base, m + "--", color="tab:red", alpha=0.5 + 0.15 * (w == 2), markersize=4)
        ax.plot(x, deg, m + "-", color="tab:blue", markersize=4, label=lab)
    ax.set_xlabel("Load factor")
    ax.set_ylabel("HC deadline-miss rate (%)")
    ax.set_ylim(-5, 105)
    ax.text(0.02, 0.5, "dashed=baseline\nsolid=degrade", transform=ax.transAxes,
            fontsize=6, va="center")
    ax.legend(fontsize=6, loc="center right")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    out = os.path.join(FIG, "fig_hc_miss.pdf")
    fig.savefig(out)
    _make_xbb(out)
    print("wrote fig_hc_miss.pdf (+.xbb)")


def fig_backlog():
    # longest overrun run vs load, baseline vs degrade, per worker
    for name in ("tcrs_e5_backlog_combined_backlog.csv", "tcrs_bk_backlog_combined_backlog.csv"):
        p = os.path.join(DATA, "e5", name)
        if os.path.exists(p):
            break
    if not os.path.exists(p):
        print("no backlog combined csv; skip fig_backlog")
        return
    agg = defaultdict(lambda: defaultdict(list))
    for r in csv.DictReader(open(p)):
        try:
            k = (int(r["worker_setting"]), r["mode"], float(r["load_factor"]))
            agg[k]["run"].append(float(r["longest_overrun_run"]))
        except (KeyError, ValueError):
            pass
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    for w, col in ((2, "tab:blue"), (1, "tab:gray")):
        loads = sorted({k[2] for k in agg if k[0] == w})
        if not loads:
            continue
        b = [st.mean(agg[(w, "baseline", L)]["run"]) for L in loads]
        d = [st.mean(agg[(w, "degrade", L)]["run"]) for L in loads]
        ax.plot(loads, b, "s--", color=col, alpha=0.5, markersize=4, label=f"baseline w{w}")
        ax.plot(loads, d, "o-", color=col, markersize=4, label=f"degrade w{w}")
    ax.set_xlabel("Load factor")
    ax.set_ylabel("Longest overrun run (tags)")
    ax.legend(fontsize=6)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    out = os.path.join(FIG, "fig_backlog.pdf")
    fig.savefig(out)
    _make_xbb(out)
    print("wrote fig_backlog.pdf (+.xbb)")


if __name__ == "__main__":
    fig_hc_miss()
    fig_backlog()
