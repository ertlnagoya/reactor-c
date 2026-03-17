#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="input_csv", required=True)
    ap.add_argument("--out-png", required=True)
    ap.add_argument("--out-pdf", required=False)
    args = ap.parse_args()

    loads = []
    baseline_hc = []
    ms_hc = []
    degrade_hc = []
    baseline_lc = []
    ms_lc = []
    degrade_lc = []

    with Path(args.input_csv).open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            loads.append(float(row["load_factor"]))
            baseline_hc.append(float(row["baseline_hc_miss_mean"]))
            ms_hc.append(float(row["ms_hc_miss_mean"]))
            degrade_hc.append(float(row["degrade_hc_miss_mean"]))
            baseline_lc.append(float(row["baseline_lc_completion_mean"]))
            ms_lc.append(float(row["ms_lc_completion_mean"]))
            degrade_lc.append(float(row["degrade_lc_completion_mean"]))

    fig, axes = plt.subplots(1, 2, figsize=(10, 4), constrained_layout=True)

    axes[0].plot(loads, baseline_hc, marker="o", label="LF baseline")
    axes[0].plot(loads, ms_hc, marker="s", label="MS only")
    axes[0].plot(loads, degrade_hc, marker="^", label="MS + degrade")
    axes[0].set_xlabel("LC load factor")
    axes[0].set_ylabel("HC miss rate (%)")
    axes[0].set_title("HC Protection")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    axes[1].plot(loads, baseline_lc, marker="o", label="LF baseline")
    axes[1].plot(loads, ms_lc, marker="s", label="MS only")
    axes[1].plot(loads, degrade_lc, marker="^", label="MS + degrade")
    axes[1].set_xlabel("LC load factor")
    axes[1].set_ylabel("LC completion ratio (%)")
    axes[1].set_title("Explicit LC Sacrifice")
    axes[1].grid(True, alpha=0.3)

    out_png = Path(args.out_png)
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png, dpi=200)
    if args.out_pdf:
        out_pdf = Path(args.out_pdf)
        out_pdf.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(out_pdf)


if __name__ == "__main__":
    main()
