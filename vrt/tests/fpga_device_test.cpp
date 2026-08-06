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
 */

/**
 * @file fpga_device_test.cpp
 *
 * End-to-end unit tests for vrt::graph::FpgaDevice driven by a raw,
 * heap-backed BAR window and the same fake-RP1 worker thread used by
 * rp1_submitter_test.  No daemon, no hardware.
 *
 * The tests build small vrt::graph::Graph instances, register an
 * FpgaDevice, compile, and run().  Assertions cover:
 *
 *  - The sentinel slot gets the expected magic written once the graph
 *    finishes (the trailing-SIGNAL completion contract).
 *  - The CQ contains one entry per kernel + the sentinel signal node.
 *  - Barrier masks and arg packing are correct for the diamond DAG.
 *  - Non-kernel Rp1Command variants (e.g. CompiledBridgeOpNode that
 *    the compiler splices for cross-device buffers) cause compileProgram
 *    to throw a descriptive diagnostic.
 *  - Deferred (global-variable) scalar resolution picks up values set
 *    on the Graph between compile() and launch().
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/cpu_fpga_bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/fpga/rp1_program.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/graph.hpp>
#include "test_support/graph_internal.hpp"
#include <vrt/graph/detail/port_bindings.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

#include "test_support/control_specs.hpp"
#include "test_helpers.hpp"

using namespace vrt::graph;

namespace {

constexpr std::size_t   kBarSize   = 128ULL << 20;
constexpr std::uint64_t kWindowOff = 64ULL << 20;

// Arbitrary distinct R5 kernel addresses for the fake-RP1 diamond DAG.
constexpr std::uint32_t kKernelA_R5 = 0x88010000u;
constexpr std::uint32_t kKernelB_R5 = 0x88020000u;
constexpr std::uint32_t kKernelC_R5 = 0x88030000u;
constexpr std::uint32_t kKernelD_R5 = 0x88040000u;

KernelDescriptor fpgaKernel(std::string name, IOTypeMap ioType = {}) {
    return KernelDescriptor{std::move(name), DeviceType::FPGA, std::nullopt,
                            std::move(ioType)};
}

LoopTripCount bindTripCount(Rp1QueueProgram& dg, std::uint32_t value, std::string name = "trip_count") {
    if (!dg.scalarValues) {
        dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    }
    (*dg.scalarValues)[scopedScalarKey(0, name)] = value;
    return LoopTripCount::scalar(ScalarType::U32, std::move(name));
}

struct DdrView {
    std::byte* base;
    rp1_ctrl_t&        ctrl()      { return *reinterpret_cast<rp1_ctrl_t*>(base + kWindowOff); }
    rp1_node_t*        nodes()     { return reinterpret_cast<rp1_node_t*>(
                                         base + kWindowOff + RP1_DEFAULT_NODE_ARRAY_OFFSET); }
    rp1_cq_entry_t*    cq()        { return reinterpret_cast<rp1_cq_entry_t*>(
                                         base + kWindowOff + RP1_DEFAULT_CQ_OFFSET); }
    std::uint32_t*     args()      { return reinterpret_cast<std::uint32_t*>(
                                         base + kWindowOff + RP1_DEFAULT_ARG_BUF_OFFSET); }
    rp1_signal_slot_t* signals()   { return reinterpret_cast<rp1_signal_slot_t*>(
                                         base + kWindowOff + RP1_DEFAULT_SIG_ARRAY_OFFSET); }
    rp1_trace_entry_t* traces()    { return reinterpret_cast<rp1_trace_entry_t*>(
                                         base + kWindowOff + RP1_DEFAULT_TRACE_OFFSET); }
    std::byte*         rp1Ptr(std::uint64_t rp1Addr) {
        return base + kWindowOff + static_cast<std::size_t>(rp1Addr - RP1_CTRL_PHYS_ADDR);
    }
    // True if [rp1Addr, rp1Addr+bytes) maps within the BAR backing buffer.
    bool inWindow(std::uint64_t rp1Addr, std::uint64_t bytes) const {
        if (rp1Addr < RP1_CTRL_PHYS_ADDR) return false;
        const std::uint64_t off = rp1Addr - RP1_CTRL_PHYS_ADDR;
        return kWindowOff + off + bytes <= kBarSize;
    }
};

const rp1_node_t* findDispatch(DdrView ddr, std::uint32_t r5Address) {
    for (std::uint32_t i = 0; i < ddr.ctrl().node_count; ++i) {
        const rp1_node_t& node = ddr.nodes()[i];
        if (node.opcode == RP1_OP_KERNEL_DISPATCH &&
            node.payload.kernel_dispatch.kernel_base_addr == r5Address) {
            return &node;
        }
    }
    return nullptr;
}

class FakeRp1 {
   public:
    using CompletionFn = std::function<
        std::optional<std::pair<std::uint32_t, std::uint32_t>>(
            std::uint32_t, const rp1_node_t&)>;

    explicit FakeRp1(
        DdrView ddr, bool writeSignals = true,
        CompletionFn completion = {})
        : ddr_(ddr), writeSignals_(writeSignals),
          completion_(std::move(completion)) {
        thread_ = std::thread([this] { run(); });
    }
    ~FakeRp1() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

   private:
    static bool conditionSatisfied(
        std::uint32_t signal, std::uint16_t operation,
        std::uint32_t value) {
        switch (operation) {
            case RP1_COP_EQ: return signal == value;
            case RP1_COP_NE: return signal != value;
            case RP1_COP_LT: return signal < value;
            case RP1_COP_GE: return signal >= value;
            case RP1_COP_AND_NZ:
                return (signal & value) != 0;
            case RP1_COP_AND_Z:
                return (signal & value) == 0;
        }
        return false;
    }

    void run() {
        while (!stop_.load(std::memory_order_relaxed)) {
            auto& c = ddr_.ctrl();
            if (c.graph_seq != c.graph_done_seq) {
                c.rp1_state = RP1_STATE_RUNNING;
                const bool failed = processGraph();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.rp1_state =
                    failed ? RP1_STATE_ERROR
                           : RP1_STATE_READY;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.graph_done_seq = c.graph_seq;
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            c.heartbeat = c.heartbeat + 1;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    bool processGraph() {
        auto& c = ddr_.ctrl();
        const std::uint32_t count   = c.node_count;
        const std::uint32_t cq_size = c.cq_size;
        for (std::uint32_t i = 0; i < count; ++i) {
            rp1_node_t& n = ddr_.nodes()[i];
            if (n.opcode == RP1_OP_WAIT) {
                const auto deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
                const auto& wait = n.payload.wait;
                while (!conditionSatisfied(
                    ddr_.signals()[wait.condition_signal].value,
                    wait.condition_op, wait.condition_value)) {
                    if (std::chrono::steady_clock::now() >
                        deadline) {
                        c.rp1_error_code = RP1_ERR_KERNEL_TIMEOUT;
                        return true;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(50));
                }
            }
            if (n.opcode == RP1_OP_KERNEL_DISPATCH) {
                const auto& kd = n.payload.kernel_dispatch;
                if (kd.arg_count >= 5) {
                    // Protocol v2: the argument buffer is an array of
                    // (reg_offset, value) pairs, so the actual values live in
                    // the odd words.  The copy kernel packs
                    // [bytes, src_lo, src_hi, dst_lo, dst_hi].
                    const std::uint32_t* args =
                        ddr_.args() + (kd.arg_buffer_offset / sizeof(std::uint32_t));
                    const std::uint32_t bytes = args[1];
                    const std::uint64_t src =
                        static_cast<std::uint64_t>(args[3]) |
                        (static_cast<std::uint64_t>(args[5]) << 32);
                    const std::uint64_t dst =
                        static_cast<std::uint64_t>(args[7]) |
                        (static_cast<std::uint64_t>(args[9]) << 32);
                    // Only emulate the copy when the request lands inside the
                    // BAR backing; other kernels (e.g. plain graph_kernel) also
                    // satisfy arg_count >= 5 but are not copies.
                    if (bytes > 0 && ddr_.inWindow(src, bytes) &&
                        ddr_.inWindow(dst, bytes)) {
                        std::memcpy(ddr_.rp1Ptr(dst), ddr_.rp1Ptr(src), bytes);
                    }
                }
            }
            if (n.opcode == RP1_OP_SIGNAL && writeSignals_) {
                const auto& pl = n.payload.signal;
                ddr_.signals()[pl.target_slot].value = pl.value;
                ddr_.signals()[pl.target_slot].last_writer_node = i;
            }
            const auto completion =
                completion_
                    ? completion_(i, n)
                    : std::optional<std::pair<
                          std::uint32_t, std::uint32_t>>(
                          std::make_pair(
                              static_cast<std::uint32_t>(
                                  RP1_CQ_OK),
                              0u));
            if ((n.flags & RP1_FLAG_SILENT) == 0u &&
                completion) {
                while (!stop_.load(std::memory_order_relaxed) &&
                       c.cq_write_idx - c.cq_read_idx == cq_size) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(50));
                }
                if (stop_.load(std::memory_order_relaxed))
                    return true;
                const std::uint32_t idx = c.cq_write_idx & (cq_size - 1u);
                rp1_cq_entry_t& e = ddr_.cq()[idx];
                e.node_index   = i;
                e.status       = completion->first;
                e.error_detail = completion->second;
                e.timestamp    = 0;
                ++c.cq_write_idx;
            }
            n.status = RP1_NODE_DONE;
            if (completion &&
                completion->first != RP1_CQ_OK) {
                c.rp1_error_code = completion->second;
                c.terminal_error_node = i;
                c.terminal_error_detail = completion->second;
                c.terminal_error_aux = 0u;
                return true;
            }
        }
        return false;
    }

    DdrView           ddr_;
    bool              writeSignals_;
    CompletionFn      completion_;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

// A faithful host port of the RP1 firmware flat scanner (rp1_loop.c): honors
// barrier buckets and executes LOOP / RERUN / COND / WAIT / SIGNAL exactly like
// the device, so control-flow images can be validated end-to-end on the host
// (data + iteration counts), not just structurally.  Kernels complete
// immediately (no inflight delay) and each dispatch is counted by base address.
// This is the "two-queue simulation harness" the plan calls for; a single
// instance models one RP1 queue, and instances can share a DDR-backed signal
// array to rendezvous via SIGNAL/WAIT.
class FaithfulRp1 {
   public:
    // @p sharedSignals, when non-null, is used as the signal array instead of
    // this queue's own DDR signals, letting multiple concurrent FaithfulRp1
    // instances rendezvous via SIGNAL/WAIT on the same host-visible slots --
    // the cross-queue channel the compiler split targets.
    explicit FaithfulRp1(DdrView ddr, rp1_signal_slot_t* sharedSignals = nullptr)
        : ddr_(ddr), signals_(sharedSignals ? sharedSignals : ddr.signals()) {
        thread_ = std::thread([this] { run(); });
    }
    ~FaithfulRp1() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    std::uint32_t dispatches(std::uint32_t base) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = dispatchCount_.find(base);
        return it == dispatchCount_.end() ? 0u : it->second;
    }

   private:
    static bool cmp(std::uint32_t sig, std::uint16_t op, std::uint32_t val) {
        switch (op) {
            case RP1_COP_EQ:     return sig == val;
            case RP1_COP_NE:     return sig != val;
            case RP1_COP_LT:     return sig < val;
            case RP1_COP_GE:     return sig >= val;
            case RP1_COP_AND_NZ: return (sig & val) != 0;
            case RP1_COP_AND_Z:  return (sig & val) == 0;
        }
        return false;
    }

    void run() {
        while (!stop_.load(std::memory_order_relaxed)) {
            auto& c = ddr_.ctrl();
            if (c.graph_seq != c.graph_done_seq) {
                c.rp1_state = RP1_STATE_RUNNING;
                processGraph();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.rp1_state      = RP1_STATE_READY;
                c.graph_done_seq = c.graph_seq;
                std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            c.heartbeat = c.heartbeat + 1;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    void processGraph() {
        auto& c = ddr_.ctrl();
        const std::uint32_t count = c.node_count;
        rp1_node_t* nodes = ddr_.nodes();
        rp1_signal_slot_t* sigs = signals_;

        std::vector<std::uint8_t> status(count, RP1_NODE_PENDING);
        std::vector<std::uint32_t> barriers(RP1_MAX_BUCKETS, 0);
        std::vector<std::uint32_t> loopIters(RP1_MAX_LOOPS, 0);

        auto setDone = [&](std::uint32_t i) {
            status[i] = RP1_NODE_DONE;
            barriers[nodes[i].barrier_set_bucket] |= nodes[i].barrier_set_mask;
        };

        for (std::uint64_t guard = 0; guard < 10'000'000ull; ++guard) {
            bool progress = false;
            for (std::uint32_t i = 0; i < count; ++i) {
                if (status[i] != RP1_NODE_PENDING) continue;
                rp1_node_t& n = nodes[i];
                if ((barriers[n.barrier_await_bucket] & n.barrier_await_mask) !=
                    n.barrier_await_mask) {
                    continue;
                }
                switch (n.opcode) {
                    case RP1_OP_KERNEL_DISPATCH: {
                        std::lock_guard<std::mutex> lk(mtx_);
                        dispatchCount_[n.payload.kernel_dispatch.kernel_base_addr]++;
                        setDone(i);
                        progress = true;
                        break;
                    }
                    case RP1_OP_SIGNAL: {
                        const auto& p = n.payload.signal;
                        switch (p.operation) {
                            case RP1_SIGOP_SET: sigs[p.target_slot].value = p.value; break;
                            case RP1_SIGOP_ADD: sigs[p.target_slot].value += p.value; break;
                            case RP1_SIGOP_OR:  sigs[p.target_slot].value |= p.value; break;
                            case RP1_SIGOP_AND: sigs[p.target_slot].value &= p.value; break;
                        }
                        setDone(i);
                        progress = true;
                        break;
                    }
                    case RP1_OP_WAIT: {
                        const auto& w = n.payload.wait;
                        if (cmp(sigs[w.condition_signal].value, w.condition_op,
                                w.condition_value)) {
                            setDone(i);
                            progress = true;
                        } else {
                            status[i] = RP1_NODE_WAITING;
                        }
                        break;
                    }
                    case RP1_OP_SCALAR_READ: {
                        // Model a body output scalar that increases by one each
                        // time it is captured (i.e. a loop variable computing
                        // i = i + 1), so a data-dependent loop predicate over
                        // this slot terminates deterministically.
                        const auto& sr = n.payload.scalar_read;
                        sigs[sr.target_slot].value = ++scalarReadCount_[sr.target_slot];
                        setDone(i);
                        progress = true;
                        break;
                    }
                    case RP1_OP_SCALAR_COPY: {
                        // Record the slot->register copy so tests can assert the
                        // carried value was fed into a kernel register (we don't
                        // model the kernel itself, only the transfer).
                        const auto& sc = n.payload.scalar_copy;
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            scalarCopies_[sc.dest_addr] = sigs[sc.source_slot].value;
                        }
                        setDone(i);
                        progress = true;
                        break;
                    }
                    case RP1_OP_LOOP: {
                        const auto& lp = n.payload.loop;
                        loopIters[lp.loop_id]++;
                        bool exit = false;
                        if (lp.max_iterations > 0 && loopIters[lp.loop_id] > lp.max_iterations)
                            exit = true;
                        if (cmp(sigs[lp.condition_signal].value, lp.condition_op,
                                lp.condition_value))
                            exit = true;
                        if (exit) {
                            setDone(i);
                        } else {
                            for (std::uint8_t b = lp.bucket_clear_start;
                                 b <= lp.bucket_clear_end; ++b)
                                barriers[b] = 0;
                            for (std::uint32_t nn = lp.body_start; nn <= lp.body_end; ++nn)
                                status[nn] = RP1_NODE_PENDING;
                            status[i] = RP1_NODE_DONE;  // no barrier on continue
                        }
                        progress = true;
                        break;
                    }
                    case RP1_OP_COND: {
                        const auto& cd = n.payload.cond;
                        if (cmp(sigs[cd.condition_signal].value, cd.condition_op,
                                cd.condition_value)) {
                            barriers[cd.done_bucket] |= cd.done_mask;
                        } else {
                            for (std::uint8_t b = cd.bucket_clear_start;
                                 b <= cd.bucket_clear_end; ++b)
                                barriers[b] = 0;
                            for (std::uint32_t nn = cd.body_start; nn <= cd.body_end; ++nn)
                                status[nn] = RP1_NODE_PENDING;
                        }
                        setDone(i);
                        progress = true;
                        break;
                    }
                    case RP1_OP_RERUN: {
                        status[n.payload.rerun.target_node] = RP1_NODE_PENDING;
                        setDone(i);
                        progress = true;
                        break;
                    }
                    default:  // NOP / PDI_LOAD / SCALAR_* -> immediate
                        setDone(i);
                        progress = true;
                        break;
                }
            }
            // Re-poll parked WAITs (a peer queue / the host may have written).
            for (std::uint32_t i = 0; i < count; ++i) {
                if (status[i] != RP1_NODE_WAITING) continue;
                const auto& w = nodes[i].payload.wait;
                if (cmp(sigs[w.condition_signal].value, w.condition_op, w.condition_value)) {
                    setDone(i);
                    progress = true;
                }
            }
            if (!progress) {
                bool waiting = false;
                for (std::uint32_t i = 0; i < count; ++i)
                    if (status[i] == RP1_NODE_WAITING) waiting = true;
                if (!waiting) break;  // graph complete
                // Outstanding WAIT with no local progress: yield so a peer queue
                // can advance the signal it is gated on.
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    }

    DdrView                                ddr_;
    rp1_signal_slot_t*                     signals_;
    std::atomic<bool>                      stop_{false};
    std::thread                            thread_;
    std::mutex                             mtx_;
    std::map<std::uint32_t, std::uint32_t> dispatchCount_;
    std::map<std::uint32_t, std::uint32_t> scalarReadCount_;

   public:
    std::uint32_t scalarCopyTo(std::uint32_t addr) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = scalarCopies_.find(addr);
        return it == scalarCopies_.end() ? 0xFFFFFFFFu : it->second;
    }

   private:
    std::map<std::uint32_t, std::uint32_t> scalarCopies_;  // dest_addr -> last value
};

void primeAsReady(DdrView ddr) {
    auto& c = ddr.ctrl();
    c.version      = RP1_PROTOCOL_VERSION;
    c.capabilities = RP1_REQUIRED_CAPABILITIES;
    c.pdi_ipi_platform_id = 0x51454D55u;
    c.rp1_state    = RP1_STATE_READY;
    c.heartbeat    = 1;
    c.magic        = RP1_CTRL_MAGIC;
}

FpgaKernelLocationLookup makeDiamondLookup() {
    return [](const std::string& name) -> FpgaKernelLocation {
        if (name == "kA") return {kKernelA_R5, 0};
        if (name == "kB") return {kKernelB_R5, 0};
        if (name == "kC") return {kKernelC_R5, 0};
        if (name == "kD") return {kKernelD_R5, 0};
        throw std::runtime_error("unknown kernel '" + name + "'");
    };
}

std::shared_ptr<fpga::FpgaVbinSpec> makeImageRuntimeSpec() {
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec imageA;
    imageA.id = "imageA";
    imageA.pdiBytes = {0xA};
    fpga::FpgaKernelSpec implicitA;
    implicitA.name = "implicit";
    implicitA.r5_base_addr = kKernelA_R5;
    imageA.kernels.emplace(
        implicitA.name, implicitA);
    spec->addImage(std::move(imageA));

    fpga::FpgaImageSpec imageB;
    imageB.id = "imageB";
    imageB.pdiBytes = {0xB};
    fpga::FpgaKernelSpec implicitB;
    implicitB.name = "implicit";
    implicitB.r5_base_addr = kKernelB_R5;
    imageB.kernels.emplace(
        implicitB.name, implicitB);
    fpga::FpgaKernelSpec failing;
    failing.name = "failing";
    failing.r5_base_addr = kKernelC_R5;
    imageB.kernels.emplace(failing.name, failing);
    spec->addImage(std::move(imageB));
    return spec;
}

Rp1QueueProgram makeImplicitImageProgram() {
    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1KernelCommand kernel;
    kernel.id = "implicit";
    kernel.deviceId = "fpga:0";
    kernel.kernel = fpgaKernel("implicit");
    program.commands.emplace_back(std::move(kernel));
    return program;
}

class FpgaDeviceFixture : public ::testing::Test {
   protected:
    void SetUp() override {
        backing_.assign(kBarSize, std::byte{0});
        ddr_ = DdrView{backing_.data()};
        primeAsReady(ddr_);
        window_ = std::make_shared<fpga::Rp1BarWindow>(backing_.data(), backing_.size(), kWindowOff);
        rp1_    = std::make_unique<FakeRp1>(ddr_);
    }
    void TearDown() override {
        // device is closed before rp1_ exits to avoid use-after-free on
        // the shared submitter.
        rp1_.reset();
    }

    std::vector<std::byte>              backing_;
    DdrView                             ddr_{};
    std::shared_ptr<fpga::Rp1BarWindow> window_;
    std::unique_ptr<FakeRp1>            rp1_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Construction validation
// ---------------------------------------------------------------------------

TEST_F(FpgaDeviceFixture, ConstructorRejectsNullWindow) {
    EXPECT_THROW(FpgaDevice("fpga:0", nullptr, makeDiamondLookup()),
                 std::invalid_argument);
}

TEST_F(FpgaDeviceFixture, ConstructorRejectsNullLookup) {
    EXPECT_THROW(FpgaDevice("fpga:0", window_, FpgaKernelLocationLookup{}),
                 std::invalid_argument);
}

TEST_F(FpgaDeviceFixture, TypeAndIdMatchIDeviceContract) {
    FpgaDevice dev("fpga:0", window_, makeDiamondLookup());
    EXPECT_EQ(dev.type(), DeviceType::FPGA);
    EXPECT_EQ(dev.id(), "fpga:0");
}

TEST_F(FpgaDeviceFixture, ScalarAndRendezvousLeasesAreUniqueAndReusable) {
    FpgaDevice dev("fpga:0", window_, makeDiamondLookup());
    auto first = dev.leaseResources(
        {RendezvousId(0), RendezvousId(1)},
        {ScalarResourceId(0), ScalarResourceId(1)});
    auto second = dev.leaseResources(
        {RendezvousId(0), RendezvousId(1)},
        {ScalarResourceId(0), ScalarResourceId(1)});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const std::set<std::uint32_t> firstSlots{
        static_cast<std::uint32_t>(
            first->rendezvousResource(RendezvousId(0)).value()),
        static_cast<std::uint32_t>(
            first->rendezvousResource(RendezvousId(1)).value()),
        static_cast<std::uint32_t>(
            first->scalarResource(ScalarResourceId(0)).value()),
        static_cast<std::uint32_t>(
            first->scalarResource(ScalarResourceId(1)).value())};
    ASSERT_EQ(firstSlots.size(), 4u);
    EXPECT_EQ(firstSlots.count(static_cast<std::uint32_t>(
                  second->rendezvousResource(RendezvousId(0)).value())), 0u);
    EXPECT_EQ(firstSlots.count(static_cast<std::uint32_t>(
                  second->rendezvousResource(RendezvousId(1)).value())), 0u);
    EXPECT_EQ(firstSlots.count(static_cast<std::uint32_t>(
                  second->scalarResource(ScalarResourceId(0)).value())), 0u);
    EXPECT_EQ(firstSlots.count(static_cast<std::uint32_t>(
                  second->scalarResource(ScalarResourceId(1)).value())), 0u);

    first.reset();
    auto third = dev.leaseResources(
        {RendezvousId(0), RendezvousId(1)},
        {ScalarResourceId(0), ScalarResourceId(1)});
    const std::set<std::uint32_t> thirdSlots{
        static_cast<std::uint32_t>(
            third->rendezvousResource(RendezvousId(0)).value()),
        static_cast<std::uint32_t>(
            third->rendezvousResource(RendezvousId(1)).value()),
        static_cast<std::uint32_t>(
            third->scalarResource(ScalarResourceId(0)).value()),
        static_cast<std::uint32_t>(
            third->scalarResource(ScalarResourceId(1)).value())};
    EXPECT_EQ(thirdSlots, firstSlots);
}

TEST_F(FpgaDeviceFixture, BarBackedBufferArenaDoesNotOverlapTraceRing) {
    FpgaDevice dev("fpga:0", window_, makeDiamondLookup());
    ddr_.traces()[0].timestamp = 0x11223344u;
    ddr_.traces()[0].event = RP1_TRACE_KERNEL_LAUNCH;
    ddr_.traces()[0].node_index = 7u;
    ddr_.traces()[0].aux0 = 0x55667788u;
    ddr_.traces()[0].aux1 = 0x99aabbccu;

    const std::uint8_t bytes[32] = {};
    dev.setInputBuffer("scratch", bytes, sizeof(bytes));

    EXPECT_EQ(ddr_.traces()[0].timestamp, 0x11223344u);
    EXPECT_EQ(ddr_.traces()[0].event, static_cast<std::uint16_t>(RP1_TRACE_KERNEL_LAUNCH));
    EXPECT_EQ(ddr_.traces()[0].node_index, 7u);
    EXPECT_EQ(ddr_.traces()[0].aux0, 0x55667788u);
    EXPECT_EQ(ddr_.traces()[0].aux1, 0x99aabbccu);
}

TEST_F(FpgaDeviceFixture, ImageNumericIdIsStableOneBasedAndZeroForUnguarded) {
    // The mock/lookup path has no vbin spec, so the guard is disabled (0).
    FpgaDevice lookupDev("fpga:0", window_, makeDiamondLookup());
    EXPECT_EQ(lookupDev.imageNumericId("imageA"), 0u);
    EXPECT_EQ(lookupDev.imageNumericId(""), 0u);

    // A vbin-spec-backed device assigns 1-based ids in (name-sorted) order.
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec a;
    a.id = "imageA";
    fpga::FpgaImageSpec b;
    b.id = "imageB";
    spec->addImage(a);
    spec->addImage(b);

    FpgaDevice dev("fpga:0", window_, spec, /*initialImageId=*/std::string{});
    EXPECT_EQ(dev.imageNumericId("imageA"), 1u);
    EXPECT_EQ(dev.imageNumericId("imageB"), 2u);
    EXPECT_EQ(dev.imageNumericId("imageC"), 0u);  // unknown -> unguarded
    EXPECT_EQ(dev.imageNumericId(""), 0u);
}

