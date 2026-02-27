#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
from collections import defaultdict

from common_parse import read_jsonl, parse_ms_log, write_csv


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="ms-eval/logs/e3")
    ap.add_argument("--out", default="ms-eval/results/e3_missrate.csv")
    ap.add_argument("--drop-initial-tags", type=int, default=0)
    args = ap.parse_args()

    log_root = Path(args.logs)
    rows = []

    for run_dir in sorted(log_root.glob("*/**/")):
        config_path = run_dir / "run_config.json"
        app_log = run_dir / "app.jsonl"
        ms_log = run_dir / "ms.log"
        if not config_path.exists() or not app_log.exists():
            continue
        config = json.loads(config_path.read_text())
        hc = config.get("hc", 0)
        steps = config.get("steps", 0)
        mode = config.get("mode", "")
        load_factor = config.get("load_factor", 1.0)

        app_events = list(read_jsonl(app_log))
        hc_events = []
        for e in app_events:
            if e.get("type") != "reaction_end":
                continue
            rid = int(e.get("reaction_id", -1))
            if rid < 0 or rid >= hc:
                continue
            logical_time_ns = int(e.get("logical_time_ns", -1))
            missed = int(e.get("missed_deadline", 0))
            hc_events.append((logical_time_ns, missed))

        if args.drop_initial_tags > 0 and hc_events:
            tags = sorted({t for (t, _) in hc_events if t >= 0})
            if 0 < args.drop_initial_tags < len(tags):
                threshold = tags[args.drop_initial_tags]
                hc_events = [(t, m) for (t, m) in hc_events if t >= threshold]

        total_hc = len(hc_events)
        misses_hc = sum(1 for (_, missed) in hc_events if missed == 1)

        ms_events = parse_ms_log(ms_log)
        degraded = 0
        violations = 0
        fallback_count = 0
        for e in ms_events:
            ev = e.get("event")
            if ev == "degrade":
                degraded += 1
            elif ev == "mismatch":
                violations += 1
            elif ev == "pick_next":
                try:
                    candidate = int(e.get("candidate", "-1"))
                except ValueError:
                    candidate = -1
                if candidate < 0:
                    fallback_count += 1

        miss_rate = (misses_hc / total_hc) * 100.0 if total_hc else 0.0

        rows.append([
            mode,
            f"{load_factor:.2f}",
            steps,
            f"{miss_rate:.3f}",
            degraded,
            fallback_count,
            violations,
        ])

    header = [
        "mode",
        "load_factor",
        "steps",
        "hc_miss_rate",
        "lc_degraded_count",
        "fallback_count",
        "violations",
    ]
    write_csv(Path(args.out), header, rows)


if __name__ == "__main__":
    main()
