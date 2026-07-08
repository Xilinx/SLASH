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
 *  - Non-kernel CompiledNode variants (e.g. CompiledBridgeOpNode that
 *    the compiler splices for cross-device buffers) cause compilePlan
 *    to throw a descriptive diagnostic.
 *  - Deferred (global-variable) scalar resolution picks up values set
 *    on the Graph between compile() and launch().
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/graph/core/graph_scalar.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/cpu_fpga_bridge.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>
#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/io_map.hpp>
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

LoopTripCount bindTripCount(DGraph& dg, std::uint32_t value, std::string name = "trip_count") {
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

class FakeRp1 {
   public:
    explicit FakeRp1(DdrView ddr) : ddr_(ddr) {
        thread_ = std::thread([this] { run(); });
    }
    ~FakeRp1() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

   private:
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
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    void processGraph() {
        auto& c = ddr_.ctrl();
        const std::uint32_t count   = c.node_count;
        const std::uint32_t cq_size = c.cq_size;
        for (std::uint32_t i = 0; i < count; ++i) {
            rp1_node_t& n = ddr_.nodes()[i];
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
            if (n.opcode == RP1_OP_SIGNAL) {
                const auto& pl = n.payload.signal;
                ddr_.signals()[pl.target_slot].value = pl.value;
                ddr_.signals()[pl.target_slot].last_writer_node = i;
            }
            if ((n.flags & RP1_FLAG_SILENT) == 0u) {
                const std::uint32_t idx = c.cq_write_idx & (cq_size - 1u);
                rp1_cq_entry_t& e = ddr_.cq()[idx];
                e.node_index   = i;
                e.status       = RP1_CQ_OK;
                e.error_detail = 0;
                e.timestamp    = 0;
                ++c.cq_write_idx;
            }
            n.status = RP1_NODE_DONE;
        }
    }

    DdrView           ddr_;
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
    c.magic     = RP1_CTRL_MAGIC;
    c.version   = RP1_PROTOCOL_VERSION;
    c.rp1_state = RP1_STATE_READY;
    c.heartbeat = 1;
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

// ---------------------------------------------------------------------------
// compilePlan: rejection paths
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
    // splices a consumer-side bridge into the FPGA DGraph, which now copies
    // the bytes into the FPGA BAR-backed buffer store before kernel dispatch.
    Graph g = Graph::withDefaults();
    g.cpuDevice()->registerKernel(std::make_shared<CopyKernel>());

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    g.registerDevice(dev);

    GraphScalar elements = g.scalarInput<std::uint64_t>("elements");
    GraphBuffer raw = g.inputBuffer(BufferType::I32, "raw", elements);
    IOTypeMap cpuIo;
    cpuIo.inputs.push_back({"in", BufferType::I32});
    cpuIo.outputs.push_back({"out", BufferType::I32});
    KernelDescriptor cpu{"copy", DeviceType::CPU, std::nullopt, cpuIo};

    IOMap io1;
    GraphBuffer staged;
    io1.bindInput("in", raw)
       .bindOutput("out", BufferType::I32, staged);
    g.addNode(cpu, std::move(io1), "cpu");

    IOTypeMap fpgaIo;
    fpgaIo.inputs.push_back({"in", BufferType::I32});
    IOMap io2;
    io2.bindInput("in", staged);
    g.addNode(fpgaKernel("kA", fpgaIo), std::move(io2), "fpga:0");

    const std::vector<std::int32_t> input = {1, 2, 3, 4};
    g.cpuDevice()->setInputBuffer("raw", input.data(), input.size() * sizeof(input[0]));

    auto debugExec = g.compile();
    debugExec.writeScalar(elements, static_cast<std::uint64_t>(input.size()));
    for (const DGraph& dg : debugExec.dgraphs()) {
        std::cerr << "DEBUG DG " << dg.deviceId << "\n";
        for (const auto& node : dg.nodes) {
            std::cerr << "  " << compiledNodeId(node) << " deps:";
            for (const auto& dep : compiledNodeDependsOn(node)) std::cerr << ' ' << dep;
            std::cerr << "\n";
        }
    }
    ASSERT_NO_THROW(debugExec.run());

    std::vector<std::int32_t> echoed(input.size(), 0);
    dev->getOutputBuffer(staged.name(), echoed.data(), echoed.size() * sizeof(echoed[0]));
    EXPECT_EQ(echoed, input);
}

TEST_F(FpgaDeviceFixture, CpuFpgaBridgeRejectsMissingProducerBuffer) {
    CpuDevice cpu("cpu");
    FpgaDevice fpga("fpga:0", window_, makeDiamondLookup());
    CpuFpgaBridge bridge(cpu, fpga);

    GraphBuffer missing = GraphBuffer::make(BufferType::I32, "missing", 0);
    BridgeStepPair step = bridge.makeTransfer(
        cpu, fpga, missing, /*sizeHintBytes=*/0, "producer", "consumer");

    EXPECT_THROW(step.producerAction(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, CpuFpgaCpuBufferRoundTripUsesPackedBufferPointers) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    g.cpuDevice()->registerKernel(std::make_shared<CopyKernel>());
    g.registerDevice(dev);

    GraphScalar elements = g.scalarInput<std::uint64_t>("elements");
    GraphBuffer raw = g.inputBuffer(BufferType::I32, "raw", elements);

    IOTypeMap cpuIo;
    cpuIo.inputs.push_back({"in", BufferType::I32});
    cpuIo.outputs.push_back({"out", BufferType::I32});
    KernelDescriptor cpu{"copy", DeviceType::CPU, std::nullopt, cpuIo};

    IOMap cpuProduceIo;
    GraphBuffer toFpga = g.buffer<std::int32_t>("toFpga", elements);
    cpuProduceIo.bindInput("in", raw)
                .bindExistingOutput("out", toFpga);
    const std::string cpuProducer = g.addNode(cpu, std::move(cpuProduceIo), "cpu");

    IOTypeMap fpgaIo;
    fpgaIo.inputScalars.push_back({"bytes", ScalarType::U32});
    fpgaIo.inputs.push_back({"in", BufferType::I32});
    fpgaIo.outputs.push_back({"out", BufferType::I32});
    GraphScalar copyBytes = g.scalarInput<std::uint32_t>("copy_bytes");

    IOMap fpgaCopyIo;
    GraphBuffer fromFpga = g.buffer<std::int32_t>("fromFpga", elements);
    constexpr std::uint32_t kBytes = 4u * sizeof(std::int32_t);
    fpgaCopyIo.bindInputScalar("bytes", copyBytes)
              .bindInput("in", toFpga)
              .bindExistingOutput("out", fromFpga);
    g.addNode(fpgaKernel("kA", fpgaIo), std::move(fpgaCopyIo), "fpga:0", {cpuProducer});

    IOMap cpuConsumeIo;
    GraphBuffer finalOut = g.buffer<std::int32_t>("finalOut", elements);
    cpuConsumeIo.bindInput("in", fromFpga)
                .bindExistingOutput("out", finalOut);
    g.addNode(cpu, std::move(cpuConsumeIo), "cpu");

    const std::vector<std::int32_t> input = {10, 20, 30, 40};
    g.cpuDevice()->setInputBuffer("raw", input.data(), input.size() * sizeof(input[0]));

    auto exec = g.compile();
    exec.writeScalar(elements, static_cast<std::uint64_t>(input.size()));
    exec.writeScalar(copyBytes, kBytes);
    std::vector<std::int32_t> output(input.size(), 0);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ASSERT_NO_THROW(exec.run());
        g.cpuDevice()->getOutputBuffer(finalOut.name(), output.data(),
                                       output.size() * sizeof(output[0]));
        if (output == input) break;
    }
    EXPECT_EQ(output, input);

    // The fake RP1 copied through the addresses packed after the scalar byte count.
    EXPECT_GE(ddr_.nodes()[0].payload.kernel_dispatch.arg_count, 5u);
}

TEST_F(FpgaDeviceFixture, CarriedBufferAliasStaysCoherentAfterGrowth) {
    FpgaDevice dev("fpga:0", window_,
                   [](const std::string&) { return FpgaKernelLocation{kKernelA_R5, 0}; });

    GraphScalar elements = GraphScalar::ref(ScalarType::U64, "elements");
    GraphBuffer parent = GraphBuffer::make(BufferType::I32, "state", 0, elements);
    GraphBuffer local = GraphBuffer::make(BufferType::I32, "state", 1, elements);

    const std::int32_t seed = 42;
    dev.setInputBuffer(scopedBufferKey(parent.scopeId(), parent.name()),
                       &seed, sizeof(seed));

    CompiledBoundaryNode importB;
    importB.id = "import";
    importB.deviceId = "fpga:0";
    importB.side = CompiledBoundaryNode::Side::Start;
    importB.bufferCopies.push_back({/*sourceName=*/parent.name(),
                                    /*sourceScopeId=*/parent.scopeId(),
                                    /*targetName=*/local.name(),
                                    /*targetScopeId=*/local.scopeId()});

    IOTypeMap bodyType;
    bodyType.inputs.push_back({"in", BufferType::I32});
    CompiledKernelNode bodyK;
    bodyK.id = "body";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel = fpgaKernel("body", bodyType);
    bodyK.ioMap.bindInput("in", local);

    auto body = std::make_shared<DGraph>();
    body->deviceId = "fpga:0";
    body->nodes.push_back(importB);
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    (*dg.scalarValues)[scopedScalarKey(0, "elements")] = 2;
    CompiledLoopNode loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 1);
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const std::int32_t grown[] = {7, 9};
    dev.setInputBuffer(scopedBufferKey(local.scopeId(), local.name()),
                       grown, sizeof(grown));

    EXPECT_EQ(dev.bufferSize(scopedBufferKey(parent.scopeId(), parent.name())),
              sizeof(grown))
        << "growing an alias must update the canonical source buffer record";
    std::int32_t readback[] = {0, 0};
    ASSERT_NO_THROW(dev.getOutputBuffer(scopedBufferKey(parent.scopeId(), parent.name()),
                                        readback, sizeof(readback)));
    EXPECT_EQ(readback[0], grown[0]);
    EXPECT_EQ(readback[1], grown[1]);
}

