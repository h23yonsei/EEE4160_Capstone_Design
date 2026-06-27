# EEE4160 Capstone Design — Near-Data Processing for LSM-tree Compaction Acceleration

**Course:** Electrical Engineering Comprehensive Design (EEE4160) · Yonsei University

A full-system study of offloading **RocksDB compaction** to a **Processing-Near-Memory
(PNM)** unit, evaluated on a modified **gem5** simulator. The project asks a single
question: *if LSM-tree compaction runs near memory instead of on the CPU, how much do
foreground reads and writes speed up?*

On the headline workload, moving compaction off the CPU raised write throughput by
**+58.6%** and cut CPU L2 misses by **−64.3%** — the full results are
[below](#results).

---

## Why this repository exists

The work spans two codebases that only make sense together, so they are combined here:

| Directory | Origin repo | Role |
|-----------|-------------|------|
| [`gem5/`](gem5/) | `EEE4160_PNM_Research` | Modified gem5 — the simulated **hardware**: a `PNMCompactor` MMIO device, the full-system run configs, the guest kernel module, and the analysis docs. |
| [`rocksdb/`](rocksdb/) | `EEE4160_PNM_RocksDB` | RocksDB fork — the simulated **software**: a `CompactionService` that offloads compaction to a near-memory worker process. |

The two halves are coupled by a hardware/software contract: a small **MMIO register
map** that is duplicated, by hand, in both
[`gem5/src/dev/pnm/pnm_compactor.hh`](gem5/src/dev/pnm/pnm_compactor.hh) and
[`rocksdb/tools/pnm_compaction_service.h`](rocksdb/tools/pnm_compaction_service.h).
Keeping them in one repository keeps that contract in sync.

---

## How it works

The design is a **hybrid functional + timing model**. gem5 accounts for *how long*
near-memory compaction would take; a real RocksDB worker process does the *actual*
compaction on a separate core, so its DRAM traffic and cache behavior are simulated
faithfully without polluting the main CPU's caches.

```
  gem5 x86 full-system guest (2 cores, DualChannel DDR4-2400, 3 GiB)
  ┌─────────────────────────────────────────────────────────────────────┐
  │                                                                       │
  │   core 0                              core 1                          │
  │  ┌────────────────────┐    UNIX      ┌────────────────────────────┐   │
  │  │  rocksdb_pnm        │   socket     │  pnm_compaction_unit       │   │
  │  │  (db_bench)         │ ───────────► │  (persistent secondary DB) │   │
  │  │                     │  job input   │  CompactWithoutInstallation│   │
  │  │  PNMCompactionSvc   │ ◄─────────── │                            │   │
  │  └─────────┬───────────┘  job result  └────────────────────────────┘   │
  │            │ MMIO via /dev/pnm (pnm_module.ko)                          │
  │            ▼                                                            │
  │  ┌──────────────────────────────────────────────┐                     │
  │  │ PNMCompactor  (gem5 device @ 0xD0000000)      │  models latency =   │
  │  │  doorbell CMD_SUBMIT → STATUS_DONE after      │  (src+dst)/25GiB/s  │
  │  │  the modeled near-memory delay                │  + 500 ns           │
  │  └──────────────────────────────────────────────┘                     │
  └─────────────────────────────────────────────────────────────────────┘
```

**Per compaction job:**

1. RocksDB schedules a compaction. `PNMCompactionService::Schedule()` serializes the
   job, sends it over a UNIX domain socket (`/tmp/pnm_compaction.sock`) to
   `pnm_compaction_unit`, and rings the gem5 doorbell by writing `CMD_SUBMIT` to the
   PNM device's MMIO register (through the `/dev/pnm` mapping provided by
   `pnm_module.ko`).
2. The gem5 `PNMCompactor` device models the near-memory cost —
   `(src_bytes + dst_bytes) / 25 GiB/s + 500 ns` — then raises `STATUS_DONE`.
3. Concurrently, `pnm_compaction_unit` performs the real compaction. It keeps a
   persistent secondary DB (`DB::OpenAsSecondary`) so the MANIFEST is parsed once;
   each job is just `TryCatchUpWithPrimary()` + `CompactWithoutInstallation()`.
4. `PNMCompactionService::Wait()` polls the modeled `STATUS`, then reads the result
   frame back over the socket and installs the new SSTs.

Because the worker runs on its own core, **compaction never blocks the RocksDB write
thread** — that concurrency, plus a deduplication pass that shrinks output SSTs, is
where the speedups come from.

---

## Repository layout

```
EEE4160_Capstone_Design/
├── README.md                          ← this file
│
├── gem5/                              modified gem5 simulator
│   ├── src/dev/pnm/                   PNMCompactor device (MMIO latency model)
│   │   ├── pnm_compactor.{hh,cc}      register map + timing model
│   │   ├── PNMCompactor.py            SimObject parameters
│   │   └── SConscript
│   ├── configs/yonsei/                full-system run scripts
│   │   ├── run_baseline.py            CPU-only compaction (control)
│   │   ├── run_pnm.py                 PNM-offloaded compaction
│   │   ├── pnm_module.c               guest kernel module → /dev/pnm
│   │   ├── pnm_module.Makefile
│   │   └── mount_disk_image.sh        builds the guest disk image
│   ├── docs/                          install guide, workloads, analyses
│   └── m5out/                         saved simulation outputs (baseline/pnm × v1–v3)
│
└── rocksdb/                           RocksDB fork
    └── tools/
        ├── pnm_compaction_service.h   CompactionService that offloads jobs
        ├── pnm_unit_main.cc           pnm_compaction_unit worker process
        ├── pnm_ipc.h                  length-prefixed UNIX-socket framing
        └── db_bench_pnm_main.cc       db_bench entry point with PNM service wired in
```

