/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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
 *
 * rp1_pipeline — manual RP1 graph submission for the five-kernel
 * polynomial-sum demo.
 *
 *     produce -> {square, cube} -> combine -> reduce -> SCALAR_READ -> SIGNAL
 *
 * Computes Σ (i² + i³) for i in [0, N).  The host uses VRT to load the
 * .vrtbin, discover kernel addresses, and allocate HBM buffers (so the
 * device-side addresses are correct).  It then drops to libslash for
 * raw BAR4 access and hand-builds the RP1 node array in shared DDR.
 * Will be deleted once libslash / VRT expose a real GraphBuilder API.
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <vrt/device.hpp>
#include <vrt/buffer.hpp>
#include <vrt/kernel.hpp>
#include <vrt/utils/logger.hpp>

extern "C" {
#include <slash/ctldev.h>
#include <slash/uapi/rp1_protocol.h>
}

// ---------------------------------------------------------------------------
// Bring-up constants — edit if your bitstream maps BAR/RP1 apertures differently.
// ---------------------------------------------------------------------------

constexpr int      BAR_NUMBER       = 4;
constexpr uint64_t BAR_CTRL_OFFSET  = 64ULL * 1024ULL * 1024ULL;   // RP1 0x3000_0000 -> here

constexpr uint64_t HOST_USER_REGION_BASE = 0x0000020200000000ULL;
constexpr uint64_t R5_USER_REGION_BASE   = 0x0000000088000000ULL;

constexpr uint32_t N                  = 64u;
constexpr uint32_t BRINGUP_CQ_SIZE    = 64u;
constexpr uint32_t SLOT_DONE          = 0u;
constexpr uint32_t SLOT_RESULT        = 1u;
constexpr uint32_t SENTINEL_MAGIC     = 0xC0FFEE01u;

constexpr int      POLL_TIMEOUT_MS    = 5000;
constexpr int      POLL_INTERVAL_MS   = 1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t to_r5(uint64_t host_addr) {
    return static_cast<uint32_t>(host_addr - HOST_USER_REGION_BASE + R5_USER_REGION_BASE);
}

// Build the arg-buffer slot stream for a single kernel.  The RP1 firmware
// writes arg_buf[0..arg_count-1] sequentially to kernel_base + 0x10, +0x14,
// +0x18, etc.  HLS may insert padding (e.g. between a uint32 scalar and a
// 64-bit pointer aligned to 8 bytes) — we honour the register-offset metadata
// from FunctionalArg.
struct KernelArgs {
    std::vector<uint32_t> slots;    // arg_buf contents
    uint16_t              count = 0; // arg_count passed to KERNEL_DISPATCH

    static uint32_t slotIndex(uint32_t reg_offset) {
        if (reg_offset < 0x10u) throw std::runtime_error("Arg register offset must be >= 0x10");
        return (reg_offset - 0x10u) / 4u;
    }

    void place(uint32_t reg_offset, uint32_t value) {
        uint32_t s = slotIndex(reg_offset);
        if (s >= slots.size()) slots.resize(s + 1, 0u);
        slots[s] = value;
        if (s + 1 > count) count = static_cast<uint16_t>(s + 1);
    }

    void place64(uint32_t reg_offset, uint64_t value) {
        place(reg_offset,           static_cast<uint32_t>(value & 0xFFFFFFFFu));
        place(reg_offset + 4u,      static_cast<uint32_t>(value >> 32));
    }
};

// Resolve a FunctionalArg by name; throws if missing.
static const vrt::FunctionalArg& argByName(const vrt::Kernel& k, std::string_view name) {
    for (const auto& a : k.getFunctionalArgs())
        if (a.name == name) return a;
    throw std::runtime_error("Arg '" + std::string(name) + "' not in kernel '" + k.getName() + "'");
}

// ---------------------------------------------------------------------------
// Raw BAR4 access through libslash
// ---------------------------------------------------------------------------

struct BarMap {
    slash_ctldev*       dev      = nullptr;
    slash_ioctl_bar_info* info   = nullptr;
    slash_bar_file*     file     = nullptr;
    volatile uint8_t*   base     = nullptr;

    explicit BarMap(const std::string& slash_ctl_path) {
        dev = slash_ctldev_open(slash_ctl_path.c_str());
        if (!dev) throw std::runtime_error("slash_ctldev_open(" + slash_ctl_path + ") failed");
        info = slash_bar_info_read(dev, BAR_NUMBER);
        if (!info || !info->usable)
            throw std::runtime_error("BAR" + std::to_string(BAR_NUMBER) + " not usable");
        file = slash_bar_file_open(dev, BAR_NUMBER, O_CLOEXEC);
        if (!file) throw std::runtime_error("slash_bar_file_open failed");
        if (slash_bar_file_start_write(file) != 0)
            throw std::runtime_error("slash_bar_file_start_write failed");
        base = static_cast<volatile uint8_t*>(file->map);
    }

