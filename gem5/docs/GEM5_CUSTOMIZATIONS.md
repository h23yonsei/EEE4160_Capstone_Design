# gem5 Customizations — PNM Research Fork

This document describes every change made to the upstream gem5 simulator for the Yonsei University PNM (Processing Near Memory) research project. The goal of the research is to evaluate the performance benefit of offloading RocksDB LSM-tree compaction to a near-memory accelerator, eliminating the CPU and memory-bandwidth cost of compaction on the host processor.

**Simulation strategy:** The system boots under KVM (fast, near-native speed) to reach the RocksDB workload start point, then switches to the detailed TIMING CPU model for the region of interest (ROI). Only the benchmark phase accumulates stats.

---

## 1. New Device: PNMCompactor (`src/dev/pnm/`)

The core addition is a new gem5 SimObject that models a near-memory compaction accelerator.

### Files

| File | Purpose |
|------|---------|
| `src/dev/pnm/pnm_compactor.hh` | Class definition, MMIO register offset constants, `PNMStats` subclass |
| `src/dev/pnm/pnm_compactor.cc` | MMIO read/write handling, latency model, optional `physProxy` result write |
| `src/dev/pnm/PNMCompactor.py` | SimObject Python wrapper; exposes parameters to the config scripts |
| `src/dev/pnm/SConscript` | Build entry — compiles `pnm_compactor.cc` into the gem5 binary |

### MMIO Register Map

Base address: `0xD0000000`, region size: `0x100` bytes. The device is attached to the x86 IO bus via the existing PCIe bridge.

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| `+0x00` | `SRC_BYTES` | WO | Input (source) byte count for the pending compaction job |
| `+0x08` | `DST_BYTES` | WO | Output (destination) byte count |
| `+0x10` | `CMD` | WO | `1` = SUBMIT job, `2` = RESET device |
| `+0x14` | `STATUS` | RO | bit 0 = BUSY, bit 1 = DONE, bit 2 = ERROR |
| `+0x18` | `BYTES_PROC` | RO | Cumulative bytes processed (across all jobs) |
| `+0x20` | `JOBS_DONE` | RO | Cumulative jobs completed |

### Latency Model

When the guest writes CMD = SUBMIT, the device schedules a completion event after:

```
completion_latency = (SRC_BYTES + DST_BYTES) / bandwidth + process_latency
```

Configured parameters (as set in `run_pnm.py`): `bandwidth = 25 GiB/s`, `process_latency = 500 ns` (AxDIMM-style model: DDR4 round-trip + buffer-chip decode overhead). These match the SimObject defaults.

These are configurable in the SimObject Python interface and can be overridden at instantiation time.

### Two Operating Modes

- **Latency-only (default, `use_dma = False`):** The device only schedules the completion event. No memory is written. STATUS transitions BUSY → DONE when the event fires.
- **DMA mode (`use_dma = True`):** After the completion event fires, a 16-byte result record is written to `dma_addr` (default `0x80000000`) via `physProxy.writeBlob()`. Layout: `[bytesProcessed:8B][jobsCompleted:4B][pad:4B]` (little-endian, cumulative across all jobs). This uses the functional memory port rather than the coherent DMA port, avoiding assertion failures caused by in-flight DMA packets during the KVM→TIMING CPU switch.

### Drain Support

`PNMCompactor` implements `DrainState drain()`. Before the config script switches the CPU from KVM to TIMING, it calls drain on the device, which cancels or completes any in-flight `onComplete` event. This prevents a KVM-issued job from firing its completion event during the TIMING phase and corrupting stats.

### Statistics

Tracked under the device's stats group:

| Stat | Description |
|------|-------------|
| `bytesProcessed` | Total bytes (src + dst) passed through the accelerator |
| `jobsCompleted` | Total compaction jobs submitted and completed |

---

## 2. Simulation Configs (`configs/yonsei/`)

### `run_baseline.py`

Full-system x86 simulation running stock RocksDB `db_bench`. Serves as the performance reference.