TEST_F(FpgaDeviceFixture, GraphReprogramNodeCompilesIntoFpgaDGraph) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    g.registerDevice(dev);

    ReprogramSpec spec;
    spec.imageId = "imageB";
    spec.pdiPath = "imageB.pdi";
    spec.device = "fpga:0";

    const std::string reprogramId = g.addReprogram(std::move(spec));
    auto exec = g.compile();

    const DGraph* fpgaDg = nullptr;
    for (const DGraph& dg : exec.dgraphs()) {
        if (dg.deviceId == "fpga:0") fpgaDg = &dg;
    }
    ASSERT_NE(fpgaDg, nullptr);
    ASSERT_EQ(fpgaDg->nodes.size(), 1u);
    const auto* node = std::get_if<CompiledReprogramNode>(&fpgaDg->nodes[0]);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->id, reprogramId);
    EXPECT_EQ(node->imageId, "imageB");
    EXPECT_EQ(node->pdiPath, "imageB.pdi");
}

TEST_F(FpgaDeviceFixture, ReprogramNodeLowersToPdiLoad) {
    const auto tmpDir = makeTempDir("fpga-reprogram-test");
    const std::string pdiPath = writeTempFile(tmpDir, "imageB.pdi", "fake-pdi-bytes");

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device = dev;

    CompiledReprogramNode rp;
    rp.id = "rp";
    rp.deviceId = "fpga:0";
    rp.imageId = "imageB";
    rp.pdiPath = pdiPath;
    rp.timeoutCycles = 12345u;
    dg.nodes.emplace_back(rp);

    auto plan = dev->compilePlan(dg);
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

TEST_F(FpgaDeviceFixture, KernelImageMismatchRequiresReprogram) {
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

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device = dev;

    CompiledKernelNode k;
    k.id = "kA";
    k.deviceId = "fpga:0";
    k.kernel = fpgaKernel("kA");
    k.kernel.image = "imageB";
    dg.nodes.emplace_back(k);

    EXPECT_THROW(dev->compilePlan(dg), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Diamond happy-path
// ---------------------------------------------------------------------------

TEST_F(FpgaDeviceFixture, DiamondGraphCompletesAndSentinelFires) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    g.registerDevice(dev);

    IOMap ioA, ioB, ioC, ioD;
    std::string a = g.addNode(fpgaKernel("kA"), std::move(ioA), "fpga:0");
    std::string b = g.addNode(fpgaKernel("kB"), std::move(ioB), "fpga:0", {a});
    std::string c = g.addNode(fpgaKernel("kC"), std::move(ioC), "fpga:0", {a});
    g.addNode(fpgaKernel("kD"), std::move(ioD), "fpga:0", {b, c});

    ASSERT_NO_THROW(g.compile().run());

    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);
    // 4 kernels + 1 sentinel signal = 5 CQ entries.
    EXPECT_EQ(ddr_.ctrl().cq_write_idx, 5u);
}

TEST_F(FpgaDeviceFixture, DiamondBarrierMasksAreCorrect) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    Graph g = Graph::withDefaults();
    g.registerDevice(dev);
    IOMap ioA, ioB, ioC, ioD;
    std::string a = g.addNode(fpgaKernel("kA"), std::move(ioA), "fpga:0");
    std::string b = g.addNode(fpgaKernel("kB"), std::move(ioB), "fpga:0", {a});
    std::string c_id = g.addNode(fpgaKernel("kC"), std::move(ioC), "fpga:0", {a});
    g.addNode(fpgaKernel("kD"), std::move(ioD), "fpga:0", {b, c_id});

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
    g.registerDevice(dev);
    GraphScalar size = g.scalarInput<std::uint32_t>("size");
    GraphScalar flags = g.scalarInput<std::uint8_t>("flags");

    IOMap io;
    io.bindInputScalar("size", size);
    io.bindInputScalar("flags", flags);
    g.addNode(fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    exec.writeScalar(size, 123u);
    exec.writeScalar<std::uint8_t>(flags, 7u);
    exec.launch();
    exec.wait();

    // Protocol v2: each arg is a (reg_offset, value) pair.  On the mock
    // lookup path offsets are handed out contiguously from 0x10, so:
    //   size=123 @ 0x10, flags=7 (zero-extended) @ 0x14.
    EXPECT_EQ(ddr_.args()[0], 0x10u);
    EXPECT_EQ(ddr_.args()[1], 123u);
    EXPECT_EQ(ddr_.args()[2], 0x14u);
    EXPECT_EQ(ddr_.args()[3], 7u);
    EXPECT_EQ(ddr_.nodes()[0].payload.kernel_dispatch.arg_count, 2u);
}

TEST_F(FpgaDeviceFixture, U64ScalarArgsConsumeTwoArgWords) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"addr", ScalarType::U64});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    g.registerDevice(dev);
    GraphScalar addr = g.scalarInput<std::uint64_t>("addr");

    IOMap io;
    io.bindInputScalar("addr", addr);
    g.addNode(fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    exec.writeScalar(addr, static_cast<std::uint64_t>(0xDEAD'BEEF'CAFE'BABEull));
    exec.launch();
    exec.wait();

    // A U64 occupies two registers (0x10, 0x14), emitted as two v2 pairs.
    EXPECT_EQ(ddr_.args()[0], 0x10u);
    EXPECT_EQ(ddr_.args()[1], 0xCAFEBABEu);
    EXPECT_EQ(ddr_.args()[2], 0x14u);
    EXPECT_EQ(ddr_.args()[3], 0xDEADBEEFu);
    EXPECT_EQ(ddr_.nodes()[0].payload.kernel_dispatch.arg_count, 2u);
}

// Note: the GraphCompiler currently rejects global-variable scalar
// bindings on non-CPU kernels with "global scalar bindings are currently
// supported only on CPU kernels". The FpgaDevice's deferred-scalar
// resolution path is therefore unreachable through the public Graph API
// today, but the code is kept (and exercised by direct DGraph
// construction in DeferredScalarsResolvedAtLaunch) so that relaxing the
// compiler restriction later Just Works.
TEST_F(FpgaDeviceFixture, GlobalScalarOnFpgaKernelUsesDeferredLaunchValue) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"size", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    g.registerDevice(dev);

    GraphScalar var = g.globalScalar(ScalarType::U32, "size");

    IOMap io;
    io.bindInputScalar("size", var);
    g.addNode(fpgaKernel("kA", iot), std::move(io), "fpga:0");

    auto exec = g.compile();
    exec.writeScalar<std::uint32_t>("size", 0x1234u);
    exec.launch();
    exec.wait();
    EXPECT_EQ(ddr_.args()[1], 0x1234u);
}

TEST_F(FpgaDeviceFixture, DeferredScalarsResolvedAtLaunch) {
    // Build a DGraph by hand to bypass the compiler's "globals only on
    // CPU kernels" restriction.  Verifies that FpgaDevice's deferred
    // scalar code patches arg_buf right before submission.
    auto scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    (*scalarValues)["scope:0:size"] = 0xAAAAu;

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = scalarValues;

    CompiledKernelNode k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA");
    k.kernel.ioType.inputScalars.push_back({"size", ScalarType::U32});
    k.ioMap.bindInputScalar("size", GraphScalar::ref(ScalarType::U32, "size", 0));
    dg.nodes.push_back(k);

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    dg.device = dev;
    auto plan = dev->compilePlan(dg);

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
    g.registerDevice(dev);

    std::vector<std::pair<GraphScalar, std::uint32_t>> scalarValues;
    auto bind = [&](std::string name, std::uint32_t v) {
        IOMap io;
        GraphScalar scalar = g.scalarInput<std::uint32_t>(std::move(name));
        scalarValues.emplace_back(scalar, v);
        io.bindInputScalar("s0", scalar);
        return io;
    };
    std::string a = g.addNode(fpgaKernel("kA", iot), bind("s0_a", 0x11), "fpga:0");
    std::string b = g.addNode(fpgaKernel("kB", iot), bind("s0_b", 0x22), "fpga:0", {a});
    g.addNode(fpgaKernel("kC", iot), bind("s0_c", 0x33), "fpga:0", {b});

    auto exec = g.compile();
    for (const auto& [scalar, value] : scalarValues) {
        exec.writeScalar(scalar, value);
    }
    exec.launch();
    exec.wait();

    // Each kernel contributes one (reg_offset=0x10, value) pair = two words.
    EXPECT_EQ(ddr_.args()[1], 0x11u);
    EXPECT_EQ(ddr_.args()[3], 0x22u);
    EXPECT_EQ(ddr_.args()[5], 0x33u);

    // Each kernel's arg_buffer_offset must point at its own (2-word) slot.
    auto findOffsetFor = [&](std::uint32_t r5) -> std::uint32_t {
        for (std::size_t i = 0; i < 3; ++i) {
            const auto& kd = ddr_.nodes()[i].payload.kernel_dispatch;
            if (kd.kernel_base_addr == r5) return kd.arg_buffer_offset;
        }
        return UINT32_MAX;
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

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device   = dev;
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    GraphScalar elements = GraphScalar::ref(ScalarType::U64, "elements");
    (*dg.scalarValues)[scopedScalarKey(elements.scopeId(), elements.varName())] = 4;
    (*dg.scalarValues)[scopedScalarKey(0, "n")] = 0x1122'3344'5566'7788ull;

    CompiledKernelNode node;
    node.id       = "k0";
    node.deviceId = "fpga:0";
    node.kernel   = KernelDescriptor{"graph_kernel", DeviceType::FPGA,
                                     std::string("imageA"), k.ioType};
    GraphBuffer outTok;
    node.ioMap
        .bindInputScalar("n", GraphScalar::ref(ScalarType::U64, "n"))
        .bindInput("in", GraphBuffer::make(BufferType::I32, "inBuf", 0, elements))
        .bindOutput("out", BufferType::I32, outTok);
    dg.nodes.push_back(std::move(node));

    auto plan = dev->compilePlan(dg);
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

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device   = dev;
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    GraphScalar elements = GraphScalar::ref(ScalarType::U64, "elements");
    (*dg.scalarValues)[scopedScalarKey(elements.scopeId(), elements.varName())] = 4;
    (*dg.scalarValues)[scopedScalarKey(0, "n")] = 0x1122'3344'5566'7788ull;

    // Descriptor renames the buffer ports, as the real example does.
    IOTypeMap renamed;
    renamed.inputScalars.push_back({"n", ScalarType::U64});
    renamed.inputs.push_back({"in", BufferType::I32});
    renamed.outputs.push_back({"image_out", BufferType::I32});

    CompiledKernelNode node;
    node.id       = "k0";
    node.deviceId = "fpga:0";
    node.kernel   = KernelDescriptor{"graph_kernel_0", DeviceType::FPGA,
                                     std::string("imageA"), renamed};
    GraphBuffer outTok;
    node.ioMap
        .bindInputScalar("n", GraphScalar::ref(ScalarType::U64, "n"))
        .bindInput("in", GraphBuffer::make(BufferType::I32, "inBuf", 0, elements))
        .bindOutput("image_out", BufferType::I32, outTok);
    dg.nodes.push_back(std::move(node));

    auto plan = dev->compilePlan(dg);
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

TEST_F(FpgaDeviceFixture, LookupReturningZeroAddressIsRejected) {
    auto bad_lookup = [](const std::string&) {
        return FpgaKernelLocation{0u, 0u};
    };
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, bad_lookup);
    Graph g = Graph::withDefaults();
    g.registerDevice(dev);
    g.addNode(fpgaKernel("kA"), IOMap{}, "fpga:0");

    EXPECT_THROW(g.compile(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, UnboundInputScalarIsRejected) {
    IOTypeMap iot;
    iot.inputScalars.push_back({"missing", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());
    Graph g = Graph::withDefaults();
    g.registerDevice(dev);
    g.addNode(fpgaKernel("kA", iot), IOMap{}, "fpga:0");

    EXPECT_THROW(g.compile(), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, OutputScalarPortsEmitScalarRead) {
    // FPGA output scalar ports are captured by a trailing SCALAR_READ so RP1
    // can feed the value into signal-slot based predicates.
    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U32});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device = dev;
    CompiledKernelNode k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA", iot);
    dg.nodes.push_back(k);

    auto plan = dev->compilePlan(dg);
    ASSERT_NE(plan, nullptr);
    ASSERT_NO_THROW(plan->launch());
    ASSERT_NO_THROW(plan->wait());

    EXPECT_EQ(ddr_.nodes()[0].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(ddr_.nodes()[1].opcode, RP1_OP_SCALAR_READ);
    EXPECT_EQ(ddr_.nodes()[1].payload.scalar_read.source_addr, kKernelA_R5 + 0x10u);
}

TEST_F(FpgaDeviceFixture, WideOutputScalarPortsAreRejected) {
    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U64});

    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device = dev;
    CompiledKernelNode k;
    k.id        = "kA";
    k.deviceId  = "fpga:0";
    k.kernel    = fpgaKernel("kA", iot);
    dg.nodes.push_back(k);

    EXPECT_THROW(dev->compilePlan(dg), std::runtime_error);
}

TEST_F(FpgaDeviceFixture, ReferencedSignalSlotsAreReservedForHostScalarIo) {
    auto dev = std::make_shared<FpgaDevice>("fpga:0", window_, makeDiamondLookup());

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.device = dev;

    CompiledSignalNode sg;
    sg.id = "reserved";
    sg.deviceId = "fpga:0";
    sg.slot = 0;
    sg.value = 7;
    sg.operation = RP1_SIGOP_SET;
    dg.nodes.emplace_back(sg);

    IOTypeMap iot;
    iot.outputScalars.push_back({"result", ScalarType::U32});
    CompiledKernelNode k;
    k.id = "kA";
    k.deviceId = "fpga:0";
    k.kernel = fpgaKernel("kA", iot);
    dg.nodes.emplace_back(k);

    auto plan = dev->compilePlan(dg);
    ASSERT_NE(plan, nullptr);

    dev->setInputScalar(scopedScalarKey(0, "fresh"), 123);
    EXPECT_EQ(ddr_.signals()[0].value, 0u)
        << "host scalar allocation must not reuse an RP1 signal slot referenced by the plan";

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
    g.registerDevice(dev);
    g.addNode(fpgaKernel("kA"), IOMap{}, "fpga:0");

    g.compile().run();

    EXPECT_EQ(ddr_.signals()[42].value, 0xC0FFEE00u);
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
    g.registerDevice(dev);
    std::string a = g.addNode(fpgaKernel("kA"), IOMap{}, "fpga:0");
    g.addNode(fpgaKernel("kB"), IOMap{}, "fpga:0", {a});

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
    g.registerDevice(dev);
    const char* names[] = {"kA", "kB", "kC", "kD"};
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        const std::vector<std::string> after = prev.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{prev};
        prev = g.addNode(fpgaKernel(names[i % 4]), IOMap{}, "fpga:0", after);
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
    g.registerDevice(dev);
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
        prev = g.addNode(fpgaKernel(names[i % 4]), IOMap{}, "fpga:0", after);
        ids.push_back(prev);
    }
    g.addNode(fpgaKernel("kD"), IOMap{}, "fpga:0",
              std::vector<std::string>{ids.front(), ids.back()});

    ASSERT_NO_THROW(g.compile().run());
    EXPECT_EQ(ddr_.ctrl().graph_seq, 1u);
    EXPECT_EQ(ddr_.signals()[kDefaultSentinelSlot].value, kDefaultSentinelValue);
}

// ---------------------------------------------------------------------------
// Autonomous control flow: fixed-count loop lowering
// ---------------------------------------------------------------------------

// A DGraph carrying a fixed-count CompiledLoopNode with an all-FPGA body must
// lower to a single RP1 image: LOOP (max_iterations, body range, per-iteration
// bucket clear) + the flattened body kernel + a RERUN re-arming the LOOP +
// the trailing sentinel.  This validates the exact node layout the firmware's
// loop_decrement / loop_fixed_count QEMU tests prove it executes.
TEST_F(FpgaDeviceFixture, FixedCountLoopLowersToLoopRerunImage) {
    FpgaDevice dev("fpga:0", window_,
                   [](const std::string&) { return FpgaKernelLocation{0x88010000u, 0}; });

    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");  // no declared ports -> zero args

    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 3);
    dg.nodes.emplace_back(loop);

    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    const rp1_node_t* n = ddr_.nodes();

    // node 0: LOOP, body range [1,2], 3 iterations, clears the body bucket.
    EXPECT_EQ(n[0].opcode, RP1_OP_LOOP);
    EXPECT_EQ(n[0].payload.loop.body_start, 1u);
    EXPECT_EQ(n[0].payload.loop.body_end, 2u);
    EXPECT_EQ(n[0].payload.loop.max_iterations, 3u);
    EXPECT_EQ(n[0].payload.loop.bucket_clear_start, 1u);
    EXPECT_EQ(n[0].payload.loop.bucket_clear_end, 1u);
    EXPECT_EQ(n[0].barrier_set_bucket, 0u);          // exit bit lives in bucket 0
    EXPECT_EQ(n[0].payload.loop.condition_op, RP1_COP_AND_NZ);  // never -> max_iter governs
    EXPECT_EQ(n[0].payload.loop.condition_value, 0u);

    // node 1: the body kernel, done-bit in the loop's body bucket (1).
    EXPECT_EQ(n[1].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(n[1].barrier_set_bucket, 1u);

    // node 2: RERUN re-arms the LOOP node (index 0), gated on the body bucket.
    EXPECT_EQ(n[2].opcode, RP1_OP_RERUN);
    EXPECT_EQ(n[2].payload.rerun.target_node, 0u);
    EXPECT_EQ(n[2].barrier_await_bucket, 1u);

    // node 3: sentinel SIGNAL gated on the loop's exit bit.
    EXPECT_EQ(n[3].opcode, RP1_OP_SIGNAL);
    EXPECT_EQ(n[3].barrier_set_mask, 1u << 31);
    EXPECT_EQ(n[3].barrier_await_mask, n[0].barrier_set_mask);
}

TEST_F(FpgaDeviceFixture, FixedCountLoopBodySpansMultipleBarrierBuckets) {
    FpgaDevice dev("fpga:0", window_,
                   [](const std::string&) { return FpgaKernelLocation{0x88010000u, 0}; });

    auto body = std::make_shared<DGraph>();
    body->deviceId = "fpga:0";
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        CompiledKernelNode k;
        k.id = "bk" + std::to_string(i);
        k.deviceId = "fpga:0";
        k.kernel = fpgaKernel("bodyK");
        if (!prev.empty()) k.dependsOn = {prev};
        prev = k.id;
        body->nodes.emplace_back(k);
    }

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    dg.nodes.emplace_back(loop);

    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");
    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 5);
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), 5u);
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
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // Body kernel produces output scalar "i" (the loop variable).
    IOTypeMap bodyType;
    bodyType.outputScalars.push_back({"i", ScalarType::U32});
    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindOutputScalar("i", GraphScalar::ref(ScalarType::U32, "i"));

    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::WhileCondition;
    loop.condition = Condition::compare(CompareOp::LT,
                                        ConditionOperand::scalar(ScalarType::U32, "i"),
                                        ConditionOperand::constant<std::uint32_t>(N));
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // Body kernel produces local scalar "next" (scope 1).
    IOTypeMap bodyType;
    bodyType.outputScalars.push_back({"out", ScalarType::U32});
    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindOutputScalar("out", GraphScalar::ref(ScalarType::U32, "next", 1));

    // End boundary exports local "next" (scope 1) to parent "counter" (scope 0).
    CompiledBoundaryNode exportB;
    exportB.id       = "export";
    exportB.deviceId = "fpga:0";
    exportB.side     = CompiledBoundaryNode::Side::End;
    exportB.scalarCopies.push_back({/*sourceName=*/"next", /*sourceScopeId=*/1,
                                    /*targetName=*/"counter", /*targetScopeId=*/0});

    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->nodes.push_back(exportB);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "counter", 0),
        ConditionOperand::constant<std::uint32_t>(N));
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // Body kernel: input scalar "in" (carried), output scalar "out" (carried).
    IOTypeMap bodyType;
    bodyType.inputScalars.push_back({"in", ScalarType::U32});
    bodyType.outputScalars.push_back({"out", ScalarType::U32});
    CompiledKernelNode bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindInputScalar("in",  GraphScalar::ref(ScalarType::U32, "lin",  1));
    bodyK.ioMap.bindOutputScalar("out", GraphScalar::ref(ScalarType::U32, "lout", 1));

    // Import counter(0) -> lin(1) (Start); export lout(1) -> counter(0) (End).
    CompiledBoundaryNode importB;
    importB.id = "import"; importB.deviceId = "fpga:0";
    importB.side = CompiledBoundaryNode::Side::Start;
    importB.scalarCopies.push_back({/*src*/"counter", 0, /*tgt*/"lin", 1});
    CompiledBoundaryNode exportB;
    exportB.id = "export"; exportB.deviceId = "fpga:0";
    exportB.side = CompiledBoundaryNode::Side::End;
    exportB.scalarCopies.push_back({/*src*/"lout", 1, /*tgt*/"counter", 0});

    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(importB);
    body->nodes.push_back(bodyK);
    body->nodes.push_back(exportB);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::WhileCondition;
    loop.condition = Condition::compare(
        CompareOp::LT, ConditionOperand::scalar(ScalarType::U32, "counter", 0),
        ConditionOperand::constant<std::uint32_t>(N));
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    IOTypeMap bodyType;
    bodyType.inputScalars.push_back({"a", ScalarType::U32});
    bodyType.inputScalars.push_back({"b", ScalarType::U32});
    CompiledKernelNode bodyK;
    bodyK.id = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel = fpgaKernel("bodyK", bodyType);
    bodyK.ioMap.bindInputScalar("a", GraphScalar::ref(ScalarType::U32, "la", 1));
    bodyK.ioMap.bindInputScalar("b", GraphScalar::ref(ScalarType::U32, "lb", 1));

    CompiledBoundaryNode importB;
    importB.id = "import";
    importB.deviceId = "fpga:0";
    importB.side = CompiledBoundaryNode::Side::Start;
    importB.scalarCopies.push_back({/*src*/"pa", 0, /*tgt*/"la", 1});
    importB.scalarCopies.push_back({/*src*/"pb", 0, /*tgt*/"lb", 1});

    auto body = std::make_shared<DGraph>();
    body->deviceId = "fpga:0";
    body->nodes.push_back(importB);
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id = "loop0";
    loop.deviceId = "fpga:0";
    loop.loopKind = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 1);
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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

        FpgaDevice dev("fpga:0", window, [](const std::string& name) -> FpgaKernelLocation {
            if (name == "pred") return {kPredBase, 0};
            if (name == "thenK") return {kThenBase, 0};
            return {kElseBase, 0};
        });

        // Main-line predicate producer with an output scalar "p".
        IOTypeMap predType;
        predType.outputScalars.push_back({"p", ScalarType::U32});
        CompiledKernelNode pred;
        pred.id = "pred"; pred.deviceId = "fpga:0"; pred.kernel = fpgaKernel("pred", predType);
        pred.ioMap.bindOutputScalar("p", GraphScalar::ref(ScalarType::U32, "p"));

        auto mk = [](const char* id, const char* name, std::uint32_t /*base*/) {
            CompiledKernelNode k;
            k.id = id; k.deviceId = "fpga:0"; k.kernel = fpgaKernel(name);
            return k;
        };
        auto thenBody = std::make_shared<DGraph>();
        thenBody->deviceId = "fpga:0";
        thenBody->nodes.push_back(mk("tk", "thenK", kThenBase));
        thenBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
        auto elseBody = std::make_shared<DGraph>();
        elseBody->deviceId = "fpga:0";
        elseBody->nodes.push_back(mk("ek", "elseK", kElseBase));
        elseBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

        DGraph dg;
        dg.deviceId = "fpga:0";
        dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
        dg.nodes.emplace_back(pred);
        CompiledConditionalNode cond;
        cond.id = "cond0"; cond.deviceId = "fpga:0"; cond.dependsOn = {"pred"};
        cond.condition = Condition::compare(CompareOp::GE,
                                            ConditionOperand::scalar(ScalarType::U32, "p"),
                                            ConditionOperand::constant<std::uint32_t>(threshold));
        dg.nodes.emplace_back(cond);
        DGraphChild thenChild;
        thenChild.parentNodeId = "cond0"; thenChild.role = DGraphChildRole::ConditionalThen;
        thenChild.dgraphs.push_back(thenBody);
        dg.childDGraphs.push_back(thenChild);
        DGraphChild elseChild;
        elseChild.parentNodeId = "cond0"; elseChild.role = DGraphChildRole::ConditionalElse;
        elseChild.dgraphs.push_back(elseBody);
        dg.childDGraphs.push_back(elseChild);

        auto plan = dev.compilePlan(dg);
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

    FpgaDevice dev("fpga:0", window, [](const std::string& name) -> FpgaKernelLocation {
        if (name == "pred") return {kPredBase, 0};
        return {kThenBase, 0};
    });

    IOTypeMap predType;
    predType.outputScalars.push_back({"p", ScalarType::U32});
    CompiledKernelNode pred;
    pred.id = "pred"; pred.deviceId = "fpga:0"; pred.kernel = fpgaKernel("pred", predType);
    pred.ioMap.bindOutputScalar("p", GraphScalar::ref(ScalarType::U32, "p"));

    auto thenBody = std::make_shared<DGraph>();
    thenBody->deviceId = "fpga:0";
    thenBody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    std::string prev;
    for (int i = 0; i < 40; ++i) {
        CompiledKernelNode k;
        k.id = "tk" + std::to_string(i);
        k.deviceId = "fpga:0";
        k.kernel = fpgaKernel("thenK");
        if (!prev.empty()) k.dependsOn = {prev};
        prev = k.id;
        thenBody->nodes.emplace_back(k);
    }

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    dg.nodes.emplace_back(pred);
    CompiledConditionalNode cond;
    cond.id = "cond0"; cond.deviceId = "fpga:0"; cond.dependsOn = {"pred"};
    cond.condition = Condition::compare(CompareOp::GE,
                                        ConditionOperand::scalar(ScalarType::U32, "p"),
                                        ConditionOperand::constant<std::uint32_t>(1));
    dg.nodes.emplace_back(cond);
    DGraphChild thenChild;
    thenChild.parentNodeId = "cond0"; thenChild.role = DGraphChildRole::ConditionalThen;
    thenChild.dgraphs.push_back(thenBody);
    dg.childDGraphs.push_back(thenChild);

    auto plan = dev.compilePlan(dg);
    ASSERT_NE(plan, nullptr);
    plan->launch();
    plan->wait();

    EXPECT_EQ(rp1.dispatches(kThenBase), 40u);
}

