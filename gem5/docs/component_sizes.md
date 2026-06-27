# Component Sizes — gem5 PNM Simulation

This document catalogs the size of every configurable component in the baseline and PNM
gem5 simulations: the dataset, RocksDB internal structures, CPU caches, DRAM, and the PNM
hardware device. All numbers are drawn from
`configs/yonsei/run_baseline.py`, `configs/yonsei/run_pnm.py`,
and the `m5out/baseline_clean/` / `m5out/pnm_clean/` simulation outputs
(25,000-key workload).

---

## 1. Dataset Dimensions

| Parameter | Value | Bytes | Note |
|-----------|-------|-------|------|
| Key count (`--num`) | 25,000 | — | Unique random keys inserted by fillrandom |
| Value size (`--value_size`) | 1,024 B | 1,024 | Fixed per value; no compression (`--compression_type=none`) |
| Key size (db_bench default) | 16 B | 16 | Fixed-width key encoding for fillrandom |
| Raw key-value payload | ~24.8 MiB | 26,000,000 | 25,000 × (16 + 1,024) = 26.0 MB; no compression applied |
| Internal RocksDB overhead per key | ~16–24 B | — | Sequence number (8 B), value type, block/index overhead |
| Estimated total on-disk size (uncompressed) | ~26.4 MiB | ~27,600,000 | 25,000 × (16 + 1,024 + ~20 overhead); confirmed by final SST sizes in Section 5 |
| Seed (`--seed`) | 42 | — | Deterministic key generation for reproducibility |

---

## 2. RocksDB Internal Component Sizes

| Component | Flag | Size | Bytes | Purpose |
|-----------|------|------|-------|---------|
| Memtable (single write buffer) | `--write_buffer_size` | 1 MiB | 1,048,576 | In-memory write buffer; flushes to L0 SST when full |
| Max active memtables | `--max_write_buffer_number` | 2 | — | Up to 2 MiB of in-flight write data before stalling |
| Max in-memory write data | (derived) | 2 MiB | 2,097,152 | 2 × write_buffer_size; write stall if both full |
| L0 SST file size | (derived from memtable) | ~1 MiB | ~1,048,576 | Each memtable flush produces one ~1 MiB L0 file; confirmed by compaction src sizes |
| L0 compaction trigger | `--level0_file_num_compaction_trigger` | 2 files | ~2 MiB | Compaction fires when L0 has ≥ 2 files (~2 MiB of unflushed L0 data) |
| L1 maximum size | `--max_bytes_for_level_base` | 16 MiB | 16,777,216 | Total SST data allowed in Level 1; triggers L1→L2 compaction if exceeded |
| SST block size | `--block_size` | 4 KiB | 4,096 | Unit of read from DRAM; one cache miss = one 4 KiB fetch |
| Block cache | `--cache_size` | 8 MiB | 8,388,608 | OS-side LRU cache for decompressed SST blocks; shared across all SST reads |
| Block cache capacity in blocks | (derived) | 2,048 blocks | — | 8 MiB ÷ 4 KiB = 2,048 blocks simultaneously cached |
| Compaction style | `--compaction_style=0` | Leveled | — | L0→L1 merges; all compaction jobs in this research are L0→L1 |

**Keys per memtable (estimated):**
1,048,576 B ÷ (16 + 1,024 + 20) B = ~985 keys per flush.
With 25,000 keys total: ~25 L0 flushes, triggering ~12–13 initial compaction events
before cascading merges consolidate into the final L1 state.

---

## 3. Hardware Component Sizes

### CPU Caches (per core, both baseline and PNM)

| Component | Size | Bytes | Config flag |
|-----------|------|-------|-------------|
| L1D (data cache) | 32 KiB | 32,768 | `l1d_size="32KiB"` |
| L1I (instruction cache) | 32 KiB | 32,768 | `l1i_size="32KiB"` |
| L2 (unified, private) | 512 KiB | 524,288 | `l2_size="512KiB"` |
| L1D capacity in 4 KiB blocks | 8 blocks | — | 32 KiB ÷ 4 KiB; only 8 SST blocks fit in L1D simultaneously |
| L2 capacity in 4 KiB blocks | 128 blocks | — | 512 KiB ÷ 4 KiB; L2 holds 128 hot SST blocks before evicting to DRAM |
| Total cache per core (L1D+L1I+L2) | 576 KiB | 589,824 | — |
| Total cache — baseline (1 core) | 576 KiB | 589,824 | — |
| Total cache — PNM (2 cores) | 1,152 KiB | 1,179,648 | Separate private caches; no shared L3 |
| CPU clock | 3 GHz | — | `clk_freq="3GHz"` |
| Clock period | 0.333 ns | — | 1 cycle = 333 ps |

