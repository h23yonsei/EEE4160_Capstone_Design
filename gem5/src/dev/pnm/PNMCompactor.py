# Copyright (c) 2026 Yonsei Research
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.Device import DmaDevice
from m5.params import *


class PNMCompactor(DmaDevice):
    """
    Processing Near Memory compaction offload engine (hardware accelerator).

    The guest writes byte counts to MMIO registers and issues CMD_SUBMIT.
    The device models compaction latency as timed delays based on the
    configured bandwidth.  If use_dma is True, a 16-byte CompactionResult is
    written to dma_addr via physProxy at job completion.  Real DRAM pressure
    comes from the pnm_compaction_unit process running on core 1.

    Completion fires after all bandwidth delays plus process_latency overhead
    (models fixed compute time in the PNM ASIC).

    MMIO register map: see pnm_compactor.hh.

    Guest submission sequence:
      write CMD=2 (reset)
      write SRC_BYTES
      write DST_BYTES
      write CMD=1 (submit)
      poll STATUS until bit1 (done) or bit2 (error) is set

    pio_addr must fall within [0xC0000000, 0xFFFF0000) so the X86Board
    bridge forwards MMIO accesses from the CPU to the IO bus.
    dma_addr must point to a valid DRAM region not used by the guest OS.
    """

    type = "PNMCompactor"
    cxx_header = "dev/pnm/pnm_compactor.hh"
    cxx_class = "gem5::PNMCompactor"

    pio_addr = Param.Addr(0xD0000000, "MMIO base address")
    pio_size = Param.Addr(0x100, "MMIO region size in bytes")
    process_latency = Param.Latency(
        "500ns", "Fixed compute overhead added after bandwidth delay"
    )
    bandwidth = Param.MemoryBandwidth(
        "25GiB/s",
        "PNM near-memory bandwidth for latency calculation "
        "(latency = (src+dst)/bandwidth + process_latency).",
    )
    dma_addr = Param.Addr(
        0x80000000,
        "Physical DRAM address where a 16-byte CompactionResult record is "
        "written via physProxy at job completion (use_dma=True only).",
    )
    use_dma = Param.Bool(
        False,
        "If True, write a CompactionResult record to dma_addr via "
        "physProxy on each job completion. Observable in stats and guest "
        "memory reads. Timing is identical in both modes: "
        "(src+dst)/bandwidth + process_latency.",
    )
