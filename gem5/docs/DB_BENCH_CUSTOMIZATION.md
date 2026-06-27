# db_bench Customization for PNM Research

This document describes every change made to RocksDB's `db_bench` tool to support
Processing-Near-Memory (PNM) compaction offload in the gem5 simulation.
All source modifications live under `tools/`.

---

## Overview

The goal was to offload RocksDB level compactions to a simulated PNM device (an
MMIO-addressable near-memory accelerator in gem5) without touching RocksDB's core.
Two extension modes were added:

| Mode | Flag | Binary |
|------|------|--------|
| Diagnostic listener — logs compaction I/O stats and fires MMIO doorbell | `--pnm_offload=1` | `db_bench` |
| True offload — routes compactions to a worker process on the PNM core | `--pnm_true_offload=1` | `rocksdb_pnm` (injects flag automatically) |

---

## New source files

All files are under `tools/`.

### `pnm_ipc.h`
Shared wire-protocol helpers used by both `rocksdb_pnm` and `pnm_compaction_unit`.

- UNIX domain socket path: `/tmp/pnm_compaction.sock`
- Framing: every message is prefixed by a 4-byte little-endian length, then the payload
- Helpers: `pnm_send_frame` / `pnm_recv_frame`; both loop on `EINTR`

### `pnm_compaction_service.h`
Implements `PNMCompactionService`, a `CompactionService` subclass.  Installed into
`DBOptions` when `--pnm_true_offload=1`.

**`Schedule(job_info, compaction_service_input)`**
1. Builds a compound message: three length-prefixed sub-fields — `db_name`, `job_id_str`,
   `CompactionServiceInput` bytes.
2. Connects to `pnm_compaction_unit` over UNIX socket (retries up to 50 × 100 ms).
3. Sends the compound message.
4. Fires the MMIO doorbell on `/dev/pnm`:
   - Resets device (`CMD_RESET`)
   - Writes actual input-file sizes to `src_bytes` and `dst_bytes` (dst estimated = src)
   - Writes `CMD_SUBMIT`
5. Returns the socket fd encoded in `scheduled_job_id` as `"<job_id>:<fd>"`.
6. Falls back to `kUseLocal` (CPU compaction) on any socket error.

**`Wait(scheduled_job_id, result)`**
1. Parses the socket fd from `scheduled_job_id`.
2. Blocks on `pnm_recv_frame` — the response only arrives after `pnm_compaction_unit`
   has finished the compaction **and** polled `MMIO STATUS_DONE`.
3. Decodes the 1-byte status prefix; on success, passes the `CompactionServiceResult`
   bytes back to RocksDB.
4. Falls back to `kUseLocal` on recv failure.

**MMIO register map** (base `0xD0000000`, size `0x100`)

| Offset | Width | Name | Direction |
|--------|-------|------|-----------|
| `0x00` | 64-bit | `src_bytes` | host → device |
| `0x08` | 64-bit | `dst_bytes` | host → device |
| `0x10` | 32-bit | `cmd` | host → device |
| `0x14` | 32-bit | `status` | device → host |

`cmd` values: `CMD_RESET = 2`, `CMD_SUBMIT = 1`.
`status` bits: `BUSY = bit 0`, `DONE = bit 1`, `ERR = bit 2`.

### `db_bench_pnm_main.cc`
Entry point for the `rocksdb_pnm` binary.

1. Pins itself to cores 0–1 via `sched_setaffinity`.
2. Locates `pnm_compaction_unit` in the same directory as the running binary
   (`/proc/self/exe`), then `fork`/`exec`s it.  Waits 500 ms for the worker to bind
   its socket before continuing.
3. Prepends `--pnm_true_offload=1` to `argv` (users can still pass
   `--nopnm_true_offload` to override).
4. Calls the normal `db_bench_tool(argc, argv)` entry point.
5. Registers `atexit(cleanup_pnm_unit)` to `SIGTERM` the worker on exit.

### `pnm_unit_main.cc`
Entry point for the `pnm_compaction_unit` binary — the PNM-core worker.

1. Pins itself to core 1 via `sched_setaffinity`.
2. Maps `/dev/pnm` into its address space (same `0x100`-byte MMIO region).
3. Binds `AF_UNIX SOCK_STREAM` on `/tmp/pnm_compaction.sock`, listens, and spawns one
   thread per accepted connection.