// The one-path FPGA executor accepts only RP1-executable nodes. Host bridge
// closures must be moved to the CPU DGraph by the compiler and represented on
// the FPGA side as SIGNAL/WAIT rendezvous nodes; a hand-built FPGA DGraph that
// still contains CompiledBridgeOpNode is invalid.
TEST(FpgaControlExecution, FpgaDGraphRejectsHostBridgeNodes) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);

    constexpr std::uint32_t kBodyBase = 0x88010000u;
    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");
    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    CompiledBridgeOpNode input;
    input.id       = "in_bridge";
    input.deviceId = "fpga:0";
    input.side     = CompiledBridgeOpNode::Side::Consumer;
    input.tryReady = [] { return true; };
    dg.nodes.emplace_back(input);

    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 4);
    loop.dependsOn = {"in_bridge"};
    dg.nodes.emplace_back(loop);

    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    EXPECT_THROW(dev.compilePlan(dg), std::logic_error);
}

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
    FpgaDevice dev("fpga:0", window, [](const std::string& name) -> FpgaKernelLocation {
        if (name == "producer") return {kProducerBase, 0};
        return {kBodyBase, 0};
    });

    // Main-line producer kernel with an output scalar.
    IOTypeMap producerType;
    producerType.outputScalars.push_back({"parity", ScalarType::U32});
    CompiledKernelNode producer;
    producer.id       = "prod";
    producer.deviceId = "fpga:0";
    producer.kernel   = fpgaKernel("producer", producerType);
    producer.ioMap.bindOutputScalar("parity", GraphScalar::ref(ScalarType::U32, "parity"));

    CompiledKernelNode bodyK;
    bodyK.id       = "bk";
    bodyK.deviceId = "fpga:0";
    bodyK.kernel   = fpgaKernel("bodyK");
    auto body = std::make_shared<DGraph>();
    body->deviceId     = "fpga:0";
    body->nodes.push_back(bodyK);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    dg.nodes.emplace_back(producer);
    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    loop.dependsOn = {"prod"};
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0";
    child.role         = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

    auto plan = dev.compilePlan(dg);
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

