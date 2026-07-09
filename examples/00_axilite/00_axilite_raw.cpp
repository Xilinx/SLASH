/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file 00_axilite_raw.cpp
 *
 * "Raw" counterpart to 00_axilite.cpp.  Where 00_axilite drives the accelerator
 * through the full software stack (VRT -> libvrtdpp -> vrtd -> libslash), this
 * program talks to the slash system-emulation daemon using *only* libslash
 * (ctldev, qdma, hotplug).  Its purpose is end-to-end integration testing of the
 * daemon with as little software between the host application and the daemon as
 * possible.
 *
 * It reproduces the exact behaviour of 00_axilite:
 *   1. Discover the QDMA control socket (PF1) for the requested board BDF.
 *   2. Transfer the VBIN to the reconfiguration aperture over QDMA (H2C).
 *   3. Hotplug PF2 (REMOVE + RESCAN) to apply the staged VBIN.
 *   4. Discover the BAR/ctl control socket (PF2) for the board BDF.
 *   5. Allocate a host (DMA) buffer, fill it with input data.
 *   6. Transfer the host buffer to card memory (HBM) over QDMA (H2C).
 *   7. Launch the increment and accumulate kernels via BAR0 AXI-Lite registers.
 *   8. Wait for both kernels to signal ap_done.
 *   9. Read the accumulate output register and check correctness.
 *  10. Tear down every allocated resource.
 *
 * Usage:
 *   00_axilite_raw <control-base-dir> <board-BDF> <vbin-file>
 *   e.g.  ./00_axilite_raw /run/slash_sysemu 0000:61:00 ./axilite_sim.vbin
 *
 * The daemon must already be running and configured to emulate <board-BDF>.
 *
 * ----------------------------------------------------------------------------
 * The register/address constants below are HARD-CODED for the 00_axilite design
 * and are documented against its system_map.xml (see comments).  If the kernels
 * are recompiled with a different layout these must be updated.  This keeps the
 * program "raw" — no VBIN untar / system_map.xml parsing between it and the
 * daemon.
 * ----------------------------------------------------------------------------
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include <glob.h>
#include <unistd.h>

#include <slash/ctldev.h>
#include <slash/hotplug.h>
#include <slash/qdma.h>
}