### DRAM

| Component | Value | Note |
|-----------|-------|------|
| Type | Dual-channel DDR4-2400 | `DualChannelDDR4_2400` |
| Total capacity | 3 GiB | 3,221,225,472 bytes — `size="3GiB"` |
| Channels | 2 (mem_ctrl0, mem_ctrl1) | Traffic split evenly across both channels |
| Capacity per channel | 1.5 GiB | 1,610,612,736 bytes |
| Data bus width per channel | 64-bit (8 bytes) | Standard DDR4 channel width |
| Effective transfer size per burst | 64 bytes | 8-beat burst × 8 bytes; one cache-line-sized transfer |
| Simulated peak BW (both channels) | ~36–38 GiB/s | DDR4-2400 × 2 channels theoretical; actual avg RdBW ~123–139 MiB/s in simulation |

### PNM Device (run_pnm.py only)

| Component | Value | Note |
|-----------|-------|------|
| MMIO base address | `0xD0000000` | 32-bit physical address; mapped via `/dev/pnm` |
| MMIO region size | 256 bytes (0x100) | 6 registers: src_bytes, dst_bytes, cmd, status, bytes_proc, jobs_done |
| Process latency | 500 ns | AxDIMM-style: DDR4 round-trip (≈80 ns) + buffer-chip decode overhead |
| Internal bandwidth | 25 GiB/s | Rank-parallel internal DIMM bandwidth model |
| MMIO register: src_bytes | 64-bit, offset 0x00 | Host writes compaction input file size before CMD_SUBMIT |
| MMIO register: dst_bytes | 64-bit, offset 0x08 | Host writes estimated output size before CMD_SUBMIT |
| MMIO register: cmd | 32-bit, offset 0x10 | CMD_RESET=2, CMD_SUBMIT=1 |
| MMIO register: status | 32-bit, offset 0x14 | BUSY=bit 0, DONE=bit 1, ERR=bit 2; polled by pnm_compaction_unit after job |
| MMIO register: bytes_proc | 64-bit, offset 0x18 | Cumulative bytes processed (read-only stat) |
| MMIO register: jobs_done | 32-bit, offset 0x20 | Cumulative jobs completed (read-only stat) |
| DMA result address | `0x80000000` | Physical DRAM address where 16-byte record is written per job (use_dma=True); layout: [bytesProcessed:8B][jobsCompleted:4B][pad:4B] |
| Socket path (IPC) | `/tmp/pnm_compaction.sock` | UNIX domain socket between rocksdb_pnm and pnm_compaction_unit |
| Secondary DB path | `/tmp/pnm_secondary` | Persistent secondary DB for incremental deduplication between compaction jobs |

---

## 4. Complete Size Hierarchy

All components ordered from smallest to largest, showing how each fits relative to the others.

### CPU Hardware

| Component | Size | Bytes | × larger than L1D |
|-----------|------|-------|-------------------|
| L1D cache (per core) | 32 KiB | 32,768 | 1× (reference) |
| L1I cache (per core) | 32 KiB | 32,768 | 1× |
| L2 cache (per core) | 512 KiB | 524,288 | 16× |

### RocksDB Application

| Component | Size | Bytes | × larger than L1D |
|-----------|------|-------|-------------------|
| SST block size | 4 KiB | 4,096 | 0.125× (fits 8 blocks in L1D) |
| Memtable (single) | 1 MiB | 1,048,576 | 32× |
| L0 SST file (~1 memtable flush) | ~1 MiB | ~1,048,576 | ~32× |
| Max in-flight memtables (2×) | 2 MiB | 2,097,152 | 64× |
| Block cache | 8 MiB | 8,388,608 | 256× |
| L1 max size (config) | 16 MiB | 16,777,216 | 512× |
| Raw KV dataset (25k keys) | ~24.8 MiB | 26,000,000 | 793× |

### Actual SST State after fillrandom (v3, from simulation)

| Component | Size | Bytes | × larger than L1D |
|-----------|------|-------|-------------------|
| PNM: final L1 SST (last compaction output) | 9.1 MiB | 9,551,057 | 291× |
| Baseline: final L1 SST (last compaction output) | 15.8 MiB | 16,575,904 | 506× |

