# Workload Version Log

Both baseline and PNM runs share the same benchmark parameters for fair comparison.
Outputs land in `m5out/<config>_<version>/` for each version.

---

## v1  (`m5out/baseline/`, `m5out/pnm/`)

| Parameter | Value |
|-----------|-------|
| Benchmarks | `fillrandom`, `waitforcompaction` |
| Keys (`--num`) | 15,000 |
| Seed | 42 |
| Value size | 1,024 B |
| Block size | 4,096 B |

**Notes:** Initial run. No `readrandom` phase — read-path performance not captured.
Compaction triggered during `fillrandom`, then flushed by `waitforcompaction`.

**Key results:** Write throughput +58.6% (29,648 → 47,029 ops/sec), write latency −36.9%.
See [comp_v1.md](comp_v1.md).

---

## v2  (`m5out/baseline_v2/`, `m5out/pnm_v2/`)

| Parameter | Value |
|-----------|-------|
| Benchmarks | `fillrandom`, `readrandom`, `waitforcompaction` |
| Keys (`--num`) | 15,000 |
| Seed | 42 |
| Value size | 1,024 B |
| Block size | 4,096 B |

**Notes:** Added `readrandom` between fill and final compaction flush to capture
read-path latency and cache behavior under a mixed workload.
All other parameters unchanged from v1.

**Key results:** Write +44.7% (30,492 → 44,124 ops/sec), read +32.7% (22,939 → 30,430 ops/sec).
See [comp_v2.md](comp_v2.md).

---

## v3  (`m5out/baseline_v3/`, `m5out/pnm_v3/`)

| Parameter | Value |
|-----------|-------|
| Benchmarks | `fillrandom`, `readrandom`, `waitforcompaction` |
| Keys (`--num`) | 25,000 |
| Seed | 42 |
| Value size | 1,024 B |
| Block size | 4,096 B |
| Write buffer size | 1 MiB |
| Block cache | 8 MiB |

**Notes:** Increased key count from 15,000 to 25,000 to stress the compaction pipeline
further. At 25,000 keys, five L0→L1 compaction jobs fire (vs four in v2), and the larger
working set exceeds the 8 MiB block cache, making read-path bandwidth the bottleneck for
both configurations.

**Key results:** Write +58.6% (29,141 → 46,217 ops/sec), read +25.0% (18,264 → 22,834 ops/sec),
DRAM reads −12%, L2 misses −64%, end-to-end sim time −0.9%.
See [comp_v3.md](comp_v3.md).

---

## clean  (`m5out/baseline_clean/`, `m5out/pnm_clean/`)

| Parameter | Value |
|-----------|-------|
| Benchmarks | `fillrandom`, `readrandom`, `waitforcompaction` |
| Keys (`--num`) | 25,000 |
| Seed | 42 |
| Value size | 1,024 B |
| Block size | 4,096 B |
| Write buffer size | 1 MiB |
| Block cache | 8 MiB |

**Notes:** Same workload parameters as v3. Re-run after a full comment/naming/structural
cleanup of `configs/yonsei/` and all documentation — no benchmark parameter changes.

**Key results:** Write +44.8% (29,381 → 42,523 ops/sec), read +12.9% (21,913 → 24,731 ops/sec).
End-to-end sim time baseline 7.162 s vs PNM 7.266 s (+1.5% — 2-CPU overhead not yet fully overcome at this run, but application throughput is strongly PNM-favorable).
See [comp_v3.md](comp_v3.md) for the v3 run's cache/DRAM/compaction breakdown (same workload parameters; application throughput differs between runs due to KVM boot non-determinism).