namespace {

/* ── Design-specific constants (from examples/00_axilite/.../system_map.xml) ── */

/*
 * Reconfiguration aperture: H2C writes to this fixed device address are appended
 * by the daemon to the staging VBIN instead of being forwarded to card memory.
 * (slash_sysemu qdma_subsystem.h kReconfigApertureAddr / architecture.md
 * "Writing the staging VBIN".)
 */
constexpr uint64_t kReconfigApertureAddr = 0x102100000ULL;

/*
 * HBM allocation base used by VRT (vrt/include/vrt/buffer.hpp nextHbm).  The
 * increment kernel's m_axi (gmem0 -> HBM1) reads its input from here, so the
 * host->card transfer targets this address and it is programmed into the
 * kernel's buffer-pointer argument.
 */
constexpr uint64_t kHbmBaseAddr = 0x4000000000ULL;

/*
 * BAR0 is the 128 MiB "user region".  VRT resolves a kernel's absolute base
 * address to a BAR0 offset by subtracting the naturally-aligned BAR window base
 * (0x20200000000).  For 00_axilite:
 *   accumulate_0 @ 0x20200000000 -> BAR0 offset 0x00000
 *   increment_0  @ 0x20200010000 -> BAR0 offset 0x10000
 */
constexpr std::size_t kBarUserRegion = 0;          /* BAR index of the user region. */
constexpr std::size_t kAccumulateBase = 0x00000;   /* accumulate_0 base in BAR0.   */
constexpr std::size_t kIncrementBase = 0x10000;    /* increment_0 base in BAR0.    */

/*
 * Vitis HLS ap_ctrl_hs register map (identical for both kernels):
 *   0x00 CTRL      bit0 ap_start (W), bit1 ap_done (R)
 *   0x10 size      scalar arg 0 (W)
 * increment_0:
 *   0x18 in_r[31:0]  buffer-pointer arg 1 low  (W)
 *   0x1c in_r[63:32] buffer-pointer arg 1 high (W)
 * accumulate_0:
 *   0x18 out_r       scalar result (R)
 *   0x1c out_r_ctrl  bit0 = result valid (R)
 */
constexpr std::size_t kRegCtrl = 0x00;
constexpr std::size_t kRegSize = 0x10;
constexpr std::size_t kRegIncrInLo = 0x18;
constexpr std::size_t kRegIncrInHi = 0x1c;
constexpr std::size_t kRegAccOut = 0x18;
constexpr std::size_t kRegAccOutCtrl = 0x1c;

constexpr uint32_t kApStart = 0x1u;
constexpr uint32_t kApDone = 0x2u;

constexpr uint32_t kSize = 1024;               /* number of float elements. */
constexpr auto kWaitTimeout = std::chrono::seconds(30);

/* ── RAII wrappers for libslash handles ─────────────────────────────────────── */

struct CtldevDeleter {
    void operator()(struct slash_ctldev *p) const { if (p) slash_ctldev_close(p); }
};
struct QdmaDeleter {
    void operator()(struct slash_qdma *p) const { if (p) slash_qdma_close(p); }
};
struct HotplugDeleter {
    void operator()(struct slash_hotplug *p) const { if (p) slash_hotplug_close(p); }
};
struct BarFileDeleter {
    void operator()(struct slash_bar_file *p) const { if (p) slash_bar_file_close(p); }
};

using CtldevPtr = std::unique_ptr<struct slash_ctldev, CtldevDeleter>;
using QdmaPtr = std::unique_ptr<struct slash_qdma, QdmaDeleter>;
using HotplugPtr = std::unique_ptr<struct slash_hotplug, HotplugDeleter>;
using BarFilePtr = std::unique_ptr<struct slash_bar_file, BarFileDeleter>;

/* ── Helpers ────────────────────────────────────────────────────────────────── */

/* Strip a trailing ".<function>" from a BDF ("DDDD:BB:DD.F" -> "DDDD:BB:DD"). */
std::string board_of(const char *bdf, std::size_t cap)
{
    std::string s(bdf, ::strnlen(bdf, cap));
    const auto dot = s.rfind('.');
    if (dot != std::string::npos) s.erase(dot);
    return s;
}

/* Glob a base directory for entries matching a pattern; returns full paths. */
std::vector<std::string> glob_paths(const std::string &pattern)
{
    std::vector<std::string> out;
    glob_t g{};
    const int rc = ::glob(pattern.c_str(), 0, nullptr, &g);
    if (rc == 0) {
        for (std::size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
    return out;
}

/*
 * The daemon assigns socket indices by config position, not by BDF, and those
 * indices are not guaranteed stable across remove/rescan.  So we enumerate every
 * slash_ctl* socket, ask each for its device info, and keep the one whose board
 * BDF matches.  Returns "" if none match.
 */
std::string discover_ctl_path(const std::string &base_dir, const std::string &board)
{
    for (const auto &path : glob_paths(base_dir + "/slash_ctl*")) {
        CtldevPtr dev(slash_ctldev_open(path.c_str()));
        if (!dev) continue;
        std::unique_ptr<struct slash_ioctl_device_info, decltype(&slash_device_info_free)> info(
            slash_device_info_read(dev.get()), slash_device_info_free);
        if (info && board_of(info->bdf, sizeof(info->bdf)) == board) return path;
    }
    return "";
}

/* Same as discover_ctl_path but for the QDMA (PF1) sockets. */
std::string discover_qdma_path(const std::string &base_dir, const std::string &board)
{
    for (const auto &path : glob_paths(base_dir + "/slash_qdma_ctl*")) {
        QdmaPtr q(slash_qdma_open(path.c_str()));
        if (!q) continue;
        struct slash_qdma_info info{};
        if (slash_qdma_info_read(q.get(), &info) == 0 &&
            board_of(info.bdf, sizeof(info.bdf)) == board) {
            return path;
        }
    }
    return "";
}

std::size_t round_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

/*
 * Perform one H2C transfer of @data (@len bytes) to device address @dev_addr,
 * using a fresh queue pair and DMA buffer on @qdma_path.  The DMA buffer is
 * page-rounded (buf_create requires a page multiple) but only @len bytes are
 * transferred, so no trailing padding reaches the device / staging VBIN.
 * Returns true on success.
 */
bool qdma_h2c(const std::string &qdma_path, uint64_t dev_addr,
              const void *data, std::size_t len)
{
    QdmaPtr q(slash_qdma_open(qdma_path.c_str()));
    if (!q) {
        std::cerr << "  qdma_open(" << qdma_path << "): " << std::strerror(errno) << "\n";
        return false;
    }

    struct slash_qdma_qpair_add add{};
    add.size = sizeof(add);
    add.mode = 0;               /* MM */
    add.dir_mask = 0x1;         /* H2C */
    add.h2c_ring_sz = 4;
    add.c2h_ring_sz = 4;
    add.cmpt_ring_sz = 4;
    if (slash_qdma_qpair_add(q.get(), &add) != 0) {
        std::cerr << "  qpair_add: " << std::strerror(errno) << "\n";
        return false;
    }
    const uint32_t qid = add.qid;

    bool ok = false;
    int xfer_fd = -1;
    struct slash_qdma_buffer buf{};

    if (slash_qdma_qpair_start(q.get(), qid) != 0) {
        std::cerr << "  qpair_start: " << std::strerror(errno) << "\n";
    } else if ((xfer_fd = slash_qdma_qpair_get_fd(q.get(), qid, 0)) < 0) {
        std::cerr << "  qpair_get_fd: " << std::strerror(errno) << "\n";
    } else {
        const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        const std::size_t buf_len = round_up(len, page);
        if (slash_qdma_qpair_buffer_create(xfer_fd, buf_len, &buf) != 0) {
            std::cerr << "  buffer_create(" << buf_len << "): " << std::strerror(errno) << "\n";
        } else {
            std::memcpy(buf.addr, data, len);
            const ssize_t sent = slash_qdma_qpair_transfer(
                xfer_fd, buf.fd, 0 /*buf_offset*/, dev_addr, len, SLASH_QDMA_XFER_H2C);
            if (sent != static_cast<ssize_t>(len)) {
                std::cerr << "  transfer returned " << sent << " (expected " << len
                          << "): " << std::strerror(errno) << "\n";
            } else {
                ok = true;
            }
        }
    }

    if (buf.addr) slash_qdma_buffer_destroy(&buf);
    if (xfer_fd >= 0) ::close(xfer_fd);
    slash_qdma_qpair_stop(q.get(), qid);
    slash_qdma_qpair_del(q.get(), qid);
    return ok;
}

/* Flock-bracketed 32-bit BAR register write (LOCK_EX for the duration). */
void reg_write(struct slash_bar_file *bar, std::size_t off, uint32_t val)
{
    slash_bar_file_start_write(bar);
    *reinterpret_cast<volatile uint32_t *>(static_cast<char *>(bar->map) + off) = val;
    slash_bar_file_end_write(bar);
}

/* Flock-bracketed 32-bit BAR register read (LOCK_SH for the duration). */
uint32_t reg_read(struct slash_bar_file *bar, std::size_t off)
{
    slash_bar_file_start_read(bar);
    const uint32_t val =
        *reinterpret_cast<volatile uint32_t *>(static_cast<char *>(bar->map) + off);
    slash_bar_file_end_read(bar);
    return val;
}

/* Spin on a kernel's CTRL register until ap_done, honouring a wall-clock budget. */
bool wait_done(struct slash_bar_file *bar, std::size_t kernel_base, const char *name)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while ((reg_read(bar, kernel_base + kRegCtrl) & kApDone) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "Timed out waiting for " << name << " ap_done\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char *argv[])
{
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <control-base-dir> <board-BDF> <vbin-file>\n"
                  << "  e.g. " << argv[0] << " /run/slash_sysemu 0000:61:00 ./axilite_sim.vbin\n";
        return 1;
    }
    const std::string base_dir = argv[1];
    const std::string board = argv[2];
    const std::string vbin_path = argv[3];

    /* ── 1. Discover the QDMA control socket (PF1) for the board BDF ────────── */
    std::string qdma_path = discover_qdma_path(base_dir, board);
    if (qdma_path.empty()) {
        std::cerr << "No QDMA socket in " << base_dir << " reports board BDF " << board << "\n";
        return 1;
    }
    std::cout << "QDMA socket: " << qdma_path << "\n";

    /* ── 2. Transfer the VBIN to the reconfiguration aperture over QDMA ─────── */
    std::ifstream vbin(vbin_path, std::ios::binary | std::ios::ate);
    if (!vbin) {
        std::cerr << "Cannot open VBIN " << vbin_path << "\n";
        return 1;
    }
    const std::streamsize vbin_size = vbin.tellg();
    vbin.seekg(0);
    std::vector<uint8_t> vbin_bytes(static_cast<std::size_t>(vbin_size));
    if (!vbin.read(reinterpret_cast<char *>(vbin_bytes.data()), vbin_size)) {
        std::cerr << "Cannot read VBIN " << vbin_path << "\n";
        return 1;
    }
    std::cout << "Transferring VBIN (" << vbin_size << " bytes) to reconfiguration aperture 0x"
              << std::hex << kReconfigApertureAddr << std::dec << "...\n";
    if (!qdma_h2c(qdma_path, kReconfigApertureAddr, vbin_bytes.data(), vbin_bytes.size())) {
        std::cerr << "VBIN transfer failed\n";
        return 1;
    }

    /* ── 3. Hotplug PF2 (REMOVE + RESCAN) to apply the staged VBIN ─────────── */
    HotplugPtr hp(slash_hotplug_open((base_dir + "/slash_hotplug").c_str()));
    if (!hp) {
        std::cerr << "slash_hotplug_open: " << std::strerror(errno) << "\n";
        return 1;
    }
    const std::string pf2 = board + ".2";
    std::cout << "Reconfiguring: REMOVE " << pf2 << " + RESCAN...\n";
    if (slash_hotplug_remove(hp.get(), pf2.c_str()) != 0) {
        std::cerr << "hotplug REMOVE(" << pf2 << "): " << std::strerror(errno) << "\n";
        return 1;
    }
    if (slash_hotplug_rescan(hp.get()) != 0) {
        std::cerr << "hotplug RESCAN: " << std::strerror(errno) << "\n";
        return 1;
    }

    /* ── 4. Discover the BAR/ctl control socket (PF2), post-reconfiguration ── */
    const std::string ctl_path = discover_ctl_path(base_dir, board);
    if (ctl_path.empty()) {
        std::cerr << "No ctl socket in " << base_dir << " reports board BDF " << board
                  << " after reconfiguration\n";
        return 1;
    }
    std::cout << "Control socket: " << ctl_path << "\n";
    CtldevPtr ctl(slash_ctldev_open(ctl_path.c_str()));
    if (!ctl) {
        std::cerr << "slash_ctldev_open(" << ctl_path << "): " << std::strerror(errno) << "\n";
        return 1;
    }
    BarFilePtr bar(slash_bar_file_open(ctl.get(), kBarUserRegion, 0));
    if (!bar || !bar->map) {
        std::cerr << "slash_bar_file_open(BAR" << kBarUserRegion << "): "
                  << std::strerror(errno) << "\n";
        return 1;
    }

    /* PF1 may have been rebuilt during reconfiguration; re-discover it. */
    qdma_path = discover_qdma_path(base_dir, board);
    if (qdma_path.empty()) {
        std::cerr << "QDMA socket vanished after reconfiguration\n";
        return 1;
    }

    /* ── 5. Allocate a host buffer and fill it with input data ─────────────── */
    std::vector<float> host_input(kSize);
    std::mt19937 gen(12345);  /* fixed seed: reproducible runs. */
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    float golden = 0.0f;
    for (uint32_t i = 0; i < kSize; ++i) {
        host_input[i] = dis(gen);
        golden += host_input[i] + 1.0f;  /* increment adds 1, accumulate sums. */
    }

    /* ── 6. Transfer the host buffer to card memory (HBM) over QDMA ─────────── */
    std::cout << "Transferring " << kSize << " floats to HBM 0x" << std::hex << kHbmBaseAddr
              << std::dec << "...\n";
    if (!qdma_h2c(qdma_path, kHbmBaseAddr, host_input.data(), kSize * sizeof(float))) {
        std::cerr << "Input transfer failed\n";
        return 1;
    }

    /* ── 7. Launch the increment and accumulate kernels via BAR0 ───────────── */
    std::cout << "Launching kernels...\n";
    /* increment_0: size arg, buffer-pointer arg (HBM addr), then ap_start. */
    reg_write(bar.get(), kIncrementBase + kRegSize, kSize);
    reg_write(bar.get(), kIncrementBase + kRegIncrInLo,
              static_cast<uint32_t>(kHbmBaseAddr & 0xFFFFFFFFu));
    reg_write(bar.get(), kIncrementBase + kRegIncrInHi,
              static_cast<uint32_t>(kHbmBaseAddr >> 32));
    reg_write(bar.get(), kIncrementBase + kRegCtrl, kApStart);
    /* accumulate_0: size arg, then ap_start (input arrives over the AXIS link). */
    reg_write(bar.get(), kAccumulateBase + kRegSize, kSize);
    reg_write(bar.get(), kAccumulateBase + kRegCtrl, kApStart);

    /* ── 8. Wait for both kernels ──────────────────────────────────────────── */
    const auto start = std::chrono::high_resolution_clock::now();
    if (!wait_done(bar.get(), kIncrementBase, "increment_0")) return 1;
    if (!wait_done(bar.get(), kAccumulateBase, "accumulate_0")) return 1;
    const auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Kernels completed in "
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()
              << " us\n";

    /* ── 9. Read the output register and check correctness ─────────────────── */
    const uint32_t out_ctrl = reg_read(bar.get(), kAccumulateBase + kRegAccOutCtrl);
    const uint32_t raw = reg_read(bar.get(), kAccumulateBase + kRegAccOut);
    float result;
    std::memcpy(&result, &raw, sizeof(result));

    const float abs_err = std::fabs(golden - result);
    constexpr float kAbsTol = 1e-3f;
    constexpr float kRelTol = 1e-6f;
    const float tol = std::max(kAbsTol, kRelTol * std::fabs(golden));

    std::cout << std::setprecision(10);
    std::cout << "Expected: " << golden << "\nGot: " << result << "\n";

    int rc = 0;
    if ((out_ctrl & 0x1u) == 0u) {
        std::cerr << "Test failed! Output valid bit not set (out_r_ctrl=0x" << std::hex
                  << out_ctrl << std::dec << ")\n";
        rc = 1;
    } else if (!std::isfinite(result)) {
        std::cerr << "Test failed! (NaN/Inf)\n";
        rc = 1;
    } else if (abs_err > tol) {
        std::cerr << "Test failed! (accuracy) absolute error " << abs_err
                  << " (tolerance " << tol << ")\n";
        rc = 2;
    } else {
        std::cout << "Absolute error: " << abs_err << " (tolerance " << tol << ")\n";
        std::cout << "Test passed!\n";
    }

    /* ── 10. Teardown — RAII closes bar/ctl/hotplug; buffers freed in qdma_h2c ─ */
    return rc;
}
