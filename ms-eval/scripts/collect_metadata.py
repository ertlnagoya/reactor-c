#!/usr/bin/env python3
import json
import platform
import subprocess
from pathlib import Path


def try_cmd(cmd):
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode().strip()
        return out
    except Exception:
        return ""


def main() -> None:
    meta = {
        "os": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "uname": try_cmd(["uname", "-a"]),
        "cpu_model": "",
        "git_commit": "",
        "lfc_version": "",
    }

    lscpu = try_cmd(["lscpu"]) if platform.system() == "Linux" else ""
    if lscpu:
        for line in lscpu.splitlines():
            if "Model name" in line:
                meta["cpu_model"] = line.split(":", 1)[1].strip()
                break

    meta["git_commit"] = try_cmd(["git", "rev-parse", "HEAD"])
    meta["lfc_version"] = try_cmd(["lfc", "--version"])

    out_path = Path("ms-eval/results/metadata.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(meta, indent=2) + "\n")


if __name__ == "__main__":
    main()