TEST_F(FpgaDeviceFixture, CqDiagnosticReportsRetainedCompletions) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1KernelCommand kernel;
    kernel.id = "kernel";
    kernel.deviceId = "fpga:0";
    kernel.kernel = fpgaKernel("kA");
    program.commands.emplace_back(std::move(kernel));

    auto plan = dev->compileProgram(program);
    ASSERT_NE(plan, nullptr);

    ScopedEnv cqTrace("VRT_RP1_CQ", std::string("1"));
    testing::internal::CaptureStderr();
    EXPECT_NO_THROW(plan->launch());
    EXPECT_NO_THROW(plan->wait());
    const std::string diagnostics = testing::internal::GetCapturedStderr();

    EXPECT_NE(diagnostics.find("[rp1-cq] entries="), std::string::npos);
    EXPECT_NE(
        diagnostics.find("opcode=KERNEL_DISPATCH status=OK(0)"),
        std::string::npos);
}

// ---------------------------------------------------------------------------
// compileProgram: rejection paths
// ---------------------------------------------------------------------------

namespace {

// CPU kernel that produces a buffer (so a CPU -> FPGA edge needs a bridge).
class CopyKernel : public CpuKernel {
   public:
    CopyKernel() : CpuKernel("copy") {
        ioType_.inputs.push_back({"in", BufferType::I32});
        ioType_.outputs.push_back({"out", BufferType::I32});
    }
    IOTypeMap ioTypeMap() const override { return ioType_; }
    void run(Args& args) override {
        const auto& in  = args.buffer("in");
        const auto& out = args.buffer("out");
        std::memcpy(out.data, in.data, std::min(in.sizeBytes, out.sizeBytes));
    }
   private:
    IOTypeMap   ioType_;
};

}  // namespace

TEST_F(FpgaDeviceFixture, CpuToFpgaBufferEdgeCopiesIntoFpgaStore) {
    // CPU kernel produces a buffer; FPGA kernel consumes it. The compiler
    // splices a consumer-side bridge into the FPGA Rp1QueueProgram, which now copies
    // the bytes into the FPGA BAR-backed buffer store before kernel dispatch.
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::cpuDevice(g)->registerKernel(std::make_shared<CopyKernel>());

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    detail::GraphTestAccess::registerDevice(g, dev);

    GraphScalar elements = detail::GraphTestAccess::scalarInput<std::uint64_t>(g, "elements");
    GraphBuffer raw = detail::GraphTestAccess::inputBuffer(g, BufferType::I32, "raw", elements);
    IOTypeMap cpuIo;
    cpuIo.inputs.push_back({"in", BufferType::I32});
    cpuIo.outputs.push_back({"out", BufferType::I32});
    KernelDescriptor cpu{"copy", DeviceType::CPU, std::nullopt, cpuIo};

    detail::PortBindings io1;
    GraphBuffer staged;
    io1.bindInput("in", raw)
       .bindOutput("out", BufferType::I32, staged);
    detail::GraphTestAccess::addNode(g, cpu, std::move(io1), "cpu");

    IOTypeMap fpgaIo;
    fpgaIo.inputs.push_back({"in", BufferType::I32});
    detail::PortBindings io2;
    io2.bindInput("in", staged);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", fpgaIo), std::move(io2), "fpga:0");

    const std::vector<std::int32_t> input = {1, 2, 3, 4};
    detail::GraphTestAccess::cpuDevice(g)->setInputBuffer("raw", input.data(), input.size() * sizeof(input[0]));

    auto debugExec = g.compile();
    detail::ExecutionTestAccess::writeScalar(debugExec, elements, static_cast<std::uint64_t>(input.size()));
    ASSERT_NO_THROW(debugExec.run());

    std::vector<std::int32_t> echoed(input.size(), 0);
    dev->getOutputBuffer(staged.name(), echoed.data(), echoed.size() * sizeof(echoed[0]));
    EXPECT_EQ(echoed, input);
}

TEST_F(FpgaDeviceFixture, CpuFpgaBridgeRejectsMissingProducerBuffer) {
    CpuDevice cpu("cpu");
    FpgaDevice fpga("fpga:0", window_, makeDiamondLookup());
    CpuFpgaBridge bridge(cpu, fpga);

    GraphBuffer missing = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "missing", 0);
    BridgeStepPair step = bridge.makeTransfer(
        cpu, fpga, missing, /*sizeHintBytes=*/0, "producer", "consumer");

    EXPECT_THROW(step.producerAction(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, CpuFpgaCpuBufferRoundTripUsesPackedBufferPointers) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::cpuDevice(g)->registerKernel(std::make_shared<CopyKernel>());
    detail::GraphTestAccess::registerDevice(g, dev);

    GraphScalar elements = detail::GraphTestAccess::scalarInput<std::uint64_t>(g, "elements");
    GraphBuffer raw = detail::GraphTestAccess::inputBuffer(g, BufferType::I32, "raw", elements);

    IOTypeMap cpuIo;
    cpuIo.inputs.push_back({"in", BufferType::I32});
    cpuIo.outputs.push_back({"out", BufferType::I32});
    KernelDescriptor cpu{"copy", DeviceType::CPU, std::nullopt, cpuIo};

    detail::PortBindings cpuProduceIo;
    GraphBuffer toFpga = detail::GraphTestAccess::buffer<std::int32_t>(g, "toFpga", elements);
    cpuProduceIo.bindInput("in", raw)
                .bindExistingOutput("out", toFpga);
    const std::string cpuProducer = detail::GraphTestAccess::addNode(g, cpu, std::move(cpuProduceIo), "cpu");

    IOTypeMap fpgaIo;
    fpgaIo.inputScalars.push_back({"bytes", ScalarType::U32});
    fpgaIo.inputs.push_back({"in", BufferType::I32});
    fpgaIo.outputs.push_back({"out", BufferType::I32});
    GraphScalar copyBytes = detail::GraphTestAccess::scalarInput<std::uint32_t>(g, "copy_bytes");

    detail::PortBindings fpgaCopyIo;
    GraphBuffer fromFpga = detail::GraphTestAccess::buffer<std::int32_t>(g, "fromFpga", elements);
    constexpr std::uint32_t kBytes = 4u * sizeof(std::int32_t);
    fpgaCopyIo.bindInputScalar("bytes", copyBytes)
              .bindInput("in", toFpga)
              .bindExistingOutput("out", fromFpga);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", fpgaIo), std::move(fpgaCopyIo), "fpga:0", {cpuProducer});

    detail::PortBindings cpuConsumeIo;
    GraphBuffer finalOut = detail::GraphTestAccess::buffer<std::int32_t>(g, "finalOut", elements);
    cpuConsumeIo.bindInput("in", fromFpga)
                .bindExistingOutput("out", finalOut);
    detail::GraphTestAccess::addNode(g, cpu, std::move(cpuConsumeIo), "cpu");

    const std::vector<std::int32_t> input = {10, 20, 30, 40};
    detail::GraphTestAccess::cpuDevice(g)->setInputBuffer("raw", input.data(), input.size() * sizeof(input[0]));

    auto exec = g.compile();
    detail::ExecutionTestAccess::writeScalar(exec, elements, static_cast<std::uint64_t>(input.size()));
    detail::ExecutionTestAccess::writeScalar(exec, copyBytes, kBytes);
    std::vector<std::int32_t> output(input.size(), 0);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_NO_THROW(exec.run());
        detail::GraphTestAccess::cpuDevice(g)->getOutputBuffer(finalOut.name(), output.data(),
                                       output.size() * sizeof(output[0]));
        if (output == input) break;
    }
    EXPECT_EQ(output, input);

    // The fake RP1 copied through the addresses packed after the scalar byte count.
    const rp1_node_t* dispatch = findDispatch(ddr_, kKernelA_R5);
    ASSERT_NE(dispatch, nullptr);
    EXPECT_GE(dispatch->payload.kernel_dispatch.arg_count, 5u);
}

TEST_F(FpgaDeviceFixture, ExecutionOwnsCrossDeviceRuntimeAfterGraphDestruction) {
    std::optional<GraphScalar> elements;
    std::optional<GraphScalar> copyBytes;
    std::optional<GraphBuffer> raw;
    std::optional<GraphBuffer> output;

    Execution exec = [&] {
        Graph graph = Graph::withDefaults();
        detail::GraphTestAccess::cpuDevice(graph)->registerKernel(
            std::make_shared<CopyKernel>());
        auto fpga = std::make_shared<FpgaDevice>(
            "fpga:0", window_, makeDiamondLookup());
        detail::GraphTestAccess::registerDevice(graph, fpga);

        elements.emplace(
            detail::GraphTestAccess::scalarInput<std::uint64_t>(
                graph, "elements"));
        copyBytes.emplace(
            detail::GraphTestAccess::scalarInput<std::uint32_t>(
                graph, "copy_bytes"));
        raw.emplace(detail::GraphTestAccess::inputBuffer(
            graph, BufferType::I32, "raw", *elements));

        IOTypeMap cpuType;
        cpuType.inputs.push_back({"in", BufferType::I32});
        cpuType.outputs.push_back({"out", BufferType::I32});
        KernelDescriptor cpu{
            "copy", DeviceType::CPU, std::nullopt, cpuType};

        GraphBuffer toFpga =
            detail::GraphTestAccess::buffer<std::int32_t>(
                graph, "to_fpga", *elements);
        detail::PortBindings produce;
        produce.bindInput("in", *raw)
            .bindExistingOutput("out", toFpga);
        detail::GraphTestAccess::addNode(
            graph, cpu, std::move(produce), "cpu");

        IOTypeMap fpgaType;
        fpgaType.inputScalars.push_back(
            {"bytes", ScalarType::U32});
        fpgaType.inputs.push_back({"in", BufferType::I32});
        fpgaType.outputs.push_back({"out", BufferType::I32});
        GraphBuffer fromFpga =
            detail::GraphTestAccess::buffer<std::int32_t>(
                graph, "from_fpga", *elements);
        detail::PortBindings deviceCopy;
        deviceCopy.bindInputScalar("bytes", *copyBytes)
            .bindInput("in", toFpga)
            .bindExistingOutput("out", fromFpga);
        detail::GraphTestAccess::addNode(
            graph, fpgaKernel("kA", fpgaType),
            std::move(deviceCopy), "fpga:0");

        output.emplace(
            detail::GraphTestAccess::outputBuffer<std::int32_t>(
                graph, "output", *elements));
        detail::PortBindings consume;
        consume.bindInput("in", fromFpga)
            .bindExistingOutput("out", *output);
        detail::GraphTestAccess::addNode(
            graph, cpu, std::move(consume), "cpu");

        return graph.compile();
    }();

    const std::vector<std::int32_t> input = {10, 20, 30, 40};
    exec.writeScalar(
        *elements, static_cast<std::uint64_t>(input.size()));
    exec.writeScalar(
        *copyBytes,
        static_cast<std::uint32_t>(
            input.size() * sizeof(input.front())));
    exec.write(*raw, input);
    ASSERT_NO_THROW(exec.run());

    std::vector<std::int32_t> result(input.size());
    ASSERT_NO_THROW(exec.read(*output, result));
    EXPECT_EQ(result, input);
}