// Phase D1: an FPGA loop whose body carries rendezvous CompiledSignalNode /
// CompiledWaitNode (lowered to RP1_OP_SIGNAL/WAIT in the body bucket) runs the
// depth-1 handshake with a host CPU peer each iteration.  Validates that the
// compiler-shaped rendezvous IR lowers and executes over N iterations.
TEST(FpgaCrossQueue, RendezvousNodesInLoopBodyHandshakeWithCpu) {
    constexpr std::uint32_t N        = 4u;
    constexpr std::uint32_t kReady   = 30u;
    constexpr std::uint32_t kDone    = 31u;
    constexpr std::uint32_t kBodyBase = 0x88010000u;

    std::vector<rp1_signal_slot_t> sig(RP1_MAX_SIGNALS);
    std::memset(sig.data(), 0, sig.size() * sizeof(rp1_signal_slot_t));

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr, sig.data());

    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // Loop body: bodyK -> SIGNAL ready=1 -> WAIT done!=0 -> SIGNAL done=0 (clear).
    CompiledKernelNode bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK");

    CompiledSignalNode sigReady;
    sigReady.id = "sig_ready"; sigReady.deviceId = "fpga:0"; sigReady.dependsOn = {"bk"};
    sigReady.slot = kReady; sigReady.value = 1; sigReady.operation = RP1_SIGOP_SET;

    CompiledWaitNode waitDone;
    waitDone.id = "wait_done"; waitDone.deviceId = "fpga:0"; waitDone.dependsOn = {"sig_ready"};
    waitDone.slot = kDone; waitDone.value = 1; waitDone.conditionOp = RP1_COP_AND_NZ;

    CompiledSignalNode sigClear;
    sigClear.id = "sig_clear"; sigClear.deviceId = "fpga:0"; sigClear.dependsOn = {"wait_done"};
    sigClear.slot = kDone; sigClear.value = 0; sigClear.operation = RP1_SIGOP_SET;

    auto body = std::make_shared<DGraph>();
    body->deviceId = "fpga:0";
    body->nodes.emplace_back(bodyK);
    body->nodes.emplace_back(sigReady);
    body->nodes.emplace_back(waitDone);
    body->nodes.emplace_back(sigClear);
    body->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph dg;
    dg.deviceId = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode loop;
    loop.id = "loop0"; loop.deviceId = "fpga:0";
    loop.loopKind = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, N);
    dg.nodes.emplace_back(loop);
    DGraphChild child;
    child.parentNodeId = "loop0"; child.role = DGraphChildRole::LoopBody;
    child.dgraphs.push_back(body);
    dg.childDGraphs.push_back(child);

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

    auto plan = dev.compilePlan(dg);
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
// handshake (CompiledWaitNode polls a slot, CompiledSignalNode SETs one) inside
// its fixed-count loop, while a real FpgaDevicePlan submits the FPGA producer
// half once and RP1 iterates autonomously.  The two share the same slots via
// the CpuDevice's wired signal accessors (the same wiring Graph::compile sets
// up), proving the CPU queue executes its slice over the BAR rather than
// relaunching child plans against a hand-rolled peer.
TEST(FpgaCrossQueue, CpuDevicePlanRendezvousesWithFpgaLoopOverBar) {
    constexpr std::uint32_t N         = 5u;
    constexpr std::uint32_t kReady    = 28u;
    constexpr std::uint32_t kDone     = 29u;
    constexpr std::uint32_t kBodyBase = 0x88010000u;

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    // No sharedSignals: RP1 uses the window's own signal region, the very slots
    // the CpuDevice reaches through readSignal/writeU32 below.
    FaithfulRp1 rp1(ddr);

    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // --- FPGA producer half: body = bodyK -> SIGNAL ready=1 -> WAIT done!=0
    //     -> SIGNAL done=0 (clear), iterated N times by the LOOP. ---
    CompiledKernelNode bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK");
    CompiledSignalNode sigReady;
    sigReady.id = "sig_ready"; sigReady.deviceId = "fpga:0"; sigReady.dependsOn = {"bk"};
    sigReady.slot = kReady; sigReady.value = 1; sigReady.operation = RP1_SIGOP_SET;
    CompiledWaitNode waitDone;
    waitDone.id = "wait_done"; waitDone.deviceId = "fpga:0"; waitDone.dependsOn = {"sig_ready"};
    waitDone.slot = kDone; waitDone.value = 1; waitDone.conditionOp = RP1_COP_AND_NZ;
    CompiledSignalNode doneClear;
    doneClear.id = "done_clear"; doneClear.deviceId = "fpga:0"; doneClear.dependsOn = {"wait_done"};
    doneClear.slot = kDone; doneClear.value = 0; doneClear.operation = RP1_SIGOP_SET;

    auto fbody = std::make_shared<DGraph>();
    fbody->deviceId = "fpga:0";
    fbody->nodes = {bodyK, sigReady, waitDone, doneClear};
    fbody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph fdg;
    fdg.deviceId = "fpga:0";
    fdg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode floop;
    floop.id = "floop"; floop.deviceId = "fpga:0";
    floop.loopKind = CompiledLoopKind::FixedCount;
    floop.tripCount = bindTripCount(fdg, N);
    fdg.nodes.emplace_back(floop);
    DGraphChild fchild;
    fchild.parentNodeId = "floop"; fchild.role = DGraphChildRole::LoopBody;
    fchild.dgraphs.push_back(fbody);
    fdg.childDGraphs.push_back(fchild);

    // --- CPU consumer half: a real CpuDevice wired to the FPGA window's
    //     signal slots, looping N times: WAIT ready!=0 -> SET ready=0 (clear)
    //     -> count -> SET done=1 (release the FPGA's WAIT). ---
    auto cpuDev = std::make_shared<CpuDevice>("cpu");
    std::atomic<std::uint32_t> cpuCount{0};
    cpuDev->registerKernel(std::make_shared<CountKernel>("counter", cpuCount));
    cpuDev->setSignalAccessors(
        [window](std::uint32_t slot) -> std::uint32_t {
            rp1_signal_slot_t s{};
            window->readSignal(slot, s);
            return s.value;
        },
        [window](std::uint32_t slot, std::uint32_t value) {
            window->writeU32(static_cast<std::uint32_t>(
                                 RP1_DEFAULT_SIG_ARRAY_OFFSET +
                                 slot * sizeof(rp1_signal_slot_t) +
                                 offsetof(rp1_signal_slot_t, value)),
                             value);
        });

    CompiledWaitNode waitReady;
    waitReady.id = "wait_ready"; waitReady.deviceId = "cpu";
    waitReady.slot = kReady; waitReady.value = 1; waitReady.conditionOp = RP1_COP_AND_NZ;
    CompiledSignalNode readyClear;
    readyClear.id = "ready_clear"; readyClear.deviceId = "cpu"; readyClear.dependsOn = {"wait_ready"};
    readyClear.slot = kReady; readyClear.value = 0; readyClear.operation = RP1_SIGOP_SET;
    CompiledKernelNode count;
    count.id = "count"; count.deviceId = "cpu"; count.dependsOn = {"ready_clear"};
    count.kernel = KernelDescriptor{"counter", DeviceType::CPU, std::nullopt, IOTypeMap{}};
    CompiledSignalNode doneSet;
    doneSet.id = "done_set"; doneSet.deviceId = "cpu"; doneSet.dependsOn = {"count"};
    doneSet.slot = kDone; doneSet.value = 1; doneSet.operation = RP1_SIGOP_SET;

    auto cbody = std::make_shared<DGraph>();
    cbody->deviceId = "cpu"; cbody->device = cpuDev;
    cbody->nodes = {waitReady, readyClear, count, doneSet};
    cbody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph cdg;
    cdg.deviceId = "cpu"; cdg.device = cpuDev;
    cdg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode cloop;
    cloop.id = "cloop"; cloop.deviceId = "cpu";
    cloop.loopKind = CompiledLoopKind::FixedCount;
    cloop.tripCount = bindTripCount(cdg, N);
    cdg.nodes.emplace_back(cloop);
    DGraphChild cchild;
    cchild.parentNodeId = "cloop"; cchild.role = DGraphChildRole::LoopBody;
    cchild.dgraphs.push_back(cbody);
    cdg.childDGraphs.push_back(cchild);

    // Launch both queues concurrently; they self-synchronize per iteration.
    auto fpgaPlan = dev.compilePlan(fdg);
    auto cpuPlan  = cpuDev->compilePlan(cdg);
    ASSERT_NE(fpgaPlan, nullptr);
    ASSERT_NE(cpuPlan, nullptr);
    fpgaPlan->launch();
    cpuPlan->launch();
    cpuPlan->wait();
    fpgaPlan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N) << "FPGA loop body ran the wrong count";
    EXPECT_EQ(cpuCount.load(), N) << "CPU queue rendezvoused the wrong count";
}

