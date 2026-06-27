# /workspaces/gem5/configs/yonsei/run_pnm.py
#
# Prerequisites:
#   Run configs/yonsei/mount_disk_image.sh once to create the custom disk
#   image with db_bench, rocksdb_pnm, pnm_compaction_unit, pnm_module.ko,
#   m5 (/sbin/m5), and a NOPASSWD sudoers rule pre-installed.
#
# Usage:
#   build/ALL/gem5.opt configs/yonsei/run_pnm.py
#
# Runs rocksdb_pnm with compaction offloaded to the PNM unit (core 1 + MMIO device).
#
# The PNM compaction offload unit (MMIO 0xD0000000) models the latency of
# near-memory compaction while rocksdb_pnm runs on core 0 concurrently,
# routing compaction jobs to pnm_compaction_unit on core 1 via UNIX socket.
import os
import sys
from pathlib import Path

import m5
from m5.objects import PNMCompactor

from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    obtain_resource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator

DISK_IMAGE = "/workspaces/gem5/disk_images/x86-ubuntu-24.04-with-db_bench.img"
ROCKSDB_PNM = "/usr/local/bin/rocksdb_pnm"
OUTDIR = Path("m5out/pnm_clean")


if not os.path.exists(DISK_IMAGE):
    print(f"CRITICAL ERROR: Custom disk image not found at {DISK_IMAGE}")
    print("Run configs/yonsei/mount_disk_image.sh first.")
    sys.exit(1)

memory = DualChannelDDR4_2400(size="3GiB")

# Boot with KVM (near-native speed), switch to TIMING at the ROI boundary.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    num_cores=2,  # core 0 = rocksdb_pnm, core 1 = pnm_compaction_unit
    isa=ISA.X86,
)

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="32KiB",
    l1i_size="32KiB",
    l2_size="512KiB",
)

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# Shell script embedded into the disk image and executed by the guest on boot.
readfile_script = f"""\
#!/bin/bash
# Load PNM kernel module before the ROI (runs in KVM mode, fast).
# gem5 user has NOPASSWD sudo via /etc/sudoers.d/gem5-nopasswd.
if sudo insmod /root/pnm_module.ko; then
    sudo chmod 666 /dev/pnm
    echo "[gem5] pnm_module.ko loaded — /dev/pnm ready"
else
    echo "[gem5] WARNING: pnm_module.ko failed to load — MMIO will be bypassed"
fi

/sbin/m5 resetstats
/sbin/m5 workbegin

# rocksdb_pnm forks pnm_compaction_unit internally (it will occupy core 1).
# Compaction jobs are routed to the PNM unit via CompactionService + MMIO.
{ROCKSDB_PNM} \\
    --db=/tmp/rocksdb_fs_pnm \\
    --benchmarks=fillrandom,readrandom,waitforcompaction \\
    --num=25000 \\
    --seed=42 \\
    --value_size=1024 \\
    --compaction_style=0 \\
    --block_size=4096 \\
    --cache_size=8388608 \\
    --write_buffer_size=1048576 \\
    --max_write_buffer_number=2 \\
    --max_bytes_for_level_base=16777216 \\
    --disable_wal=1 \\
    --stats_interval=1000 \\
    --level0_file_num_compaction_trigger=2 \\
    --compression_type=none \\
    --statistics=1

/sbin/m5 workend
/sbin/m5 dumpresetstats
/sbin/m5 exit
"""

board.set_kernel_disk_workload(
    kernel=obtain_resource("x86-linux-kernel-6.8.0-52-generic"),
    disk_image=DiskImageResource(local_path=DISK_IMAGE, root_partition="2"),
    readfile_contents=readfile_script,
    kernel_args=[
        "earlyprintk=ttyS0",
        "console=ttyS0",
        "lpj=7999923",
        "root=/dev/sda2",
    ],
)

# ── PNM compaction offload unit ──────────────────────────────────────────────
# Connect the PNM device to the system:
#   pio: exposes MMIO registers to the CPU via the IO bus bridge
#   dma: required port for DmaDevice base class; data movement uses physProxy, not this port
pnm = PNMCompactor(
    pio_addr=0xD0000000,
    process_latency="500ns",  # AxDIMM-style: DDR4 round-trip + buffer-chip decode
    bandwidth="25GiB/s",  # rank-parallel internal DIMM bandwidth
    use_dma=True,
    dma_addr=0x80000000,
)
board.pnm_compactor = pnm
pnm.pio = board.get_io_bus().mem_side_ports
pnm.dma = cache_hierarchy.membus.cpu_side_ports
# ─────────────────────────────────────────────────────────────────────────────


def workbegin_handler():
    print(">>> ROI start: draining in-flight ops before KVM -> TIMING switch")
    m5.drain()
    print(">>> Drained; switching to TIMING CPU")
    simulator.switch_processor()
    yield False


def workend_handler():
    print(">>> ROI end: benchmark complete")
    yield False


def exit_handler():
    print(">>> Simulation exiting cleanly")
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.WORKBEGIN: workbegin_handler(),
        ExitEvent.WORKEND: workend_handler(),
        ExitEvent.EXIT: exit_handler(),
    },
    outdir=OUTDIR,
)

print("---------------------------------------------------------------------")
print("Launching x86 Full-System Simulation with true PNM compaction offload")
print("  Cores: 0 = rocksdb_pnm, 1 = PNM unit (pnm_compaction_unit)")
print(
    "  PNM MMIO base : 0xD0000000  process_latency: 500ns  bandwidth: 25GiB/s"
)
print(
    "  CompactionService routes compactions to pnm_compaction_unit via socket"
)
print("---------------------------------------------------------------------")
simulator.run()
print("Simulation complete.")