TEST_F(FpgaDeviceFixture, CpuWaitConsumesPerIterationFpgaPublication) {
    rp1_.reset();
    primeAsReady(ddr_);
    FaithfulRp1 faithfulRp1(ddr_);

    Graph graph = Graph::withDefaults();
    detail::GraphTestAccess::cpuDevice(graph)->registerKernel(
        std::make_shared<CopyKernel>());
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements =
        graph.scalarInput<std::uint64_t>("elements");
    GraphScalar copyBytes =
        graph.scalarInput<std::uint32_t>("copy_bytes");
    GraphScalar iterations =
        graph.scalarInput<std::uint32_t>("iterations");
    GraphBuffer input =
        graph.input<std::int32_t>("input", elements);
    GraphBuffer output =
        graph.output<std::int32_t>("output", elements);

    KernelHandle cpu{
        "copy", DeviceType::CPU, std::nullopt,
        IOTypeMap{}.in<std::int32_t>("in")
            .out<std::int32_t>("out"),
        "cpu"};
    KernelHandle deviceCopy{
        "kA", DeviceType::FPGA, std::nullopt,
        IOTypeMap{}.scalarIn<std::uint32_t>("bytes")
            .in<std::int32_t>("in")
            .out<std::int32_t>("out"),
        "fpga:0"};

    auto loop = graph.addLoop({
        .count = iterations,
        .inputs = {{"state", input}},
        .outputs = {{"state", output}},
    });
    GraphBuffer staged =
        loop.buffer<std::int32_t>("staged", elements);
    loop.addKernelCall({
        .kernel = cpu,
        .inputs = {{"in", loop.input("state")}},
        .outputs = {{"out", staged}},
    });
    GraphBuffer copied =
        loop.buffer<std::int32_t>("copied", elements);
    loop.addKernelCall({
        .kernel = deviceCopy,
        .inputScalars = {{"bytes", copyBytes}},
        .inputs = {{"in", staged}},
        .outputs = {{"out", copied}},
    });
    loop.addKernelCall({
        .kernel = cpu,
        .inputs = {{"in", copied}},
        .outputs = {{"out", loop.output("state")}},
    });

    Execution exec = graph.compile();
    const std::vector<std::int32_t> values = {1, 2, 3, 4};
    exec.writeScalar(
        elements, static_cast<std::uint64_t>(values.size()));
    exec.writeScalar(
        copyBytes,
        static_cast<std::uint32_t>(
            values.size() * sizeof(values.front())));
    exec.writeScalar(iterations, 2u);
    exec.write(input, values);
    ASSERT_NO_THROW(exec.run());

    std::size_t consumedEvents = 0;
    for (std::uint32_t i = 0; i < ddr_.ctrl().node_count; ++i) {
        const rp1_node_t& node = ddr_.nodes()[i];
        if (node.opcode != RP1_OP_SIGNAL ||
            node.payload.signal.value != 1u ||
            node.payload.signal.target_slot ==
                kDefaultSentinelSlot) {
            continue;
        }
        ++consumedEvents;
        EXPECT_EQ(
            ddr_.signals()[node.payload.signal.target_slot].value,
            0u);
    }
    EXPECT_GT(consumedEvents, 0u);
}

TEST_F(FpgaDeviceFixture, CarriedBufferAliasStaysCoherentAfterGrowth) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });

    GraphScalar elements = ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "elements");
    GraphBuffer parent = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "state", 0, elements);
    GraphBuffer local = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "state", 1, elements);

    const std::int32_t seed = 42;
    dev->setInputBuffer(
        scopedBufferKey(parent.scopeId(), parent.name()),
        &seed, sizeof(seed));

    Rp1BoundaryCommand importB;
    importB.id = "import";
    importB.deviceId = "fpga:0";
    importB.side = Rp1BoundaryCommand::Side::Start;
    importB.bufferCopies.push_back({/*sourceName=*/parent.name(),
                                    /*sourceScopeId=*/parent.scopeId(),
                                    /*targetName=*/local.name(),
                                    /*targetScopeId=*/local.scopeId()});

    IOTypeMap bodyType;
    bodyType.inputs.push_back({"in", BufferType::I32});
    Rp1KernelCommand bodyK;
    bodyK.id = "body";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel = fpgaKernel("body", bodyType);
    bodyK.ioMap.bindInput("in", local);

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(importB);
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    (*dg.scalarValues)[scopedScalarKey(0, "elements")] = 2;
    Rp1LoopCommand loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 1);
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const std::int32_t grown[] = {7, 9};
    dev->setInputBuffer(
        scopedBufferKey(local.scopeId(), local.name()),
        grown, sizeof(grown));

    EXPECT_EQ(dev->bufferSize(scopedBufferKey(parent.scopeId(), parent.name())),
              sizeof(grown))
        << "growing an alias must update the canonical source buffer record";
    std::int32_t readback[] = {0, 0};
    ASSERT_NO_THROW(dev->getOutputBuffer(
        scopedBufferKey(parent.scopeId(), parent.name()),
        readback, sizeof(readback)));
    EXPECT_EQ(readback[0], grown[0]);
    EXPECT_EQ(readback[1], grown[1]);
}

TEST_F(FpgaDeviceFixture, LoopCarriedOutputAliasPreservesInitialInput) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });

    GraphScalar elements = ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "elements");
    GraphBuffer parentInput =
        ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "input", 0, elements);
    GraphBuffer parentOutput =
        ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "output", 0, elements);
    GraphBuffer localInput =
        ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "local_input", 1, elements);
    GraphBuffer localOutput =
        ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "local_output", 1, elements);

    const std::int32_t seed[] = {7, 11};
    dev->setInputBuffer(
        scopedBufferKey(0, "input"), seed, sizeof(seed));

    Rp1BoundaryCommand importB;
    importB.id = "import";
    importB.deviceId = "fpga:0";
    importB.side = Rp1BoundaryCommand::Side::Start;
    importB.bufferCopies.push_back(
        {parentInput.name(), 0, localInput.name(), 1});

    IOTypeMap bodyType;
    bodyType.inputs.push_back({"in", BufferType::I32});
    bodyType.outputs.push_back({"out", BufferType::I32});
    Rp1KernelCommand bodyK;
    bodyK.id = "body";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel = fpgaKernel("body", bodyType);
    bodyK.ioMap.bindInput("in", localInput)
               .bindExistingOutput("out", localOutput);

    Rp1BoundaryCommand exportB;
    exportB.id = "export";
    exportB.deviceId = "fpga:0";
    exportB.side = Rp1BoundaryCommand::Side::End;
    exportB.dependsOn = {"body"};
    exportB.bufferCopies.push_back(
        {localOutput.name(), 1, parentOutput.name(), 0});
    exportB.bufferCopies.push_back(
        {localOutput.name(), 1, parentInput.name(), 0});

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands = {importB, bodyK, exportB};
    body->scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();
    (*dg.scalarValues)[scopedScalarKey(0, "elements")] = 2;
    Rp1LoopCommand loop;
    loop.id = "loop";
    loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 1);
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop";
    child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    std::int32_t readback[] = {0, 0};
    ASSERT_NO_THROW(dev->getOutputBuffer(
        scopedBufferKey(0, "output"), readback, sizeof(readback)));
    EXPECT_EQ(readback[0], seed[0]);
    EXPECT_EQ(readback[1], seed[1]);
}

TEST_F(FpgaDeviceFixture, AutonomousLoopPublishesCarriedOutputToCpu) {
    rp1_.reset();
    FaithfulRp1 faithfulRp1(ddr_);

    Graph graph = Graph::withDefaults();
    detail::GraphTestAccess::cpuDevice(graph)->registerKernel(std::make_shared<CopyKernel>());
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements = detail::GraphTestAccess::scalarInput<std::uint64_t>(graph, "elements");
    GraphBuffer input = detail::GraphTestAccess::inputBuffer(graph, BufferType::I32, "input", elements);
    GraphBuffer sharpened =
        ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "sharpened", 0, elements);

    auto body = detail::GraphTestAccess::root(graph).createChild();
    GraphBuffer localInput = body->inputBuffer(BufferType::I32, "state", elements);
    body->importFromParent({{input, localInput}});

    IOTypeMap bodyType;
    bodyType.inputs.push_back({"in", BufferType::I32});
    bodyType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings bodyIo;
    GraphBuffer localOutput;
    bodyIo.bindInput("in", localInput)
          .bindOutput("out", BufferType::I32, localOutput, elements, body->scopeId());
    const std::string bodyId = body->addKernel(
        fpgaKernel("body", bodyType), std::move(bodyIo), "fpga:0");
    body->exportToParent({{localOutput, sharpened}, {localOutput, input}}, {bodyId});

    GraphScalar iterations =
        detail::GraphTestAccess::scalarInput<std::uint32_t>(
            graph, "iterations");
    ::vrt::graph::detail::LoopRecord loopSpec = test_support::fixedLoopRecord(
        test_support::tripCount(iterations), body);
    detail::GraphTestAccess::addLoop(graph, std::move(loopSpec));

    IOTypeMap copyType;
    copyType.inputs.push_back({"in", BufferType::I32});
    copyType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings copyIo;
    GraphBuffer output = detail::GraphTestAccess::outputBuffer<std::int32_t>(graph, "output", elements);
    copyIo.bindInput("in", sharpened)
          .bindExistingOutput("out", output);
    detail::GraphTestAccess::addNode(graph,
        KernelDescriptor{"copy", DeviceType::CPU, std::nullopt, copyType},
        std::move(copyIo), "cpu");

    auto exec = graph.compile();
    const std::int32_t seed[] = {7, 11};
    detail::ExecutionTestAccess::writeScalar(exec, elements, std::uint64_t{2});
    detail::ExecutionTestAccess::writeScalar(
        exec, iterations, 0u);
    detail::ExecutionTestAccess::writeBuffer(exec, input, seed, sizeof(seed));
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        ddr_.signals()[slot].value = 1u;
    }
    ASSERT_NO_THROW(exec.run());
    std::int32_t zeroReadback[] = {0, 0};
    ASSERT_NO_THROW(detail::ExecutionTestAccess::readBuffer(
        exec, output, zeroReadback, sizeof(zeroReadback)));
    EXPECT_EQ(zeroReadback[0], seed[0]);
    EXPECT_EQ(zeroReadback[1], seed[1]);
    EXPECT_EQ(faithfulRp1.dispatches(kKernelA_R5), 0u);
    const rp1_node_t* zeroLoop = std::find_if(
        ddr_.nodes(), ddr_.nodes() + ddr_.ctrl().node_count,
        [](const rp1_node_t& node) {
            return node.opcode == RP1_OP_LOOP;
        });
    ASSERT_NE(zeroLoop, ddr_.nodes() + ddr_.ctrl().node_count);
    EXPECT_EQ(zeroLoop->payload.loop.condition_op, RP1_COP_AND_Z);
    ASSERT_EQ((zeroLoop + 1)->opcode, RP1_OP_COND);
    EXPECT_EQ(
        (zeroLoop + 1)->payload.cond.condition_op,
        RP1_COP_AND_NZ);

    detail::ExecutionTestAccess::writeScalar(
        exec, iterations, 4u);
    for (int run = 0; run < 2; ++run) {
        SCOPED_TRACE(run);
        ASSERT_NO_THROW(exec.run());
        std::int32_t readback[] = {0, 0};
        ASSERT_NO_THROW(detail::ExecutionTestAccess::readBuffer(exec, output, readback, sizeof(readback)));
        EXPECT_EQ(readback[0], seed[0]);
        EXPECT_EQ(readback[1], seed[1]);
    }
    std::optional<std::uint32_t> loopIndex;
    for (std::uint32_t i = 0; i < ddr_.ctrl().node_count; ++i) {
        if (ddr_.nodes()[i].opcode == RP1_OP_LOOP) {
            loopIndex = i;
            break;
        }
    }
    ASSERT_TRUE(loopIndex.has_value());
    EXPECT_TRUE(std::any_of(
        ddr_.nodes(), ddr_.nodes() + ddr_.ctrl().node_count,
        [&](const rp1_node_t& node) {
            return node.opcode == RP1_OP_RERUN &&
                   node.payload.rerun.target_node == *loopIndex;
        }))
        << "RERUN must target the scheduled LOOP even when it is nonzero";
}

TEST_F(FpgaDeviceFixture,
       AutonomousWhileFalsePublishesInitialCarriedOutput) {
    rp1_.reset();
    FaithfulRp1 faithfulRp1(ddr_);

    Graph graph = Graph::withDefaults();
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements =
        graph.scalarInput<std::uint64_t>("elements");
    GraphScalar enabled =
        graph.scalarInput<std::uint32_t>("enabled");
    GraphBuffer input =
        graph.input<std::int32_t>("input", elements);
    GraphBuffer output =
        graph.output<std::int32_t>("output", elements);

    auto body = detail::GraphTestAccess::root(graph).createChild();
    GraphScalar localEnabled =
        body->scalar(ScalarType::U32, "local_enabled");
    GraphScalar nextEnabled =
        body->scalar(ScalarType::U32, "next_enabled");
    GraphBuffer localInput =
        body->inputBuffer(BufferType::I32, "local_input", elements);
    GraphBuffer localOutput =
        body->buffer(BufferType::I32, "local_output", elements);
    BoundaryMappings imports;
    imports.scalars.push_back({enabled, localEnabled});
    imports.buffers.push_back({input, localInput});
    const std::string start =
        body->importFromParent(std::move(imports));

    IOTypeMap bodyType;
    bodyType.inputScalars.push_back(
        {"enabled_in", ScalarType::U32});
    bodyType.outputScalars.push_back(
        {"enabled_out", ScalarType::U32});
    bodyType.inputs.push_back({"in", BufferType::I32});
    bodyType.outputs.push_back({"out", BufferType::I32});
    detail::PortBindings bodyIo;
    bodyIo.bindInputScalar("enabled_in", localEnabled)
        .bindOutputScalar("enabled_out", nextEnabled)
        .bindInput("in", localInput)
        .bindExistingOutput("out", localOutput);
    const std::string kernel = body->addKernel(
        fpgaKernel("body", bodyType), std::move(bodyIo),
        "fpga:0", {start});

    BoundaryMappings exports;
    exports.scalars.push_back({nextEnabled, enabled});
    exports.buffers.push_back({localOutput, output});
    exports.buffers.push_back({localOutput, input});
    body->exportToParent(std::move(exports), {kernel});

    detail::LoopRecord loop;
    loop.kind = LoopKind::WhileCondition;
    loop.condition = enabled != 0u;
    loop.body = std::move(body);
    detail::GraphTestAccess::addLoop(graph, std::move(loop));

    Execution execution = graph.compile();
    const std::vector<std::int32_t> initial{2, 3, 5};
    execution.writeScalar(
        elements, static_cast<std::uint64_t>(initial.size()));
    execution.writeScalar(enabled, 0u);
    execution.write(input, initial);
    ASSERT_NO_THROW(execution.run());
    EXPECT_EQ(faithfulRp1.dispatches(kKernelA_R5), 0u);
    std::vector<std::int32_t> result(initial.size());
    execution.read(output, result);
    EXPECT_EQ(result, initial);
}

TEST_F(FpgaDeviceFixture, DeviceCopyActionRefreshesTargetBuffer) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });

    const GraphBuffer source = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "source", 0);
    const GraphBuffer target = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "target", 0);
    const std::string sourceKey = scopedBufferKey(source.scopeId(), source.name());
    const std::string targetKey = scopedBufferKey(target.scopeId(), target.name());

    const std::int32_t first[] = {1, 2, 3};
    dev->setInputBuffer(sourceKey, first, sizeof(first));
    auto copy = dev->makeDeviceCopyAction(
        source, target, BufferType::I32, "HBM0", "HBM1");
    ASSERT_NO_THROW(copy());

    std::int32_t out[] = {0, 0, 0};
    ASSERT_NO_THROW(
        dev->getOutputBuffer(targetKey, out, sizeof(out)));
    EXPECT_EQ(out[0], first[0]);
    EXPECT_EQ(out[1], first[1]);
    EXPECT_EQ(out[2], first[2]);

    const std::int32_t second[] = {4, 5, 6};
    dev->setInputBuffer(sourceKey, second, sizeof(second));
    ASSERT_NO_THROW(copy());
    ASSERT_NO_THROW(
        dev->getOutputBuffer(targetKey, out, sizeof(out)));
    EXPECT_EQ(out[0], second[0]);
    EXPECT_EQ(out[1], second[1]);
    EXPECT_EQ(out[2], second[2]);
}

TEST_F(FpgaDeviceFixture, InoutBufferPacksOnePointerAndAliasesOutput) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });

    GraphScalar elements = ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "elements");
    GraphBuffer input = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "input", 0, elements);
    GraphBuffer output = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "output", 0, elements);
    const std::int32_t seed[] = {3, 5};
    dev->setInputBuffer(
        scopedBufferKey(0, "input"), seed, sizeof(seed));

    IOTypeMap rwType;
    rwType.inouts.push_back({{"data", BufferType::I32}, {"data_out", BufferType::I32}});

    Rp1KernelCommand kernel;
    kernel.id = "rw";
    kernel.deviceId = "fpga:0";
    kernel.kernel = fpgaKernel("kA", rwType);
    kernel.ioMap.bindExistingInout("data", "data_out", input, output);

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    (*dg.scalarValues)[scopedScalarKey(0, "elements")] = 2;
    dg.commands.emplace_back(kernel);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    ASSERT_EQ(ddr_.nodes()[0].opcode, RP1_OP_KERNEL_DISPATCH);
    const rp1_node_t* dispatch = findDispatch(ddr_, kKernelA_R5);
    ASSERT_NE(dispatch, nullptr);
    EXPECT_EQ(dispatch->payload.kernel_dispatch.arg_count, 2u);

    std::int32_t readback[] = {0, 0};
    ASSERT_NO_THROW(dev->getOutputBuffer(
        scopedBufferKey(0, "output"),
        readback, sizeof(readback)));
    EXPECT_EQ(readback[0], seed[0]);
    EXPECT_EQ(readback[1], seed[1]);
}

