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

#include "dev/pnm/pnm_compactor.hh"

#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/PNMCompactor.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{

PNMCompactor::PNMCompactor(const Params &p)
    : DmaDevice(p),
      pioAddr(p.pio_addr),
      pioSize(p.pio_size),
      processLatency(p.process_latency),
      bandwidth(p.bandwidth),
      useDma(p.use_dma),
      dmaAddr(p.dma_addr),
      srcBytesReg(0),
      dstBytesReg(0),
      statusReg(0),
      busy(false),
      completeEvent([this]{ onComplete(); }, name()),
      stats(*this)
{
    DPRINTF(PNMCompactor, "PNMCompactor at 0x%x mode=%s\n",
            pioAddr, useDma ? "DMA+functional-write" : "latency");
}

AddrRangeList
PNMCompactor::getAddrRanges() const
{
    return {AddrRange(pioAddr, pioAddr + pioSize)};
}

// ---------------------------------------------------------------------------
// Width-safe packet accessors
// ---------------------------------------------------------------------------
static uint64_t
pktGet(PacketPtr pkt)
{
    switch (pkt->getSize()) {
      case 8: return pkt->getLE<uint64_t>();
      case 4: return pkt->getLE<uint32_t>();
      case 2: return pkt->getLE<uint16_t>();
      default: return pkt->getLE<uint8_t>();
    }
}

static void
pktSet(PacketPtr pkt, uint64_t val)
{
    switch (pkt->getSize()) {
      case 8: pkt->setLE<uint64_t>(val); break;
      case 4: pkt->setLE<uint32_t>(static_cast<uint32_t>(val)); break;
      case 2: pkt->setLE<uint16_t>(static_cast<uint16_t>(val)); break;
      default: pkt->setLE<uint8_t>(static_cast<uint8_t>(val)); break;
    }
}

// ---------------------------------------------------------------------------
// MMIO read
// ---------------------------------------------------------------------------
Tick
PNMCompactor::read(PacketPtr pkt)
{
    const Addr off = pkt->getAddr() - pioAddr;
    switch (off) {
      case OFF_STATUS:
        pktSet(pkt, statusReg);
        break;
      case OFF_BYTES_PROC:
        pktSet(pkt, stats.bytesProcessed.value());
        break;
      case OFF_JOBS_DONE:
        pktSet(pkt, stats.jobsCompleted.value());
        break;
      default:
        pktSet(pkt, 0);  // write-only register or out-of-range; return 0
        break;
    }
    pkt->makeAtomicResponse();
    return 0;
}

// ---------------------------------------------------------------------------
// MMIO write
// ---------------------------------------------------------------------------
Tick
PNMCompactor::write(PacketPtr pkt)
{
    const Addr off = pkt->getAddr() - pioAddr;

    switch (off) {
      case OFF_SRC_BYTES:
        srcBytesReg = pktGet(pkt);
        DPRINTF(PNMCompactor, "SRC_BYTES = %llu\n", srcBytesReg);
        break;

      case OFF_DST_BYTES:
        dstBytesReg = pktGet(pkt);
        DPRINTF(PNMCompactor, "DST_BYTES = %llu\n", dstBytesReg);
        break;

      case OFF_CMD: {
        const uint32_t cmd = static_cast<uint32_t>(pktGet(pkt));
        if (cmd == CMD_SUBMIT) {
            if (busy) {
                warn("PNMCompactor: CMD_SUBMIT while busy — ignored\n");
                break;
            }
            if (srcBytesReg > MAX_JOB_BYTES) {
                warn("PNMCompactor: CMD_SUBMIT invalid src_bytes %llu"
                     " — ignored\n", srcBytesReg);
                statusReg = STATUS_ERROR;
                break;
            }

            busy      = true;
            statusReg = STATUS_BUSY;

            // Schedule completion after bandwidth + fixed-overhead latency.
            const uint64_t totalBytes = srcBytesReg + dstBytesReg;
            const Tick bwLatency =
                (bandwidth > 0.0 && totalBytes > 0)
                ? static_cast<Tick>(
                      static_cast<double>(totalBytes) / bandwidth)
                : 0;
            const Tick totalLatency = bwLatency + processLatency;
            DPRINTF(PNMCompactor,
                    "CMD_SUBMIT: src=%llu dst=%llu total=%llu B "
                    "lat=%llu ticks\n",
                    srcBytesReg, dstBytesReg, totalBytes, totalLatency);
            schedule(completeEvent, curTick() + totalLatency);

        } else if (cmd == CMD_RESET) {
            DPRINTF(PNMCompactor, "CMD_RESET\n");
            if (completeEvent.scheduled())
                deschedule(completeEvent);
            busy      = false;
            statusReg = 0;
        }
        break;
      }

      default:
        break;
    }
    pkt->makeAtomicResponse();
    return 0;
}

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------
void
PNMCompactor::onComplete()
{
    const uint64_t total = srcBytesReg + dstBytesReg;
    DPRINTF(PNMCompactor, "Job done: %llu B total\n", total);

    stats.bytesProcessed += total;
    stats.jobsCompleted++;

    if (useDma) {
        // Write a 16-byte result record to guest DRAM via physProxy.
        // physProxy is used (not sendFunctional) because sendFunctional
        // snoops through the IO bridge and corrupts its response-queue state.
        // Record layout: [bytesProcessed:8B][jobsCompleted:4B][pad:4B]
        uint8_t resultBuf[16] = {};
        const uint64_t bProc =
            static_cast<uint64_t>(stats.bytesProcessed.value());
        const uint32_t jDone =
            static_cast<uint32_t>(stats.jobsCompleted.value());
        std::memcpy(resultBuf,      &bProc, sizeof(bProc));
        std::memcpy(resultBuf + 8,  &jDone, sizeof(jDone));
        sys->physProxy.writeBlob(dmaAddr, resultBuf, sizeof(resultBuf));
        DPRINTF(PNMCompactor,
                "physProxy write: bytes=%llu jobs=%u -> 0x%x\n",
                bProc, jDone, dmaAddr);
    }

    statusReg = STATUS_DONE;
    busy      = false;

    if (drainState() == DrainState::Draining)
        signalDrainDone();
}

// ---------------------------------------------------------------------------
// Drain
// ---------------------------------------------------------------------------
// Stall drain until any in-flight compaction job finishes.
DrainState
PNMCompactor::drain()
{
    if (completeEvent.scheduled())
        return DrainState::Draining;
    return DrainState::Drained;
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------
PNMCompactor::PNMStats::PNMStats(PNMCompactor &pnm)
    : statistics::Group(&pnm),
      ADD_STAT(bytesProcessed, statistics::units::Byte::get(),
               "Total bytes processed by the PNM compactor"),
      ADD_STAT(jobsCompleted, statistics::units::Count::get(),
               "Total compaction jobs completed by the PNM compactor")
{
}

} // namespace gem5
