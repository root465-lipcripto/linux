# Ryuji Adaptive Memory (RAM)

Experimental userspace policy controller for bounded ZRAM recompression.

The goal is to test whether scheduling existing ZRAM recompression only during
high memory pressure and low CPU contention can reduce ZRAM's physical memory
use without adding unacceptable CPU pressure. This project does not claim a
performance improvement. Results must come from repeatable measurements.

## Scope and design

Linux already provides the mechanisms this experiment coordinates:

* PSI exposes memory and CPU stall pressure through `/proc/pressure/`.
* ZRAM exposes memory accounting through `mm_stat`.
* With `CONFIG_ZRAM_MULTI_COMP`, ZRAM can recompress idle pages using a
  configured secondary compression algorithm.
* The `idle` and `recompress` sysfs controls support selecting idle pages and
  limiting each recompression batch with `max_pages`.

The controller samples PSI's `some avg10` values and `mm_stat`'s
`mem_used_total`. It requests one bounded batch only when memory PSI reaches
its configured threshold, CPU PSI remains below its limit, enough ZRAM memory
is in use, and the cooldown has elapsed. It first marks allocated ZRAM pages
idle, then asks ZRAM to recompress idle pages using a configured algorithm
priority. Accessing a page clears its idle mark according to the ZRAM
interface.

This is an experiment in `tools/memory/ram-adaptive/`; it does not change the
kernel's ZRAM implementation, set up a ZRAM device, select an algorithm, or
configure swap. Those operations remain the administrator's responsibility.
No kernel patch is proposed until measurements identify a limitation that
cannot be addressed from userspace.

## Requirements

* Linux with PSI enabled and `/proc/pressure/memory` plus
  `/proc/pressure/cpu` available.
* A configured ZRAM device with a secondary algorithm in `recomp_algorithm`.
* Kernel support for multi-compressor ZRAM and its `idle` and `recompress`
  sysfs controls.
* A C11 compiler and Python 3 for building and tests.
* Root privileges only when applying actions to real ZRAM sysfs controls.

If PSI, `mm_stat`, or the secondary compressor configuration is unavailable,
the controller fails closed or skips recompression; it never configures the
device on its own.

## Build and tests

From this directory:

```sh
make
make test
```

The integration tests use temporary procfs/sysfs-shaped files. They exercise
dry-run behavior, an allowed bounded batch, CPU and memory pressure gates, a
missing secondary compressor, and a missing metrics file. They do not require
root and do not touch the host's ZRAM device.

## Safe first run

The program is dry-run by default. First inspect one sample:

```sh
./ram-adaptive --once
```

To observe continuously without changing ZRAM:

```sh
./ram-adaptive --interval 10
```

The defaults are a memory PSI `some avg10` threshold of 10%, a CPU PSI
`some avg10` ceiling of 5%, at least 128 MiB in `mem_used_total`, a maximum of
256 pages per batch, recompression priority 1, a 10-second poll interval, and
a 60-second cooldown. These are initial experiment settings, not universal
recommendations. Tune them for the device and record the chosen values.

Only after confirming the device, algorithm priority, and behavior in dry-run
mode, apply actions explicitly:

```sh
sudo ./ram-adaptive --apply --device zram0 \
  --memory-psi 10 --cpu-psi 5 --min-used-mib 128 \
  --max-pages 256 --priority 1 --interval 10 --cooldown 60
```

Each action writes `all` to the device's `idle` attribute and then writes a
`type=idle priority=N max_pages=M` request to `recompress`. A failed write is
reported and the controller exits. The controller does not change device
configuration, reset ZRAM, write to backing storage, or disable any safety
limit.

## Reproducible benchmark protocol

Use a disposable VM with a fixed kernel build, fixed memory size, fixed ZRAM
configuration, and the same workload for every run. Do not use the primary
desktop as the first test target. Record the kernel commit, VM memory, CPU
count, ZRAM algorithm configuration, controller arguments, workload command,
and whether each run is a baseline, fixed recompression, or adaptive policy.

For each condition:

1. Start from a fresh VM boot and use the same ZRAM setup.
2. Run the same workload for the same duration and warm-up period.
3. Collect a baseline time series without the controller.
4. For the fixed-policy comparison, issue the same bounded ZRAM recompression
   request at a predeclared fixed interval.
5. For the adaptive condition, run the controller with `--apply` and the
   predeclared thresholds.
6. Repeat each condition at least five times, alternating order where
   practical. Report every run and summarize median and spread; do not discard
   outliers without explaining why.
7. Compare memory PSI, CPU PSI, `orig_data_size`, `compr_data_size`,
   `mem_used_total`, compression ratio, workload completion time, and an
   application latency measure relevant to the workload.

The read-only collector writes timestamped CSV samples and does not change
system state:

```sh
python3 benchmark.py --label baseline --seconds 300 \
  --device zram0 --output results/baseline-01.csv
```

Repeat with distinct labels and output paths for `fixed` and `adaptive` runs.
The collector records PSI `some avg10` and the first three `mm_stat` values.
It does not itself create memory pressure or issue recompression commands.

## Interpretation and limitations

* CPU PSI measures task contention for CPU time, not raw CPU utilization. The
  CPU gate is therefore a contention safeguard; it is not a direct CPU usage
  cap.
* PSI `avg10` is a moving average, not a per-batch prediction. Results depend
  on workload, compressor, CPU, memory size, and ZRAM configuration.
* `mem_used_total` is accounting data, not a direct measurement of all host
  memory impact or application latency.
* Recompression can cost CPU and may produce no useful reduction. A bounded
  batch and cooldown limit the scope of each experiment but do not guarantee a
  net benefit.
* A missing kernel feature, permission error, malformed metric, or unavailable
  PSI interface must be treated as a failed or skipped sample, never as zero
  pressure.
* No claim about improving a 4 GB or 8 GB laptop is justified until the
  benchmark protocol has been run on representative hardware or a clearly
  described VM.

## Current validation status

Automated tests cover the controller's decision logic with synthetic metrics.
They do not validate real ZRAM writes, kernel behavior, VM performance, or
application latency. Those checks require a disposable test VM configured
with multi-compressor ZRAM and a representative workload.

## Kernel fork diagnostics

The local kernel branch also extends `/sys/block/zram<id>/debug_stat` to
version 2. It preserves `miss_free` and reports:

* `recompress_attempted_pages`: pages passed to the secondary compressor.
* `recompress_saved_pages`: pages successfully moved to a smaller zsmalloc
  allocation class.
* `recompress_errors`: per-page recompression errors returned by ZRAM.
* `recompress_time_ns`: cumulative nanoseconds spent scanning and processing
  recompression requests.

The counters reset when the ZRAM device is reset, along with the rest of its
statistics. `debug_stat` is developer diagnostics, not a stable userspace
interface; its format may change. The instrumentation does not modify
`mm_stat` or trigger recompression by itself. Runtime verification requires
booting or loading a kernel built from this branch; a successful object build
alone does not verify live counter values.