TEST_F(FpgaDeviceFixture,
       MultipleFpgaInoutConsumersUseIndependentCopies) {
    Graph graph = Graph::withDefaults();
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements =
        graph.scalarInput<std::uint64_t>("elements");
    GraphBuffer input =
        graph.input<std::int32_t>("input", elements);
    GraphBuffer firstOutput =
        graph.output<std::int32_t>("first_output", elements);
    GraphBuffer secondOutput =
        graph.output<std::int32_t>("second_output", elements);
    IOTypeMap type =
        IOTypeMap{}.inout<std::int32_t>("data");

    detail::PortBindings first;
    first.bindExistingInout(
        "data", "data", input, firstOutput);
    detail::GraphTestAccess::addNode(
        graph, fpgaKernel("kA", type), std::move(first),
        "fpga:0");
    detail::PortBindings second;
    second.bindExistingInout(
        "data", "data", input, secondOutput);
    detail::GraphTestAccess::addNode(
        graph, fpgaKernel("kB", type), std::move(second),
        "fpga:0");

    Execution execution = graph.compile();
    const std::vector<std::int32_t> seed{3, 5, 8, 13};
    execution.writeScalar(
        elements, static_cast<std::uint64_t>(seed.size()));
    execution.write(input, seed);
    ASSERT_NO_THROW(execution.run());

    const rp1_node_t* firstDispatch =
        findDispatch(ddr_, kKernelA_R5);
    const rp1_node_t* secondDispatch =
        findDispatch(ddr_, kKernelB_R5);
    ASSERT_NE(firstDispatch, nullptr);
    ASSERT_NE(secondDispatch, nullptr);
    auto pointerLow = [&](const rp1_node_t& dispatch) {
        const std::size_t word =
            dispatch.payload.kernel_dispatch.arg_buffer_offset /
            sizeof(std::uint32_t);
        return ddr_.args()[word + 1];
    };
    EXPECT_NE(
        pointerLow(*firstDispatch),
        pointerLow(*secondDispatch));

    std::vector<std::int32_t> firstResult(seed.size());
    std::vector<std::int32_t> secondResult(seed.size());
    execution.read(firstOutput, firstResult);
    execution.read(secondOutput, secondResult);
    EXPECT_EQ(firstResult, seed);
    EXPECT_EQ(secondResult, seed);
}

TEST_F(FpgaDeviceFixture, ReprogramNodeLowersToPdiLoad) {
    const auto tmpDir = makeTempDir("fpga-reprogram-test");
    const std::string pdiPath = writeTempFile(tmpDir, "imageB.pdi", "fake-pdi-bytes");

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1ReprogramCommand rp;
    rp.id = "rp";
    rp.deviceId = "fpga:0";
    rp.imageId = "imageB";
    rp.pdiPath = pdiPath;
    rp.timeoutCycles = 12345u;
    dg.commands.emplace_back(rp);

    auto plan = dev->compileProgram(dg);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    EXPECT_EQ(ddr_.nodes()[0].opcode, RP1_OP_PDI_LOAD);
    EXPECT_EQ(ddr_.nodes()[0].payload.pdi_load.timeout_cycles, 12345u);
    const std::uint64_t pdiAddr =
        static_cast<std::uint64_t>(ddr_.nodes()[0].payload.pdi_load.pdi_addr_lo) |
        (static_cast<std::uint64_t>(ddr_.nodes()[0].payload.pdi_load.pdi_addr_hi) << 32);
    EXPECT_GE(pdiAddr, static_cast<std::uint64_t>(RP1_CTRL_PHYS_ADDR));
    EXPECT_EQ(ddr_.nodes()[1].opcode, RP1_OP_SIGNAL);
    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);

    std::filesystem::remove_all(tmpDir);
}

TEST_F(FpgaDeviceFixture,
       SuccessfulPdiBeforeKernelFailureUpdatesActiveImage) {
    rp1_.reset();
    rp1_ = std::make_unique<FakeRp1>(
        ddr_, true,
        [](std::uint32_t, const rp1_node_t& node)
            -> std::optional<
                std::pair<std::uint32_t, std::uint32_t>> {
            if (node.opcode == RP1_OP_KERNEL_DISPATCH) {
                return std::make_pair(
                    static_cast<std::uint32_t>(RP1_CQ_ERROR),
                    0x55u);
            }
            return std::make_pair(
                static_cast<std::uint32_t>(RP1_CQ_OK), 0u);
        });
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeImageRuntimeSpec(),
        std::string{});

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1ReprogramCommand load;
    load.id = "load_b";
    load.deviceId = "fpga:0";
    load.imageId = "imageB";
    program.commands.emplace_back(load);
    Rp1KernelCommand failing;
    failing.id = "failing";
    failing.deviceId = "fpga:0";
    failing.kernel = KernelDescriptor{
        "failing", DeviceType::FPGA,
        std::string("imageB"), {}};
    failing.dependsOn = {load.id};
    program.commands.emplace_back(failing);

    auto plan = dev->compileProgram(program);
    plan->launch();
    EXPECT_THROW(plan->wait(), std::runtime_error);
    plan.reset();

    fpga::Rp1GraphImage projected;
    ASSERT_NO_THROW(
        projected = dev->projectProgram(
            makeImplicitImageProgram()));
    ASSERT_FALSE(projected.nodes.empty());
    EXPECT_EQ(
        projected.nodes.front()
            .payload.kernel_dispatch.kernel_base_addr,
        kKernelB_R5);
}

TEST_F(FpgaDeviceFixture, FailedPdiMakesActiveImageUnknown) {
    rp1_.reset();
    rp1_ = std::make_unique<FakeRp1>(
        ddr_, true,
        [](std::uint32_t, const rp1_node_t& node)
            -> std::optional<
                std::pair<std::uint32_t, std::uint32_t>> {
            if (node.opcode == RP1_OP_PDI_LOAD) {
                return std::make_pair(
                    static_cast<std::uint32_t>(RP1_CQ_ERROR),
                    static_cast<std::uint32_t>(
                        RP1_ERR_PDI_FAILED));
            }
            return std::make_pair(
                static_cast<std::uint32_t>(RP1_CQ_OK), 0u);
        });
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeImageRuntimeSpec(),
        "imageA");

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1ReprogramCommand load;
    load.id = "load_b";
    load.deviceId = "fpga:0";
    load.imageId = "imageB";
    program.commands.emplace_back(load);

    auto plan = dev->compileProgram(program);
    plan->launch();
    EXPECT_THROW(plan->wait(), std::runtime_error);
    plan.reset();

    EXPECT_THROW(
        dev->projectProgram(makeImplicitImageProgram()),
        std::runtime_error);
}

TEST_F(FpgaDeviceFixture,
       PdiSubmissionTimeoutPoisonsAndQuarantinesDevice) {
    rp1_.reset();
    primeAsReady(ddr_);
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeImageRuntimeSpec(),
        "imageA");
    std::weak_ptr<FpgaDevice> quarantinedDevice = dev;
    dev->setWaitTimeout(std::chrono::milliseconds(20));

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1ReprogramCommand load;
    load.id = "load_b";
    load.deviceId = "fpga:0";
    load.imageId = "imageB";
    program.commands.emplace_back(load);

    auto plan = dev->compileProgram(program);
    plan->launch();
    EXPECT_THROW(plan->wait(), fpga::Rp1TimeoutError);
    EXPECT_TRUE(dev->executionPoisoned());
    EXPECT_TRUE(dev->submitter()->poisoned());
    plan.reset();

    EXPECT_THROW(
        dev->compileProgram(program),
        std::runtime_error);
    EXPECT_THROW(
        dev->leaseResources(
            {RendezvousId(0)}, {}),
        std::runtime_error);
    EXPECT_THROW(
        dev->projectProgram(makeImplicitImageProgram()),
        std::runtime_error);

    dev.reset();
    EXPECT_FALSE(quarantinedDevice.expired())
        << "poison quarantine must retain the device and its DMA/PDI pins";
}

TEST_F(FpgaDeviceFixture,
       ImageReconciliationIgnoresPdiWithoutCqEvidence) {
    const auto spec = makeImageRuntimeSpec();
    const std::uint32_t imageAId = 1u;
    rp1_.reset();
    rp1_ = std::make_unique<FakeRp1>(
        ddr_, true,
        [imageAId](
            std::uint32_t, const rp1_node_t& node)
            -> std::optional<
                std::pair<std::uint32_t, std::uint32_t>> {
            if (node.opcode == RP1_OP_PDI_LOAD &&
                node.payload.pdi_load.image_id == imageAId) {
                return std::nullopt;
            }
            return std::make_pair(
                static_cast<std::uint32_t>(RP1_CQ_OK), 0u);
        });
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, spec, std::string{});

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1ReprogramCommand loadB;
    loadB.id = "load_b";
    loadB.deviceId = "fpga:0";
    loadB.imageId = "imageB";
    program.commands.emplace_back(loadB);
    Rp1ReprogramCommand skippedA;
    skippedA.id = "skipped_a";
    skippedA.deviceId = "fpga:0";
    skippedA.imageId = "imageA";
    skippedA.dependsOn = {loadB.id};
    program.commands.emplace_back(skippedA);

    auto plan = dev->compileProgram(program);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());
    plan.reset();

    const fpga::Rp1GraphImage projected =
        dev->projectProgram(makeImplicitImageProgram());
    ASSERT_FALSE(projected.nodes.empty());
    EXPECT_EQ(
        projected.nodes.front()
            .payload.kernel_dispatch.kernel_base_addr,
        kKernelB_R5);
}

TEST_F(FpgaDeviceFixture,
       ExplicitKernelMetadataDoesNotAssumeActiveImage) {
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec imageA;
    imageA.id = "imageA";
    imageA.pdiPath = "a.pdi";
    fpga::FpgaKernelSpec kernelA;
    kernelA.name = "kA";
    kernelA.r5_base_addr = kKernelA_R5;
    imageA.kernels.emplace("kA", kernelA);
    spec->addImage(std::move(imageA));

    fpga::FpgaImageSpec imageB;
    imageB.id = "imageB";
    imageB.pdiPath = "b.pdi";
    fpga::FpgaKernelSpec kernelB;
    kernelB.name = "kA";
    kernelB.r5_base_addr = kKernelB_R5;
    imageB.kernels.emplace("kA", kernelB);
    spec->addImage(std::move(imageB));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1KernelCommand k;
    k.id = "kA";
    k.deviceId = "fpga:0";
    k.kernel = fpgaKernel("kA");
    k.kernel.image = "imageB";
    dg.commands.emplace_back(k);

    fpga::Rp1GraphImage image;
    ASSERT_NO_THROW(image = dev->projectProgram(dg));
    ASSERT_FALSE(image.nodes.empty());
    EXPECT_EQ(
        image.nodes.front()
            .payload.kernel_dispatch.kernel_base_addr,
        kKernelB_R5);
    EXPECT_EQ(
        image.nodes.front()
            .payload.kernel_dispatch.expected_image_id,
        dev->imageNumericId("imageB"));
}

TEST_F(FpgaDeviceFixture,
       ExplicitReprogramGraphRecompilesAfterActiveImageChanges) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeImageRuntimeSpec(),
        "imageA");
    auto makeProgram = [](const std::string& imageId) {
        Rp1QueueProgram program;
        program.device = DeviceId("fpga:0");

        Rp1ReprogramCommand load;
        load.id = "load_" + imageId;
        load.deviceId = "fpga:0";
        load.imageId = imageId;
        program.commands.emplace_back(load);

        Rp1KernelCommand kernel;
        kernel.id = "run_" + imageId;
        kernel.deviceId = "fpga:0";
        kernel.kernel = fpgaKernel("implicit");
        kernel.kernel.image = imageId;
        kernel.dependsOn = {load.id};
        program.commands.emplace_back(kernel);
        return program;
    };

    auto loadB = dev->compileProgram(
        makeProgram("imageB"));
    ASSERT_NO_THROW(loadB->launch());
    ASSERT_NO_THROW(loadB->wait());
    loadB.reset();

    const fpga::Rp1GraphImage activeB =
        dev->projectProgram(makeImplicitImageProgram());
    ASSERT_FALSE(activeB.nodes.empty());
    EXPECT_EQ(
        activeB.nodes.front()
            .payload.kernel_dispatch.kernel_base_addr,
        kKernelB_R5);

    std::unique_ptr<IBackendExecutable> loadA;
    ASSERT_NO_THROW(
        loadA = dev->compileProgram(
            makeProgram("imageA")));
    ASSERT_NO_THROW(loadA->launch());
    ASSERT_NO_THROW(loadA->wait());
    loadA.reset();

    const fpga::Rp1GraphImage activeA =
        dev->projectProgram(makeImplicitImageProgram());
    ASSERT_FALSE(activeA.nodes.empty());
    EXPECT_EQ(
        activeA.nodes.front()
            .payload.kernel_dispatch.kernel_base_addr,
        kKernelA_R5);
}

// ---------------------------------------------------------------------------
// Diamond happy-path
// ---------------------------------------------------------------------------

TEST_F(FpgaDeviceFixture, DiamondGraphCompletesAndSentinelFires) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);

    detail::PortBindings ioA, ioB, ioC, ioD;
    std::string a = detail::GraphTestAccess::addNode(g, fpgaKernel("kA"), std::move(ioA), "fpga:0");
    std::string b = detail::GraphTestAccess::addNode(g, fpgaKernel("kB"), std::move(ioB), "fpga:0", {a});
    std::string c = detail::GraphTestAccess::addNode(g, fpgaKernel("kC"), std::move(ioC), "fpga:0", {a});
    detail::GraphTestAccess::addNode(g, fpgaKernel("kD"), std::move(ioD), "fpga:0", {b, c});

    ASSERT_NO_THROW(g.compile().run());

    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);
    // 4 kernels + 1 sentinel signal = 5 CQ entries.
    EXPECT_EQ(ddr_.ctrl().cq_write_idx, 5u);
}

TEST_F(FpgaDeviceFixture, MissingLifecycleSentinelRejectsCompletion) {
    rp1_.reset();
    primeAsReady(ddr_);
    rp1_ = std::make_unique<FakeRp1>(
        ddr_, /*writeSignals=*/false);

    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);

    detail::PortBindings io;
    detail::GraphTestAccess::addNode(
        g, fpgaKernel("kA"), std::move(io), "fpga:0");

    EXPECT_THROW(g.compile().run(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, DiamondBarrierMasksAreCorrect) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    detail::PortBindings ioA, ioB, ioC, ioD;
    std::string a = detail::GraphTestAccess::addNode(g, fpgaKernel("kA"), std::move(ioA), "fpga:0");
    std::string b = detail::GraphTestAccess::addNode(g, fpgaKernel("kB"), std::move(ioB), "fpga:0", {a});
    std::string c_id = detail::GraphTestAccess::addNode(g, fpgaKernel("kC"), std::move(ioC), "fpga:0", {a});
    detail::GraphTestAccess::addNode(g, fpgaKernel("kD"), std::move(ioD), "fpga:0", {b, c_id});

    auto exec = g.compile();
    exec.launch();  // submits the graph
    exec.wait();    // joins; firmware has by now processed the nodes

    // Inspect the node array we wrote to DDR.
    const rp1_node_t* n = ddr_.nodes();

    // The compiler may reorder topologically; locate nodes by R5 addr.
    auto find = [&](std::uint32_t r5) -> const rp1_node_t* {
        for (std::size_t i = 0; i < 4; ++i) {
            if (n[i].opcode == RP1_OP_KERNEL_DISPATCH &&
                n[i].payload.kernel_dispatch.kernel_base_addr == r5) {
                return &n[i];
            }
        }
        return nullptr;
    };
    const rp1_node_t* na = find(kKernelA_R5);
    const rp1_node_t* nb = find(kKernelB_R5);
    const rp1_node_t* nc = find(kKernelC_R5);
    const rp1_node_t* nd = find(kKernelD_R5);
    ASSERT_NE(na, nullptr);
    ASSERT_NE(nb, nullptr);
    ASSERT_NE(nc, nullptr);
    ASSERT_NE(nd, nullptr);

    EXPECT_NE(na->flags & RP1_FLAG_HALT_ON_ERROR, 0u);
    EXPECT_NE(nb->flags & RP1_FLAG_HALT_ON_ERROR, 0u);
    EXPECT_NE(nc->flags & RP1_FLAG_HALT_ON_ERROR, 0u);
    EXPECT_NE(nd->flags & RP1_FLAG_HALT_ON_ERROR, 0u);
    EXPECT_EQ(na->barrier_await_mask, 0u);
    EXPECT_EQ(nb->barrier_await_mask, na->barrier_set_mask);
    EXPECT_EQ(nc->barrier_await_mask, na->barrier_set_mask);
    EXPECT_EQ(nd->barrier_await_mask, nb->barrier_set_mask | nc->barrier_set_mask);

    // Sentinel awaits only D (the unique leaf).
    const rp1_node_t& sentinel = n[4];
    EXPECT_EQ(sentinel.opcode, RP1_OP_SIGNAL);
    EXPECT_EQ(sentinel.barrier_await_mask, nd->barrier_set_mask);
    EXPECT_EQ(sentinel.payload.signal.value, kDefaultSentinelValue);
    EXPECT_EQ(sentinel.payload.signal.target_slot, kDefaultSentinelSlot);
}

TEST_F(FpgaDeviceFixture, ScalarArgsAreConstantsBakedAtCompileTime) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"size", ScalarType::U32});
    iot.inputScalars.push_back({"flags", ScalarType::U8});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    GraphScalar size = detail::GraphTestAccess::scalarInput<std::uint32_t>(g, "size");
    GraphScalar flags = detail::GraphTestAccess::scalarInput<std::uint8_t>(g, "flags");

    detail::PortBindings io;
    io.bindInputScalar("size", size);
    io.bindInputScalar("flags", flags);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    detail::ExecutionTestAccess::writeScalar(exec, size, 123u);
    detail::ExecutionTestAccess::writeScalar(exec, flags, std::uint8_t{7});
    exec.launch();
    exec.wait();

    // Protocol v2: each arg is a (reg_offset, value) pair.  On the mock
    // lookup path offsets are handed out contiguously from 0x10, so:
    //   size=123 @ 0x10, flags=7 (zero-extended) @ 0x14.
    EXPECT_EQ(ddr_.args()[0], 0x10u);
    EXPECT_EQ(ddr_.args()[1], 123u);
    EXPECT_EQ(ddr_.args()[2], 0x14u);
    EXPECT_EQ(ddr_.args()[3], 7u);
    const rp1_node_t* dispatch = findDispatch(ddr_, kKernelA_R5);
    ASSERT_NE(dispatch, nullptr);
    EXPECT_EQ(dispatch->payload.kernel_dispatch.arg_count, 2u);
}