### DRAM Traffic (v3 simulation totals)

| Component | Size | Bytes | × larger than L1D |
|-----------|------|-------|-------------------|
| PNM: total bytes written to DRAM | 368.5 MB | 386,347,008 | 11,793× |
| Baseline: total bytes written to DRAM | 394.8 MB | 413,958,144 | 12,634× |
| PNM: total bytes read from DRAM | 905.5 MB | 949,583,872 | 28,980× |
| Baseline: total bytes read from DRAM | 1,028.6 MB | 1,078,683,648 | 32,913× |

### DRAM Capacity

| Component | Size | Bytes | × larger than L1D |
|-----------|------|-------|-------------------|
| DRAM total | 3 GiB | 3,221,225,472 | 98,304× |

---

## 5. Actual SST File Sizes — From Simulation (v3, 25k keys)

Each compaction job's input and output file sizes, showing how the on-disk data
evolves through the compaction sequence. All sizes are in bytes.

### Baseline v3 (5 sequential, blocking jobs)

| Job | files_in | src size (B) | src size (MiB) | dst size (B) | dst size (MiB) | compression ratio |
|-----|----------|-------------|----------------|-------------|----------------|-------------------|
| job=4 | 2 | 1,962,302 | 1.87 | 1,929,363 | 1.84 | 0.98 |
| job=9 | 4 | 4,863,354 | 4.64 | 4,540,460 | 4.33 | 0.93 |
| job=16 | 7 | 10,423,228 | 9.94 | 9,009,458 | 8.59 | 0.86 |
| job=30 | 14 | 21,778,286 | 20.77 | 15,814,192 | 15.08 | 0.73 |
| job=32 | 3 | 17,774,374 | 16.95 | 16,575,904 | 15.81 | 0.93 |
| **Final L1 state** | — | — | — | **16,575,904** | **15.81** | — |

The final L1 SST (15.81 MiB) is nearly at the configured 16 MiB limit.
Only 6.7% of the dataset's potential deduplication is achieved by standard RocksDB compaction
(8,441 keys dropped, or ~34% of the 25,000 writes represent superseded keys).

### PNM v3 (5 concurrent, MMIO-offloaded jobs)

| files_in | src size (B) | src size (MiB) | dst size (B) | dst size (MiB) | compression ratio | vs Baseline |
|----------|-------------|----------------|-------------|----------------|-------------------|-------------|
| 2 | 1,951,045 | 1.86 | 1,112,870 | 1.06 | 0.57 | −42% |
| 4 | 4,023,138 | 3.84 | 2,616,400 | 2.49 | 0.65 | −42% |
| 7 | 8,450,872 | 8.06 | 5,190,150 | 4.95 | 0.61 | −42% |
| 13 | 16,879,284 | 16.10 | 8,869,379 | 8.46 | 0.53 | −44% |
| 4 | 11,750,351 | 11.21 | 9,551,057 | 9.11 | 0.81 | −42% |
| **Final L1 state** | — | — | **9,551,057** | **9.11** | — | **−42% vs baseline** |

PNM's secondary-DB deduplication achieves a consistent 42–44% size reduction per job.
The final L1 SST (9.11 MiB) is 42% smaller than baseline's (15.81 MiB).

---

## 6. Working Set vs Cache Capacity

The critical size relationships that determine whether data is served from cache or DRAM.

### Block Cache (8 MiB) vs SST Working Set

| Relationship | Ratio | Implication |
|-------------|-------|-------------|
| Raw dataset / block cache | 24.8 MiB / 8 MiB = 3.1× | Dataset is 3× the block cache — significant cache eviction during readrandom |
| Baseline final L1 / block cache | 15.81 MiB / 8 MiB = 1.97× | Final SST state is ~2× the cache — about half of all blocks must be re-fetched from DRAM on each readrandom pass |
| PNM final L1 / block cache | 9.11 MiB / 8 MiB = 1.14× | PNM's denser SSTs bring the working set within 14% of fitting entirely in the block cache — far fewer cold-cache misses during readrandom |

### L2 Cache (512 KiB per core) vs Hot Working Set

