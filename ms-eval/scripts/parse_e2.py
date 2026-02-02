#!/usr/bin/env python3
import argparse
from pathlib import Path
from collections import defaultdict

from common_parse import read_jsonl, parse_ms_log, write_csv


def parse_correctness(ms_events):
    ready_sets = defaultdict(set)
    pick_events = []
    mismatch = 0
    runtime_missing = 0

    for e in ms_events:
        ev = e.get("event")
        if ev == "ready":
            try:
                logical = int(e.get("logical", "0"))
                rid = int(e.get("reaction_index", "-1"))
            except ValueError:
                continue
            ready_sets[logical].add(rid)
        elif ev == "ready_drop":
            try:
                logical = int(e.get("logical", "0"))
                rid = int(e.get("reaction_index", "-1"))
            except ValueError:
                continue
            if rid in ready_sets.get(logical, set()):
                ready_sets[logical].remove(rid)
        elif ev == "pick_next":
            pick_events.append(e)
        elif ev == "mismatch":
            mismatch += 1
        elif ev == "runtime_selected_missing":
            runtime_missing += 1

    membership_violations = 0
    logical_violations = 0
    fallback_count = 0

    last_logical = None
    total_dispatches = 0
    for e in pick_events:
        try:
            logical = int(e.get("logical", "0"))
            candidate = int(e.get("candidate", "-1"))
        except ValueError:
            continue
        if last_logical is not None and logical < last_logical:
            logical_violations += 1
        last_logical = logical
        if candidate < 0:
            fallback_count += 1
            continue
        total_dispatches += 1
        if candidate not in ready_sets.get(logical, set()):
            membership_violations += 1

    return {
        "total_dispatches": total_dispatches,
        "membership_violations": membership_violations,
        "logical_time_violations": logical_violations,
        "fallback_count": fallback_count,
        "guard_fail_count": mismatch + runtime_missing,
    }


def parse_miss_detection(app_events):
    injections = []
    detected = set()

    for e in app_events:
        if e.get("type") == "injection":
            injections.append(e)
        elif e.get("type") == "reaction_end":
            if int(e.get("missed_deadline", 0)) == 1:
                detected.add((e.get("reaction_id"), e.get("logical_time_ns")))

    rows = []
    true_positive = 0
    false_negative = 0
    false_positive = 0

    for inj in injections:
        key = (inj.get("reaction_id"), inj.get("logical_time_ns"))
        expected = int(inj.get("expected_miss", 0))
        detected_miss = 1 if key in detected else 0
        if expected == 1 and detected_miss == 1:
            true_positive += 1
        elif expected == 1 and detected_miss == 0:
            false_negative += 1
        elif expected == 0 and detected_miss == 1:
            false_positive += 1

        rows.append([
            inj.get("reaction_id"),
            inj.get("logical_time_ns"),
            inj.get("deadline_us"),
            inj.get("injected_delay_us"),
            expected,
            detected_miss,
        ])

    injected_count = len(injections)
    detected_count = len(detected)
    recall = (true_positive / (true_positive + false_negative)) if (true_positive + false_negative) else 0.0
    precision = (true_positive / (true_positive + false_positive)) if (true_positive + false_positive) else 0.0

    return {
        "summary": {
            "injected_count": injected_count,
            "detected_count": detected_count,
            "true_positive": true_positive,
            "false_negative": false_negative,
            "false_positive": false_positive,
            "recall": f"{recall:.3f}",
            "precision": f"{precision:.3f}",
        },
        "rows": rows,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="ms-eval/logs/e2")
    ap.add_argument("--out-correct", default="ms-eval/results/e2_correctness.csv")
    ap.add_argument("--out-miss", default="ms-eval/results/e2_miss_detection.csv")
    ap.add_argument("--out-events", default="ms-eval/results/e2_miss_events.csv")
    args = ap.parse_args()

    log_root = Path(args.logs)
    all_ms_events = []
    all_app_events = []

    for ms_log in log_root.glob("**/ms.log"):
        all_ms_events.extend(parse_ms_log(ms_log))
    for app_log in log_root.glob("**/app.jsonl"):
        all_app_events.extend(read_jsonl(app_log))

    correctness = parse_correctness(all_ms_events)
    write_csv(
        Path(args.out_correct),
        [
            "total_dispatches",
            "membership_violations",
            "logical_time_violations",
            "fallback_count",
            "guard_fail_count",
        ],
        [[
            correctness["total_dispatches"],
            correctness["membership_violations"],
            correctness["logical_time_violations"],
            correctness["fallback_count"],
            correctness["guard_fail_count"],
        ]],
    )

    miss = parse_miss_detection(all_app_events)
    write_csv(
        Path(args.out_miss),
        [
            "injected_count",
            "detected_count",
            "true_positive",
            "false_negative",
            "false_positive",
            "recall",
            "precision",
        ],
        [[
            miss["summary"]["injected_count"],
            miss["summary"]["detected_count"],
            miss["summary"]["true_positive"],
            miss["summary"]["false_negative"],
            miss["summary"]["false_positive"],
            miss["summary"]["recall"],
            miss["summary"]["precision"],
        ]],
    )

    write_csv(
        Path(args.out_events),
        [
            "reaction_id",
            "logical_time",
            "deadline_us",
            "injected_delay_us",
            "expected_miss",
            "detected_miss",
        ],
        miss["rows"],
    )


if __name__ == "__main__":
    main()