TEST_F(FpgaDeviceFixture, U64ScalarArgsConsumeTwoArgWords) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"addr", ScalarType::U64});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    GraphScalar addr = detail::GraphTestAccess::scalarInput<std::uint64_t>(g, "addr");

    detail::PortBindings io;
    io.bindInputScalar("addr", addr);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    detail::ExecutionTestAccess::writeScalar(exec, addr, static_cast<std::uint64_t>(0xDEAD'BEEF'CAFE'BABEull));
    exec.launch();
    exec.wait();

    // A U64 occupies two registers (0x10, 0x14), emitted as two v2 pairs.
    EXPECT_EQ(ddr_.args()[0], 0x10u);
    EXPECT_EQ(ddr_.args()[1], 0xCAFEBABEu);
    EXPECT_EQ(ddr_.args()[2], 0x14u);
    EXPECT_EQ(ddr_.args()[3], 0xDEADBEEFu);
    const rp1_node_t* dispatch = findDispatch(ddr_, kKernelA_R5);
    ASSERT_NE(dispatch, nullptr);
    EXPECT_EQ(dispatch->payload.kernel_dispatch.arg_count, 2u);
}

TEST_F(FpgaDeviceFixture, GlobalScalarOnFpgaKernelUsesDeferredLaunchValue) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"size", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);

    GraphScalar var = detail::GraphTestAccess::scalar(g, ScalarType::U32, "size");

    detail::PortBindings io;
    io.bindInputScalar("size", var);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    detail::ExecutionTestAccess::writeScalar(exec, var, 0x1234u);
    exec.launch();
    exec.wait();
    EXPECT_EQ(ddr_.args()[1], 0x1234u);
}

TEST_F(FpgaDeviceFixture, DeferredScalarsResolvedAtLaunch) {
    // Build a Rp1QueueProgram by hand to bypass the compiler's "globals only on
    // CPU kernels" restriction.  Verifies that FpgaDevice's deferred
    // scalar code patches arg_buf right before submission.
    auto scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    (*scalarValues)["scope:0:size"] = 0xAAAAu;

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = scalarValues;

    Rp1KernelCommand k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA");
    k.kernel.ioType.inputScalars.push_back({"size", ScalarType::U32});
    k.ioMap.bindInputScalar("size", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "size", 0));
    dg.commands.push_back(k);

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    auto plan = dev->compileProgram(dg);

    // Protocol v2: arg[0] is the reg_offset (0x10), arg[1] the patched value.
    plan->launch();
    plan->wait();
    EXPECT_EQ(ddr_.args()[0], 0x10u);
    EXPECT_EQ(ddr_.args()[1], 0xAAAAu);

    (*scalarValues)["scope:0:size"] = 0xBBBBu;
    plan->launch();
    plan->wait();
    EXPECT_EQ(ddr_.args()[1], 0xBBBBu);
}

TEST_F(FpgaDeviceFixture, ArgBufferIsContiguousAcrossMultipleKernels) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"s0", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);

    std::vector<std::pair<GraphScalar, std::uint32_t>> scalarValues;
    auto bind = [&](std::string name, std::uint32_t v) {
        detail::PortBindings io;
        GraphScalar scalar = detail::GraphTestAccess::scalarInput<std::uint32_t>(g, std::move(name));
        scalarValues.emplace_back(scalar, v);
        io.bindInputScalar("s0", scalar);
        return io;
    };
    std::string a = detail::GraphTestAccess::addNode(g, fpgaKernel("kA", iot), bind("s0_a", 0x11), "fpga:0");
    std::string b = detail::GraphTestAccess::addNode(g, fpgaKernel("kB", iot), bind("s0_b", 0x22), "fpga:0", {a});
    detail::GraphTestAccess::addNode(g, fpgaKernel("kC", iot), bind("s0_c", 0x33), "fpga:0", {b});

    auto exec = g.compile();
    for (const auto& [scalar, value] : scalarValues) {
        detail::ExecutionTestAccess::writeScalar(exec, scalar, value);
    }
    exec.launch();
    exec.wait();

    // Each kernel contributes one (reg_offset=0x10, value) pair = two words.
    EXPECT_EQ(ddr_.args()[1], 0x11u);
    EXPECT_EQ(ddr_.args()[3], 0x22u);
    EXPECT_EQ(ddr_.args()[5], 0x33u);

    // Each kernel's arg_buffer_offset must point at its own (2-word) slot.
    auto findOffsetFor = [&](std::uint32_t r5) -> std::uint32_t {
        const rp1_node_t* node = findDispatch(ddr_, r5);
        return node ? node->payload.kernel_dispatch.arg_buffer_offset
                    : UINT32_MAX;
    };
    EXPECT_EQ(findOffsetFor(kKernelA_R5), 0u * sizeof(std::uint32_t));
    EXPECT_EQ(findOffsetFor(kKernelB_R5), 2u * sizeof(std::uint32_t));
    EXPECT_EQ(findOffsetFor(kKernelC_R5), 4u * sizeof(std::uint32_t));
}

TEST_F(FpgaDeviceFixture, NonContiguousSystemMapOffsetsAreHonored) {
    // Regression guard for the rp1_graph_vbin_full bug: the HLS s_axilite map
    // for graph_kernel(ap_uint<64> n, const int* in, int* out) places args at
    // non-contiguous offsets with reserved gaps -- n@0x10, in@0x1c, out@0x28.
    // The protocol-v2 packer must emit (reg_offset, value) pairs that land
    // each argument at its own register, not dense from 0x10.
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec image;
    image.id = "imageA";
    image.pdiPath = "a.pdi";

    fpga::FpgaKernelSpec k;
    k.name = "graph_kernel";
    k.r5_base_addr = kKernelA_R5;
    k.ioType.inputScalars.push_back({"n", ScalarType::U64});
    k.ioType.inputs.push_back({"in", BufferType::I32});
    k.ioType.outputs.push_back({"out", BufferType::I32});
    k.args.push_back({0u, "n",   "ap_uint<64>", 0x10u, 64u, false, false, ""});
    k.args.push_back({1u, "in",  "int*",        0x1cu, 64u, false, false, ""});
    k.args.push_back({2u, "out", "int*",        0x28u, 64u, true,  false, ""});
    image.kernels.emplace("graph_kernel", k);
    spec->addImage(std::move(image));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    GraphScalar elements = ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "elements");
    (*dg.scalarValues)[scopedScalarKey(elements.scopeId(), elements.varName())] = 4;
    (*dg.scalarValues)[scopedScalarKey(0, "n")] = 0x1122'3344'5566'7788ull;

    Rp1KernelCommand node;
    node.id       = "k0";
    node.deviceId = "fpga:0";
    node.kernel   = KernelDescriptor{"graph_kernel", DeviceType::FPGA,
                                     std::string("imageA"), k.ioType};
    GraphBuffer outTok;
    node.ioMap
        .bindInputScalar("n", ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "n"))
        .bindInput("in", ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "inBuf", 0, elements))
        .bindOutput("out", BufferType::I32, outTok);
    dg.commands.push_back(std::move(node));

    auto plan = dev->compileProgram(dg);
    plan->launch();
    plan->wait();

    const auto& kd = ddr_.nodes()[0].payload.kernel_dispatch;
    EXPECT_EQ(ddr_.nodes()[0].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(kd.kernel_base_addr, kKernelA_R5);
    // n(2) + in(2) + out(2) = 6 (reg_offset, value) pairs.
    EXPECT_EQ(kd.arg_count, 6u);

    const std::uint32_t* a =
        ddr_.args() + (kd.arg_buffer_offset / sizeof(std::uint32_t));
    // n @ 0x10/0x14 == 0x1122334455667788 (little-endian words).
    EXPECT_EQ(a[0], 0x10u);
    EXPECT_EQ(a[1], 0x55667788u);
    EXPECT_EQ(a[2], 0x14u);
    EXPECT_EQ(a[3], 0x11223344u);
    // in @ 0x1c/0x20.
    EXPECT_EQ(a[4], 0x1cu);
    EXPECT_EQ(a[6], 0x20u);
    // out @ 0x28/0x2c -- the register that was previously never written.
    EXPECT_EQ(a[8], 0x28u);
    EXPECT_EQ(a[10], 0x2cu);

    const std::uint64_t inAddr =
        static_cast<std::uint64_t>(a[5]) | (static_cast<std::uint64_t>(a[7]) << 32);
    const std::uint64_t outAddr =
        static_cast<std::uint64_t>(a[9]) | (static_cast<std::uint64_t>(a[11]) << 32);
    EXPECT_GE(inAddr, static_cast<std::uint64_t>(RP1_CTRL_PHYS_ADDR));
    EXPECT_GE(outAddr, static_cast<std::uint64_t>(RP1_CTRL_PHYS_ADDR));
}

TEST_F(FpgaDeviceFixture, RenamedDescriptorPortsResolveToSystemMapArgs) {
    // Regression guard for the hardware finding: examples rename FPGA kernel
    // ports (e.g. the HLS args "in_r"/"out_r" become graph ports "in"/
    // "image_out" via fpgaVectorIo/refinedGraphKernel).  The packer must still
    // resolve each port's register offset against the real system_map arg name.
    //
    // Crucially this mirrors the real hardware system_map: every HLS m_axi
    // pointer register is write-only (r=0, w=1, the *host* writes the pointer),
    // so ioTypeMapFromFunctionalArgs lumps BOTH buffer args into inputs
    // regardless of data-flow direction.  The descriptor, by contrast, splits
    // them into input/output by intent.  Mapping must therefore be by
    // scalar-vs-buffer position over the idx-ordered args, not by per-category
    // correspondence (which would leave the renamed output port unmapped).
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec image;
    image.id = "imageA";
    image.pdiPath = "a.pdi";

    fpga::FpgaKernelSpec k;
    k.name = "graph_kernel_0";
    k.r5_base_addr = kKernelA_R5;
    // Canonical IOTypeMap as ioTypeMapFromFunctionalArgs would build it from the
    // real flags: both pointer args land in inputs (write-only registers).
    k.ioType.inputScalars.push_back({"n", ScalarType::U64});
    k.ioType.inputs.push_back({"in_r", BufferType::I32});
    k.ioType.inputs.push_back({"out_r", BufferType::I32});
    k.args.push_back({0u, "n",     "ap_uint<64>", 0x10u, 64u, false, false, ""});
    k.args.push_back({1u, "in_r",  "int*",        0x1cu, 64u, false, true,  "m_axi_gmem0"});
    k.args.push_back({2u, "out_r", "int*",        0x28u, 64u, false, true,  "m_axi_gmem1"});
    image.kernels.emplace("graph_kernel_0", k);
    spec->addImage(std::move(image));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    GraphScalar elements = ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "elements");
    (*dg.scalarValues)[scopedScalarKey(elements.scopeId(), elements.varName())] = 4;
    (*dg.scalarValues)[scopedScalarKey(0, "n")] = 0x1122'3344'5566'7788ull;

    // Descriptor renames the buffer ports, as the real example does.
    IOTypeMap renamed;
    renamed.inputScalars.push_back({"n", ScalarType::U64});
    renamed.inputs.push_back({"in", BufferType::I32});
    renamed.outputs.push_back({"image_out", BufferType::I32});

    Rp1KernelCommand node;
    node.id       = "k0";
    node.deviceId = "fpga:0";
    node.kernel   = KernelDescriptor{"graph_kernel_0", DeviceType::FPGA,
                                     std::string("imageA"), renamed};
    GraphBuffer outTok;
    node.ioMap
        .bindInputScalar("n", ::vrt::graph::detail::makeGraphScalar(ScalarType::U64, "n"))
        .bindInput("in", ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "inBuf", 0, elements))
        .bindOutput("image_out", BufferType::I32, outTok);
    dg.commands.push_back(std::move(node));

    auto plan = dev->compileProgram(dg);
    plan->launch();
    plan->wait();

    const auto& kd = ddr_.nodes()[0].payload.kernel_dispatch;
    EXPECT_EQ(kd.arg_count, 6u);
    const std::uint32_t* a =
        ddr_.args() + (kd.arg_buffer_offset / sizeof(std::uint32_t));
    // n -> arg "n" @ 0x10/0x14.
    EXPECT_EQ(a[0], 0x10u);
    EXPECT_EQ(a[2], 0x14u);
    // "in" -> arg "in_r" @ 0x1c/0x20.
    EXPECT_EQ(a[4], 0x1cu);
    EXPECT_EQ(a[6], 0x20u);
    // "image_out" -> arg "out_r" @ 0x28/0x2c.
    EXPECT_EQ(a[8], 0x28u);
    EXPECT_EQ(a[10], 0x2cu);
}

TEST_F(FpgaDeviceFixture, ConflictingBufferMemoryRegionsAreRejected) {
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec image;
    image.id = "imageA";

    auto addKernel = [&](const std::string& name, std::uint32_t base,
                         std::uint8_t hbmPort) {
        fpga::FpgaKernelSpec kernel;
        kernel.name = name;
        kernel.r5_base_addr = base;
        kernel.ioType.inputs.push_back({"in", BufferType::I32});
        kernel.args.push_back(
            {0u, "in", "int*", 0x10u, 64u, false, true, "m_axi_gmem0"});
        kernel.argMemory["in"] =
            ::vrt::MemoryConfig{::vrt::MemoryRangeType::HBM, hbmPort};
        image.kernels.emplace(name, std::move(kernel));
    };
    addKernel("kA", kKernelA_R5, 0u);
    addKernel("kB", kKernelB_R5, 1u);
    spec->addImage(std::move(image));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    const GraphBuffer shared = ::vrt::graph::detail::makeGraphBuffer(BufferType::I32, "shared", 0);

    for (const std::string& name : {"kA", "kB"}) {
        Rp1KernelCommand node;
        node.id = name;
        node.deviceId = "fpga:0";
        IOTypeMap ioType;
        ioType.inputs.push_back({"in", BufferType::I32});
        node.kernel =
            KernelDescriptor{name, DeviceType::FPGA, std::string("imageA"), ioType};
        node.ioMap.bindInput("in", shared);
        dg.commands.push_back(std::move(node));
    }

    EXPECT_THROW(dev->compileProgram(dg), std::logic_error);
}

TEST_F(FpgaDeviceFixture, LookupReturningZeroAddressIsRejected) {
    auto bad_lookup = [](const std::string&) {
        return FpgaKernelLocation{0u, 0u};
    };
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, bad_lookup);
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA"), detail::PortBindings{}, "fpga:0");

    EXPECT_THROW(g.compile(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, UnboundInputScalarIsRejected) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"missing", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA", iot), detail::PortBindings{}, "fpga:0");

    EXPECT_THROW(g.compile(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, OutputScalarPortsEmitScalarRead) {
    // FPGA output scalar ports are captured by a trailing SCALAR_READ so RP1
    // can feed the value into signal-slot based predicates.
    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1KernelCommand k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA", iot);
    dg.commands.push_back(k);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    EXPECT_EQ(ddr_.nodes()[0].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(ddr_.nodes()[1].opcode, RP1_OP_SCALAR_READ);
    EXPECT_EQ(ddr_.nodes()[1].payload.scalar_read.source_addr, kKernelA_R5 + 0x10u);
}

TEST_F(FpgaDeviceFixture, PointerTypedOutputScalarUsesSystemMapOffset) {
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec image;
    image.id = "imageA";

    fpga::FpgaKernelSpec kernel;
    kernel.name = "kA";
    kernel.r5_base_addr = kKernelA_R5;
    kernel.ioType.outputScalars.push_back({"level", ScalarType::I32});
    kernel.args.push_back(
        {0u, "level", "ap_int<32>*", 0x24u, 32u, true, false, ""});
    image.kernels.emplace(kernel.name, kernel);
    spec->addImage(std::move(image));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1KernelCommand producer;
    producer.id = "producer";
    producer.deviceId = "fpga:0";
    producer.kernel = KernelDescriptor{
        "kA", DeviceType::FPGA, std::string("imageA"), kernel.ioType};
    producer.ioMap.bindOutputScalar(
        "level", ::vrt::graph::detail::makeGraphScalar(ScalarType::I32, "level"));
    dg.commands.push_back(producer);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    ASSERT_EQ(ddr_.nodes()[1].opcode, RP1_OP_SCALAR_READ);
    EXPECT_EQ(ddr_.nodes()[1].payload.scalar_read.source_addr,
              kKernelA_R5 + 0x24u);
}

TEST_F(FpgaDeviceFixture, MissingOutputScalarOffsetIsRejectedWithVbinSpec) {
    auto spec = std::make_shared<fpga::FpgaVbinSpec>();
    fpga::FpgaImageSpec image;
    image.id = "imageA";

    fpga::FpgaKernelSpec kernel;
    kernel.name = "kA";
    kernel.r5_base_addr = kKernelA_R5;
    kernel.ioType.outputScalars.push_back({"level", ScalarType::I32});
    image.kernels.emplace(kernel.name, kernel);
    spec->addImage(std::move(image));

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, spec, "imageA");

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1KernelCommand producer;
    producer.id = "producer";
    producer.deviceId = "fpga:0";
    producer.kernel = KernelDescriptor{
        "kA", DeviceType::FPGA, std::string("imageA"), kernel.ioType};
    producer.ioMap.bindOutputScalar(
        "level", ::vrt::graph::detail::makeGraphScalar(ScalarType::I32, "level"));
    dg.commands.push_back(producer);

    EXPECT_THROW(dev->compileProgram(dg), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, OutputScalarFeedsDownstreamKernelViaScalarCopy) {
    IOTypeMap producerType;
    producerType.outputScalars.push_back({"level", ScalarType::I32});
    IOTypeMap consumerType;
    consumerType.inputScalars.push_back({"level", ScalarType::I32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    const GraphScalar level = ::vrt::graph::detail::makeGraphScalar(ScalarType::I32, "level");

    Rp1KernelCommand producer;
    producer.id = "producer";
    producer.deviceId = "fpga:0";
    producer.kernel = fpgaKernel("kA", producerType);
    producer.ioMap.bindOutputScalar("level", level);
    dg.commands.push_back(producer);

    Rp1KernelCommand consumer;
    consumer.id = "consumer";
    consumer.deviceId = "fpga:0";
    consumer.kernel = fpgaKernel("kB", consumerType);
    consumer.ioMap.bindInputScalar("level", level);
    consumer.dependsOn = {"producer"};
    dg.commands.push_back(consumer);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    const rp1_node_t& scalarRead = ddr_.nodes()[1];
    const rp1_node_t& scalarCopy = ddr_.nodes()[2];
    const rp1_node_t& dispatch = ddr_.nodes()[3];

    ASSERT_EQ(scalarRead.opcode, RP1_OP_SCALAR_READ);
    ASSERT_EQ(scalarCopy.opcode, RP1_OP_SCALAR_COPY);
    EXPECT_EQ(scalarCopy.payload.scalar_copy.source_slot,
              scalarRead.payload.scalar_read.target_slot);
    EXPECT_EQ(scalarCopy.payload.scalar_copy.dest_addr, kKernelB_R5 + 0x10u);
    EXPECT_EQ(scalarCopy.barrier_await_bucket, scalarRead.barrier_set_bucket);
    EXPECT_NE(scalarCopy.barrier_await_mask & scalarRead.barrier_set_mask, 0u);

    ASSERT_EQ(dispatch.opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(dispatch.payload.kernel_dispatch.kernel_base_addr, kKernelB_R5);
    EXPECT_EQ(dispatch.payload.kernel_dispatch.arg_count, 0u);
    EXPECT_EQ(dispatch.barrier_await_bucket, scalarCopy.barrier_set_bucket);
    EXPECT_NE(dispatch.barrier_await_mask & scalarCopy.barrier_set_mask, 0u);
}

TEST_F(FpgaDeviceFixture, WideOutputScalarPortsAreRejected) {
    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U64});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1KernelCommand k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA", iot);
    dg.commands.push_back(k);

    EXPECT_THROW(dev->compileProgram(dg), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, DirectProgramRejectsSecondLiveExecutionAndReleasesLease) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1SignalCommand signal;
    signal.id = "lease_signal";
    signal.deviceId = "fpga:0";
    signal.slot = 0;
    signal.value = 1;
    signal.operation = RP1_SIGOP_SET;
    program.commands.emplace_back(signal);

    std::unique_ptr<IBackendExecutable> first =
        dev->compileProgram(program);
    ASSERT_NE(first, nullptr);
    EXPECT_THROW(dev->compileProgram(program), std::runtime_error);

    first.reset();
    std::unique_ptr<IBackendExecutable> afterRelease =
        dev->compileProgram(program);
    EXPECT_NE(afterRelease, nullptr);
}

TEST_F(FpgaDeviceFixture, DirectProgramPinsItsDeviceLifetime) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    std::weak_ptr<FpgaDevice> weakDevice = dev;

    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1SignalCommand signal;
    signal.id = "lifetime_signal";
    signal.deviceId = "fpga:0";
    signal.slot = 0;
    signal.value = 1;
    signal.operation = RP1_SIGOP_SET;
    program.commands.emplace_back(signal);

    std::unique_ptr<IBackendExecutable> plan =
        dev->compileProgram(program);
    dev.reset();
    EXPECT_FALSE(weakDevice.expired());
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    plan.reset();
    EXPECT_TRUE(weakDevice.expired());
}

TEST_F(FpgaDeviceFixture, ReferencedSignalSlotsAreReservedForPlanScalars) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");

    Rp1SignalCommand sg;
    sg.id = "reserved";
    sg.deviceId = "fpga:0";
    sg.slot = 0;
    sg.value = 7;
    sg.operation = RP1_SIGOP_SET;
    dg.commands.emplace_back(sg);

    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U32});
    Rp1KernelCommand k;
    k.id = "kA";
    k.deviceId = "fpga:0";
    k.kernel = fpgaKernel("kA", iot);
    dg.commands.emplace_back(k);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);

    plan->launch();
    plan->wait();
    EXPECT_EQ(ddr_.signals()[0].value, 7u);

    bool sawScalarRead = false;
    for (std::uint32_t i = 0; i < ddr_.ctrl().node_count; ++i) {
        if (ddr_.nodes()[i].opcode != RP1_OP_SCALAR_READ) continue;
        sawScalarRead = true;
        EXPECT_NE(ddr_.nodes()[i].payload.scalar_read.target_slot, 0u)
            << "plan-local scalar reads must not reuse a preassigned rendezvous slot";
    }
    EXPECT_TRUE(sawScalarRead);
}