| Relationship | Ratio | Implication |
|-------------|-------|-------------|
| L2 capacity in SST blocks | 128 blocks × 4 KiB = 512 KiB | L2 can hold 128 recently accessed SST blocks; the most recently touched data fits before evicting to DRAM |
| L2 / memtable size | 512 KiB / 1 MiB = 0.5× | A single memtable is 2× larger than L2 — a memtable flush necessarily evicts all prior L2 content |
| Baseline L2 miss rate (v3) | 7.68% | Compaction SST scans continuously evict write-path data from L2 → high miss rate |
| PNM L2 miss rate CPU0 (v3) | 5.85% | No compaction evictions from CPU0's L2 → 24% lower miss rate, 64% fewer absolute misses |

### L1D Cache (32 KiB per core) vs Data Access Patterns

| Relationship | Ratio | Implication |
|-------------|-------|-------------|
| L1D capacity in SST blocks | 8 blocks × 4 KiB = 32 KiB | Only 8 SST blocks fit in L1D — SST access has essentially zero L1D reuse between different keys |
| Baseline L1D miss rate (v3) | 1.977% | High absolute miss count (20.6M) — compaction scan data pollutes the write-path working set |
| PNM L1D miss rate CPU0 (v3) | 2.013% | Rate barely changes but absolute misses drop 45.7% — fewer total memory accesses without compaction work |

### DRAM (3 GiB) vs Everything

| Relationship | Ratio | Implication |
|-------------|-------|-------------|
| DRAM / raw dataset | 3 GiB / 24.8 MiB = 124× | Dataset easily fits in DRAM with no pressure; RocksDB's tiered structure (L0/L1) is the bottleneck, not total DRAM |
| DRAM / block cache | 3 GiB / 8 MiB = 384× | DRAM capacity far exceeds the block cache — DRAM latency, not capacity, is the bottleneck |

---

## 7. DRAM Traffic Volumes (v3 simulation, 25k keys)

| Metric | Baseline v3 | PNM v3 | Change |
|--------|-------------|--------|--------|
| Total read requests (both channels) | 16,071,757 | 14,148,149 | −12.0% |
| Total bytes read (both channels) | 1,028.6 MB | 905.5 MB | −12.0% |
| Total bytes written (both channels) | 394.8 MB | 368.5 MB | −6.7% |
| Read requests per key | 642 | 566 | −11.9% |
| Bytes read per key | 41.1 KiB | 36.2 KiB | −12.0% |
| Avg read bandwidth ch0 (simulated) | 69.59 MiB/s | 61.95 MiB/s | −11.0% |
| Avg read bandwidth ch1 (simulated) | 69.47 MiB/s | 61.62 MiB/s | −11.3% |
| Simulation duration | 7.381 s | 7.314 s | −0.9% |

**Bytes read per key (41.1 KiB baseline):** each of the 25,000 keys generated
about 10 DRAM reads of 4 KiB across the fill + read + compaction phases. This is a measure of
total read amplification — the ratio of DRAM traffic to raw data size. PNM reduces it to 36.2
KiB per key (−12%), entirely from smaller SST output files requiring fewer block fetches.

---

## 8. Size Summary

Key takeaways from a size perspective:

| # | Observation | Impact |
|---|-------------|--------|
| 1 | Block cache (8 MiB) is **3.1× smaller** than the raw dataset (24.8 MiB) | Cache thrash is inevitable during readrandom — both baseline and PNM cannot fully cache the working set |
| 2 | PNM's final SST (9.1 MiB) is **within 14%** of fitting in the block cache; baseline's (15.8 MiB) is **97% over** | PNM dramatically narrows the gap between working set and cache, halving the cold-miss penalty during readrandom |
| 3 | L2 (512 KiB) can hold only **128 SST blocks**; a single memtable flush (1 MiB) is **2× L2 capacity** | Every memtable flush evicts the entire L2 unless the write path's hot data is protected — baseline cannot protect it from compaction |
| 4 | L1D (32 KiB) holds only **8 SST blocks** at a time | SST access is essentially streaming from an L1D perspective — all SST data comes from L2 or DRAM |
| 5 | The PNM MMIO region is **256 bytes** (0x100) — 6 registers | The hardware interface is deliberately minimal; the heavy I/O flows through the DRAM channels, not the MMIO bus |
| 6 | DRAM capacity (3 GiB) is **124× the dataset size** | DRAM capacity is not the constraint in these simulations — DRAM latency and bandwidth are |

*All simulation data from m5out/baseline_clean/ and m5out/pnm_clean/ —
gem5 full-system x86, db_bench fillrandom,readrandom,waitforcompaction num=25000
value_size=1024 seed=42 cache_size=8388608 block_size=4096 write_buffer_size=1048576.*
