#!/usr/bin/env python3
import argparse
from pathlib import Path
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="ms-eval/results/e3_missrate.csv")
    ap.add_argument("--out", default="ms-eval/figures/fig3_missrate.pdf")
    args = ap.parse_args()

    data = {}
    with open(args.inp, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            mode = row["mode"]
            load = float(row["load_factor"])
            miss = float(row["hc_miss_rate"])
            data.setdefault(mode, {})[load] = miss

    plt.figure(figsize=(6, 3.5))
    for mode, series in data.items():
        xs = sorted(series.keys())
        ys = [series[x] for x in xs]
        plt.plot(xs, ys, marker="o", label=mode)

    plt.xlabel("Load factor (L)")
    plt.ylabel("High-criticality miss rate (%)")
    plt.title("Fig3: HC Miss Rate vs Load")
    plt.legend()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(args.out)


if __name__ == "__main__":
    main()
