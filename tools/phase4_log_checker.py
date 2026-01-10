#!/usr/bin/env python3
"""
Simple Phase 4 log checker.

Run after exercising LF_MS_OS_ENABLE=1 to verify we saw os_policy events
even when the kernel denied a privilege (e.g., without CAP_SYS_NICE).
"""

from __future__ import annotations

import argparse
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Look for Phase 4 OS policy events in the master scheduler log."
    )
    parser.add_argument(
        "--log",
        type=pathlib.Path,
        default=pathlib.Path("/tmp/lf_master_scheduler_phase0.log"),
        help="Master scheduler log to scan.",
    )
    args = parser.parse_args()

    if not args.log.exists():
        print(f"Log file not found: {args.log}", file=sys.stderr)
        return 2

    with args.log.open("r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    if "event=os_policy_fail" in content:
        print("Found os_policy_fail entries; Phase 4 guard logs confirmed.")
        return 0

    if "event=os_policy_apply" in content:
        print("Found os_policy_apply entries; Phase 4 logs are present.")
        return 0

    print("No Phase 4 os_policy_* entries detected.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