---

## Simulated machine

| Component | Configuration |
|-----------|---------------|
| Board / ISA | `X86Board`, x86-64 full system, 3 GHz |
| Boot | KVM (near-native) → switch to **TIMING** CPU at the region of interest |
| Cores | 2 — core 0 `rocksdb_pnm`, core 1 `pnm_compaction_unit` |
| Caches | Private L1 (32 KiB I + 32 KiB D) / L2 (512 KiB) |
| Memory | Dual-channel DDR4-2400, 3 GiB |
| Guest | Ubuntu 24.04, Linux 6.8.0-52 |
| PNM device | MMIO @ `0xD0000000`, `process_latency=500ns`, `bandwidth=25GiB/s` (AxDIMM-style) |

---

## Build & run

Full setup is in [`gem5/docs/INSTALL_GUIDE.md`](gem5/docs/INSTALL_GUIDE.md). In short:

```bash
# 1. Build gem5 (the ALL build includes the PNMCompactor device)
cd gem5
scons build/ALL/gem5.opt -j"$(nproc)"

# 2. Build the guest disk image (compiles rocksdb_pnm, pnm_compaction_unit,
#    pnm_module.ko and installs them, plus /sbin/m5 and a NOPASSWD sudoers rule)
./configs/yonsei/mount_disk_image.sh

# 3. Run the control and the PNM-offloaded configurations
./build/ALL/gem5.opt configs/yonsei/run_baseline.py   # ~1.7 h host time
./build/ALL/gem5.opt configs/yonsei/run_pnm.py        # ~2.4 h host time
```

**Prerequisites:** Ubuntu 22.04/24.04 (x86-64), 16 GB RAM (32 GB recommended), ~35 GB
free disk, KVM enabled (`ls /dev/kvm`). The RocksDB benchmark driver is
`db_bench fillrandom,readrandom,waitforcompaction`, 25,000 keys × 1 KiB values,
level-style compaction.

---

## Results

Current workload (**v3**), `m5out/baseline_v3/` vs `m5out/pnm_v3/`:

| Metric | Baseline (CPU) | PNM | Change |
|--------|----------------|-----|--------|
| Write throughput | 29,141 ops/s | 46,217 ops/s | **+58.6%** |
| Write latency | 34.3 µs/op | 21.6 µs/op | **−36.9%** |
| Read throughput | 18,264 ops/s | 22,834 ops/s | **+25.0%** |
| Read latency | 54.8 µs/op | 43.8 µs/op | **−20.0%** |
| Compaction wall-clock | 1,595,756 µs | 1,118,829 µs | **−29.9%** |
| Compaction output size | 0.89× input | 0.63× input | **−42% SST size** |
| DRAM read requests | 16.07 M | 14.15 M | **−12.0%** |
| CPU L2 misses (core 0) | 11.06 M | 3.94 M | **−64.3%** |

The dominant benefit is **concurrency** — five L0→L1 jobs ran in parallel on the PNM
core without stalling writes — amplified by a secondary-DB deduplication pass that
dropped 8,441 superseded keys, producing 40–44% smaller output SSTs that cut both read
amplification and DRAM traffic. Full breakdown:
[`gem5/docs/comp_v3.md`](gem5/docs/comp_v3.md).

---

## Documentation

All in [`gem5/docs/`](gem5/docs/):

| File | Contents |
|------|----------|
| [`INSTALL_GUIDE.md`](gem5/docs/INSTALL_GUIDE.md) | From-scratch build and environment setup |
| [`USEFUL_COMMANDS.md`](gem5/docs/USEFUL_COMMANDS.md) | Running and analyzing simulations |
| [`WORKLOADS.md`](gem5/docs/WORKLOADS.md) | `db_bench` parameters per simulation version |
| [`GEM5_CUSTOMIZATIONS.md`](gem5/docs/GEM5_CUSTOMIZATIONS.md) | gem5 modifications and extensions |
| [`DB_BENCH_CUSTOMIZATION.md`](gem5/docs/DB_BENCH_CUSTOMIZATION.md) | RocksDB `db_bench` configuration |
| [`comp_v3.md`](gem5/docs/comp_v3.md) | Full performance analysis (current workload) |
| [`component_sizes.md`](gem5/docs/component_sizes.md) | Dataset / cache / DRAM / PNM size hierarchy |

---

## Licensing

Each component keeps its upstream license, unchanged, in its own subdirectory:

- **`gem5/`** — BSD 3-Clause ([`gem5/LICENSE`](gem5/LICENSE))
- **`rocksdb/`** — dual GPLv2 / Apache 2.0 ([`rocksdb/COPYING`](rocksdb/COPYING),
  [`rocksdb/LICENSE.Apache`](rocksdb/LICENSE.Apache))

A combined binary that links both would be governed by GPLv2 (with which BSD-3 and
Apache-2.0 are compatible); the simulator and the database are *built and run
separately*, so no such combined binary is produced here.

## Provenance

Assembled from two previously separate repositories, snapshot at their respective
default branches:

- `EEE4160_PNM_Research` (`stable`) → [`gem5/`](gem5/)
- `EEE4160_PNM_RocksDB` (`main`) → [`rocksdb/`](rocksdb/)
