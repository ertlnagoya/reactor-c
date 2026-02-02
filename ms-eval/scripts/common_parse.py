import json
from pathlib import Path

def read_jsonl(path: Path):
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            yield json.loads(line)


def percentile(sorted_vals, pct):
    if not sorted_vals:
        return 0.0
    if pct <= 0:
        return float(sorted_vals[0])
    if pct >= 100:
        return float(sorted_vals[-1])
    k = (len(sorted_vals) - 1) * (pct / 100.0)
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return float(sorted_vals[f])
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def parse_ms_log(path: Path):
    events = []
    if not path.exists():
        return events
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 2)
            if len(parts) < 3:
                continue
            try:
                ts = int(parts[0])
            except ValueError:
                continue
            level = parts[1]
            rest = parts[2]
            kv = {"ts_mono_ns": ts, "level": level}
            for token in rest.split():
                if "=" not in token:
                    continue
                k, v = token.split("=", 1)
                kv[k] = v
            events.append(kv)
    return events


def write_csv(path: Path, header, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        f.write(",".join(header) + "\n")
        for row in rows:
            f.write(",".join(str(x) for x in row) + "\n")
