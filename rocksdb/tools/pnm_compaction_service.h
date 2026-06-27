// PNMCompactionService — routes RocksDB compactions to pnm_compaction_unit.
//
// Flow per compaction job:
//   Schedule(): open UNIX socket to pnm_compaction_unit, send job, write MMIO
//               CMD_SUBMIT (doorbell), return socket fd in scheduled_job_id.
//   Wait():     poll MMIO STATUS (PNM latency model), then block on socket for
//               the actual compaction result from pnm_compaction_unit.
//
// MSHR safety: MMIO polling happens on the main CPU cores (0-1) while
// pnm_compaction_unit does DRAM-heavy SST I/O on core 2.  Separate cores →
// separate L1 MSHRs → no cache-hierarchy conflict.
#pragma once

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "db/compaction/compaction_job.h"
#include "rocksdb/options.h"
#include "tools/pnm_ipc.h"

class PNMCompactionService : public ROCKSDB_NAMESPACE::CompactionService {
 public:
  // MMIO register offsets — must match pnm_compactor.hh
  static constexpr size_t   kPNMSize      = 0x100;
  static constexpr uint32_t kOffSrcBytes  = 0x00;
  static constexpr uint32_t kOffDstBytes  = 0x08;
  static constexpr uint32_t kOffCmd       = 0x10;
  static constexpr uint32_t kOffStatus    = 0x14;
  static constexpr uint32_t kCmdSubmit    = 1u;
  static constexpr uint32_t kCmdReset     = 2u;
  static constexpr uint32_t kStatusDone   = (1u << 1);
  static constexpr uint32_t kStatusError  = (1u << 2);

  PNMCompactionService() : mmio_fd_(-1), mmio_base_(nullptr) {
    mmio_fd_ = open("/dev/pnm", O_RDWR | O_SYNC);
    if (mmio_fd_ < 0) {
      fprintf(stderr, "[pnm] /dev/pnm not available (%s) — MMIO disabled\n",
              strerror(errno));
      return;
    }
    void* p = mmap(nullptr, kPNMSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                   mmio_fd_, 0);
    if (p == MAP_FAILED) {
      fprintf(stderr, "[pnm] mmap /dev/pnm failed (%s)\n", strerror(errno));
      close(mmio_fd_);
      mmio_fd_ = -1;
      return;
    }
    mmio_base_ = static_cast<volatile uint8_t*>(p);
    fprintf(stderr, "[pnm] PNM compaction service ready\n");
  }

  ~PNMCompactionService() override {
    if (mmio_base_) munmap(const_cast<uint8_t*>(mmio_base_), kPNMSize);
    if (mmio_fd_ >= 0) close(mmio_fd_);
  }

  const char* Name() const override { return "PNMCompactionService"; }

