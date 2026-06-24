#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path


def read_jsonl(path: Path):
    if not path.exists():
        return
    with path.open(errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def longest_true_run(values):
    best = 0
    cur = 0
    for v in values:
        if v:
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    return best


def summarize_run(run_dir: Path, period_us: int, drop_initial_tags: int):
    cfg_path = run_dir / "run_config.json"
    app_path = run_dir / "app.jsonl"
    if not cfg_path.exists() or not app_path.exists():
        return None

    cfg = json.loads(cfg_path.read_text())
    per_tag_end = {}
    per_tag_hc_end = {}
    hc = int(cfg.get("hc", 0))

    for event in read_jsonl(app_path):
        if event.get("type") != "reaction_end":
            continue
        logical = int(event.get("logical_time_ns", -1))
        end = int(event.get("ts_mono_ns", -1))
        rid = int(event.get("reaction_id", -1))
        if logical < 0 or end < logical:
            continue
        per_tag_end[logical] = max(per_tag_end.get(logical, end), end)
        if 0 <= rid < hc:
            per_tag_hc_end[logical] = max(per_tag_hc_end.get(logical, end), end)

    tags = sorted(per_tag_end)
    if drop_initial_tags > 0 and drop_initial_tags < len(tags):
        tags = tags[drop_initial_tags:]

    if not tags:
        return None

    period_ns = period_us * 1000
    tag_lat_us = [(per_tag_end[t] - t) / 1000.0 for t in tags]
    hc_tag_lat_us = [
        (per_tag_hc_end[t] - t) / 1000.0
        for t in tags
        if t in per_tag_hc_end
    ]
    overruns = [per_tag_end[t] > t + period_ns for t in tags]

    return {
        "timestamp": run_dir.parts[-3] if len(run_dir.parts) >= 3 else "",
        "mode": cfg.get("mode", ""),
        "load_factor": f"{float(cfg.get('load_factor', 1.0)):.2f}",
        "workers": cfg.get("workers", ""),
        "observed_tags": len(tags),
        "tag_completion_mean_us": f"{sum(tag_lat_us) / len(tag_lat_us):.3f}",
        "tag_completion_max_us": f"{max(tag_lat_us):.3f}",
        "hc_tag_completion_mean_us": (
            f"{sum(hc_tag_lat_us) / len(hc_tag_lat_us):.3f}"
            if hc_tag_lat_us else ""
        ),
        "tag_overrun_ratio": f"{(sum(overruns) / len(overruns)) * 100.0:.3f}",
        "longest_overrun_run": longest_true_run(overruns),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True, help="E3 timestamp log directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--period-us", type=int, default=1000)
    ap.add_argument("--drop-initial-tags", type=int, default=0)
    args = ap.parse_args()

    rows = []
    root = Path(args.logs)
    for run_dir in sorted(root.glob("*/*")):
        row = summarize_run(run_dir, args.period_us, args.drop_initial_tags)
        if row:
            rows.append(row)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "timestamp",
        "mode",
        "load_factor",
        "workers",
        "observed_tags",
        "tag_completion_mean_us",
        "tag_completion_max_us",
        "hc_tag_completion_mean_us",
        "tag_overrun_ratio",
        "longest_overrun_run",
    ]
    with out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
