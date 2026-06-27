// PNM compaction IPC — shared between rocksdb_pnm and pnm_compaction_unit.
// Simple length-prefixed framing over a UNIX domain socket.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <string>

static const char* kPNMSockPath = "/tmp/pnm_compaction.sock";

// Send exactly `len` bytes; returns false on partial write or error.
static bool pnm_send_all(int fd, const void* buf, size_t len) {
  const char* p = static_cast<const char*>(buf);
  while (len > 0) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;  // restart on signal interrupt
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Receive exactly `len` bytes; returns false on partial read or error.
static bool pnm_recv_all(int fd, void* buf, size_t len) {
  char* p = static_cast<char*>(buf);
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n < 0 && errno == EINTR) continue;  // restart on signal interrupt
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

// Send a length-prefixed frame (uint32 LE + payload).
static bool pnm_send_frame(int fd, const std::string& data) {
  uint32_t len = static_cast<uint32_t>(data.size());
  if (!pnm_send_all(fd, &len, 4)) return false;
  if (len > 0 && !pnm_send_all(fd, data.data(), len)) return false;
  return true;
}

// Receive a length-prefixed frame into `out`.
static bool pnm_recv_frame(int fd, std::string* out) {
  uint32_t len = 0;
  if (!pnm_recv_all(fd, &len, 4)) return false;
  out->resize(len);
  if (len > 0 && !pnm_recv_all(fd, &(*out)[0], len)) return false;
  return true;
}
