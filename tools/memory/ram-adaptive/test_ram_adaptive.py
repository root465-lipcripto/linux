#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Integration tests against temporary procfs/sysfs-shaped fixtures."""

import pathlib
import csv
import subprocess
import sys
import tempfile


PROGRAM = pathlib.Path(sys.argv[1]).resolve()


def fixture(memory_psi="some avg10=15.00 avg60=10.00 avg300=5.00 total=99\n",
            cpu_psi="some avg10=1.00 avg60=1.00 avg300=1.00 total=1\n",
            mem_used="0 0 268435456 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            algorithms="#1: [lz4] zstd\n"):
    temporary = tempfile.TemporaryDirectory(prefix="ram-adaptive-test-")
    root = pathlib.Path(temporary.name)
    proc = root / "proc"
    sysfs = root / "sys"
    (proc / "pressure").mkdir(parents=True)
    zram = sysfs / "block" / "zram0"
    zram.mkdir(parents=True)
    (proc / "pressure" / "memory").write_text(memory_psi)
    (proc / "pressure" / "cpu").write_text(cpu_psi)
    (zram / "mm_stat").write_text(mem_used)
    (zram / "recomp_algorithm").write_text(algorithms)
    (zram / "idle").write_text("")
    (zram / "recompress").write_text("")
    return temporary, proc, sysfs, zram


def run(proc, sysfs, *extra):
    return subprocess.run(
        [str(PROGRAM), "--once", "--proc-root", str(proc), "--sysfs-root",
         str(sysfs), "--min-used-mib", "1", "--max-pages", "17", *extra],
        text=True, capture_output=True, check=False)


def test_dry_run_is_default():
    tmp, proc, sysfs, zram = fixture()
    try:
        result = run(proc, sysfs)
        assert result.returncode == 0, result.stderr
        assert "decision=recompress" in result.stdout
        assert "DRY-RUN write idle: all" in result.stdout
        assert (zram / "idle").read_text() == ""
        assert (zram / "recompress").read_text() == ""
    finally:
        tmp.cleanup()


def test_apply_marks_idle_then_limits_batch():
    tmp, proc, sysfs, zram = fixture()
    try:
        result = run(proc, sysfs, "--apply")
        assert result.returncode == 0, result.stderr
        assert (zram / "idle").read_text() == "all\n"
        assert (zram / "recompress").read_text() == \
            "type=idle priority=1 max_pages=17\n"
    finally:
        tmp.cleanup()


def test_high_cpu_pressure_skips():
    tmp, proc, sysfs, zram = fixture(
        cpu_psi="some avg10=8.00 avg60=7.00 avg300=6.00 total=42\n")
    try:
        result = run(proc, sysfs, "--apply")
        assert result.returncode == 0, result.stderr
        assert "reason=cpu-pressure-above-threshold" in result.stdout
        assert (zram / "idle").read_text() == ""
    finally:
        tmp.cleanup()


def test_low_memory_pressure_skips():
    tmp, proc, sysfs, zram = fixture(
        memory_psi="some avg10=2.00 avg60=1.00 avg300=0.50 total=5\n")
    try:
        result = run(proc, sysfs, "--apply")
        assert result.returncode == 0, result.stderr
        assert "reason=memory-pressure-below-threshold" in result.stdout
        assert (zram / "recompress").read_text() == ""
    finally:
        tmp.cleanup()


def test_missing_secondary_algorithm_skips():
    tmp, proc, sysfs, zram = fixture(algorithms="")
    try:
        result = run(proc, sysfs, "--apply")
        assert result.returncode == 0, result.stderr
        assert "reason=no-secondary-algorithm-configured" in result.stdout
        assert (zram / "idle").read_text() == ""
    finally:
        tmp.cleanup()


def test_missing_metrics_fails_closed():
    tmp, proc, sysfs, zram = fixture()
    try:
        (proc / "pressure" / "cpu").unlink()
        result = run(proc, sysfs, "--apply")
        assert result.returncode != 0
        assert "no action taken" in result.stderr
        assert (zram / "idle").read_text() == ""
    finally:
        tmp.cleanup()


def test_rejects_non_zram_device_name():
    tmp, proc, sysfs, _ = fixture()
    try:
        result = run(proc, sysfs, "--device", "../zram0")
        assert result.returncode != 0
        assert "Invalid option" in result.stderr
    finally:
        tmp.cleanup()


def test_benchmark_collector_writes_csv():
    tmp, proc, sysfs, _ = fixture()
    try:
        output = pathlib.Path(tmp.name) / "samples.csv"
        result = subprocess.run(
            [sys.executable, str(pathlib.Path(__file__).with_name("benchmark.py")),
             "--label", "test", "--seconds", "1", "--interval", "0.1",
             "--proc-root", str(proc), "--sysfs-root", str(sysfs),
             "--output", str(output)], text=True, capture_output=True,
            check=False)
        assert result.returncode == 0, result.stderr
        rows = list(csv.DictReader(output.open(encoding="utf-8")))
        assert rows and rows[0]["label"] == "test"
        assert rows[0]["orig_data_size_bytes"] == "0"
        assert rows[0]["mem_used_total_bytes"] == "268435456"
        assert rows[0]["memory_psi_some_avg10"] == "15.0"
    finally:
        tmp.cleanup()


def main():
    tests = [test_dry_run_is_default, test_apply_marks_idle_then_limits_batch,
             test_high_cpu_pressure_skips, test_low_memory_pressure_skips,
             test_missing_secondary_algorithm_skips,
             test_missing_metrics_fails_closed,
             test_rejects_non_zram_device_name,
             test_benchmark_collector_writes_csv]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
    print(f"{len(tests)} tests passed")


if __name__ == "__main__":
    main()
