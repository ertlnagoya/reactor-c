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
    ap.add_argument("--reaction-indices", default="")
    ap.add_argument("--extra-high-indices", default="")
    args = ap.parse_args()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines = []
    lines.append(f"degrade_action={args.degrade_action}")
    lines.append(f"budget_type={args.budget_type}")
    lines.append(f"budget_window_ns={args.window_ns}")
    # default_budget is the env-level LC allowance per budget window during
    # degrade pressure. Previously hard-coded to -1, which disabled budgeting
    # entirely and made --lc-budget a no-op; now plumbed through so degradation
    # sheds gracefully (keep up to lc_budget LC per window) instead of all LC.
    lines.append(f"default_budget={args.lc_budget}")

    if args.reaction_indices.strip():
        parsed = [x.strip() for x in args.reaction_indices.split(",") if x.strip()]
        reaction_indices = [int(x) for x in parsed]
        if len(reaction_indices) < (args.hc + args.lc):
            raise ValueError(
                f"need at least {args.hc + args.lc} reaction indices, got {len(reaction_indices)}"
            )
    else:
        # Fallback: assumes contiguous reaction indices 0..N-1.
        reaction_indices = list(range(args.hc + args.lc))

    for rid in range(args.hc + args.lc):
        crit = "high" if rid < args.hc else "low"
        degradable = "false" if crit == "high" else "true"
        budget = -1
        lines.append(f"reaction,0,{reaction_indices[rid]},{crit},{budget},{degradable}")

    if args.extra_high_indices.strip():
        for token in args.extra_high_indices.split(","):
            token = token.strip()
            if not token:
                continue
            lines.append(f"reaction,0,{int(token)},high,-1,false")

    out_path.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
