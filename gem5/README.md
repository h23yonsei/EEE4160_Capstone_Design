# EEE4160: Near-Data Processing for LSM-tree Compaction Acceleration

**Course:** Electrical Engineering Comprehensive Design (EEE4160)
**Institution:** Yonsei University

A modified gem5 full-system simulator implementing a Processing-Near-Memory (PNM) unit to offload LSM-tree compaction from the CPU. The research evaluates whether moving compaction computation closer to memory reduces CPU load and memory bandwidth pressure in RocksDB workloads.

## Problem

LSM-tree key-value stores like RocksDB perform compaction — merging and sorting data across levels — entirely on the CPU. This creates a bottleneck: compaction is I/O and memory-bandwidth intensive, competing with foreground read/write operations. Near-data processing can relieve this by executing compaction logic inside the memory subsystem.

## Approach

Two simulation configurations are compared:

- **Baseline** (`configs/yonsei/run_baseline.py`): standard CPU-only RocksDB compaction, KVM boot → TIMING mode simulation, 3 GiB DDR4, L1/L2 cache hierarchy
- **PNM** (`configs/yonsei/run_pnm.py`): same setup with a PNM unit (`src/dev/pnm/`) attached to the memory bus that intercepts and executes compaction DMA operations

The PNM unit (`PNMCompactor`) is implemented in gem5 as a custom `SimObject` modeled after AxDIMM-style near-memory processing (500 ns MMIO round-trip latency, 25 GiB/s bandwidth). When triggered via MMIO, it reads L0 SST files directly from memory, merges and sorts key-value pairs using a secondary-DB deduplication pass, and writes compacted output SSTs back to memory — all without routing data through the CPU's cache hierarchy or blocking the RocksDB write thread.

## Repository Structure

```
configs/yonsei/           simulation entry points (run_baseline.py, run_pnm.py)
src/dev/pnm/              PNM unit implementation (PNMCompactor SimObject)
docs/                     documentation and simulation output analysis
m5out/                    gem5 simulation outputs (stats.txt, etc.)
```

## Prerequisites

- **OS:** Ubuntu 22.04 or 24.04 LTS (x86-64)
- **RAM:** 16 GB minimum, 32 GB recommended
- **Disk:** ~35 GB free (gem5 build ~15 GB, RocksDB ~3 GB, disk image ~5 GB)
- **CPU:** 8+ cores recommended; KVM support required for reasonable simulation times
- **Sim time:** baseline ~1.7 hours host time, PNM ~2.4 hours (on an 8-core host)

Check KVM availability: `ls /dev/kvm && echo "KVM available"`

## Quick Start

See [docs/INSTALL_GUIDE.md](docs/INSTALL_GUIDE.md) for full build and environment setup.

```bash
# Build gem5 (ALL includes all ISAs and device models, including PNMCompactor)
scons build/ALL/gem5.opt -j$(nproc)

# Run baseline simulation (~1.7 hours)
./build/ALL/gem5.opt configs/yonsei/run_baseline.py

# Run PNM simulation (~2.4 hours)
./build/ALL/gem5.opt configs/yonsei/run_pnm.py
```

## Results (v3 Workload)

All numbers from `m5out/baseline_v3/` vs `m5out/pnm_v3/` — gem5 full-system x86, `db_bench fillrandom,readrandom,waitforcompaction`, 25,000 keys, 1 KiB values.

| Metric | Baseline | PNM | Change |
|--------|----------|-----|--------|
| Write throughput | 29,141 ops/sec | 46,217 ops/sec | **+58.6%** |
| Write latency | 34.3 µs/op | 21.6 µs/op | **−36.9%** |
| Read throughput | 18,264 ops/sec | 22,834 ops/sec | **+25.0%** |
| Read latency | 54.8 µs/op | 43.8 µs/op | **−20.0%** |
| Compaction wall-clock | 1,595,756 µs total | 1,118,829 µs total | **−29.9%** |
| Compaction output size | avg 0.89× input | avg 0.63× input | **−42% SST size** |
| DRAM read requests | 16.07 M | 14.15 M | **−12.0%** |
| CPU L2 misses (CPU0) | 11.06 M | 3.94 M | **−64.3%** |
| End-to-end sim time | 7.381 s | 7.314 s | −0.9% |

The dominant benefit is **concurrency**: compaction never blocks the write thread. Five L0→L1 compaction jobs ran in parallel on the PNM core, and the PNM's secondary-DB deduplication pass eliminated 8,441 superseded keys before writing output SSTs — producing 40–44% smaller files that reduce both read amplification and DRAM traffic. See [docs/comp_v3.md](docs/comp_v3.md) for full analysis.

## Workloads

RocksDB `db_bench` is used as the benchmark driver. See [docs/WORKLOADS.md](docs/WORKLOADS.md) for workload parameters across simulation versions.

Current workload (v3): `fillrandom` + `readrandom`, 25,000 keys.

## Documentation

| File | Contents |
|------|----------|
| [docs/USEFUL_COMMANDS.md](docs/USEFUL_COMMANDS.md) | Quick-reference commands for running and analyzing sims |
| [docs/INSTALL_GUIDE.md](docs/INSTALL_GUIDE.md) | From-scratch installation and setup |
| [docs/WORKLOADS.md](docs/WORKLOADS.md) | Benchmark parameters per simulation version |
| [docs/comp_v3.md](docs/comp_v3.md) | Full performance analysis (current v3 workload) |
| [docs/component_sizes.md](docs/component_sizes.md) | Size hierarchy: dataset, caches, DRAM, PNM device |
| [docs/GEM5_CUSTOMIZATIONS.md](docs/GEM5_CUSTOMIZATIONS.md) | gem5 modifications and extensions |
| [docs/DB_BENCH_CUSTOMIZATION.md](docs/DB_BENCH_CUSTOMIZATION.md) | RocksDB db_bench configuration |

## Related Repository

RocksDB fork with PNM compaction service: [h23yonsei/EEE4160_PNM_RocksDB](https://github.com/h23yonsei/EEE4160_PNM_RocksDB)

## License

gem5 is licensed under BSD 3-Clause. See [LICENSE](LICENSE) for details. Project-specific additions follow the same terms.