TEST_F(FpgaDeviceFixture, SentinelSlotAndValueAreCustomisable) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    dev->setSentinelSlot(42);
    dev->setSentinelValue(0xC0FFEE00u);

    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    detail::GraphTestAccess::addNode(g, fpgaKernel("kA"), detail::PortBindings{}, "fpga:0");

    g.compile().run();

    EXPECT_EQ(ddr_.signals()[42].value, 0xC0FFEE00u);
}

TEST_F(FpgaDeviceFixture, ZeroSentinelValueIsRejected) {
    FpgaDevice dev("fpga:0", window_, makeDiamondLookup());
    EXPECT_THROW(dev.setSentinelValue(0u), std::invalid_argument);
}

TEST_F(FpgaDeviceFixture, SentinelCannotMoveOntoReservedSlot) {
    FpgaDevice dev("fpga:0", window_, makeDiamondLookup());
    auto lease = dev.leaseResources({RendezvousId(0)}, {});
    ASSERT_NE(lease, nullptr);
    const std::uint32_t occupied = static_cast<std::uint32_t>(
        lease->rendezvousResource(RendezvousId(0)).value());
    EXPECT_THROW(dev.setSentinelSlot(occupied), std::invalid_argument);
}

TEST_F(FpgaDeviceFixture, SentinelConfigurationLocksAfterLowering) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1SignalCommand signal;
    signal.id = "signal";
    signal.deviceId = "fpga:0";
    signal.slot = 0;
    signal.value = 1;
    signal.operation = RP1_SIGOP_SET;
    program.commands.emplace_back(signal);

    auto image = dev->projectProgram(program);
    EXPECT_FALSE(image.nodes.empty());
    EXPECT_THROW(dev->setSentinelSlot(42), std::logic_error);
    EXPECT_THROW(dev->setSentinelValue(0x12345678u), std::logic_error);
}

TEST_F(FpgaDeviceFixture, ExplicitSignalCannotCollideWithSentinel) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_, makeDiamondLookup());
    Rp1QueueProgram program;
    program.device = DeviceId("fpga:0");
    Rp1SignalCommand signal;
    signal.id = "collision";
    signal.deviceId = "fpga:0";
    signal.slot = kDefaultSentinelSlot;
    signal.value = 1;
    signal.operation = RP1_SIGOP_SET;
    program.commands.emplace_back(signal);

    EXPECT_THROW(dev->projectProgram(program), std::logic_error);
}

TEST_F(FpgaDeviceFixture, KernelLocationLookupIsCalledOncePerKernel) {
    int kAcalls = 0;
    int kBcalls = 0;
    auto counting = [&](const std::string& n) -> FpgaKernelLocation {
        if (n == "kA") { ++kAcalls; return {kKernelA_R5, 0}; }
        if (n == "kB") { ++kBcalls; return {kKernelB_R5, 0}; }
        throw std::runtime_error("unknown kernel '" + n + "'");
    };
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, counting);

    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    std::string a = detail::GraphTestAccess::addNode(g, fpgaKernel("kA"), detail::PortBindings{}, "fpga:0");
    detail::GraphTestAccess::addNode(g, fpgaKernel("kB"), detail::PortBindings{}, "fpga:0", {a});

    auto exec = g.compile();
    EXPECT_EQ(kAcalls, 1);
    EXPECT_EQ(kBcalls, 1);

    exec.run();
    EXPECT_EQ(kAcalls, 1) << "lookup should not be re-called at launch";
    EXPECT_EQ(kBcalls, 1);
}

TEST_F(FpgaDeviceFixture, ManyKernelsSpanMultipleBarrierBuckets) {
    // >31 kernels no longer trip a per-bucket cap: they are submitted as one
    // segment with barrier set-bits spread across multiple buckets.
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    const char* names[] = {"kA", "kB", "kC", "kD"};
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        const std::vector<std::string> after = prev.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{prev};
        prev = detail::GraphTestAccess::addNode(g, fpgaKernel(names[i % 4]), detail::PortBindings{}, "fpga:0", after);
    }
    ASSERT_NO_THROW(g.compile().run());
    // Whole 40-kernel chain went out as a single RP1 submission.
    EXPECT_EQ(ddr_.ctrl().graph_seq, 1u);
    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);
}

TEST_F(FpgaDeviceFixture, CrossBucketFanInInsertsJoinAggregator) {
    // A kernel that depends on predecessors in two different barrier buckets
    // forces a NOP join aggregator (a node can only await one bucket).
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    detail::GraphTestAccess::registerDevice(g, dev);
    const char* names[] = {"kA", "kB", "kC", "kD"};

    // k0..k30 form a chain occupying bucket 0 (bits 0..30); k31 lands in
    // bucket 1. The final kernel depends on both k0 (bucket 0) and k31
    // (bucket 1) -> cross-bucket fan-in.
    std::vector<std::string> ids;
    std::string prev;
    for (int i = 0; i < 32; ++i) {
        const std::vector<std::string> after = prev.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{prev};
        prev = detail::GraphTestAccess::addNode(g, fpgaKernel(names[i % 4]), detail::PortBindings{}, "fpga:0", after);
        ids.push_back(prev);
    }
    detail::GraphTestAccess::addNode(g, fpgaKernel("kD"), detail::PortBindings{}, "fpga:0",
              std::vector<std::string>{ids.front(), ids.back()});

    ASSERT_NO_THROW(g.compile().run());
    EXPECT_EQ(ddr_.ctrl().graph_seq, 1u);
    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);
}

// ---------------------------------------------------------------------------
// Autonomous control flow: fixed-count loop lowering
// ---------------------------------------------------------------------------

// A Rp1QueueProgram carrying a fixed-count Rp1LoopCommand with an all-FPGA body must
// lower to a single RP1 image: LOOP (max_iterations, body range, per-iteration
// bucket clear) + the flattened body kernel + a RERUN re-arming the LOOP +
// the trailing sentinel.  This validates the exact node layout the firmware's
// loop_decrement / loop_fixed_count QEMU tests prove it executes.
TEST_F(FpgaDeviceFixture, FixedCountLoopLowersToLoopRerunImage) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{0x88010000u, 0};
        });

    Rp1KernelCommand bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");  // no declared ports -> zero args

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 3);
    dg.commands.emplace_back(loop);

    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const rp1_node_t* n = ddr_.nodes();

    // node 0: LOOP, body range [1,3], 3 iterations, clears the body bucket.
    EXPECT_EQ(n[0].opcode, RP1_OP_LOOP);
    EXPECT_EQ(n[0].payload.loop.body_start, 1u);
    EXPECT_EQ(n[0].payload.loop.body_end, 3u);
    EXPECT_EQ(n[0].payload.loop.max_iterations, 3u);
    EXPECT_EQ(n[0].payload.loop.bucket_clear_start, 1u);
    EXPECT_EQ(n[0].payload.loop.bucket_clear_end, 1u);
    EXPECT_EQ(n[0].barrier_set_bucket, 0u);          // exit bit lives in bucket 0
    EXPECT_EQ(n[0].payload.loop.condition_op, RP1_COP_AND_NZ);  // never -> max_iter governs
    EXPECT_EQ(n[0].payload.loop.condition_value, 0u);

    // node 1: pre-test gate. Positive counts open the body; zero closes it.
    EXPECT_EQ(n[1].opcode, RP1_OP_COND);
    EXPECT_EQ(n[1].payload.cond.condition_op, RP1_COP_AND_Z);
    EXPECT_EQ(n[1].payload.cond.condition_value, 0u);

    // node 2: the body kernel, done-bit in the loop's body bucket (1).
    EXPECT_EQ(n[2].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(n[2].barrier_set_bucket, 1u);

    // node 3: RERUN re-arms the LOOP node (index 0), gated on the body bucket.
    EXPECT_EQ(n[3].opcode, RP1_OP_RERUN);
    EXPECT_EQ(n[3].payload.rerun.target_node, 0u);
    EXPECT_EQ(n[3].barrier_await_bucket, 1u);

    // node 4: sentinel SIGNAL gated on the loop's exit bit.
    EXPECT_EQ(n[4].opcode, RP1_OP_SIGNAL);
    EXPECT_EQ(n[4].barrier_set_mask, 1u << 31);
    EXPECT_EQ(n[4].barrier_await_mask, n[0].barrier_set_mask);
}

TEST_F(FpgaDeviceFixture, RerunTargetsNonzeroLoopPacket) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{0x88010000u, 0};
        });

    Rp1KernelCommand pre;
    pre.id = "pre";
    pre.deviceId = "fpga:0";
    pre.kernel = fpgaKernel("pre");

    Rp1KernelCommand bodyKernel;
    bodyKernel.id = "body";
    bodyKernel.deviceId = "fpga:0";
    bodyKernel.kernel = fpgaKernel("body");
    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.emplace_back(bodyKernel);
    body->scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram graph;
    graph.device = DeviceId("fpga:0");
    graph.scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();
    graph.commands.emplace_back(pre);
    Rp1LoopCommand loop;
    loop.id = "loop";
    loop.deviceId = "fpga:0";
    loop.dependsOn = {pre.id};
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(graph, 2);
    graph.commands.emplace_back(loop);
    graph.children.push_back(
        Rp1ChildProgram{
            loop.id, Rp1ChildRole::LoopBody, {body}});

    auto plan = dev->compileProgram(graph);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const rp1_node_t* nodes = ddr_.nodes();
    ASSERT_EQ(nodes[0].opcode, RP1_OP_KERNEL_DISPATCH);
    ASSERT_EQ(nodes[1].opcode, RP1_OP_LOOP);
    ASSERT_EQ(nodes[4].opcode, RP1_OP_RERUN);
    EXPECT_EQ(nodes[4].payload.rerun.target_node, 1u);
}

TEST_F(FpgaDeviceFixture, FixedCountLoopBodySpansMultipleBarrierBuckets) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{0x88010000u, 0};
        });

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        Rp1KernelCommand k;
        k.id = "bk" + std::to_string(i);
        k.deviceId = "fpga:0";
        k.kernel = fpgaKernel("bodyK");
        if (!prev.empty()) k.dependsOn = {prev};
        prev = k.id;
        body->commands.emplace_back(k);
    }

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    dg.commands.emplace_back(loop);

    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const rp1_node_t* n = ddr_.nodes();
    ASSERT_EQ(n[0].opcode, RP1_OP_LOOP);
    EXPECT_EQ(n[0].payload.loop.bucket_clear_start, 1u);
    EXPECT_GT(n[0].payload.loop.bucket_clear_end, n[0].payload.loop.bucket_clear_start)
        << "40 body kernels should occupy more than one reset-domain bucket";

    bool sawSecondBodyBucket = false;
    for (std::uint32_t i = n[0].payload.loop.body_start; i <= n[0].payload.loop.body_end; ++i) {
        if (n[i].opcode == RP1_OP_KERNEL_DISPATCH && n[i].barrier_set_bucket > 1u) {
            sawSecondBodyBucket = true;
        }
    }
    EXPECT_TRUE(sawSecondBodyBucket);
}

// End-to-end execution: a fixed-count loop must dispatch its body kernel
// exactly N times when run on a faithful host RP1 scanner (LOOP/RERUN +
// per-iteration body-bucket clear), proving the lowering iterates correctly
// rather than just emitting a structurally-plausible image.
TEST(FpgaControlExecution, FixedCountLoopExecutesNIterations) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    Rp1KernelCommand bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");
    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 5);
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), 5u);

    plan->launch();
    plan->wait();
    EXPECT_EQ(rp1.dispatches(kBodyBase), 10u);
}

TEST(FpgaControlExecution,
     FixedCountLoopWithReprogramExecutesNIterations) {
    constexpr std::uint32_t iterations = 4u;
    const auto pdiDirectory = makeTempDir("fpga-loop-reprogram-test");
    const std::string pdiPath =
        writeTempFile(pdiDirectory, "image.pdi", "fake-pdi-bytes");

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(
            backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    Rp1ReprogramCommand reprogram;
    reprogram.id = "load";
    reprogram.deviceId = "fpga:0";
    reprogram.imageId = "image";
    reprogram.pdiPath = pdiPath;

    Rp1KernelCommand kernel;
    kernel.id = "body";
    kernel.deviceId = "fpga:0";
    kernel.kernel = fpgaKernel("body");
    kernel.dependsOn = {reprogram.id};

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands = {reprogram, kernel};
    body->scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram graph;
    graph.device = DeviceId("fpga:0");
    graph.scalarValues =
        std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id = "loop";
    loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(graph, iterations);
    graph.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = loop.id;
    child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    graph.children.push_back(std::move(child));

    auto plan = dev->compileProgram(graph);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());
    EXPECT_EQ(rp1.dispatches(kBodyBase), iterations);

    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());
    EXPECT_EQ(rp1.dispatches(kBodyBase), 2u * iterations);

    std::filesystem::remove_all(pdiDirectory);
}

// Phase F.1: a data-dependent (while) FPGA loop terminates autonomously when a
// body output scalar's SCALAR_READ slot crosses the predicate threshold.  The
// body kernel produces a monotonically increasing "i"; the loop continues while
// i < N, so RP1 must exit (compare GE N) once the slot reaches N -- dispatching
// the body exactly N times with no host round-trip.
TEST(FpgaControlExecution, WhileLoopExitsOnBodyScalarPredicate) {
    constexpr std::uint32_t N = 6u;
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    // Body kernel produces output scalar "i" (the loop variable).
    IOTypeMap bodyType;
    bodyType.outputScalars.push_back({"i", ScalarType::U32});
    Rp1KernelCommand bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindOutputScalar("i", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "i"));

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::WhileCondition;
    loop.condition = Condition::compare(CompareOp::LT,
                                        ConditionOperand::scalar(ScalarType::U32, "i"),
                                        ConditionOperand::constant<std::uint32_t>(N));
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N)
        << "while-loop body should dispatch until the predicate slot reaches N";
}

// Phase F.1b: a data-dependent while-loop whose predicate reads a *parent*
// scalar produced by the body and exported each iteration (the loop-carried
// authoring shape).  The body kernel binds a local output scalar; an end
// boundary exports it to the parent scalar; the LOOP predicate reads the parent.
// The device lowering must alias the parent scalar to the body output's
// SCALAR_READ slot so the loop terminates on the freshly-produced value.
TEST(FpgaControlExecution, WhileLoopExitsOnExportedParentScalarPredicate) {
    constexpr std::uint32_t N = 4u;
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    // Body kernel produces local scalar "next" (scope 1).
    IOTypeMap bodyType;
    bodyType.outputScalars.push_back({"out", ScalarType::U32});
    Rp1KernelCommand bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindOutputScalar("out", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "next", 1));

    // End boundary exports local "next" (scope 1) to parent "counter" (scope 0).
    Rp1BoundaryCommand exportB;
    exportB.id       = "export";
    exportB.deviceId = "fpga:0";
    exportB.side     = Rp1BoundaryCommand::Side::End;
    exportB.scalarCopies.push_back({/*sourceName=*/"next", /*sourceScopeId=*/1,
                                    /*targetName=*/"counter", /*targetScopeId=*/0});

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(bodyK);
    body->commands.push_back(exportB);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "counter", 0),
        ConditionOperand::constant<std::uint32_t>(N));
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N)
        << "while-loop should iterate until the exported parent predicate reaches N";
}

