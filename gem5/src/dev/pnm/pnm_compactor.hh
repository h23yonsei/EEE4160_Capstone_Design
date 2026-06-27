// Copyright (c) 2026 Yonsei Research
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met: redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer;
// redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution;
// neither the name of the copyright holders nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef __DEV_PNM_PNM_COMPACTOR_HH__
#define __DEV_PNM_PNM_COMPACTOR_HH__

#include <cstdint>

#include "base/statistics.hh"
#include "base/types.hh"
#include "dev/dma_device.hh"
#include "params/PNMCompactor.hh"
#include "sim/eventq.hh"

namespace gem5
{

class PNMCompactor : public DmaDevice
{
  public:
    // MMIO register map (base = pio_addr):
    //
    //  +0x00  8B  WO  SRC_BYTES   total bytes to read for this job
    //  +0x08  8B  WO  DST_BYTES   total bytes to write for this job
    //  +0x10  4B  WO  CMD         1=submit, 2=reset
    //  +0x14  4B  RO  STATUS      bit0=busy, bit1=done, bit2=error
    //  +0x18  8B  RO  BYTES_PROC  cumulative bytes processed
    //  +0x20  4B  RO  JOBS_DONE   cumulative jobs completed
    //
    // Two modes controlled by the use_dma parameter:
    //
    //   Latency model (use_dma=False, default):
    //     Timed delay: (src+dst) / bandwidth + process_latency.
    //     No data is moved; DRAM pressure comes from pnm_compaction_unit
    //     running on core 1.
    //
    //   DMA model (use_dma=True):
    //     Same latency as above, plus writes a 16-byte result record to
    //     dma_addr in guest DRAM when each job completes (see onComplete()).

    static constexpr Addr OFF_SRC_BYTES  = 0x00;
    static constexpr Addr OFF_DST_BYTES  = 0x08;
    static constexpr Addr OFF_CMD        = 0x10;
    static constexpr Addr OFF_STATUS     = 0x14;
    static constexpr Addr OFF_BYTES_PROC = 0x18;
    static constexpr Addr OFF_JOBS_DONE  = 0x20;

    static constexpr uint32_t STATUS_BUSY  = (1u << 0);
    static constexpr uint32_t STATUS_DONE  = (1u << 1);
    static constexpr uint32_t STATUS_ERROR = (1u << 2);

    static constexpr uint32_t CMD_SUBMIT = 1u;
    static constexpr uint32_t CMD_RESET  = 2u;

    // 512 MiB: guard against runaway src_bytes/dst_bytes writes; well above
    // the largest compaction seen in this research (~87 MB for 25k keys).
    static constexpr uint64_t MAX_JOB_BYTES = 512ULL * 1024 * 1024;

  private:
    const Addr   pioAddr;
    const Addr   pioSize;
    const Tick   processLatency;
    const double bandwidth;      // bytes per tick (latency model)
    const bool   useDma;         // true = DMA mode, false = latency model

    // DMA mode state (use_dma=True: physProxy write target)
    const Addr  dmaAddr;

    // Job registers
    uint64_t srcBytesReg;
    uint64_t dstBytesReg;
    uint32_t statusReg;
    bool     busy;

    // Fires after latency model delay: (src+dst)/bandwidth + process_latency
    EventFunctionWrapper completeEvent;
    void onComplete();

  public:
    PARAMS(PNMCompactor);
    PNMCompactor(const Params &p);

    AddrRangeList getAddrRanges() const override;
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    DrainState drain() override;

    struct PNMStats : public statistics::Group
    {
        PNMStats(PNMCompactor &pnm);
        statistics::Scalar bytesProcessed;
        statistics::Scalar jobsCompleted;
    } stats;
};

} // namespace gem5

#endif // __DEV_PNM_PNM_COMPACTOR_HH__
