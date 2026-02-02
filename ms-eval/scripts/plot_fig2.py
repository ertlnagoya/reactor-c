#!/usr/bin/env python3
import argparse
from pathlib import Path
import csv

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", default="ms-eval/results/e1_overhead.csv")
    ap.add_argument("--out", default="ms-eval/figures/fig2_overhead.pdf")
    args = ap.parse_args()

    data = {}
    with open(args.inp, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            mode = row["mode"]
            N = int(row["N"])
            mean = float(row["mean_step_us"])
            data.setdefault(mode, {})[N] = mean

    modes = sorted(data.keys())
    Ns = sorted({n for m in data.values() for n in m.keys()})

    width = 0.25
    x = list(range(len(Ns)))

    plt.figure(figsize=(6, 3.5))
    for i, mode in enumerate(modes):
        vals = [data.get(mode, {}).get(n, 0) for n in Ns]
        plt.bar([p + i * width for p in x], vals, width=width, label=mode)

    plt.xticks([p + width for p in x], [str(n) for n in Ns])
    plt.xlabel("Ready set size (N)")
    plt.ylabel("Mean step time (us)")
    plt.title("Fig2: Overhead vs Ready Set Size")
    plt.legend()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    plt.tight_layout()
    plt.savefig(args.out)


if __name__ == "__main__":
    main()