    ~BarMap() {
        if (file) { slash_bar_file_end_write(file); slash_bar_file_close(file); }
        if (info) slash_bar_info_free(info);
        if (dev)  slash_ctldev_close(dev);
    }

    BarMap(const BarMap&) = delete;
    BarMap& operator=(const BarMap&) = delete;

    // Translate an R5 absolute address (in the BAR-visible DDR window) to a
    // pointer in our local mapping.
    template <typename T>
    volatile T* at(uint64_t r5_addr) {
        uint64_t bar_off = BAR_CTRL_OFFSET + (r5_addr - RP1_CTRL_PHYS_ADDR);
        return reinterpret_cast<volatile T*>(base + bar_off);
    }
};

// ---------------------------------------------------------------------------
// RP1 node builders
// ---------------------------------------------------------------------------

namespace {

void setHeader(volatile rp1_node_t* n, uint16_t opcode,
               uint8_t aw_b, uint32_t aw_m,
               uint8_t st_b, uint32_t st_m) {
    n->opcode               = opcode;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;
}

void programCtrl(volatile rp1_ctrl_t* c, uint32_t node_count) {
    c->node_count        = node_count;
    c->cq_size           = BRINGUP_CQ_SIZE;
    c->node_base_lo      = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    c->node_base_hi      = 0;
    c->cq_base_lo        = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET);
    c->cq_base_hi        = 0;
    c->arg_buf_base_lo   = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    c->arg_buf_base_hi   = 0;
    c->sig_array_base_lo = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    c->sig_array_base_hi = 0;
}