| Parameter | Value |
|-----------|-------|
| CPU cores | 1, KVM (boot) → TIMING (ROI) |
| ISA | x86\_64 |
| Memory | 3 GiB DDR4-2400, dual-channel |
| L1 cache | 32 KiB I-cache + 32 KiB D-cache (private) |
| L2 cache | 512 KiB (private) |
| Workload | `db_bench fillrandom` (25,000 keys) → `readrandom` → `waitforcompaction` |
| ROI markers | `m5 workbegin` / `m5 workend` magic ops in the guest binary |
| Output dir | `m5out/baseline_clean/` |

The guest calls `m5 resetstats` **before** `workbegin` (to clear boot-phase stats), then `m5 workbegin` / `m5 workend` bracket the ROI, followed by `m5 dumpresetstats` and `m5 exit`. The stats file therefore captures only the benchmark phase.

### `run_pnm.py`

Identical to `run_baseline.py` except:

| Parameter | Value |
|-----------|-------|
| CPU cores | 2 — core 0: `rocksdb_pnm`, core 1: `pnm_compaction_unit` (spawned by rocksdb) |
| PNMCompactor | Attached to x86 IO bus at `0xD0000000`; `bandwidth = 25 GiB/s`, `process_latency = 500 ns` (AxDIMM-style), `use_dma = True` |
| Drain | Config calls `m5.drain()` in `workbegin_handler` before `simulator.switch_processor()` |
| Output dir | `m5out/pnm_clean/` |

`rocksdb_pnm` is a patched build of RocksDB that intercepts the compaction path and, instead of doing compaction on the CPU, writes job descriptors to `/dev/pnm` (the MMIO interface), then polls STATUS until DONE. `pnm_compaction_unit` is a helper process that occupies core 1 — it represents the near-memory compute element and handles the actual data movement via shared memory with `rocksdb_pnm`.

### `mount_disk_image.sh`

Automates building the guest disk image (`disk_images/x86-ubuntu-24.04-with-db_bench.img`, ~4.9 GB, Ubuntu 24.04). Steps:

1. Mount the raw image via loop device
2. Build `db_bench` as a static binary with gflags support
3. Build `rocksdb_pnm` and `pnm_compaction_unit`
4. Build the `m5` guest utility with `workbegin`/`workend` support
5. Copy all binaries to `/usr/local/bin/` inside the image
6. Cross-compile `pnm_module.ko` against kernel 6.8.0-52-generic via chroot
7. Install the kernel module and configure passwordless sudo for the gem5 user

---

## 3. Guest Kernel Module (`configs/yonsei/pnm_module.c`)

A GPL-2.0 Linux kernel module that provides userspace access to the PNMCompactor MMIO region.

- Registers `/dev/pnm` as a character device
- The `mmap` file operation maps the 256-byte MMIO window at `0xD0000000` into userspace with `pgprot_noncached` page attributes — critical so that polling the STATUS register does not read a stale cached value
- Eliminates the need for `/dev/mem` access; `rocksdb_pnm` opens `/dev/pnm` and mmaps it directly
- Compiled by `configs/yonsei/pnm_module.Makefile` against kernel 6.8.0-52-generic headers

---

## 4. gem5 Core Bug Fixes

All five files below were patched to fix assertion failures and crashes that appear when running multi-core KVM→TIMING simulations with MMIO devices. The underlying cause in every case: packets issued during the KVM phase (atomic request packets) arrive at the cache or interconnect during the TIMING phase already partially converted to response format, violating assumptions baked into upstream gem5.

### `src/mem/cache/mshr.cc`

**`TargetList::updateFlags()`**
A deferred MSHR target can be a `WritebackAck` response when a writeback completes while the MSHR is still in service. Upstream code calls `pkt->needsWritable()` on every target without checking whether it is a request. `needsWritable()` asserts on response packets.
- Fix: guard with `pkt->isRequest()` before calling `needsWritable()`.

**`promoteReadable()`**
Same class of bug — the promotable predicate checks `needsWritable()` without confirming the packet is a request. Response packets in deferred targets must be treated as promotable (return `true` early) rather than having their writable bit inspected.
- Fix: guard with `pkt->isRequest()`; return `true` immediately for response packets.

### `src/mem/cache/base.cc`

**Null `senderState` in `recvTimingResp()`**
During the mode switch, some synthesized/orphaned response packets arrive with no `senderState`. Upstream code unconditionally dereferences it.
- Fix: drop packets with null `senderState`; the real response will eventually arrive and clean up the MSHR.

