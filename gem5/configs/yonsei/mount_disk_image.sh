#!/usr/bin/env bash
# Prepares a local copy of the gem5 Ubuntu 24.04 disk image with all PNM
# research binaries pre-installed under /usr/local/bin/ and the PNM kernel
# module at /root/pnm_module.ko. Run this once before simulating.
set -e

SRC_IMAGE="${HOME}/.cache/gem5/x86-ubuntu-24.04-img-4.0.0"
DEST_DIR="/workspaces/gem5/disk_images"
DEST_IMAGE="${DEST_DIR}/x86-ubuntu-24.04-with-db_bench.img"
BINARY="/workspaces/rocksdb/build_gflags/db_bench_static"
ROCKSDB_PNM_STATIC="/workspaces/rocksdb/build_gflags/rocksdb_pnm_static"
PNM_UNIT_STATIC="/workspaces/rocksdb/build_gflags/pnm_compaction_unit_static"
M5_BINARY="/workspaces/gem5/util/m5/build/x86/out/m5"
PNM_MODULE_SRC="/workspaces/gem5/configs/yonsei/pnm_module.c"
PNM_MODULE_MK="/workspaces/gem5/configs/yonsei/pnm_module.Makefile"
MOUNT_POINT="/mnt/gem5img"

# Safety net: unmount everything if the script exits early due to set -e or
# an error in the chroot build. 2>/dev/null suppresses "not mounted" noise on
# the normal exit path where the explicit umounts below already ran.
trap 'sudo umount "${MOUNT_POINT}/proc" 2>/dev/null || true; \
      sudo umount "${MOUNT_POINT}/sys"  2>/dev/null || true; \
      sudo umount "${MOUNT_POINT}"      2>/dev/null || true' EXIT

# Ubuntu 24.04 image uses GPT: root partition at sector 4096 (offset 2097152)
PART_OFFSET=$((4096 * 512))

CXXFLAGS="-W -Wextra -Wall -pthread -Wsign-compare -Wshadow \
    -Wno-unused-parameter -Wno-unused-variable -Woverloaded-virtual \
    -Wnon-virtual-dtor -Wno-missing-field-initializers -Wno-strict-aliasing \
    -Wno-invalid-offsetof -fno-omit-frame-pointer -momit-leaf-frame-pointer \
    -Werror -fno-builtin-memcmp -O3 -DNDEBUG -fno-rtti \
    -static-libgcc -static-libstdc++"

if [ ! -f "${SRC_IMAGE}" ]; then
    echo "ERROR: Source image not found at ${SRC_IMAGE}"
    echo "Run a gem5 simulation once to download it, or adjust SRC_IMAGE path."
    exit 1
fi

# ── Build host-side binaries if not already present ──────────────────────────
if [ ! -f "${BINARY}" ]; then
    echo "Building static db_bench with gflags support ..."
    mkdir -p /workspaces/rocksdb/build_gflags
    (cd /workspaces/rocksdb/build_gflags && \
        cmake .. -DCMAKE_BUILD_TYPE=Release -DPORTABLE=ON -DWITH_GFLAGS=1 -DWITH_TESTS=OFF \
            -DWITH_TOOLS=ON -DWITH_BENCHMARK_TOOLS=ON -DROCKSDB_BUILD_SHARED=OFF \
            -DWITH_SNAPPY=ON -DWITH_LIBURING=OFF \
        && make -j"$(nproc)" db_bench \
        && /usr/bin/c++ ${CXXFLAGS} \
            CMakeFiles/db_bench.dir/tools/simulated_hybrid_file_system.cc.o \
            CMakeFiles/db_bench.dir/tools/db_bench.cc.o \
            CMakeFiles/db_bench.dir/tools/tool_hooks.cc.o \
            CMakeFiles/db_bench.dir/tools/db_bench_tool.cc.o \
            -o db_bench_static \
            librocksdb.a \
            /usr/lib/x86_64-linux-gnu/libgflags.a \
            /usr/lib/x86_64-linux-gnu/libsnappy.a \
            -lz -lpthread -ldl)
fi