// A loop whose body is not FPGA-resident (cross-device) is the Phase-2
// cross-queue case; phase-1 lowering rejects it with a clear diagnostic.
TEST_F(FpgaDeviceFixture, LoopWithoutFpgaBodyThrows) {
    FpgaDevice dev("fpga:0", window_,
                   [](const std::string&) { return FpgaKernelLocation{0x88010000u, 0}; });

    DGraph dg;
    dg.deviceId     = "fpga:0";
    dg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    CompiledLoopNode loop;
    loop.id        = "loop0";
    loop.deviceId  = "fpga:0";
    loop.loopKind  = CompiledLoopKind::FixedCount;
    loop.tripCount = bindTripCount(dg, 2);
    dg.nodes.emplace_back(loop);
    // No childDGraphs -> no FPGA body.

    EXPECT_THROW(dev.compilePlan(dg), std::logic_error);
}

// Phase F.3b: a data-dependent cross-device *split* loop runs as two
// rendezvousing queues -- a CPU Authority that drives the loop and broadcasts
// its per-iteration continue/stop decision, and an FPGA Follower whose RP1 LOOP
// reads that broadcast as its exit predicate.  Neither queue is told the count
// up front; the Follower runs exactly as many bodies as the Authority dictates,
// the two staying in lockstep via the ready/ack broadcast handshake.
TEST(FpgaCrossQueue, DataDependentSplitLoopAuthorityDrivesFollower) {
    constexpr std::uint32_t N         = 5u;
    constexpr std::uint32_t kDecision = 16u;
    constexpr std::uint32_t kReady    = 17u;
    constexpr std::uint32_t kAck      = 18u;
    constexpr std::uint32_t kBodyBase = 0x88010000u;

    std::vector<std::byte> backing(kBarSize, std::byte{0});
    DdrView ddr{backing.data()};
    primeAsReady(ddr);
    auto window =
        std::make_shared<fpga::Rp1BarWindow>(backing.data(), backing.size(), kWindowOff);
    FaithfulRp1 rp1(ddr);  // uses the window's own signals (shared with the CPU accessors)

    FpgaDevice dev("fpga:0", window,
                   [](const std::string&) { return FpgaKernelLocation{kBodyBase, 0}; });

    // FPGA Follower: LOOP body = bodyK; lowering injects the broadcast tail
    // handshake + RERUN and sets the LOOP exit predicate to kDecision.
    CompiledKernelNode bodyK;
    bodyK.id = "bk"; bodyK.deviceId = "fpga:0"; bodyK.kernel = fpgaKernel("bodyK");
    auto fbody = std::make_shared<DGraph>();
    fbody->deviceId = "fpga:0";
    fbody->nodes.push_back(bodyK);
    fbody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph fdg;
    fdg.deviceId = "fpga:0";
    fdg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode floop;
    floop.id = "floop"; floop.deviceId = "fpga:0";
    floop.loopKind = CompiledLoopKind::WhileCondition;
    floop.broadcastRole = SplitBroadcastRole::Follower;
    floop.conditionBroadcastSlot = kDecision;
    floop.broadcastReadySlot = kReady;
    floop.broadcastAckSlot = kAck;
    fdg.nodes.emplace_back(floop);
    DGraphChild fchild;
    fchild.parentNodeId = "floop"; fchild.role = DGraphChildRole::LoopBody;
    fchild.dgraphs.push_back(fbody);
    fdg.childDGraphs.push_back(fchild);

    // CPU Authority: drives the loop and broadcasts the decision, wired to the
    // FPGA window's signal slots.  (Fixed count here for a deterministic check;
    // a data-dependent Condition takes the same executeLoopAuthority path.)
    auto cpuDev = std::make_shared<CpuDevice>("cpu");
    std::atomic<std::uint32_t> cpuCount{0};
    cpuDev->registerKernel(std::make_shared<CountKernel>("counter", cpuCount));
    cpuDev->setSignalAccessors(
        [window](std::uint32_t slot) -> std::uint32_t {
            rp1_signal_slot_t s{};
            window->readSignal(slot, s);
            return s.value;
        },
        [window](std::uint32_t slot, std::uint32_t value) {
            window->writeU32(static_cast<std::uint32_t>(
                                 RP1_DEFAULT_SIG_ARRAY_OFFSET +
                                 slot * sizeof(rp1_signal_slot_t) +
                                 offsetof(rp1_signal_slot_t, value)),
                             value);
        });

    CompiledKernelNode count;
    count.id = "count"; count.deviceId = "cpu";
    count.kernel = KernelDescriptor{"counter", DeviceType::CPU, std::nullopt, IOTypeMap{}};
    auto cbody = std::make_shared<DGraph>();
    cbody->deviceId = "cpu"; cbody->device = cpuDev;
    cbody->nodes.push_back(count);
    cbody->scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();

    DGraph cdg;
    cdg.deviceId = "cpu"; cdg.device = cpuDev;
    cdg.scalarValues = std::make_shared<std::map<std::string, std::uint64_t>>();
    CompiledLoopNode cloop;
    cloop.id = "cloop"; cloop.deviceId = "cpu";
    cloop.loopKind = CompiledLoopKind::FixedCount;
    cloop.tripCount = bindTripCount(cdg, N);
    cloop.broadcastRole = SplitBroadcastRole::Authority;
    cloop.conditionBroadcastSlot = kDecision;
    cloop.broadcastReadySlot = kReady;
    cloop.broadcastAckSlot = kAck;
    cdg.nodes.emplace_back(cloop);
    DGraphChild cchild;
    cchild.parentNodeId = "cloop"; cchild.role = DGraphChildRole::LoopBody;
    cchild.dgraphs.push_back(cbody);
    cdg.childDGraphs.push_back(cchild);

    auto fpgaPlan = dev.compilePlan(fdg);
    auto cpuPlan  = cpuDev->compilePlan(cdg);
    ASSERT_NE(fpgaPlan, nullptr);
    ASSERT_NE(cpuPlan, nullptr);
    fpgaPlan->launch();
    cpuPlan->launch();
    cpuPlan->wait();
    fpgaPlan->wait();

    EXPECT_EQ(rp1.dispatches(kBodyBase), N) << "FPGA follower ran the wrong body count";
    EXPECT_EQ(cpuCount.load(), N) << "CPU authority ran the wrong body count";
}