**`LockedRMWWriteReq` with evicted block**
If a cache block is evicted during the KVM→TIMING switch, a pending `LockedRMWWriteReq` for that block asserts because it expects the block to be present.
- Fix: if the block is missing, demote the packet to a plain `WriteReq` and handle it as a normal miss.

**Stats recording for response targets**
Hit/miss stats are stored in a vector indexed by packet command. Response packets have different command values than requests; indexing the stats vector with a response command causes an out-of-bounds access.
- Fix: guard stats recording behind `initial_tgt->pkt->isRequest()`.

### `src/mem/cache/cache.cc`

**Software prefetch path (`needsWritable` assert)**
The software prefetch handling calls `needsWritable()` without an `isRequest()` guard. Larger workloads (25,000 keys) reach this code path more frequently and reliably trigger the assertion.
- Fix: guard with `isRequest()` before the `needsWritable()` call (~line 745).

**`isUncacheable()` miss-latency guard**
MMIO writes that are orphaned into the MSHR during the mode switch have no cacheable miss latency to record. Upstream unconditionally updates the miss-latency stat.
- Fix: skip the stat when `isUncacheable()` is true (~line 815).

**Data-copy loop in `serviceMSHRTargets()`**
When iterating MSHR targets, a target that is already a response packet must be skipped — its data was already handled. Additionally, byte-level copies between a block-aligned response and a sub-block target need bounds checking.
- Fix: check `!tgt_pkt->isRequest()` to skip already-response targets; add defensive bounds check for overlapping byte copies.

**`copyResponderFlags()` guard**
Called unconditionally on every target; asserts on response packets.
- Fix: guard with `isRequest()` (~line 897).

**Response path restructure (~lines 901–933)**
Multiple related issues consolidated:
- Break early if the target packet is already a response (avoids use-after-free from `makeTimingResponse()`)
- Check `needsResponse()` before calling `makeTimingResponse()` — `InvalidateReq`, `CleanEvict`, and similar housekeeping packets must not receive ACKs
- **Fresh `ReadReq` path:** when a deferred MSHR target is already a response, a new `ReadReq` is created and re-circulated rather than calling `makeTimingResponse()` on a stale response packet. Background: a `WriteResp` never pops the MSHR sender state, causing the MSHR to stay in-service forever; a `ReadResp` pops it correctly. Re-circulating as a `ReadReq` ensures the MSHR is cleaned up.

### `src/mem/coherent_xbar.cc`

**Orphan response assertion**
Upstream asserts when a response packet arrives at the crossbar with no `routeTo` entry (meaning the crossbar never saw the original request). This can happen when a device synthesizes a response without going through the normal request path, particularly during bridge/device misbehavior at the KVM→TIMING boundary.
- Fix: replace the assertion with a defensive packet drop and a warning.

### `src/mem/bridge.cc`

**`BridgeRequestPort::recvTimingResp()` (~lines 135–148)**
Some x86 IO devices return the original request packet from their timing response callback instead of calling `makeAtomicResponse()` first. Upstream asserts that the received packet is a response.
- Fix: if `pkt->isRequest() && pkt->needsResponse()`, attempt `pkt->makeResponse()` to convert it; discard the packet if it cannot be converted.

**`BridgeResponsePort::trySendTiming()` (~lines 310–334)**
Same class of issue on the response port side — dequeued packets may be unconverted request packets.
- Fix: attempt `makeResponse()` conversion; dequeue and discard unconvertible packets, reschedule the send event, and handle retry requests after cleanup.

---

## 5. Simulation Outputs (`m5out/`)

| Directory | Config | Workload |
|-----------|--------|---------|
| `m5out/baseline_clean/` | `run_baseline.py` | `fillrandom` 25k keys + `readrandom` + `waitforcompaction` |
| `m5out/pnm_clean/` | `run_pnm.py` | same workload, with PNMCompactor offload |

Each directory contains: `config.ini`, `config.json`, `config.dot`, `stats.txt`, and the board device tree trace. The `stats.txt` files cover only the ROI (post-`workbegin` to `workend`).

---

## 6. Related Documentation

- [docs/WORKLOADS.md](WORKLOADS.md) — workload version history and benchmark parameter log
- [docs/comp_v3.md](comp_v3.md) — full performance analysis for the current (v3) workload
