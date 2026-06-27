# /workspaces/gem5/configs/yonsei/run_baseline.py
#
# Prerequisites:
#   Run configs/yonsei/mount_disk_image.sh once to create the custom disk
#   image with db_bench pre-installed at /usr/local/bin/db_bench.
#
# Usage:
#   build/ALL/gem5.opt configs/yonsei/run_baseline.py
#
# Runs db_bench fillrandom inside a full-system x86 VM and captures gem5 stats.
import os
import sys
from pathlib import Path

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
DB_BENCH = "/usr/local/bin/db_bench"
OUTDIR = Path("m5out/baseline_clean")


if not os.path.exists(DISK_IMAGE):
    print(f"CRITICAL ERROR: Custom disk image not found at {DISK_IMAGE}")
    print("Run configs/yonsei/mount_disk_image.sh first.")
    sys.exit(1)

memory = DualChannelDDR4_2400(size="3GiB")

# Boot with KVM (near-native speed), switch to TIMING at the ROI boundary.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    num_cores=1,
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
# m5 ops mark the ROI boundary: resetstats clears counters, workbegin/end bracket the benchmark.
/sbin/m5 resetstats
/sbin/m5 workbegin
{DB_BENCH} \\
    --db=/tmp/rocksdb_fs_baseline \\
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


def workbegin_handler():
    print(">>> ROI start: switching KVM -> TIMING CPU for detailed simulation")
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
print("Launching optimized x86 Full-System Simulation (KVM boot + TIMING ROI)")
print("---------------------------------------------------------------------")
simulator.run()
print("Simulation complete.")