// Phase F.1b Tier 2: a loop-carried scalar *input*.  The body kernel reads the
// carried value from an s_axilite register (fed each iteration by SCALAR_COPY
// from the carried slot) and writes the next value back (captured by SCALAR_READ
// into the same slot).  The LOOP predicate reads that slot; the carried value
// flows entirely on-FPGA across iterations via the new RP1_OP_SCALAR_COPY.
TEST(FpgaControlExecution, WhileLoopCarriesScalarInputViaScalarCopy) {
    constexpr std::uint32_t N = 4u;
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    // Body kernel: input scalar "in" (carried), output scalar "out" (carried).
    IOTypeMap bodyType;
    bodyType.inputScalars.push_back({"in", ScalarType::U32});
    bodyType.outputScalars.push_back({"out", ScalarType::U32});
    Rp1KernelCommand bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindInputScalar("in",  ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "lin",  1));
    bodyK.ioMap.bindOutputScalar("out", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "lout", 1));

    // Import counter(0) -> lin(1) (Start); export lout(1) -> counter(0) (End).
    Rp1BoundaryCommand importB;
    importB.id = "import"; importB.deviceId = "fpga:0";
    importB.side = Rp1BoundaryCommand::Side::Start;
    importB.scalarCopies.push_back({/*src*/"counter", 0, /*tgt*/"lin", 1});
    Rp1BoundaryCommand exportB;
    exportB.id = "export"; exportB.deviceId = "fpga:0";
    exportB.side = Rp1BoundaryCommand::Side::End;
    exportB.scalarCopies.push_back({/*src*/"lout", 1, /*tgt*/"counter", 0});

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(importB);
    body->commands.push_back(bodyK);
    body->commands.push_back(exportB);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "counter", 0),
        ConditionOperand::constant<std::uint32_t>(N));
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N)
        << "carried-scalar while-loop should iterate until the predicate reaches N";
    // The carried value was fed into the kernel's input register each iteration;
    // the final SCALAR_COPY carried N-1 (the value the last iteration consumed).
    EXPECT_EQ(rp1.scalarCopyTo(kBodyBase + 0x10u), N - 1u)
        << "carried scalar should have been copied into the kernel input register";
}

TEST(FpgaControlExecution, ScalarCopyUsesContiguousOffsetsForMultipleCarriedInputs) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    IOTypeMap bodyType;
    bodyType.inputScalars.push_back({"a", ScalarType::U32});
    bodyType.inputScalars.push_back({"b", ScalarType::U32});
    Rp1KernelCommand bodyK;
    bodyK.id = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindInputScalar("a", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "la", 1));
    bodyK.ioMap.bindInputScalar("b", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "lb", 1));

    Rp1BoundaryCommand importB;
    importB.id = "import";
    importB.deviceId = "fpga:0";
    importB.side = Rp1BoundaryCommand::Side::Start;
    importB.scalarCopies.push_back({/*src*/"pa", 0, /*tgt*/"la", 1});
    importB.scalarCopies.push_back({/*src*/"pb", 0, /*tgt*/"lb", 1});

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(importB);
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 1);
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_NE(rp1.scalarCopyTo(kBodyBase + 0x10u), 0xFFFFFFFFu);
    EXPECT_NE(rp1.scalarCopyTo(kBodyBase + 0x14u), 0xFFFFFFFFu)
        << "the second carried input scalar must copy into the second AXI-Lite register";
}

// Phase F.2: an autonomous FPGA conditional gates exactly one branch via
// RP1_OP_COND.  A main-line producer's output scalar (captured to a slot via
// SCALAR_READ; FaithfulRp1 models it as 1) drives the predicate; the COND-gated
// then/else lowering must dispatch only the taken branch and OR-join so the
// graph completes either way.  Predicate "p >= K": K=1 takes then, K=2 takes
// else (since the modelled p == 1).
TEST(FpgaControlExecution, ConditionalGatesExactlyOneBranch) {
    constexpr std::uint32_t kPredBase = 0x88010000u;
    constexpr std::uint32_t kThenBase = 0x88020000u;
    constexpr std::uint32_t kElseBase = 0x88030000u;

    auto runWithThreshold = [&](std::uint32_t threshold,
                                std::uint32_t& thenDisp, std::uint32_t& elseDisp) {
        std::vector<std::byte> backing(kBarSize, std::byte{0});
        DdrView ddr{backing.data()};
        primeAsReady(ddr);
        auto window =
            std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
        FaithfulRp1 rp1(ddr);

        auto dev = std::make_shared<FpgaDevice>(
            "fpga:0", window,
            [](const std::string& name) -> FpgaKernelLocation {
                if (name == "pred") return {kPredBase, 0};
                if (name == "thenK") return {kThenBase, 0};
                return {kElseBase, 0};
            });

        // Main-line predicate producer with an output scalar "p".
        IOTypeMap predType;
        predType.outputScalars.push_back({"p", ScalarType::U32});
        Rp1KernelCommand pred;
        pred.id = "pred"; pred.deviceId = "fpga:0"; pred.kernel = fpgaKernel("pred", predType);
        pred.ioMap.bindOutputScalar("p", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "p"));

        auto mk = [](const char* id, const char* name, std::uint32_t /*base*/) {
            Rp1KernelCommand k;
            k.id = id; k.deviceId = "fpga:0"; k.kernel = fpgaKernel(name);
            return k;
        };
        auto thenBody = std::make_shared<Rp1QueueProgram>();
        thenBody->device = DeviceId("fpga:0");
        thenBody->commands.push_back(mk("tk", "thenK", kThenBase));
        thenBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
        auto elseBody = std::make_shared<Rp1QueueProgram>();
        elseBody->device = DeviceId("fpga:0");
        elseBody->commands.push_back(mk("ek", "elseK", kElseBase));
        elseBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

        Rp1QueueProgram dg;
        dg.device = DeviceId("fpga:0");
        dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
        dg.commands.emplace_back(pred);
        Rp1ConditionalCommand cond;
        cond.id = "cond0"; cond.deviceId = "fpga:0"; cond.dependsOn = {"pred"};
        cond.condition = Condition::compare(CompareOp::GE,
                                            ConditionOperand::scalar(ScalarType::U32, "p"),
                                            ConditionOperand::constant<std::uint32_t>(threshold));
        dg.commands.emplace_back(cond);
        Rp1ChildProgram thenChild;
        thenChild.parentCommandId = "cond0"; thenChild.role = Rp1ChildRole::ConditionalThen;
        thenChild.programs.push_back(thenBody);
        dg.children.push_back(thenChild);
        Rp1ChildProgram elseChild;
        elseChild.parentCommandId = "cond0"; elseChild.role = Rp1ChildRole::ConditionalElse;
        elseChild.programs.push_back(elseBody);
        dg.children.push_back(elseChild);

        auto plan = dev->compileProgram(dg);
        ASSERT_NE(plan, nullptr);
        plan->launch();
        plan->wait();
        thenDisp = rp1.dispatches(kThenBase);
        elseDisp = rp1.dispatches(kElseBase);
    };

    std::uint32_t thenDisp = 0, elseDisp = 0;
    runWithThreshold(1u, thenDisp, elseDisp);   // p(=1) >= 1 true -> then
    EXPECT_EQ(thenDisp, 1u) << "then branch should run when predicate holds";
    EXPECT_EQ(elseDisp, 0u) << "else branch must be skipped when predicate holds";

    runWithThreshold(2u, thenDisp, elseDisp);   // p(=1) >= 2 false -> else
    EXPECT_EQ(thenDisp, 0u) << "then branch must be skipped when predicate fails";
    EXPECT_EQ(elseDisp, 1u) << "else branch should run when predicate fails";
}

TEST(FpgaControlExecution, ConditionalBranchSpansMultipleBarrierBuckets) {
    constexpr std::uint32_t kPredBase = 0x88010000u;
    constexpr std::uint32_t kThenBase = 0x88020000u;
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string& name) -> FpgaKernelLocation {
            if (name == "pred") return {kPredBase, 0};
            return {kThenBase, 0};
        });

    IOTypeMap predType;
    predType.outputScalars.push_back({"p", ScalarType::U32});
    Rp1KernelCommand pred;
    pred.id = "pred"; pred.deviceId = "fpga:0"; pred.kernel = fpgaKernel("pred", predType);
    pred.ioMap.bindOutputScalar("p", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "p"));

    auto thenBody = std::make_shared<Rp1QueueProgram>();
    thenBody->device = DeviceId("fpga:0");
    thenBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        Rp1KernelCommand k;
        k.id = "tk" + std::to_string(i);
        k.deviceId = "fpga:0";
        k.kernel = fpgaKernel("thenK");
        if (!prev.empty()) k.dependsOn = {prev};
        prev = k.id;
        thenBody->commands.emplace_back(k);
    }

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    dg.commands.emplace_back(pred);
    Rp1ConditionalCommand cond;
    cond.id = "cond0"; cond.deviceId = "fpga:0"; cond.dependsOn = {"pred"};
    cond.condition = Condition::compare(CompareOp::GE,
                                        ConditionOperand::scalar(ScalarType::U32, "p"),
                                        ConditionOperand::constant<std::uint32_t>(1));
    dg.commands.emplace_back(cond);
    Rp1ChildProgram thenChild;
    thenChild.parentCommandId = "cond0"; thenChild.role = Rp1ChildRole::ConditionalThen;
    thenChild.programs.push_back(thenBody);
    dg.children.push_back(thenChild);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kThenBase), 40u);
}

// The one-path FPGA executor accepts only RP1-executable nodes. Host bridge
// closures must be moved to the CPU Rp1QueueProgram by the compiler and represented on
// the FPGA side as SIGNAL/WAIT rendezvous nodes; a hand-built FPGA Rp1QueueProgram that
// still contains CompiledBridgeOpNode is invalid.
// Phase B: a kernel declaring an output scalar must, in a control image, get a
// trailing RP1_OP_SCALAR_READ that captures the kernel's output register into a
// signal slot (the value a downstream condition will evaluate).
TEST(FpgaControlExecution, OutputScalarEmitsScalarReadInControlImage) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);

    constexpr std::uint32_t kProducerBase = 0x88010000u;
    constexpr std::uint32_t kBodyBase     = 0x88020000u;
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string& name) -> FpgaKernelLocation {
            if (name == "producer") return {kProducerBase, 0};
            return {kBodyBase, 0};
        });

    // Main-line producer kernel with an output scalar.
    IOTypeMap producerType;
    producerType.outputScalars.push_back({"parity", ScalarType::U32});
    Rp1KernelCommand producer;
    producer.id       = "prod";
    producer.deviceId = "fpga:0";
    producer.kernel   = fpgaKernel("producer", producerType);
    producer.ioMap.bindOutputScalar("parity", ::vrt::graph::detail::makeGraphScalar(ScalarType::U32, "parity"));

    Rp1KernelCommand bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");
    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    dg.commands.emplace_back(producer);
    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    loop.dependsOn = {"prod"};
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0";
    child.role         = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const rp1_node_t* n = ddr.nodes();
    const std::uint32_t count = ddr.ctrl().node_count;
    bool foundRead = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (n[i].opcode == RP1_OP_SCALAR_READ &&
            n[i].payload.scalar_read.source_addr == kProducerBase + 0x10u) {
            foundRead = true;
        }
    }
    EXPECT_TRUE(foundRead) << "expected a SCALAR_READ of the producer's output register";
}

// Cross-queue rendezvous: two concurrently-running RP1 queues sharing a
// host-visible signal array.  The consumer queue's WAIT must hold its kernel
// until the producer queue raises the signal -- the SIGNAL/WAIT primitive the
// compiler cross-queue split will emit at device boundaries.
TEST(FpgaCrossQueue, SignalWaitRendezvousAcrossConcurrentQueues) {
    constexpr std::uint32_t kSlot     = 12u;
    constexpr std::uint32_t kToken    = 0xCAFEu;
    constexpr std::uint32_t kBaseA    = 0x88010000u;  // producer queue kernel
    constexpr std::uint32_t kBaseB    = 0x88020000u;  // consumer queue kernel

    std::vector<rp1_signal_slot_t> sharedSignals(RP1_MAX_SIGNALS);
    std::memset(sharedSignals.data(), 0, sharedSignals.size() * sizeof(rp1_signal_slot_t));

    std::vector<std::byte> backingA(kBarSize, std::byte{0});
    std::vector<std::byte> backingB(kBarSize, std::byte{0});
    DdrView ddrA{backingA.data()};
    DdrView ddrB{backingB.data()};
    primeAsReady(ddrA);
    primeAsReady(ddrB);

    auto mkNode = [](std::uint16_t opcode, std::uint8_t awB, std::uint32_t awM,
                     std::uint8_t stB, std::uint32_t stM) {
        rp1_node_t n{};
        n.opcode               = opcode;
        n.status               = RP1_NODE_PENDING;
        n.barrier_await_bucket = awB;
        n.barrier_await_mask   = awM;
        n.barrier_set_bucket   = stB;
        n.barrier_set_mask     = stM;
        return n;
    };
    auto submit = [](DdrView ddr, const std::vector<rp1_node_t>& ns) {
        auto& c = ddr.ctrl();
        c.cq_size    = 64u;
        c.node_count = static_cast<std::uint32_t>(ns.size());
        for (std::size_t i = 0; i < ns.size(); ++i) ddr.nodes()[i] = ns[i];
        std::atomic_thread_fence(std::memory_order_seq_cst);
        c.graph_seq = 1u;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    };
    auto waitDone = [](DdrView ddr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ddr.ctrl().graph_done_seq == ddr.ctrl().graph_seq) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };

    // Producer queue A: kernel -> SIGNAL slot = token.
    std::vector<rp1_node_t> queueA;
    queueA.push_back(mkNode(RP1_OP_KERNEL_DISPATCH, 0, 0x0, 0, 0x1));
    queueA.back().payload.kernel_dispatch.kernel_base_addr = kBaseA;
    queueA.push_back(mkNode(RP1_OP_SIGNAL, 0, 0x1, 0, 0x2));
    queueA.back().payload.signal.target_slot = kSlot;
    queueA.back().payload.signal.value       = kToken;
    queueA.back().payload.signal.operation   = RP1_SIGOP_SET;

    // Consumer queue B: WAIT slot == token -> kernel.
    std::vector<rp1_node_t> queueB;
    queueB.push_back(mkNode(RP1_OP_WAIT, 0, 0x0, 0, 0x1));
    queueB.back().payload.wait.condition_signal = kSlot;
    queueB.back().payload.wait.condition_op     = RP1_COP_EQ;
    queueB.back().payload.wait.condition_value  = kToken;
    queueB.push_back(mkNode(RP1_OP_KERNEL_DISPATCH, 0, 0x1, 0, 0x2));
    queueB.back().payload.kernel_dispatch.kernel_base_addr = kBaseB;

    FaithfulRp1 rpA(ddrA, sharedSignals.data());
    FaithfulRp1 rpB(ddrB, sharedSignals.data());

    // Start the consumer first; it must park on WAIT, not run its kernel.
    submit(ddrB, queueB);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(rpB.dispatches(kBaseB), 0u) << "consumer ran before producer signalled";
    EXPECT_NE(ddrB.ctrl().graph_done_seq, ddrB.ctrl().graph_seq)
        << "consumer queue completed while still gated on WAIT";

    // Now run the producer; its SIGNAL releases the consumer's WAIT.
    submit(ddrA, queueA);

    ASSERT_TRUE(waitDone(ddrA)) << "producer queue did not complete";
    ASSERT_TRUE(waitDone(ddrB)) << "consumer queue did not complete after rendezvous";
    EXPECT_EQ(rpA.dispatches(kBaseA), 1u);
    EXPECT_EQ(rpB.dispatches(kBaseB), 1u);
    EXPECT_EQ(sharedSignals[kSlot].value, kToken);
}

