#!/usr/bin/env python3
import argparse
from pathlib import Path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--hc", type=int, required=True)
    ap.add_argument("--lc", type=int, required=True)
    ap.add_argument("--lc-budget", type=int, required=True)
    ap.add_argument("--window-ns", type=int, default=1_000_000_000)
    ap.add_argument("--degrade-action", default="skip")
    ap.add_argument("--budget-type", default="reaction_count")
    args = ap.parse_args()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines = []
    lines.append(f"degrade_action={args.degrade_action}")
    lines.append(f"budget_type={args.budget_type}")
    lines.append(f"budget_window_ns={args.window_ns}")
    lines.append("default_budget=-1")

    # Assumes reaction indices are contiguous and ordered as in the LF generator.
    for rid in range(args.hc + args.lc):
        crit = "high" if rid < args.hc else "low"
        budget = -1 if crit == "high" else args.lc_budget
        lines.append(f"reaction,0,{rid},{crit},{budget}")

    out_path.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