# Build the PNM-specific binaries (separate targets; db_bench is NOT touched).
if [ ! -f "${ROCKSDB_PNM_STATIC}" ] || [ ! -f "${PNM_UNIT_STATIC}" ]; then
    echo "Building rocksdb_pnm and pnm_compaction_unit ..."
    (cd /workspaces/rocksdb/build_gflags && \
        cmake .. -DCMAKE_BUILD_TYPE=Release -DPORTABLE=ON -DWITH_GFLAGS=1 -DWITH_TESTS=OFF \
            -DWITH_TOOLS=ON -DWITH_BENCHMARK_TOOLS=ON -DROCKSDB_BUILD_SHARED=OFF \
            -DWITH_SNAPPY=ON -DWITH_LIBURING=OFF \
        && make -j"$(nproc)" rocksdb_pnm pnm_compaction_unit \
        && /usr/bin/c++ ${CXXFLAGS} \
            CMakeFiles/rocksdb_pnm.dir/tools/simulated_hybrid_file_system.cc.o \
            CMakeFiles/rocksdb_pnm.dir/tools/db_bench_pnm_main.cc.o \
            CMakeFiles/rocksdb_pnm.dir/tools/tool_hooks.cc.o \
            CMakeFiles/rocksdb_pnm.dir/tools/db_bench_tool.cc.o \
            -o rocksdb_pnm_static \
            librocksdb.a \
            /usr/lib/x86_64-linux-gnu/libgflags.a \
            /usr/lib/x86_64-linux-gnu/libsnappy.a \
            -lz -lpthread -ldl \
        && /usr/bin/c++ ${CXXFLAGS} \
            CMakeFiles/pnm_compaction_unit.dir/tools/pnm_unit_main.cc.o \
            -o pnm_compaction_unit_static \
            librocksdb.a \
            /usr/lib/x86_64-linux-gnu/libgflags.a \
            /usr/lib/x86_64-linux-gnu/libsnappy.a \
            -lz -lpthread -ldl)
fi

if [ ! -f "${M5_BINARY}" ]; then
    echo "Building m5 guest utility (needed for workbegin/workend support) ..."
    (cd /workspaces/gem5/util/m5 && scons build/x86/out/m5)
fi

# ── Install binaries into disk image ─────────────────────────────────────────
echo "Copying disk image to ${DEST_IMAGE} ..."
mkdir -p "${DEST_DIR}"
cp "${SRC_IMAGE}" "${DEST_IMAGE}"

echo "Mounting image ..."
sudo mkdir -p "${MOUNT_POINT}"
sudo mount -o loop,offset=${PART_OFFSET} "${DEST_IMAGE}" "${MOUNT_POINT}"

echo "Copying db_bench to /usr/local/bin/db_bench inside image ..."
sudo cp "${BINARY}" "${MOUNT_POINT}/usr/local/bin/db_bench"
sudo chmod 755 "${MOUNT_POINT}/usr/local/bin/db_bench"

echo "Installing rocksdb_pnm and pnm_compaction_unit ..."
sudo cp "${ROCKSDB_PNM_STATIC}" "${MOUNT_POINT}/usr/local/bin/rocksdb_pnm"
sudo cp "${PNM_UNIT_STATIC}"    "${MOUNT_POINT}/usr/local/bin/pnm_compaction_unit"
sudo chmod 755 "${MOUNT_POINT}/usr/local/bin/rocksdb_pnm"
sudo chmod 755 "${MOUNT_POINT}/usr/local/bin/pnm_compaction_unit"

echo "Installing m5 binary (with workbegin/workend support) ..."
sudo mkdir -p "${MOUNT_POINT}/sbin"
sudo cp "${M5_BINARY}" "${MOUNT_POINT}/sbin/m5"
sudo chmod +x "${MOUNT_POINT}/sbin/m5"

echo "Installing NOPASSWD sudoers rule for gem5 user ..."
echo 'gem5 ALL=(ALL) NOPASSWD: ALL' | sudo tee "${MOUNT_POINT}/etc/sudoers.d/gem5-nopasswd" > /dev/null
sudo chmod 440 "${MOUNT_POINT}/etc/sudoers.d/gem5-nopasswd"

# ── Build and install PNM kernel module inside guest (chroot) ────────────────
echo "Building PNM kernel module (chroot) ..."
sudo mkdir -p "${MOUNT_POINT}/root/pnm_build"
sudo cp "${PNM_MODULE_SRC}" "${MOUNT_POINT}/root/pnm_build/pnm_module.c"
sudo cp "${PNM_MODULE_MK}"  "${MOUNT_POINT}/root/pnm_build/Makefile"
sudo mount --bind /proc "${MOUNT_POINT}/proc"
sudo mount --bind /sys  "${MOUNT_POINT}/sys"
sudo chroot "${MOUNT_POINT}" /bin/bash -c "cd /root/pnm_build && make"
sudo cp "${MOUNT_POINT}/root/pnm_build/pnm_module.ko" \
        "${MOUNT_POINT}/root/pnm_module.ko"
sudo umount "${MOUNT_POINT}/proc"
sudo umount "${MOUNT_POINT}/sys"

echo "Unmounting ..."
sudo umount "${MOUNT_POINT}"

echo "Done. Image ready at ${DEST_IMAGE}"