  ROCKSDB_NAMESPACE::CompactionServiceScheduleResponse Schedule(
      const ROCKSDB_NAMESPACE::CompactionServiceJobInfo& info,
      const std::string& compaction_service_input) override {
    // info.job_id is (job_id_ << 32 | sub_job_id); strip the sub_job_id so
    // our log messages use the same sequential number RocksDB prints.
    uint32_t rdb_job = static_cast<uint32_t>(info.job_id >> 32);
    std::string job_id_str = std::to_string(rdb_job);
    std::string msg;
    append_frame(msg, info.db_name);
    append_frame(msg, job_id_str);
    append_frame(msg, compaction_service_input);

    int sock = connect_to_unit();
    if (sock < 0) {
      fprintf(stderr,
              "[pnm] cannot connect to pnm_compaction_unit — using local\n");
      return ROCKSDB_NAMESPACE::CompactionServiceScheduleResponse(
          ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kUseLocal);
    }

    if (!pnm_send_frame(sock, msg)) {
      fprintf(stderr, "[pnm] send job failed (job=%u)\n", rdb_job);
      close(sock);
      return ROCKSDB_NAMESPACE::CompactionServiceScheduleResponse(
          ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kUseLocal);
    }

    // MMIO doorbell: CMD_SUBMIT with actual input file sizes.
    // dst_bytes estimated as equal to src_bytes (typical 1:1 level compaction).
    if (mmio_base_) {
      uint64_t src_bytes = 0;
      size_t files_in = 0;
      ROCKSDB_NAMESPACE::CompactionServiceInput csi;
      if (ROCKSDB_NAMESPACE::CompactionServiceInput::Read(
              compaction_service_input, &csi).ok()) {
        files_in = csi.input_files.size();
        for (const auto& fname : csi.input_files) {
          struct stat st{};
          std::string full = (!fname.empty() && fname[0] == '/')
                                 ? fname
                                 : info.db_name + "/" + fname;
          if (::stat(full.c_str(), &st) == 0)
            src_bytes += static_cast<uint64_t>(st.st_size);
        }
      }

      std::lock_guard<std::mutex> lock(mmio_mutex_);
      w32(kOffCmd,      kCmdReset);
      w64(kOffSrcBytes, src_bytes);
      w64(kOffDstBytes, src_bytes);
      w32(kOffCmd,      kCmdSubmit);

      fprintf(stderr, "[pnm] submit job=%u src=%lu B (%zu files) db=%s\n",
              rdb_job, src_bytes, files_in, info.db_name.c_str());
    } else {
      fprintf(stderr, "[pnm] submit job=%u db=%s\n",
              rdb_job, info.db_name.c_str());
    }

    std::string scheduled = job_id_str + ":" + std::to_string(sock);
    return ROCKSDB_NAMESPACE::CompactionServiceScheduleResponse(
        scheduled, ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kSuccess);
  }

  ROCKSDB_NAMESPACE::CompactionServiceJobStatus Wait(
      const std::string& scheduled_job_id, std::string* result) override {
    size_t colon = scheduled_job_id.find(':');
    if (colon == std::string::npos) {
      return ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kFailure;
    }
    int sock = std::stoi(scheduled_job_id.substr(colon + 1));

    // Block for actual compaction result from pnm_compaction_unit.
    // pnm_compaction_unit polls MMIO STATUS_DONE on core 2 before sending,
    // so this recv() unblocks only after both real compaction and the PNM
    // timing model have completed — cores 0-1 sleep here instead of spinning.
    // Response wire format: [status_byte (1 B)][output_data (rest)]
    std::string resp;
    if (!pnm_recv_frame(sock, &resp)) {
      // recv failed (EINTR after signal, pnm_unit crash, or socket error).
      // Return kUseLocal so this job falls back to CPU compaction without
      // marking the CompactionService as permanently failed.
      fprintf(stderr, "[pnm] recv result failed (fd=%d) — falling back to local\n",
              sock);
      close(sock);
      return ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kUseLocal;
    }
    close(sock);

    if (resp.empty()) {
      return ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kFailure;
    }
    uint8_t status_byte = static_cast<uint8_t>(resp[0]);
    *result = resp.substr(1);

    return (status_byte == 0)
               ? ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kSuccess
               : ROCKSDB_NAMESPACE::CompactionServiceJobStatus::kFailure;
  }

 private:
  int mmio_fd_;
  volatile uint8_t* mmio_base_;
  std::mutex mmio_mutex_;

  // Build a UNIX socket connection to pnm_compaction_unit with retries.
  static int connect_to_unit() {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kPNMSockPath, sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 50; ++i) {
      if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) == 0) {
        return sock;
      }
      usleep(100000);  // 100 ms
    }
    close(sock);
    return -1;
  }

  // Append a length-prefixed string field into a compound message.
  static void append_frame(std::string& buf, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    buf.append(reinterpret_cast<const char*>(&len), 4);
    buf.append(s);
  }

  void w64(uint32_t off, uint64_t v) const {
    __atomic_store_n(
        reinterpret_cast<volatile uint64_t*>(mmio_base_ + off),
        v, __ATOMIC_SEQ_CST);
  }
  void w32(uint32_t off, uint32_t v) const {
    __atomic_store_n(
        reinterpret_cast<volatile uint32_t*>(mmio_base_ + off),
        v, __ATOMIC_SEQ_CST);
  }
  uint32_t r32(uint32_t off) const {
    return __atomic_load_n(
        reinterpret_cast<const volatile uint32_t*>(mmio_base_ + off),
        __ATOMIC_SEQ_CST);
  }
};