// Phase C: a full depth-1 per-iteration handshake between an FPGA LOOP queue
// and a host CPU queue over shared signal slots.  Each iteration the FPGA body
// raises READY, the CPU polls it, consumes, clears READY (ack) and raises DONE;
// the FPGA WAITs DONE, clears it, and proceeds.  Over N iterations the per-side
// counters must both equal N -- no lost or duplicated iterations.
TEST(FpgaCrossQueue, PerIterationHandshakeOverNIterations) {
    constexpr std::uint32_t N        = 5u;
    constexpr std::uint32_t kReady   = 20u;
    constexpr std::uint32_t kDone    = 21u;
    constexpr std::uint32_t kCountF  = 22u;  // FPGA-side iteration counter

    std::vector<rp1_signal_slot_t> sig(RP1_MAX_SIGNALS);
    std::memset(sig.data(), 0, sig.size() * sizeof(rp1_signal_slot_t));

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);

    auto node = [](std::uint16_t op, std::uint8_t awB, std::uint32_t awM,
                   std::uint8_t stB, std::uint32_t stM) {
        rp1_node_t n{};
        n.opcode = op;
        n.status = RP1_NODE_PENDING;
        n.barrier_await_bucket = awB;
        n.barrier_await_mask = awM;
        n.barrier_set_bucket = stB;
        n.barrier_set_mask = stM;
        return n;
    };

    // FPGA producer queue: LOOP body=[1..5] (count, raise READY, WAIT DONE,
    // clear DONE, RERUN), bucket 1 cleared each iteration.
    std::vector<rp1_node_t> q;
    {
        rp1_node_t loop = node(RP1_OP_LOOP, 0, 0x0, 0, 0x2);
        loop.payload.loop.body_start = 1;
        loop.payload.loop.body_end = 5;
        loop.payload.loop.max_iterations = N;
        loop.payload.loop.condition_op = RP1_COP_AND_NZ;  // never -> max_iter governs
        loop.payload.loop.condition_value = 0;
        loop.payload.loop.bucket_clear_start = 1;
        loop.payload.loop.bucket_clear_end = 1;
        loop.payload.loop.loop_id = 0;
        q.push_back(loop);

        rp1_node_t count = node(RP1_OP_SIGNAL, 1, 0x0, 1, 0x1);
        count.payload.signal.target_slot = kCountF;
        count.payload.signal.value = 1;
        count.payload.signal.operation = RP1_SIGOP_ADD;
        q.push_back(count);

        rp1_node_t ready = node(RP1_OP_SIGNAL, 1, 0x1, 1, 0x2);
        ready.payload.signal.target_slot = kReady;
        ready.payload.signal.value = 1;
        ready.payload.signal.operation = RP1_SIGOP_SET;
        q.push_back(ready);

        rp1_node_t waitDone = node(RP1_OP_WAIT, 1, 0x2, 1, 0x4);
        waitDone.payload.wait.condition_signal = kDone;
        waitDone.payload.wait.condition_op = RP1_COP_AND_NZ;
        waitDone.payload.wait.condition_value = 1;
        q.push_back(waitDone);

        rp1_node_t clearDone = node(RP1_OP_SIGNAL, 1, 0x4, 1, 0x8);
        clearDone.payload.signal.target_slot = kDone;
        clearDone.payload.signal.value = 0;
        clearDone.payload.signal.operation = RP1_SIGOP_SET;
        q.push_back(clearDone);

        rp1_node_t rerun = node(RP1_OP_RERUN, 1, 0x8, 1, 0x10);
        rerun.payload.rerun.target_node = 0;
        q.push_back(rerun);
    }

    auto& c = ddr.ctrl();
    c.cq_size = 64u;
    c.node_count = static_cast<std::uint32_t>(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) ddr.nodes()[i] = q[i];

    FaithfulRp1 rp1(ddr, sig.data());

    // Host CPU queue: rendezvous N times via the shared slots over the "BAR".
    std::atomic<std::uint32_t> cpuCount{0};
    std::thread cpu([&] {
        for (std::uint32_t i = 0; i < N; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (sig[kReady].value == 0) {
                if (std::chrono::steady_clock::now() > deadline) return;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            // consume iteration i
            cpuCount.fetch_add(1, std::memory_order_relaxed);
            sig[kReady].value = 0;  // ack/clear
            sig[kDone].value = 1;   // release the FPGA WAIT
        }
    });

    std::atomic_thread_fence(std::memory_order_seq_cst);
    c.graph_seq = 1u;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (c.graph_done_seq != c.graph_seq &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cpu.join();

    EXPECT_EQ(c.graph_done_seq, c.graph_seq) << "FPGA loop did not complete";
    EXPECT_EQ(sig[kCountF].value, N) << "FPGA body ran the wrong number of iterations";
    EXPECT_EQ(cpuCount.load(), N) << "CPU consumed the wrong number of iterations";
}

// Phase D1: an FPGA loop whose body carries rendezvous Rp1SignalCommand /
// Rp1WaitCommand (lowered to RP1_OP_SIGNAL/WAIT in the body bucket) runs the
// depth-1 handshake with a host CPU peer each iteration.  Validates that the
// compiler-shaped rendezvous IR lowers and executes over N iterations.
TEST(FpgaCrossQueue, RendezvousNodesInLoopBodyHandshakeWithCpu) {
    constexpr std::uint32_t N        = 4u;
    constexpr std::uint32_t kReady   = 30u;
    constexpr std::uint32_t kDone    = 31u;
    constexpr std::uint32_t kBodyBase = 0x88010000u;

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    rp1_signal_slot_t* sig = ddr.signals();
    FaithfulRp1 rp1(ddr);

    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window,
        [](const std::string&) {
            return FpgaKernelLocation{kBodyBase, 0};
        });

    // Loop body: bodyK -> SIGNAL ready=1 -> WAIT done!=0 -> SIGNAL done=0 (clear).
    Rp1KernelCommand bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK");

    Rp1SignalCommand sigReady;
    sigReady.id = "sig_ready"; sigReady.deviceId = "fpga:0"; sigReady.dependsOn = {"bk"};
    sigReady.resourceOwner = DeviceId("fpga:0");
    sigReady.slot = kReady; sigReady.value = 1; sigReady.operation = RP1_SIGOP_SET;

    Rp1WaitCommand waitDone;
    waitDone.id = "wait_done"; waitDone.deviceId = "fpga:0"; waitDone.dependsOn = {"sig_ready"};
    waitDone.resourceOwner = DeviceId("fpga:0");
    waitDone.slot = kDone; waitDone.value = 1; waitDone.conditionOp = RP1_COP_AND_NZ;

    Rp1SignalCommand sigClear;
    sigClear.id = "sig_clear"; sigClear.deviceId = "fpga:0"; sigClear.dependsOn = {"wait_done"};
    sigClear.slot = kDone; sigClear.value = 0; sigClear.operation = RP1_SIGOP_SET;

    auto body = std::make_shared<Rp1QueueProgram>();
    body->device = DeviceId("fpga:0");
    body->commands.emplace_back(bodyK);
    body->commands.emplace_back(sigReady);
    body->commands.emplace_back(waitDone);
    body->commands.emplace_back(sigClear);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    Rp1LoopCommand loop;
    loop.id = "loop0"; loop.deviceId = "fpga:0";
    loop.loopKind = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, N);
    dg.commands.emplace_back(loop);
    Rp1ChildProgram child;
    child.parentCommandId = "loop0"; child.role = Rp1ChildRole::LoopBody;
    child.programs.push_back(body);
    dg.children.push_back(child);

    // Host CPU peer: each iteration await READY, ack (clear READY), raise DONE.
    std::atomic<std::uint32_t> cpuCount{0};
    std::atomic<bool> stop{false};
    std::thread cpu([&] {
        for (std::uint32_t i = 0; i < N; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (sig[kReady].value == 0) {
                if (stop.load() || std::chrono::steady_clock::now() > deadline) return;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            cpuCount.fetch_add(1, std::memory_order_relaxed);
            sig[kReady].value = 0;
            sig[kDone].value = 1;
        }
    });

    auto plan = dev->compileProgram(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();
    stop.store(true);
    cpu.join();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N) << "FPGA loop body ran the wrong count";
    EXPECT_EQ(cpuCount.load(), N) << "CPU peer rendezvoused the wrong count";
}

// Phase F.3: an autonomous FPGA while-loop terminates on a predicate *broadcast*
// by a peer over a shared host-visible signal slot.  The peer (a host thread
// standing in for the other queue) bumps the broadcast predicate each iteration
// and only then releases the rendezvous, so the FPGA loop sees a fresh value
// before its next top-of-loop check.  The loop must run exactly N bodies and
// exit when the broadcast predicate reaches N -- proving a queue's control flow
// can be driven by a condition another device computes and broadcasts.
TEST(FpgaCrossQueue, WhileLoopTerminatesOnBroadcastPredicate) {
    constexpr std::uint32_t N         = 6u;
    constexpr std::uint32_t kPred     = 24u;  // broadcast predicate (shared slot)
    constexpr std::uint32_t kReady    = 25u;
    constexpr std::uint32_t kDone     = 26u;
    constexpr std::uint32_t kBodyBase = 0x88010000u;

    std::vector<rp1_signal_slot_t> sig(RP1_MAX_SIGNALS);
    std::memset(sig.data(), 0, sig.size() * sizeof(rp1_signal_slot_t));

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    FaithfulRp1 rp1(ddr, sig.data());

    auto node = [](std::uint16_t op, std::uint8_t awB, std::uint32_t awM,
                   std::uint8_t stB, std::uint32_t stM) {
        rp1_node_t n{};
        n.opcode = op;
        n.status = RP1_NODE_PENDING;
        n.barrier_await_bucket = awB;
        n.barrier_await_mask = awM;
        n.barrier_set_bucket = stB;
        n.barrier_set_mask = stM;
        return n;
    };

    // FPGA while-loop driven by the broadcast predicate kPred (exit when >= N).
    // body=[1..4]: dispatch, raise READY, WAIT DONE, clear DONE; RERUN at 5.
    std::vector<rp1_node_t> q;
    {
        rp1_node_t loop = node(RP1_OP_LOOP, 0, 0x0, 0, 0x1);
        loop.payload.loop.body_start = 1;
        loop.payload.loop.body_end = 5;
        loop.payload.loop.max_iterations = 100;  // safety cap; predicate governs
        loop.payload.loop.condition_signal = kPred;
        loop.payload.loop.condition_op = RP1_COP_GE;
        loop.payload.loop.condition_value = N;
        loop.payload.loop.bucket_clear_start = 1;
        loop.payload.loop.bucket_clear_end = 1;
        loop.payload.loop.loop_id = 0;
        q.push_back(loop);

        rp1_node_t body = node(RP1_OP_KERNEL_DISPATCH, 1, 0x0, 1, 0x1);
        body.payload.kernel_dispatch.kernel_base_addr = kBodyBase;
        q.push_back(body);

        rp1_node_t ready = node(RP1_OP_SIGNAL, 1, 0x1, 1, 0x2);
        ready.payload.signal.target_slot = kReady;
        ready.payload.signal.value = 1;
        ready.payload.signal.operation = RP1_SIGOP_SET;
        q.push_back(ready);

        rp1_node_t waitDone = node(RP1_OP_WAIT, 1, 0x2, 1, 0x4);
        waitDone.payload.wait.condition_signal = kDone;
        waitDone.payload.wait.condition_op = RP1_COP_AND_NZ;
        waitDone.payload.wait.condition_value = 1;
        q.push_back(waitDone);

        rp1_node_t clearDone = node(RP1_OP_SIGNAL, 1, 0x4, 1, 0x8);
        clearDone.payload.signal.target_slot = kDone;
        clearDone.payload.signal.value = 0;
        clearDone.payload.signal.operation = RP1_SIGOP_SET;
        q.push_back(clearDone);

        rp1_node_t rerun = node(RP1_OP_RERUN, 1, 0x8, 1, 0x10);
        rerun.payload.rerun.target_node = 0;
        q.push_back(rerun);
    }

    auto& c = ddr.ctrl();
    c.cq_size = 64u;
    c.node_count = static_cast<std::uint32_t>(q.size());
    for (std::size_t i = 0; i < q.size(); ++i) ddr.nodes()[i] = q[i];

    // Peer queue (host thread): each iteration await READY, broadcast the next
    // predicate value into the shared slot, then ack READY and raise DONE.  The
    // predicate is written *before* DONE, so the FPGA's next loop-top check sees
    // it (happens-before via the DONE handshake).
    std::atomic<std::uint32_t> peerTicks{0};
    std::thread peer([&] {
        for (std::uint32_t i = 1; i <= N; ++i) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (sig[kReady].value == 0) {
                if (std::chrono::steady_clock::now() > deadline) return;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            sig[kPred].value = i;   // broadcast predicate (reaches N on the last tick)
            peerTicks.fetch_add(1, std::memory_order_relaxed);
            sig[kReady].value = 0;  // ack
            sig[kDone].value = 1;   // release the FPGA WAIT (after kPred is set)
        }
    });

    std::atomic_thread_fence(std::memory_order_seq_cst);
    c.graph_seq = 1u;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (c.graph_done_seq != c.graph_seq &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    peer.join();

    EXPECT_EQ(c.graph_done_seq, c.graph_seq) << "FPGA while-loop did not terminate";
    EXPECT_EQ(rp1.dispatches(kBodyBase), N)
        << "loop body should run exactly N times before the broadcast predicate reaches N";
    EXPECT_EQ(peerTicks.load(), N) << "peer should have broadcast N predicate updates";
    EXPECT_EQ(sig[kPred].value, N);
}

namespace {
// A no-I/O CPU kernel that counts its invocations — stands in for the CPU
// body slice's compute in the Phase E rendezvous test.
class CountKernel : public CpuKernel {
   public:
    CountKernel(std::string name, std::atomic<std::uint32_t>& counter)
        : CpuKernel(std::move(name)), counter_(counter) {}
    IOTypeMap ioTypeMap() const override { return IOTypeMap{}; }
    void run(Args&) override { counter_.fetch_add(1, std::memory_order_relaxed); }

   private:
    std::atomic<std::uint32_t>& counter_;
};
}  // namespace

// Phase E: a split cross-device loop runs concurrently as two queues that
// rendezvous each iteration through host-visible signal slots in the FPGA's BAR
// window.  Here a *real* CpuDevicePlan executes the CPU consumer half of the
// handshake (Rp1WaitCommand polls a slot, Rp1SignalCommand SETs one) inside
// its fixed-count loop, while a real FpgaDevicePlan submits the FPGA producer
// half once and RP1 iterates autonomously.  The two share the same slots via
// execution-plan owner binding (the same binding Graph::compile creates),
// proving the CPU queue executes its slice over the BAR rather than
// relaunching child plans against a hand-rolled peer.
// A loop whose body is not FPGA-resident (cross-device) is the Phase-2
// cross-queue case; phase-1 lowering rejects it with a clear diagnostic.
TEST_F(FpgaDeviceFixture, LoopWithoutFpgaBodyThrows) {
    auto dev = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{0x88010000u, 0};
        });

    Rp1QueueProgram dg;
    dg.device = DeviceId("fpga:0");
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    Rp1LoopCommand loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = Rp1LoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    dg.commands.emplace_back(loop);
    // No children -> no FPGA body.

    EXPECT_THROW(dev->compileProgram(dg), std::logic_error);
}

// Phase F.3b: a data-dependent cross-device *split* loop runs as two
// rendezvousing queues -- a CPU Authority that drives the loop and broadcasts
// its per-iteration continue/stop decision, and an FPGA Follower whose RP1 LOOP
// reads that broadcast as its exit predicate.  Neither queue is told the count
// up front; the Follower runs exactly as many bodies as the Authority dictates,
// the two staying in lockstep via the ready/ack broadcast handshake.
TEST_F(FpgaDeviceFixture,
       SplitFixedLoopPretestsZeroAndKeepsPositiveLockstep) {
    rp1_.reset();
    primeAsReady(ddr_);
    FaithfulRp1 faithfulRp1(ddr_);

    Graph graph = Graph::withDefaults();
    std::atomic<std::uint32_t> cpuCount{0};
    detail::GraphTestAccess::cpuDevice(graph)->registerKernel(
        std::make_shared<CountKernel>("counter", cpuCount));
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements =
        graph.scalarInput<std::uint64_t>("elements");
    GraphScalar iterations =
        graph.scalarInput<std::uint32_t>("iterations");
    GraphBuffer input =
        graph.input<std::int32_t>("input", elements);
    GraphBuffer output =
        graph.output<std::int32_t>("output", elements);
    KernelHandle cpu{
        "counter", DeviceType::CPU, std::nullopt, {}, "cpu"};
    KernelHandle device{
        "body", DeviceType::FPGA, std::nullopt,
        IOTypeMap{}.in<std::int32_t>("in")
            .out<std::int32_t>("out"),
        "fpga:0"};
    auto loop = graph.addLoop({
        .count = iterations,
        .inputs = {{"state", input}},
        .outputs = {{"state", output}},
        .outputPlacement = {
            .buffers = {{"state", "fpga:0"}}},
    });
    loop.addKernelCall({.kernel = cpu});
    loop.addKernelCall({
        .kernel = device,
        .inputs = {{"in", loop.input("state")}},
        .outputs = {{"out", loop.output("state")}},
    });

    Execution execution = graph.compile();
    const std::vector<std::int32_t> initial{7, 11};
    execution.writeScalar(
        elements, static_cast<std::uint64_t>(initial.size()));
    execution.write(input, initial);
    execution.writeScalar(iterations, 0u);
    ASSERT_NO_THROW(execution.run());
    EXPECT_EQ(cpuCount.load(), 0u);
    EXPECT_EQ(faithfulRp1.dispatches(kKernelA_R5), 0u);
    std::vector<std::int32_t> result(initial.size());
    execution.read(output, result);
    EXPECT_EQ(result, initial);
    const rp1_node_t* followerLoop = std::find_if(
        ddr_.nodes(), ddr_.nodes() + ddr_.ctrl().node_count,
        [](const rp1_node_t& node) {
            return node.opcode == RP1_OP_LOOP;
        });
    ASSERT_NE(
        followerLoop,
        ddr_.nodes() + ddr_.ctrl().node_count);
    EXPECT_EQ(followerLoop->payload.loop.max_iterations, 0u)
        << "split followers must be authority-driven without a local cap";

    constexpr std::uint32_t kIterations = 4;
    execution.writeScalar(iterations, kIterations);
    ASSERT_NO_THROW(execution.run());
    EXPECT_EQ(cpuCount.load(), kIterations);
    EXPECT_EQ(
        faithfulRp1.dispatches(kKernelA_R5), kIterations);
    execution.read(output, result);
    EXPECT_EQ(result, initial);
}

TEST_F(FpgaDeviceFixture, SplitWhileLoopPretestsFalseCondition) {
    rp1_.reset();
    primeAsReady(ddr_);
    FaithfulRp1 faithfulRp1(ddr_);

    Graph graph = Graph::withDefaults();
    std::atomic<std::uint32_t> cpuCount{0};
    detail::GraphTestAccess::cpuDevice(graph)->registerKernel(
        std::make_shared<CountKernel>("counter", cpuCount));
    auto fpga = std::make_shared<FpgaDevice>(
        "fpga:0", window_,
        [](const std::string&) {
            return FpgaKernelLocation{kKernelA_R5, 0};
        });
    detail::GraphTestAccess::registerDevice(graph, fpga);

    GraphScalar elements =
        graph.scalarInput<std::uint64_t>("elements");
    GraphScalar enabled =
        graph.scalarInput<std::uint32_t>("enabled");
    GraphBuffer input =
        graph.input<std::int32_t>("input", elements);
    GraphBuffer output =
        graph.output<std::int32_t>("output", elements);
    KernelHandle cpu{
        "counter", DeviceType::CPU, std::nullopt, {}, "cpu"};
    KernelHandle device{
        "body", DeviceType::FPGA, std::nullopt,
        IOTypeMap{}.in<std::int32_t>("in")
            .out<std::int32_t>("out"),
        "fpga:0"};
    LoopBuildSpec loopSpec;
    loopSpec.condition = enabled != 0u;
    loopSpec.inputs = {{"state", input}};
    loopSpec.outputs = {{"state", output}};
    loopSpec.outputPlacement.buffers["state"] = "fpga:0";
    auto loop = graph.addLoop(loopSpec);
    loop.addKernelCall({.kernel = cpu});
    loop.addKernelCall({
        .kernel = device,
        .inputs = {{"in", loop.input("state")}},
        .outputs = {{"out", loop.output("state")}},
    });

    Execution execution = graph.compile();
    const std::vector<std::int32_t> initial{13, 21};
    execution.writeScalar(
        elements, static_cast<std::uint64_t>(initial.size()));
    execution.write(input, initial);
    execution.writeScalar(enabled, 0u);
    ASSERT_NO_THROW(execution.run());
    EXPECT_EQ(cpuCount.load(), 0u);
    EXPECT_EQ(faithfulRp1.dispatches(kKernelA_R5), 0u);
    std::vector<std::int32_t> result(initial.size());
    execution.read(output, result);
    EXPECT_EQ(result, initial);
}