const char* stateStr(uint32_t s) {
    switch (s) {
        case RP1_STATE_INIT:    return "INIT";
        case RP1_STATE_READY:   return "READY";
        case RP1_STATE_RUNNING: return "RUNNING";
        case RP1_STATE_ERROR:   return "ERROR";
        case RP1_STATE_HALTED:  return "HALTED";
        default:                return "?";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <BDF> <vrtbin> <slash_ctl_path>\n"
                     "  e.g. " << argv[0] << " 0000:21:00.0 pipeline_hw.vrtbin /dev/slash_ctl0\n";
        return 1;
    }
    const std::string bdf            = argv[1];
    const std::string vrtbin         = argv[2];
    const std::string slash_ctl_path = argv[3];

    vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::INFO);
    std::cout << "VRT version: " << vrt::getVersion() << "\n";

    try {
        // -----------------------------------------------------------------
        // 1. Open the device + discover kernels.
        // -----------------------------------------------------------------
        vrt::Device device(bdf, vrtbin);

        vrt::Kernel produce(device, "produce_0");
        vrt::Kernel square (device, "square_0");
        vrt::Kernel cube   (device, "cube_0");
        vrt::Kernel combine(device, "combine_0");
        vrt::Kernel reduce (device, "reduce_0");

        std::cout << "Kernel R5 addresses:\n";
        for (const vrt::Kernel* k : {&produce, &square, &cube, &combine, &reduce}) {
            std::cout << "  " << std::left << std::setw(12) << k->getName()
                      << " host=0x" << std::hex << std::setw(12) << std::setfill('0')
                      << k->getPhysAddr()
                      << "  r5=0x" << std::setw(8) << to_r5(k->getPhysAddr())
                      << std::setfill(' ') << std::dec << "\n";
        }

        // -----------------------------------------------------------------
        // 2. Allocate the four intermediate buffers on the right HBM channels.
        //    VRT picks the channel from the kernel's connectivity metadata.
        // -----------------------------------------------------------------
        vrt::Buffer<uint32_t> bufX(device, N, produce.argMemoryConfig("out"));
        vrt::Buffer<uint32_t> bufY(device, N, square .argMemoryConfig("out"));
        vrt::Buffer<uint32_t> bufZ(device, N, cube   .argMemoryConfig("out"));
        vrt::Buffer<uint32_t> bufC(device, N, combine.argMemoryConfig("out"));

        std::cout << "Buffer device addresses:\n"
                  << std::hex << std::setfill('0')
                  << "  X=0x" << std::setw(12) << bufX.getPhysAddr() << " (HBM, " << N << " words)\n"
                  << "  Y=0x" << std::setw(12) << bufY.getPhysAddr() << "\n"
                  << "  Z=0x" << std::setw(12) << bufZ.getPhysAddr() << "\n"
                  << "  C=0x" << std::setw(12) << bufC.getPhysAddr() << "\n"
                  << std::setfill(' ') << std::dec;

        // -----------------------------------------------------------------
        // 3. Stage arg slots per kernel using the system_map.xml-reported
        //    register offsets (HLS may pad between scalar+pointer args).
        // -----------------------------------------------------------------
        KernelArgs aProduce, aSquare, aCube, aCombine, aReduce;

        aProduce.place  (argByName(produce, "size").offset, N);
        aProduce.place64(argByName(produce, "out") .offset, bufX.getPhysAddr());

        aSquare.place  (argByName(square, "size").offset, N);
        aSquare.place64(argByName(square, "in")  .offset, bufX.getPhysAddr());
        aSquare.place64(argByName(square, "out") .offset, bufY.getPhysAddr());

        aCube.place  (argByName(cube, "size").offset, N);
        aCube.place64(argByName(cube, "in")  .offset, bufX.getPhysAddr());
        aCube.place64(argByName(cube, "out") .offset, bufZ.getPhysAddr());

        aCombine.place  (argByName(combine, "size").offset, N);
        aCombine.place64(argByName(combine, "a")   .offset, bufY.getPhysAddr());
        aCombine.place64(argByName(combine, "b")   .offset, bufZ.getPhysAddr());
        aCombine.place64(argByName(combine, "out") .offset, bufC.getPhysAddr());

        aReduce.place  (argByName(reduce, "size").offset, N);
        aReduce.place64(argByName(reduce, "in")  .offset, bufC.getPhysAddr());

        const uint32_t result_reg_offset = argByName(reduce, "result").offset;
        std::cout << "reduce.result @ kernel offset 0x" << std::hex << result_reg_offset
                  << std::dec << "\n";

        // -----------------------------------------------------------------
        // 4. Open BAR4 and hand-build the RP1 graph in shared DDR.
        //    Graph layout (bucket 0):
        //
        //      n0: KERNEL_DISPATCH produce   await=0/0x00   set=0/0x01
        //      n1: KERNEL_DISPATCH square    await=0/0x01   set=0/0x02
        //      n2: KERNEL_DISPATCH cube      await=0/0x01   set=0/0x04
        //      n3: KERNEL_DISPATCH combine   await=0/0x06   set=0/0x08
        //      n4: KERNEL_DISPATCH reduce    await=0/0x08   set=0/0x10
        //      n5: SCALAR_READ reduce.result await=0/0x10   set=0/0x20
        //      n6: SIGNAL slot[DONE]=MAGIC   await=0/0x20   set=0/0x40
        // -----------------------------------------------------------------
        BarMap bar(slash_ctl_path);

        auto* ctrl = bar.at<rp1_ctrl_t>(RP1_CTRL_PHYS_ADDR);
        auto* nodes = bar.at<rp1_node_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
        auto* argbuf = bar.at<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
        auto* sigs = bar.at<rp1_signal_slot_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);

        // Zero everything we touch.
        for (size_t i = 0; i < 7u * sizeof(rp1_node_t); ++i)
            reinterpret_cast<volatile uint8_t*>(nodes)[i] = 0;
        for (size_t i = 0; i < 4u; ++i) { sigs[i].value = 0; sigs[i].last_writer_node = 0; sigs[i].flags = 0; }

        // Each kernel's args land in a distinct slab inside the shared arg buffer.
        // The KERNEL_DISPATCH node's arg_buffer_offset is in bytes.
        struct ArgSlab { const KernelArgs* args; uint32_t byte_offset; };
        std::vector<ArgSlab> slabs;
        uint32_t cur = 0;
        auto stage = [&](const KernelArgs& a) -> uint32_t {
            uint32_t off = cur;
            for (size_t i = 0; i < a.slots.size(); ++i) argbuf[cur/4 + i] = a.slots[i];
            cur += static_cast<uint32_t>(a.slots.size()) * 4u;
            // Align next slab to 8 bytes for tidiness.
            cur = (cur + 7u) & ~7u;
            return off;
        };
        const uint32_t off_produce = stage(aProduce);
        const uint32_t off_square  = stage(aSquare);
        const uint32_t off_cube    = stage(aCube);
        const uint32_t off_combine = stage(aCombine);
        const uint32_t off_reduce  = stage(aReduce);

        struct KDef {
            uint32_t r5_addr;
            uint16_t arg_count;
            uint32_t arg_off;
            uint8_t  aw_b; uint32_t aw_m;
            uint8_t  st_b; uint32_t st_m;
        };
        const KDef kdef[5] = {
            { to_r5(produce.getPhysAddr()), aProduce.count, off_produce, 0, 0x00, 0, 0x01 },
            { to_r5(square .getPhysAddr()), aSquare.count,  off_square,  0, 0x01, 0, 0x02 },
            { to_r5(cube   .getPhysAddr()), aCube.count,    off_cube,    0, 0x01, 0, 0x04 },
            { to_r5(combine.getPhysAddr()), aCombine.count, off_combine, 0, 0x06, 0, 0x08 },
            { to_r5(reduce .getPhysAddr()), aReduce.count,  off_reduce,  0, 0x08, 0, 0x10 },
        };
        for (uint32_t i = 0; i < 5; ++i) {
            volatile rp1_node_t* n = &nodes[i];
            setHeader(n, RP1_OP_KERNEL_DISPATCH,
                      kdef[i].aw_b, kdef[i].aw_m,
                      kdef[i].st_b, kdef[i].st_m);
            n->payload.kernel_dispatch.kernel_base_addr  = kdef[i].r5_addr;
            n->payload.kernel_dispatch.arg_buffer_offset = kdef[i].arg_off;
            n->payload.kernel_dispatch.arg_count         = kdef[i].arg_count;
            n->payload.kernel_dispatch.ctrl_flags        = 0;
            n->payload.kernel_dispatch.timeout_cycles    = 0;
        }

        // SCALAR_READ reduce.result -> signal[SLOT_RESULT].
        setHeader(&nodes[5], RP1_OP_SCALAR_READ,
                  0, 0x10,
                  0, 0x20);
        nodes[5].payload.scalar_read.source_addr = to_r5(reduce.getPhysAddr()) + result_reg_offset;
        nodes[5].payload.scalar_read.target_slot = SLOT_RESULT;

        // SIGNAL slot[SLOT_DONE] = sentinel (only fires after SCALAR_READ).
        setHeader(&nodes[6], RP1_OP_SIGNAL,
                  0, 0x20,
                  0, 0x40);
        nodes[6].payload.signal.target_slot = SLOT_DONE;
        nodes[6].payload.signal.value       = SENTINEL_MAGIC;
        nodes[6].payload.signal.operation   = RP1_SIGOP_SET;

        programCtrl(ctrl, /*node_count*/ 7);

        const uint32_t prior_cq = ctrl->cq_write_idx;
        const uint32_t want_seq = ctrl->graph_done_seq + 1;
        __sync_synchronize();
        ctrl->graph_seq = want_seq;
        __sync_synchronize();

        std::cout << "Submitted graph seq=" << want_seq
                  << " (7 nodes), polling..." << std::endl;

        auto start = std::chrono::steady_clock::now();
        while (ctrl->graph_done_seq < want_seq) {
            if (std::chrono::steady_clock::now() - start
                > std::chrono::milliseconds(POLL_TIMEOUT_MS)) {
                std::cerr << "TIMEOUT: graph_done_seq=" << ctrl->graph_done_seq
                          << " (want " << want_seq << ")"
                          << " cq_delta=" << (ctrl->cq_write_idx - prior_cq)
                          << " state=" << stateStr(ctrl->rp1_state)
                          << " cur_node=" << ctrl->rp1_current_node
                          << " err=" << ctrl->rp1_error_code << "\n";
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();

        __sync_synchronize();
        const uint32_t observed_done   = sigs[SLOT_DONE  ].value;
        const uint32_t observed_result = sigs[SLOT_RESULT].value;
        const uint32_t cq_delta        = ctrl->cq_write_idx - prior_cq;

        // Golden: Σ (i² + i³)  for i in [0, N)
        uint64_t golden = 0;
        for (uint32_t i = 0; i < N; ++i)
            golden += static_cast<uint64_t>(i) * i + static_cast<uint64_t>(i) * i * i;
        const uint32_t golden_u32 = static_cast<uint32_t>(golden & 0xFFFFFFFFu);

        std::cout << "Done in " << elapsed << " us\n"
                  << "  cq_delta             = " << cq_delta << " (expect 7)\n"
                  << "  slot[" << SLOT_DONE   << "] (sentinel) = 0x"
                      << std::hex << observed_done   << std::dec
                      << "  (expect 0x" << std::hex << SENTINEL_MAGIC << std::dec << ")\n"
                  << "  slot[" << SLOT_RESULT << "] (result)   = 0x"
                      << std::hex << observed_result << std::dec
                      << "  (golden 0x" << std::hex << golden_u32 << std::dec
                      << " = " << golden_u32 << ")\n";

        if (observed_done != SENTINEL_MAGIC) {
            std::cerr << "FAIL: sentinel slot mismatch\n";
            return 1;
        }
        if (observed_result != golden_u32) {
            std::cerr << "FAIL: pipeline result mismatch\n";
            return 1;
        }
        if (cq_delta != 7u) {
            std::cerr << "FAIL: cq_delta=" << cq_delta << ", expected 7\n";
            return 1;
        }

        std::cout << "PASS\n";
        device.cleanup();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
}
