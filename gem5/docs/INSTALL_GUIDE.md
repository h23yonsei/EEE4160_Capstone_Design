# Installation and User Guide: gem5 PNM Compaction Research

This guide walks through setting up and running the full-system gem5 simulation from scratch — from a bare Ubuntu host to producing comparable results for the baseline and PNM configurations.

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Prerequisites](#2-prerequisites)
3. [Repository Setup](#3-repository-setup)
4. [Build gem5](#4-build-gem5)
5. [Build RocksDB and PNM Binaries](#5-build-rocksdb-and-pnm-binaries)
6. [One-Time Disk Image Setup](#6-one-time-disk-image-setup)
7. [Running Simulations](#7-running-simulations)
8. [Analyzing Results](#8-analyzing-results)
9. [Expected Results (v3 Workload)](#9-expected-results-v3-workload)
10. [Troubleshooting](#10-troubleshooting)
11. [Project Structure Reference](#11-project-structure-reference)

---

## 1. Project Overview

This project evaluates the performance benefit of offloading RocksDB LSM-tree compaction to a **Processing-Near-Memory (PNM)** accelerator, simulated inside gem5 full-system x86 simulation.

**Baseline:** 1 CPU running standard `db_bench` (RocksDB benchmark) with all compaction done on-CPU.

**PNM:** 2 CPUs where core 0 runs the main RocksDB workload and core 1 acts as a PNM compaction unit, connected via a custom MMIO device (`PNMCompactor`) that models AxDIMM-style near-memory processing (500 ns latency, 25 GiB/s bandwidth).

**Workload (v3):** `fillrandom` → `readrandom` → `waitforcompaction` with 25,000 keys, 1 KiB values, seed 42.

**Key results (v3):**
- Write throughput: +58.6% (29,141 → 46,217 ops/sec)
- Read throughput: +25.0% (18,264 → 22,834 ops/sec)

For full analysis, see [docs/comp_v3.md](comp_v3.md).

---

## 2. Prerequisites

### Host System

- **OS:** Ubuntu 22.04 or 24.04 LTS (x86-64). KVM-capable host is strongly recommended for reasonable simulation times.
- **RAM:** 16 GB minimum (32 GB recommended)
- **Disk:** ~35 GB free space (gem5 build ~15 GB, RocksDB build ~3 GB, disk image ~5 GB)
- **CPU:** At least 8 cores recommended for parallel builds and KVM-accelerated boot

Check KVM availability:

```bash
ls /dev/kvm && echo "KVM available"
# If missing: sudo modprobe kvm_intel (or kvm_amd)
```

### System Packages

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential git python3 python3-dev python3-pip \
    scons m4 zlib1g-dev libprotobuf-dev protobuf-compiler \
    libgflags-dev libsnappy-dev \
    cmake ninja-build \
    qemu-utils util-linux mount \
    linux-headers-$(uname -r)
```

> **Note:** `linux-headers-$(uname -r)` is required for building the PNM kernel module inside the guest disk image via `chroot`. The kernel version inside the guest is pinned to `6.8.0-52-generic`; the `mount_disk_image.sh` script handles the chroot build automatically.

### Python Packages

```bash
pip3 install -r /workspaces/gem5/requirements.txt
# Or the minimal set:
pip3 install scons pyelftools
```

---

## 3. Repository Setup

### 3.1 Clone the gem5 Research Repository

```bash
git clone https://github.com/h23yonsei/EEE4160_PNM_Research.git gem5
cd gem5
git checkout stable
```

This branch contains:
- The `PNMCompactor` MMIO device (`src/dev/pnm/`)
- Config scripts (`configs/yonsei/`)
- Kernel module source (`configs/yonsei/pnm_module.c`)
- All documentation (`docs/`)
- Five gem5 core bug fixes for multi-core KVM→TIMING simulation

### 3.2 Clone the RocksDB PNM Fork

> **Important:** The RocksDB modifications (PNM compaction offload) must be cloned from the PNM fork, not upstream facebook/rocksdb. The fork adds three new files: `tools/pnm_ipc.h`, `tools/pnm_compaction_service.h`, `tools/db_bench_pnm_main.cc`, and `tools/pnm_unit_main.cc`.

```bash
# Clone alongside the gem5 directory so paths align
git clone https://github.com/h23yonsei/EEE4160_PNM_RocksDB.git rocksdb
```

Expected layout after cloning both:

```
/workspaces/
├── gem5/       ← this repo (EEE4160_PNM_Research)
└── rocksdb/    ← PNM fork of RocksDB
```

> The config scripts and `mount_disk_image.sh` are hardcoded to `/workspaces/gem5/` and `/workspaces/rocksdb/`. If you clone elsewhere, update the path constants at the top of those files before proceeding.

---

## 4. Build gem5

From the gem5 repository root:

```bash
cd /workspaces/gem5
scons build/ALL/gem5.opt -j$(nproc)
```

This builds the gem5 binary with support for all ISAs and all device models including `PNMCompactor`. The build takes approximately 15–40 minutes depending on CPU count.

**Output:** `build/ALL/gem5.opt`

If the build fails, check:

```bash
python3 --version    # must be 3.6+
scons --version      # must be 3.0+
g++ --version        # must be GCC 7+ or Clang 6+
```

### Build the m5 Guest Utility

The `m5` binary is a small program that runs inside the simulated guest OS to signal gem5 (reset stats, mark ROI start/end, exit). Build it separately:

```bash
cd /workspaces/gem5/util/m5
scons build/x86/out/m5
```

**Output:** `util/m5/build/x86/out/m5`

---

## 5. Build RocksDB and PNM Binaries

All three binaries (`db_bench`, `rocksdb_pnm`, `pnm_compaction_unit`) are built from the same RocksDB fork using CMake. They are statically linked so they run inside the minimal guest OS without shared library dependencies.

```bash
cd /workspaces/rocksdb
mkdir -p build_gflags
cd build_gflags

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DPORTABLE=ON \
    -DWITH_GFLAGS=1 \
    -DWITH_TESTS=OFF \
    -DWITH_TOOLS=ON \
    -DWITH_BENCHMARK_TOOLS=ON \
    -DROCKSDB_BUILD_SHARED=OFF \
    -DWITH_SNAPPY=ON \
    -DWITH_LIBURING=OFF

make -j$(nproc) db_bench rocksdb_pnm pnm_compaction_unit
```

After the CMake build, create static-linked versions (required for the guest):

```bash
CXXFLAGS="-W -Wextra -Wall -pthread -Wsign-compare -Wshadow \
    -Wno-unused-parameter -Wno-unused-variable -Woverloaded-virtual \
    -Wnon-virtual-dtor -Wno-missing-field-initializers -Wno-strict-aliasing \
    -Wno-invalid-offsetof -fno-omit-frame-pointer -momit-leaf-frame-pointer \
    -Werror -fno-builtin-memcmp -O3 -DNDEBUG -fno-rtti \
    -static-libgcc -static-libstdc++"

# Baseline benchmark binary
/usr/bin/c++ $CXXFLAGS \
    CMakeFiles/db_bench.dir/tools/simulated_hybrid_file_system.cc.o \
    CMakeFiles/db_bench.dir/tools/db_bench.cc.o \
    CMakeFiles/db_bench.dir/tools/tool_hooks.cc.o \
    CMakeFiles/db_bench.dir/tools/db_bench_tool.cc.o \
    -o db_bench_static \
    librocksdb.a \
    /usr/lib/x86_64-linux-gnu/libgflags.a \
    /usr/lib/x86_64-linux-gnu/libsnappy.a \
    -lz -lpthread -ldl

# PNM main process (routes compaction to pnm_compaction_unit)
/usr/bin/c++ $CXXFLAGS \
    CMakeFiles/rocksdb_pnm.dir/tools/simulated_hybrid_file_system.cc.o \
    CMakeFiles/rocksdb_pnm.dir/tools/db_bench_pnm_main.cc.o \
    CMakeFiles/rocksdb_pnm.dir/tools/tool_hooks.cc.o \
    CMakeFiles/rocksdb_pnm.dir/tools/db_bench_tool.cc.o \
    -o rocksdb_pnm_static \
    librocksdb.a \
    /usr/lib/x86_64-linux-gnu/libgflags.a \
    /usr/lib/x86_64-linux-gnu/libsnappy.a \
    -lz -lpthread -ldl

# PNM worker process (receives compaction jobs via UNIX socket, drives MMIO)
/usr/bin/c++ $CXXFLAGS \
    CMakeFiles/pnm_compaction_unit.dir/tools/pnm_unit_main.cc.o \
    -o pnm_compaction_unit_static \
    librocksdb.a \
    /usr/lib/x86_64-linux-gnu/libgflags.a \
    /usr/lib/x86_64-linux-gnu/libsnappy.a \
    -lz -lpthread -ldl
```

**Outputs:** `build_gflags/db_bench_static`, `build_gflags/rocksdb_pnm_static`, `build_gflags/pnm_compaction_unit_static`

> **Tip:** `mount_disk_image.sh` (Section 6) performs all of the above automatically if the static binaries are missing. You can skip this section and let the script handle it.

---

## 6. One-Time Disk Image Setup

This step creates the guest OS disk image with all binaries and the PNM kernel module pre-installed. It only needs to run once (or again if you rebuild the binaries).

```bash
cd /workspaces/gem5/configs/yonsei
chmod +x mount_disk_image.sh
./mount_disk_image.sh
```

**What this script does:**

1. Builds static binaries (if not already built, see Section 5)
2. Builds the `m5` guest utility (if not already built, see Section 4)
3. Copies the base Ubuntu 24.04 image from `~/.cache/gem5/x86-ubuntu-24.04-img-4.0.0` to `disk_images/x86-ubuntu-24.04-with-db_bench.img`
4. Loop-mounts the image and installs:
   - `/usr/local/bin/db_bench` (baseline benchmark)
   - `/usr/local/bin/rocksdb_pnm` (PNM main process)
   - `/usr/local/bin/pnm_compaction_unit` (PNM worker)
   - `/sbin/m5` (gem5 guest signalling utility)
5. Adds a passwordless sudoers rule for the `gem5` guest user (needed for `insmod`)
6. Builds and installs the PNM kernel module via `chroot`:
   - Source: `configs/yonsei/pnm_module.c`
   - Output in guest: `/root/pnm_module.ko`

**The base disk image** is downloaded automatically by gem5 on the first simulation run if it is not already cached. If you have not run gem5 before, trigger the download first:

```bash
cd /workspaces/gem5
python3 -c "from gem5.resources.resource import obtain_resource; obtain_resource('x86-ubuntu-24.04-img-4.0.0')"
```

**Output:** `disk_images/x86-ubuntu-24.04-with-db_bench.img` (~4.9 GB)

**Expected runtime:** 5–15 minutes (dominated by the chroot kernel module build).

---

## 7. Running Simulations

Both simulations follow the same pattern: the guest OS boots under KVM at near-native speed, then gem5 switches to cycle-accurate TIMING mode at the start of the RocksDB benchmark region (ROI).

### 7.1 Baseline Simulation

```bash
cd /workspaces/gem5
./build/ALL/gem5.opt configs/yonsei/run_baseline.py
```

**System configuration:**
- 1 CPU core, KVM boot → TIMING ROI
- 3 GiB DDR4-2400 dual-channel
- Per-core L1D/L1I (32 KiB each), L2 (512 KiB)
- Workload: `db_bench fillrandom,readrandom,waitforcompaction --num=25000 --value_size=1024 --seed=42`

**Output directory:** `m5out/baseline_clean/`

**Expected runtime:** 1.5–2.5 hours (most of it is the TIMING-mode ROI)

### 7.2 PNM Simulation

```bash
cd /workspaces/gem5
./build/ALL/gem5.opt configs/yonsei/run_pnm.py
```

**System configuration:**
- 2 CPU cores, KVM boot → TIMING ROI
- Same memory and cache hierarchy as baseline
- `PNMCompactor` MMIO device at `0xD0000000` (500 ns latency, 25 GiB/s)
- Core 0: `rocksdb_pnm` (main thread); Core 1: `pnm_compaction_unit` (compaction worker)

**Output directory:** `m5out/pnm_clean/`

**Expected runtime:** 2–3 hours

### 7.3 Monitoring Progress

Gem5 prints progress to stdout. Key lines to watch:

```
# KVM boot phase (fast, a few minutes):
system.pc.com_1.device: [boot messages...]

# ROI start (TIMING mode begins, simulation slows down):
>>> ROI start: switching KVM -> TIMING CPU for detailed simulation

# For PNM only, drain is called first:
>>> ROI start: draining in-flight ops before KVM -> TIMING switch
>>> Drained; switching to TIMING CPU

# Benchmark running (guest output forwarded):
fillrandom   :   34.315 micros/op 29141 ops/sec ...
readrandom   :   54.752 micros/op 18264 ops/sec ...

# ROI end:
>>> ROI end: benchmark complete
>>> Simulation exiting cleanly
Simulation complete.
```

---

## 8. Analyzing Results

All results are in the `m5out/` directories. The two key output files are:

- `stats.txt` — gem5 microarchitectural counters (IPC, cache, memory)
- `board.pc.com_1.device` — RocksDB application output (throughput, latency, compaction stats)

### 8.1 Application Throughput

```bash
# Baseline
grep "fillrandom\|readrandom" m5out/baseline_clean/board.pc.com_1.device \
    | grep -v "thread\|benchmarks\|DB path"

# PNM
grep "fillrandom\|readrandom" m5out/pnm_clean/board.pc.com_1.device \
    | grep -v "thread\|benchmarks\|DB path"
```

### 8.2 CPU IPC

```bash
# Baseline (1 core)
grep "switch.*\.ipc " m5out/baseline_clean/stats.txt | head -1

# PNM (2 cores: switch0 = main RocksDB, switch1 = PNM unit)
grep "switch[01].*\.ipc " m5out/pnm_clean/stats.txt | head -4
```

### 8.3 L1D Cache (CPU 0)

```bash
grep "l1d-cache-0\.demandMisses::total\|l1d-cache-0\.demandAccesses::total\|l1d-cache-0\.demandMissRate::total\|l1d-cache-0\.demandAvgMissLatency::total" \
    m5out/baseline_clean/stats.txt | head -4

grep "l1d-cache-0\.demandMisses::total\|l1d-cache-0\.demandAccesses::total\|l1d-cache-0\.demandMissRate::total\|l1d-cache-0\.demandAvgMissLatency::total" \
    m5out/pnm_clean/stats.txt | head -4
```

### 8.4 L2 Cache (CPU 0)

```bash
grep "l2-cache-0\.demandMisses::total\|l2-cache-0\.demandHits::total\|l2-cache-0\.demandMissRate::total" \
    m5out/baseline_clean/stats.txt | head -3

grep "l2-cache-0\.demandMisses::total\|l2-cache-0\.demandHits::total\|l2-cache-0\.demandMissRate::total" \
    m5out/pnm_clean/stats.txt | head -3
```

### 8.5 DRAM Traffic

```bash
grep "mem_ctrl.*\.readReqs\b" m5out/baseline_clean/stats.txt | head -2
grep "mem_ctrl.*\.readReqs\b" m5out/pnm_clean/stats.txt | head -2
```

### 8.6 RocksDB Compaction Internals

```bash
grep "compaction\.times\.micros\|compaction\.key\.drop\.new\|numfiles\.in\.single" \
    m5out/baseline_clean/board.pc.com_1.device

grep "compaction\.times\.micros\|compaction\.key\.drop\.new\|numfiles\.in\.single" \
    m5out/pnm_clean/board.pc.com_1.device
```

---

## 9. Expected Results (v3 Workload)

Use these as a sanity check that your simulation produced correct output.

> **Note:** The numbers below are from the v3 run (`m5out/baseline_v3/`, `m5out/pnm_v3/`). The current config files output to `m5out/baseline_clean/` and `m5out/pnm_clean/`. Runs at identical workload parameters can produce different numbers due to KVM boot non-determinism and host scheduling; expect application throughput to be in the same ballpark but not identical. See [WORKLOADS.md](WORKLOADS.md) for the clean run's reported results.

### Application Throughput

| Metric | Baseline | PNM | Change |
|--------|----------|-----|--------|
| fillrandom throughput | 29,141 ops/sec | ~46,000–48,000 ops/sec | +58% |
| fillrandom latency | 34.3 µs/op | ~21 µs/op | −38% |
| readrandom throughput | 18,264 ops/sec | ~22,000–23,000 ops/sec | +25% |
| readrandom latency | 54.8 µs/op | ~44 µs/op | −20% |

### CPU IPC (TIMING mode, ROI)

| Core | Baseline | PNM |
|------|----------|-----|
| Core 0 (main) | 0.111 | 0.064 |
| Core 1 (PNM unit) | — | 0.085 |

> PNM core 0 IPC drops because it now shares DRAM bandwidth with core 1. The throughput gain comes from compaction being overlapped rather than serialized.

### L2 Cache (CPU 0)

| Metric | Baseline | PNM | Change |
|--------|----------|-----|--------|
| L2 misses | 11,057,107 | 3,944,040 | −64% |
| L2 miss rate | 7.68% | 5.85% | −1.9 pp |

### DRAM Read Requests

| Config | Read Requests |
|--------|--------------|
| Baseline | ~16,071,757 |
| PNM | ~14,148,149 |

---

## 10. Troubleshooting

**Disk image not found:**
```
CRITICAL ERROR: Custom disk image not found at /workspaces/gem5/disk_images/x86-ubuntu-24.04-with-db_bench.img
```
Run `configs/yonsei/mount_disk_image.sh` first. If the base image is also missing, run the resource download command from Section 6.

---

**Base image not cached:**
```
ERROR: Source image not found at /root/.cache/gem5/x86-ubuntu-24.04-img-4.0.0
```
Trigger the download by running gem5 once with any config that uses the same kernel/image resource, or use the Python snippet in Section 6.

---

**Kernel module fails to load in guest:**
```
[gem5] WARNING: pnm_module.ko failed to load — MMIO will be bypassed
```
The module was built against a different kernel version. The guest OS requires kernel `6.8.0-52-generic`. Re-run `mount_disk_image.sh` with matching kernel headers on the host. Verify with:
```bash
sudo chroot /mnt/gem5img uname -r   # should print 6.8.0-52-generic
```

---

**Simulation hangs at CPU switch:**

Symptom: gem5 prints the drain/switch messages and then appears to hang indefinitely.

Cause: The `PNMCompactor` had an in-flight completion event that was not drained before the switch. This is handled by `m5.drain()` in `workbegin_handler` in `run_pnm.py`. If you modify the config, ensure `m5.drain()` is always called before `simulator.switch_processor()` when a MMIO device is present.

---

**MSHR assertion failure:**
```
panic: MSHR target is not a request
```
This indicates a gem5 cache bug triggered during multi-core KVM→TIMING switch. The five core bug-fix patches in this repository (`src/mem/cache/mshr.cc`, `src/mem/cache/base.cc`, `src/mem/cache/cache.cc`, `src/mem/coherent_xbar.cc`, `src/mem/bridge.cc`) fix this. If you are merging with upstream gem5, ensure these patches are applied. See [docs/GEM5_CUSTOMIZATIONS.md](GEM5_CUSTOMIZATIONS.md) for details.

---

**Path mismatch errors:**

Config scripts and `mount_disk_image.sh` use hardcoded paths:
- gem5 at `/workspaces/gem5/`
- rocksdb at `/workspaces/rocksdb/`

If you cloned elsewhere, update the path constants at the top of:
- `configs/yonsei/run_baseline.py` (line: `DISK_IMAGE = ...`)
- `configs/yonsei/run_pnm.py` (line: `DISK_IMAGE = ...`)
- `configs/yonsei/mount_disk_image.sh` (lines: `DEST_DIR`, `BINARY`, etc.)

---

**gem5 build fails (SCons errors):**

Clean the build cache and retry:
```bash
cd /workspaces/gem5
rm -rf build/ALL/
scons build/ALL/gem5.opt -j$(nproc)
```

---

## 11. Project Structure Reference

```
gem5/
├── build/ALL/gem5.opt              ← gem5 simulator binary (after build)
├── configs/yonsei/
│   ├── run_baseline.py             ← baseline simulation config
│   ├── run_pnm.py                  ← PNM simulation config
│   ├── mount_disk_image.sh         ← one-time disk image builder
│   ├── pnm_module.c                ← PNM kernel driver (guest-side MMIO access)
│   └── pnm_module.Makefile
├── disk_images/
│   └── x86-ubuntu-24.04-with-db_bench.img   ← built by mount_disk_image.sh
├── docs/
│   ├── INSTALL_GUIDE.md            ← this file
│   ├── WORKLOADS.md                ← workload version history (v1, v2, v3)
│   ├── comp_v1.md                  ← v1 workload analysis (15k keys, fillrandom + waitforcompaction)
│   ├── comp_v2.md                  ← v2 workload analysis (15k keys + readrandom)
│   ├── comp_v3.md                  ← v3 performance analysis and comparison commands
│   ├── component_sizes.md          ← size hierarchy: dataset, caches, DRAM, PNM device
│   ├── GEM5_CUSTOMIZATIONS.md      ← technical reference for all gem5 changes
│   └── DB_BENCH_CUSTOMIZATION.md   ← reference for RocksDB modifications
├── m5out/
│   ├── baseline_clean/
│   │   ├── stats.txt               ← gem5 microarchitectural counters
│   │   ├── board.pc.com_1.device   ← RocksDB throughput and compaction output
│   │   └── config.ini              ← complete system configuration dump
│   └── pnm_clean/                     ← same structure for PNM run
├── src/dev/pnm/
│   ├── PNMCompactor.py             ← gem5 SimObject wrapper
│   ├── pnm_compactor.hh            ← MMIO register map, stats definitions
│   ├── pnm_compactor.cc            ← latency model, DMA, completion handling
│   └── SConscript
└── util/m5/build/x86/out/m5       ← guest ROI signalling utility (after build)

rocksdb/
├── tools/
│   ├── pnm_ipc.h                   ← UNIX socket framing helpers
│   ├── pnm_compaction_service.h    ← RocksDB CompactionService subclass
│   ├── db_bench_pnm_main.cc        ← rocksdb_pnm entry point
│   └── pnm_unit_main.cc            ← pnm_compaction_unit entry point
└── build_gflags/
    ├── db_bench_static             ← baseline binary (installed to guest)
    ├── rocksdb_pnm_static          ← PNM main binary (installed to guest)
    └── pnm_compaction_unit_static  ← PNM worker binary (installed to guest)
```
