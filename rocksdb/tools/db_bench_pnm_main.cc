// Entry point for the rocksdb_pnm binary.
//
// Differences from plain db_bench:
//   1. Forks pnm_compaction_unit as a subprocess (the PNM near-memory core).
//   2. Pins this process to cores 0-1 (main CPU); pnm_compaction_unit pins
//      itself to core 2.
//   3. Injects --pnm_true_offload=1 so compactions route to the PNM unit by
//      default.  Users may still override with --nopnm_true_offload.

#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run rocksdb tools\n");
  return 1;
}
#else

#include <sched.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rocksdb/db_bench_tool.h"

static pid_t g_pnm_unit_pid = -1;

static void cleanup_pnm_unit() {
  if (g_pnm_unit_pid > 0) {
    kill(g_pnm_unit_pid, SIGTERM);
    waitpid(g_pnm_unit_pid, nullptr, 0);
    g_pnm_unit_pid = -1;
  }
}

int main(int argc, char** argv) {
  // Pin this process to cores 0-1 (main CPU).
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(0, &cpuset);
  CPU_SET(1, &cpuset);
  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
    fprintf(stderr, "[rocksdb_pnm] sched_setaffinity cores 0-1 failed (%s)\n",
            strerror(errno));
  }

  // Locate pnm_compaction_unit next to this binary.
  char exe_path[4096] = {};
  if (readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) > 0) {
    std::string dir(exe_path);
    size_t slash = dir.rfind('/');
    std::string unit_path =
        (slash != std::string::npos ? dir.substr(0, slash + 1) : "./") +
        "pnm_compaction_unit";

    g_pnm_unit_pid = fork();
    if (g_pnm_unit_pid == 0) {
      // Child: become pnm_compaction_unit.
      execl(unit_path.c_str(), "pnm_compaction_unit", nullptr);
      fprintf(stderr, "[rocksdb_pnm] exec pnm_compaction_unit failed: %s\n",
              strerror(errno));
      _exit(1);
    } else if (g_pnm_unit_pid < 0) {
      fprintf(stderr, "[rocksdb_pnm] fork failed: %s — running without PNM unit\n",
              strerror(errno));
    } else {
      atexit(cleanup_pnm_unit);
      // Give pnm_compaction_unit time to bind its socket.
      usleep(500000);  // 500 ms
    }
  } else {
    fprintf(stderr, "[rocksdb_pnm] readlink /proc/self/exe failed — "
                    "pnm_compaction_unit will not be launched\n");
  }

  // Inject --pnm_true_offload=1: compaction is routed to pnm_compaction_unit
  // on core 2 via socket.  PNMCompactionService fires MMIO CMD_SUBMIT with
  // actual SST file sizes at Schedule() time.
  std::vector<char*> new_argv;
  new_argv.push_back(argv[0]);
  static char pnm_flag[] = "--pnm_true_offload=1";
  new_argv.push_back(pnm_flag);
  for (int i = 1; i < argc; ++i) new_argv.push_back(argv[i]);
  new_argv.push_back(nullptr);

  int new_argc = static_cast<int>(new_argv.size()) - 1;
  return ROCKSDB_NAMESPACE::db_bench_tool(new_argc, new_argv.data());
}
#endif  // GFLAGS
