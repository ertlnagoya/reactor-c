#!/usr/bin/env python3
import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from statistics import mean


ROOT = Path("/Users/yutaka/program/reactor-c")


def parse_ms_log(path: Path) -> list[dict[str, str]]:
    events = []
    if not path.exists():
        return events
    for line in path.read_text(errors="ignore").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split(",", 2)
        if len(parts) < 3:
            continue
        payload = parts[2]
        fields = {}
        for token in payload.split():
            if "=" in token:
                k, v = token.split("=", 1)
                fields[k] = v
        if "event" in fields:
            events.append(fields)
    return events


def read_jsonl(path: Path):
    if not path.exists():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            continue


def percentile(sorted_vals: list[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = (len(sorted_vals) - 1) * (p / 100.0)
    lo = math.floor(idx)
    hi = math.ceil(idx)
    if lo == hi:
        return sorted_vals[int(idx)]
    frac = idx - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def parse_pidstat(path: Path) -> dict[tuple[int, int], dict[str, float]]:
    by_tid = defaultdict(lambda: {"cpu_samples": [], "cswch": 0.0, "nvcswch": 0.0, "command": ""})
    if not path.exists():
        return by_tid
    current_tgid = None
    for line in path.read_text(errors="ignore").splitlines():
        line = line.strip()
        if not line or line.startswith("Linux") or line.startswith("# Time"):
            continue
        parts = line.split()
        if len(parts) < 13:
            continue
        try:
            cpu = float(parts[8])
            cswch = float(parts[10])
            nvcswch = float(parts[11])
        except ValueError:
            continue
        if parts[2] != "-" and parts[3] == "-":
            try:
                current_tgid = int(parts[2])
            except ValueError:
                current_tgid = None
            continue
        if parts[2] == "-" and parts[3] != "-" and current_tgid is not None:
            try:
                tgid = current_tgid
                tid = int(parts[3])
            except ValueError:
                continue
        else:
            continue
        rec = by_tid[(tgid, tid)]
        rec["cpu_samples"].append(cpu)
        rec["cswch"] += cswch
        rec["nvcswch"] += nvcswch
        rec["command"] = parts[12]
    return by_tid


def select_mode_dir(condition: str, ts: str, load: str) -> Path:
    if condition == "baseline":
        return ROOT / f"ms-eval/logs/e3/{ts}/baseline/L{load}"
    return ROOT / f"ms-eval/logs/e3/{ts}/degrade/L{load}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--out-thread", default="ms-eval/results/e2_thread_utilization.csv")
    ap.add_argument("--out-latency", default="ms-eval/results/e2_reaction_latency_distribution.csv")
    ap.add_argument("--out-single", default="ms-eval/results/e2_single_worker_thread_comparison.csv")
    args = ap.parse_args()

    manifest_path = Path(args.manifest)
    with manifest_path.open() as f:
        manifest = list(csv.DictReader(f))

    thread_rows = []
    latency_rows = []
    single_rows = []

    for row in manifest:
        rep = int(row["rep"])
        condition = row["condition"]
        ts = row["ts"]
        load = f"{float(row['load']):.2f}"
        run_dir = select_mode_dir(condition, ts, load)
        ms_log = run_dir / "ms.log"
        app_log = run_dir / "app.jsonl"
        pidstat_log = Path(row["pidstat_log"])

        ms_events = parse_ms_log(ms_log)
        worker_meta = {}
        rt_workers = set()
        for e in ms_events:
            if e.get("event") == "register_worker":
                try:
                    wid = int(e.get("worker_id", "-1"))
                    worker_meta[wid] = {
                        "os_pid": int(e.get("os_pid", "-1")),
                        "os_tid": int(e.get("os_tid", "-1")),
                        "name": e.get("name", ""),
                    }
                except ValueError:
                    continue
            elif e.get("event") == "os_policy_apply":
                try:
                    wid = int(e.get("worker_id", "-1"))
                except ValueError:
                    continue
                policy = e.get("policy", "")
                if policy.startswith("rt") or policy in ("fifo", "rr"):
                    rt_workers.add(wid)

        pidstat = parse_pidstat(pidstat_log)
        for wid, meta in sorted(worker_meta.items()):
            key = (meta["os_pid"], meta["os_tid"])
            rec = pidstat.get(key, {"cpu_samples": [], "cswch": 0.0, "nvcswch": 0.0, "command": ""})
            cpu_mean = mean(rec["cpu_samples"]) if rec["cpu_samples"] else 0.0
            thread_rows.append([
                condition,
                rep,
                wid,
                meta["os_pid"],
                meta["os_tid"],
                1 if wid in rt_workers else 0,
                f"{cpu_mean:.3f}",
                f"{rec['cswch']:.3f}",
                f"{rec['nvcswch']:.3f}",
                len(rec["cpu_samples"]),
                meta["name"],
            ])

        hc_lat = []
        lc_lat = []
        all_lat = []
        for e in read_jsonl(app_log):
            if e.get("type") != "reaction_end":
                continue
            logical = int(e.get("logical_time_ns", -1))
            end_ns = int(e.get("ts_mono_ns", -1))
            rid = int(e.get("reaction_id", -1))
            if logical < 0 or end_ns < logical or rid < 0:
                continue
            lat_us = (end_ns - logical) / 1000.0
            all_lat.append(lat_us)
            if rid < 1:
                hc_lat.append(lat_us)
            else:
                lc_lat.append(lat_us)

        all_lat.sort()
        hc_lat.sort()
        lc_lat.sort()
        latency_rows.append([
            condition,
            rep,
            len(all_lat),
            f"{percentile(all_lat, 50):.3f}",
            f"{percentile(all_lat, 95):.3f}",
            f"{percentile(all_lat, 99):.3f}",
            f"{(mean(all_lat) if all_lat else 0.0):.3f}",
            f"{(mean(hc_lat) if hc_lat else 0.0):.3f}",
            f"{(mean(lc_lat) if lc_lat else 0.0):.3f}",
        ])

        if condition == "rt_single_worker":
            rt_cpu = []
            nonrt_cpu = []
            for wid, meta in worker_meta.items():
                key = (meta["os_pid"], meta["os_tid"])
                rec = pidstat.get(key, {"cpu_samples": []})
                cpu_mean = mean(rec["cpu_samples"]) if rec["cpu_samples"] else 0.0
                if wid in rt_workers:
                    rt_cpu.append(cpu_mean)
                else:
                    nonrt_cpu.append(cpu_mean)
            single_rows.append([
                rep,
                f"{(mean(hc_lat) if hc_lat else 0.0):.3f}",
                f"{(mean(lc_lat) if lc_lat else 0.0):.3f}",
                f"{(mean(rt_cpu) if rt_cpu else 0.0):.3f}",
                f"{(mean(nonrt_cpu) if nonrt_cpu else 0.0):.3f}",
                len(rt_workers),
                len(worker_meta) - len(rt_workers),
            ])

    with Path(args.out_thread).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "condition", "rep", "worker_id", "os_pid", "os_tid", "rt_applied",
            "cpu_pct_mean", "voluntary_ctx_switch_est", "involuntary_ctx_switch_est",
            "samples", "worker_name",
        ])
        writer.writerows(thread_rows)

    with Path(args.out_latency).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "condition", "rep", "n_reactions",
            "latency_p50_us", "latency_p95_us", "latency_p99_us",
            "latency_mean_us", "hc_latency_mean_us", "lc_latency_mean_us",
        ])
        writer.writerows(latency_rows)

    with Path(args.out_single).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "rep",
            "rt_designated_hc_latency_mean_us",
            "non_rt_designated_lc_latency_mean_us",
            "rt_thread_cpu_pct_mean",
            "non_rt_thread_cpu_pct_mean",
            "rt_thread_count",
            "non_rt_thread_count",
        ])
        writer.writerows(single_rows)


if __name__ == "__main__":
    main()
