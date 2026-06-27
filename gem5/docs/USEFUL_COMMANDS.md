# Useful Commands

Quick reference for running, monitoring, and analyzing gem5 PNM simulations.

---

## tmux

```bash
# Install
apt-get update && apt-get install -y tmux

# New named session
tmux new -s sim

# List sessions
tmux ls

# Re-attach
tmux attach -t sim

# Detach (from inside session)
Ctrl-b d

# Split pane horizontally (two terminals side-by-side)
Ctrl-b %

# Split pane vertically (stacked)
Ctrl-b "

# Switch between panes
Ctrl-b arrow-key

# Scroll mode (to read output)
Ctrl-b [        # enter scroll; q to exit
```

---

## Build

```bash
# Build gem5 (ALL variant includes PNMCompactor)
cd /workspaces/gem5
scons build/ALL/gem5.opt -j$(nproc)

# Check KVM availability
ls /dev/kvm && echo "KVM available" || echo "KVM not available"
sudo modprobe kvm_intel   # or kvm_amd

# Build m5 guest utility
cd /workspaces/gem5/util/m5
scons build/x86/out/m5
```

---

## Build RocksDB Binaries

```bash
cd /workspaces/rocksdb
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

> Then run `mount_disk_image.sh` to static-link and install all three binaries into the disk
> image. See [INSTALL_GUIDE.md](INSTALL_GUIDE.md) section 5 for the full static-link step.

---

## Run Simulations

```bash
# Clear previous outputs
rm -rf /workspaces/gem5/m5out/*

# Run baseline
cd /workspaces/gem5
./build/ALL/gem5.opt configs/yonsei/run_baseline.py

# Run PNM
cd /workspaces/gem5
./build/ALL/gem5.opt configs/yonsei/run_pnm.py
```

Typical runtimes on an 8-core host: baseline ~1.7 h, PNM ~2.4 h.

---

## Monitor a Running Simulation

```bash
# Stream RocksDB application output (throughput, compaction events)
tail -f /workspaces/gem5/m5out/board.pc.com_1.device

# Stream the same for a named output dir
tail -f /workspaces/gem5/m5out/baseline_clean/board.pc.com_1.device
tail -f /workspaces/gem5/m5out/pnm_clean/board.pc.com_1.device

# Watch gem5 progress (simulated seconds elapsed)
watch -n 5 'grep "simSeconds" /workspaces/gem5/m5out/stats.txt 2>/dev/null | head -2'

# Check how long the host has been running the sim
ps aux | grep gem5.opt

# Kill a running simulation
pkill -f gem5.opt
```

---

## Compare Results After Simulation

```bash
# --- Application throughput ---
grep "fillrandom\|readrandom" m5out/baseline_clean/board.pc.com_1.device \
    | grep -v "thread\|benchmarks\|DB path"
grep "fillrandom\|readrandom" m5out/pnm_clean/board.pc.com_1.device \
    | grep -v "thread\|benchmarks\|DB path"

# --- CPU IPC (TIMING phase) ---
grep "switch.*\.ipc " m5out/baseline_clean/stats.txt | head -2
grep "switch[01].*\.ipc " m5out/pnm_clean/stats.txt | head -4

# --- L2 cache misses (CPU0) ---
grep "l2-cache-0\.demandMisses::total" m5out/baseline_clean/stats.txt
grep "l2-cache-0\.demandMisses::total" m5out/pnm_clean/stats.txt

# --- DRAM read requests ---
grep "mem_ctrl.*\.readReqs\b" m5out/baseline_clean/stats.txt
grep "mem_ctrl.*\.readReqs\b" m5out/pnm_clean/stats.txt

# --- Simulated time ---
grep "^simSeconds" m5out/baseline_clean/stats.txt | head -1
grep "^simSeconds" m5out/pnm_clean/stats.txt | head -1

# --- PNM device stats ---
grep "pnm_compactor\." m5out/pnm_clean/stats.txt

# --- RocksDB internal compaction stats ---
grep "compaction\.times\.micros\|compaction\.key\.drop\.new\|numfiles\.in\.single" \
    m5out/baseline_clean/board.pc.com_1.device
grep "compaction\.times\.micros\|compaction\.key\.drop\.new\|numfiles\.in\.single" \
    m5out/pnm_clean/board.pc.com_1.device
```

---

## Disk Image

```bash
# Download the base Ubuntu 24.04 image (first time only)
cd /workspaces/gem5
python3 -c "from gem5.resources.resource import obtain_resource; \
    obtain_resource('x86-ubuntu-24.04-img-4.0.0')"

# Build the custom disk image with db_bench + PNM binaries installed
cd /workspaces/gem5/configs/yonsei
chmod +x mount_disk_image.sh
./mount_disk_image.sh

# Check image exists and size
ls -lh /workspaces/gem5/disk_images/x86-ubuntu-24.04-with-db_bench.img
```

---

## Git

```bash
# Check which branch you are on
git branch

# Switch to the stable working branch
git checkout stable

# See what changed vs the last commit
git diff

# Stash uncommitted changes before switching branches
git stash
git stash pop
```
