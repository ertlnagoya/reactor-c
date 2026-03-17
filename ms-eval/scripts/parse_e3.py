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
        lc = config.get("lc", 0)
        steps = config.get("steps", 0)
        mode = config.get("mode", "")
        load_factor = config.get("load_factor", 1.0)

        app_events = list(read_jsonl(app_log))
        hc_events = []
        lc_events = []
        for e in app_events:
            if e.get("type") != "reaction_end":
                continue
            rid = int(e.get("reaction_id", -1))
            if rid < 0:
                continue
            logical_time_ns = int(e.get("logical_time_ns", -1))
            missed = int(e.get("missed_deadline", 0))
            if rid < hc:
                hc_events.append((logical_time_ns, missed))
            elif rid < hc + lc:
                lc_events.append((logical_time_ns, missed))
            else:
                continue

        if args.drop_initial_tags > 0 and hc_events:
            tags = sorted({t for (t, _) in hc_events if t >= 0})
            if 0 < args.drop_initial_tags < len(tags):
                threshold = tags[args.drop_initial_tags]
                hc_events = [(t, m) for (t, m) in hc_events if t >= threshold]
                lc_events = [(t, m) for (t, m) in lc_events if t >= threshold]

        total_hc = len(hc_events)
        misses_hc = sum(1 for (_, missed) in hc_events if missed == 1)
        observed_tags = sorted({t for (t, _) in hc_events if t >= 0})
        expected_lc = len(observed_tags) * lc
        completed_lc = len(lc_events)
        lc_completion_ratio = (completed_lc / expected_lc) * 100.0 if expected_lc else 0.0
        hc_latencies_us = []
        for e in app_events:
            if e.get("type") != "reaction_end":
                continue
            rid = int(e.get("reaction_id", -1))
            if rid < 0 or rid >= hc:
                continue
            logical_time_ns = int(e.get("logical_time_ns", -1))
            ts_mono_ns = int(e.get("ts_mono_ns", -1))
            if logical_time_ns < 0 or ts_mono_ns < logical_time_ns:
                continue
            hc_latencies_us.append((ts_mono_ns - logical_time_ns) / 1000.0)
        hc_latencies_us.sort()
        hc_latency_mean_us = sum(hc_latencies_us) / len(hc_latencies_us) if hc_latencies_us else 0.0
        if hc_latencies_us:
            idx = int(round(0.95 * (len(hc_latencies_us) - 1)))
            hc_latency_p95_us = hc_latencies_us[idx]
        else:
            hc_latency_p95_us = 0.0

        ms_events = parse_ms_log(ms_log)
        degraded = 0
        violations = 0
        fallback_count = 0
        fallback_missing_metadata = 0
        fallback_no_candidate = 0
        for e in ms_events:
            ev = e.get("event")
            if ev == "degrade":
                degraded += 1
            elif ev == "mismatch":
                violations += 1
            elif ev == "fallback":
                fallback_count += 1
                reason = e.get("reason", "")
                if reason == "missing_metadata":
                    fallback_missing_metadata += 1
                elif reason == "no_candidate":
                    fallback_no_candidate += 1

        miss_rate = (misses_hc / total_hc) * 100.0 if total_hc else 0.0

        rows.append([
            mode,
            f"{load_factor:.2f}",
            steps,
            f"{miss_rate:.3f}",
            total_hc,
            completed_lc,
            expected_lc,
            f"{lc_completion_ratio:.3f}",
            f"{hc_latency_mean_us:.3f}",
            f"{hc_latency_p95_us:.3f}",
            degraded,
            fallback_count,
            fallback_missing_metadata,
            fallback_no_candidate,
            violations,
        ])

    header = [
        "mode",
        "load_factor",
        "steps",
        "hc_miss_rate",
        "hc_samples",
        "lc_completed",
        "lc_expected",
        "lc_completion_ratio",
        "hc_latency_mean_us",
        "hc_latency_p95_us",
        "lc_degraded_count",
        "fallback_count",
        "fallback_missing_metadata",
        "fallback_no_candidate",
        "violations",
    ]
    write_csv(Path(args.out), header, rows)


if __name__ == "__main__":
    main()