4. Each thread calls `handle_job()`:
   - Parses the three sub-fields from the compound frame.
   - Opens a persistent secondary DB via `DB::OpenAsSecondary()` on first call
     (`/tmp/pnm_secondary`); subsequent calls reuse it.
   - Calls `TryCatchUpWithPrimary()` for a fast incremental MANIFEST sync.
   - Parses `CompactionServiceInput`, then calls
     `CompactWithoutInstallation()` — compaction only, no re-open overhead.
   - Falls back to the heavier `DB::OpenAndCompact()` path if secondary-DB open fails.
   - After the compaction finishes, polls `MMIO STATUS_DONE` (spin on `status` register)
     to absorb the PNM timing-model latency before unblocking the sender.
   - Sends back a 1-byte status (`0x00` = ok) + serialised `CompactionServiceResult`.

---

## Modifications to `db_bench_tool.cc`

Two new flags (defined with `DEFINE_bool`):

- **`--pnm_offload`** — installs `PNMCompactionOffloadListener` as an
  `EventListener`.  On every compaction completion the listener reads
  `stats.total_input_bytes` / `stats.total_output_bytes` and fires a single MMIO write
  to the `cmd` register.  Diagnostic only; compaction still runs on the CPU.

- **`--pnm_true_offload`** — instantiates `PNMCompactionService` and assigns it to
  `options.compaction_service`.  All compaction jobs are then routed through the IPC
  + MMIO path described above.

---

## Benchmark flags used in gem5 simulations

Both `run_baseline.py` and `run_pnm.py` use an identical flag set:

```
--db=/tmp/rocksdb_fs_{baseline|pnm}
--benchmarks=fillrandom,readrandom,waitforcompaction
--num=25000
--seed=42
--value_size=1024
--compaction_style=0              # Leveled compaction
--block_size=4096
--cache_size=8388608              # 8 MiB block cache
--write_buffer_size=1048576       # 1 MiB memtable
--max_write_buffer_number=2
--max_bytes_for_level_base=16777216   # 16 MiB
--disable_wal=1
--stats_interval=1000
--level0_file_num_compaction_trigger=2
--compression_type=none
--statistics=1
```

The PNM run uses the `rocksdb_pnm` binary, which automatically appends
`--pnm_true_offload=1`.  The baseline run uses the unmodified `db_bench` binary.

---

## gem5 simulation differences

| Property | Baseline (`run_baseline.py`) | PNM (`run_pnm.py`) |
|----------|------------------------------|---------------------|
| Binary | `/usr/local/bin/db_bench` | `/usr/local/bin/rocksdb_pnm` |
| CPU cores | 1 (KVM → TIMING) | 2 (core 0 = main, core 1 = PNM worker) |
| PNM device | — | `PNMCompactor` at `0xD0000000` |
| Kernel module | — | `pnm_module.ko` loaded before ROI |
| Output dir | `m5out/baseline_clean` | `m5out/pnm_clean` |
| CPU switch point | `workbegin` | `workbegin` (after `m5.drain()`) |

The PNM gem5 device is configured with `process_latency=500ns` and
`bandwidth=25GiB/s` (AxDIMM-style model).

---

## Build

All binaries are statically linked (no shared-lib dependency inside the disk image).

```bash
# From /workspaces/rocksdb
mkdir -p build_gflags && cd build_gflags
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

Static link step (see `configs/yonsei/mount_disk_image.sh` for full flags) produces:
- `build_gflags/db_bench_static`
- `build_gflags/rocksdb_pnm_static`
- `build_gflags/pnm_compaction_unit_static`

---

## Disk image installation

`configs/yonsei/mount_disk_image.sh` mounts the gem5 Ubuntu 24.04 image and installs:

| Host path | Guest path |
|-----------|------------|
| `build_gflags/db_bench_static` | `/usr/local/bin/db_bench` |
| `build_gflags/rocksdb_pnm_static` | `/usr/local/bin/rocksdb_pnm` |
| `build_gflags/pnm_compaction_unit_static` | `/usr/local/bin/pnm_compaction_unit` |
| `util/m5/build/x86/out/m5` | `/sbin/m5` |
| `configs/yonsei/pnm_module.ko` (chroot build) | `/root/pnm_module.ko` |
