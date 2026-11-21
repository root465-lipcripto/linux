#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Collect comparable PSI and ZRAM samples as CSV; never changes system state."""

import argparse
import csv
import pathlib
import time
from datetime import datetime, timezone


def pressure(path):
    for line in path.read_text().splitlines():
        if line.startswith("some "):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            return float(fields["avg10"])
    raise ValueError(f"missing PSI some line in {path}")


def mm_stat(path):
    values = [int(item) for item in path.read_text().split()]
    if len(values) < 3:
        raise ValueError(f"expected at least three mm_stat values in {path}")
    return values[0], values[1], values[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", required=True,
                        help="run label, e.g. baseline, fixed, or adaptive")
    parser.add_argument("--device", default="zram0")
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--proc-root", type=pathlib.Path, default=pathlib.Path("/proc"))
    parser.add_argument("--sysfs-root", type=pathlib.Path, default=pathlib.Path("/sys"))
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        parser.error("seconds and interval must be positive")

    zram = args.sysfs_root / "block" / args.device
    memory_psi = args.proc_root / "pressure" / "memory"
    cpu_psi = args.proc_root / "pressure" / "cpu"
    fields = ["timestamp_utc", "label", "memory_psi_some_avg10",
              "cpu_psi_some_avg10", "orig_data_size_bytes",
              "compr_data_size_bytes", "mem_used_total_bytes",
              "compression_ratio_orig_over_used"]
    deadline = time.monotonic() + args.seconds
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        while True:
            orig, compressed, used = mm_stat(zram / "mm_stat")
            writer.writerow({
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "label": args.label,
                "memory_psi_some_avg10": pressure(memory_psi),
                "cpu_psi_some_avg10": pressure(cpu_psi),
                "orig_data_size_bytes": orig,
                "compr_data_size_bytes": compressed,
                "mem_used_total_bytes": used,
                "compression_ratio_orig_over_used":
                    f"{orig / used:.6f}" if used else "",
            })
            output.flush()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(args.interval, remaining))


if __name__ == "__main__":
    main()
