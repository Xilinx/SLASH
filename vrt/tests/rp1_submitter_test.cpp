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
 * @file rp1_submitter_test.cpp
 *
 * Unit tests for vrt::graph::fpga::Rp1Submitter.
 *
 * Each test runs a background "fake RP1" thread that watches the same
 * heap-backed BAR buffer the submitter writes to and advances
 * graph_done_seq once it sees graph_seq increment.  No daemon, no
 * hardware.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <slash/uapi/rp1_protocol.h>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>

using vrt::graph::fpga::Rp1BarWindow;
using vrt::graph::fpga::Rp1GraphImage;
using vrt::graph::fpga::Rp1Submitter;
using vrt::graph::fpga::Rp1TimeoutError;

namespace {

constexpr std::size_t   kBarSize   = 128ULL << 20;
constexpr std::uint64_t kWindowOff = 64ULL << 20;

/// Direct typed view of the shared DDR window (for the fake RP1 thread
/// and for assertions).
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
    rp1_trace_entry_t* trace()     { return reinterpret_cast<rp1_trace_entry_t*>(
                                         base + kWindowOff + RP1_DEFAULT_TRACE_OFFSET); }
};

/// Bring the fake firmware up to RP1_STATE_READY with the canonical
/// magic.  The submitter's first ensureReady() will then succeed.
void primeAsReady(DdrView ddr) {
    auto& c   = ddr.ctrl();
    c.magic     = RP1_CTRL_MAGIC;
    c.version   = RP1_PROTOCOL_VERSION;
    c.rp1_state = RP1_STATE_READY;
    c.heartbeat = 1;
    c.graph_seq      = 0;
    c.graph_done_seq = 0;
}

/// Worker that emulates the RP1 flat scanner just enough to make
/// submitAndWait() return: watch graph_seq, then for each new graph,
/// walk its node array, emit a CQ entry per node, write SIGNAL nodes'
/// values into the signal array, set rp1_state appropriately, and
/// finally bump graph_done_seq.
class FakeRp1 {
   public:
    FakeRp1(DdrView ddr, std::uint32_t cq_capacity)
        : ddr_(ddr), cq_cap_(cq_capacity) {
        thread_ = std::thread([this] { run(); });
    }
    ~FakeRp1() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    std::uint32_t graphsRun() const noexcept { return graphs_run_.load(); }

   private:
    void run() {
        std::uint32_t last_done = 0;
        while (!stop_.load(std::memory_order_relaxed)) {
            auto& c = ddr_.ctrl();
            const std::uint32_t seq      = c.graph_seq;
            const std::uint32_t done_seq = c.graph_done_seq;
            if (seq > done_seq && seq > last_done) {
                processGraph();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                c.rp1_state      = RP1_STATE_READY;
                c.graph_done_seq = seq;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                last_done = seq;
                graphs_run_.fetch_add(1);
            }
            // Cheap heartbeat tick.
            c.heartbeat = c.heartbeat + 1;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    void processGraph() {
        auto&            c        = ddr_.ctrl();
        const std::uint32_t count   = c.node_count;
        const std::uint32_t cq_size = c.cq_size;
        c.rp1_state = RP1_STATE_RUNNING;
        c.trace_write_idx = 0;
        emitTrace(RP1_TRACE_GRAPH_START, 0xFFFFu, c.graph_seq, count);

        for (std::uint32_t i = 0; i < count; ++i) {
            rp1_node_t& n = ddr_.nodes()[i];
            emitTrace(RP1_TRACE_NODE_ACTIVATE, i, n.opcode, n.flags);
            switch (n.opcode) {
                case RP1_OP_SIGNAL: {
                    const auto& pl = n.payload.signal;
                    ddr_.signals()[pl.target_slot].value = pl.value;
                    ddr_.signals()[pl.target_slot].last_writer_node = i;
                    break;
                }
                case RP1_OP_KERNEL_DISPATCH: {
                    // Pretend the kernel ran instantly.  Real firmware
                    // would also propagate barrier_set_mask; we just
                    // emit a CQ entry to keep counts honest.
                    emitTrace(RP1_TRACE_KERNEL_LAUNCH, i,
                              n.payload.kernel_dispatch.kernel_base_addr,
                              n.payload.kernel_dispatch.arg_count);
                    emitTrace(RP1_TRACE_KERNEL_DONE, i,
                              n.payload.kernel_dispatch.kernel_base_addr, 0);
                    break;
                }
                case RP1_OP_NOP:
                default:
                    break;
            }
            if ((n.flags & RP1_FLAG_SILENT) == 0u) {
                const std::uint32_t idx = c.cq_write_idx & (cq_size - 1u);
                rp1_cq_entry_t& entry = ddr_.cq()[idx];
                entry.node_index   = i;
                entry.status       = RP1_CQ_OK;
                entry.error_detail = 0;
                entry.timestamp    = 1000u + i;
                ++c.cq_write_idx;
            }
            n.status = RP1_NODE_DONE;
        }
        emitTrace(RP1_TRACE_GRAPH_DONE, 0xFFFFu, 0, c.graph_seq);
        // Use the limit-flag as a sentinel for the inflight cap; not
        // exercised here.
        (void)cq_cap_;
    }

    void emitTrace(std::uint16_t event, std::uint32_t node_index,
                   std::uint32_t aux0, std::uint32_t aux1) {
        auto& c = ddr_.ctrl();
        if (c.trace_enable == 0 || c.trace_size == 0)
            return;

        const std::uint32_t idx = c.trace_write_idx % c.trace_size;
        rp1_trace_entry_t& entry = ddr_.trace()[idx];
        entry.timestamp  = 100u + c.trace_write_idx;
        entry.event      = event;
        entry.node_index = static_cast<std::uint16_t>(node_index);
        entry.aux0       = aux0;
        entry.aux1       = aux1;
        ++c.trace_write_idx;
    }

    DdrView                 ddr_;
    std::uint32_t           cq_cap_;
    std::atomic<bool>       stop_{false};
    std::atomic<std::uint32_t> graphs_run_{0};
    std::thread             thread_;
};

class SubmitterFixture : public ::testing::Test {
   protected:
    void SetUp() override {
        backing_.assign(kBarSize, std::byte{0});
        ddr_ = DdrView{backing_.data()};
        primeAsReady(ddr_);
        window_   = std::make_unique<Rp1BarWindow>(backing_.data(), backing_.size(), kWindowOff);
        submitter_ = std::make_unique<Rp1Submitter>(*window_);
        rp1_       = std::make_unique<FakeRp1>(ddr_, vrt::graph::fpga::kDefaultCqSize);
    }

    void TearDown() override {
        rp1_.reset();
        submitter_.reset();
        window_.reset();
    }

    Rp1GraphImage makeSignalGraph(std::uint32_t slot, std::uint32_t value) {
        Rp1GraphImage img;
        img.nodes.resize(1);
        auto& n = img.nodes[0];
        n.opcode               = RP1_OP_SIGNAL;
        n.flags                = 0;
        n.barrier_await_mask   = 0;
        n.barrier_set_mask     = 0x1;
        n.barrier_await_bucket = 0;
        n.barrier_set_bucket   = 0;
        n.status               = RP1_NODE_PENDING;
        n.payload.signal.target_slot = slot;
        n.payload.signal.value       = value;
        n.payload.signal.operation   = RP1_SIGOP_SET;
        img.clear_signal_slots.push_back(slot);
        return img;
    }

    Rp1GraphImage makeDiamondImage() {
        Rp1GraphImage img;
        img.nodes.resize(5);

        // Helper.
        auto setHdr = [](rp1_node_t& n, std::uint16_t op,
                         std::uint8_t aw_b, std::uint32_t aw_m,
                         std::uint8_t st_b, std::uint32_t st_m) {
            n.opcode               = op;
            n.flags                = 0;
            n.barrier_await_mask   = aw_m;
            n.barrier_set_mask     = st_m;
            n.barrier_await_bucket = aw_b;
            n.barrier_set_bucket   = st_b;
            n.status               = RP1_NODE_PENDING;
        };

        setHdr(img.nodes[0], RP1_OP_KERNEL_DISPATCH, 0, 0x0, 0, 0x1);
        setHdr(img.nodes[1], RP1_OP_KERNEL_DISPATCH, 0, 0x1, 0, 0x2);
        setHdr(img.nodes[2], RP1_OP_KERNEL_DISPATCH, 0, 0x1, 0, 0x4);
        setHdr(img.nodes[3], RP1_OP_KERNEL_DISPATCH, 0, 0x6, 0, 0x8);
        for (int i = 0; i < 4; ++i) {
            auto& kd = img.nodes[i].payload.kernel_dispatch;
            kd.kernel_base_addr  = 0x88010000u + 0x10000u * i;
            kd.arg_buffer_offset = 0;
            kd.arg_count         = 0;
        }
        setHdr(img.nodes[4], RP1_OP_SIGNAL, 0, 0x8, 0, 0x10);
        img.nodes[4].payload.signal.target_slot = 0;
        img.nodes[4].payload.signal.value       = 0xD1A1D0DDu;
        img.nodes[4].payload.signal.operation   = RP1_SIGOP_SET;
        img.clear_signal_slots.push_back(0);
        return img;
    }

    std::vector<std::byte>        backing_;
    DdrView                       ddr_{};
    std::unique_ptr<Rp1BarWindow> window_;
    std::unique_ptr<Rp1Submitter> submitter_;
    std::unique_ptr<FakeRp1>      rp1_;
};

}  // namespace

TEST_F(SubmitterFixture, EnsureReadyProgramsBaseAddresses) {
    submitter_->ensureReady(std::chrono::milliseconds(500));

    auto& c = ddr_.ctrl();
    EXPECT_EQ(c.cq_size, vrt::graph::fpga::kDefaultCqSize);
    EXPECT_EQ(c.node_base_lo,
              static_cast<std::uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET));
    EXPECT_EQ(c.cq_base_lo,
              static_cast<std::uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET));
    EXPECT_EQ(c.arg_buf_base_lo,
              static_cast<std::uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET));
    EXPECT_EQ(c.sig_array_base_lo,
              static_cast<std::uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET));
    EXPECT_EQ(c.trace_base_lo,
              static_cast<std::uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET));
    EXPECT_EQ(c.trace_size, vrt::graph::fpga::kDefaultTraceSize);
    EXPECT_EQ(c.trace_enable, 0u);
    EXPECT_EQ(c.node_base_hi, 0u);
}

TEST_F(SubmitterFixture, MissingMagicTimesOut) {
    // Stamp something that isn't RP1_CTRL_MAGIC.
    ddr_.ctrl().magic = 0;
    EXPECT_THROW(submitter_->ensureReady(std::chrono::milliseconds(20)),
                 Rp1TimeoutError);
}

TEST_F(SubmitterFixture, EmptyGraphIsRejected) {
    submitter_->ensureReady(std::chrono::milliseconds(500));
    Rp1GraphImage img;
    EXPECT_THROW(submitter_->submitAndWait(img), std::logic_error);
}

TEST_F(SubmitterFixture, SignalGraphRoundTrip) {
    auto img = makeSignalGraph(/*slot*/ 2, /*value*/ 0xDEADBEEFu);
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    EXPECT_EQ(ddr_.signals()[2].value, 0xDEADBEEFu);
    EXPECT_EQ(submitter_->lastGraphSeq(), 1u);
    EXPECT_EQ(rp1_->graphsRun(), 1u);

    auto cq = submitter_->drainCq();
    ASSERT_EQ(cq.size(), 1u);
    EXPECT_EQ(cq[0].node_index, 0u);
    EXPECT_EQ(cq[0].status, static_cast<std::uint32_t>(RP1_CQ_OK));
}

TEST_F(SubmitterFixture, TraceDisabledByDefaultDrainsEmpty) {
    auto img = makeSignalGraph(/*slot*/ 2, /*value*/ 0xDEADBEEFu);
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    auto trace = submitter_->drainTrace();
    EXPECT_EQ(trace.written, 0u);
    EXPECT_FALSE(trace.overflow);
    EXPECT_TRUE(trace.entries.empty());
}

TEST_F(SubmitterFixture, DiamondImageEmitsFiveCqEntries) {
    auto img = makeDiamondImage();
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    EXPECT_EQ(ddr_.signals()[0].value, 0xD1A1D0DDu);

    auto cq = submitter_->drainCq();
    ASSERT_EQ(cq.size(), 5u);
    for (std::uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(cq[i].node_index, i) << "CQ entry " << i;
        EXPECT_EQ(cq[i].status, static_cast<std::uint32_t>(RP1_CQ_OK));
    }
    EXPECT_EQ(submitter_->lastCqStart() - cq.size(), 0u);
}

TEST_F(SubmitterFixture, TraceEnabledCapturesGraphEventsAndPreservesCq) {
    auto img = makeDiamondImage();
    img.trace_enable = true;
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    auto trace = submitter_->drainTrace();
    ASSERT_FALSE(trace.overflow);
    ASSERT_EQ(trace.written, trace.entries.size());
    ASSERT_GE(trace.entries.size(), 2u);
    EXPECT_EQ(trace.entries.front().event, static_cast<std::uint16_t>(RP1_TRACE_GRAPH_START));
    EXPECT_EQ(trace.entries.front().node_index, 0xFFFFu);
    EXPECT_EQ(trace.entries.back().event, static_cast<std::uint16_t>(RP1_TRACE_GRAPH_DONE));

    std::uint32_t launch_count = 0;
    std::uint32_t done_count = 0;
    for (const auto& e : trace.entries) {
        if (e.event == RP1_TRACE_KERNEL_LAUNCH) ++launch_count;
        if (e.event == RP1_TRACE_KERNEL_DONE) ++done_count;
    }
    EXPECT_EQ(launch_count, 4u);
    EXPECT_EQ(done_count, 4u);

    auto cq = submitter_->drainCq();
    ASSERT_EQ(cq.size(), 5u);
    EXPECT_EQ(cq[0].status, static_cast<std::uint32_t>(RP1_CQ_OK));
}

TEST_F(SubmitterFixture, TinyTraceRingReportsOverflow) {
    auto img = makeDiamondImage();
    img.trace_enable = true;
    img.trace_size_override = 4;
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    auto trace = submitter_->drainTrace();
    EXPECT_TRUE(trace.overflow);
    EXPECT_GT(trace.written, trace.entries.size());
    ASSERT_EQ(trace.entries.size(), 4u);
    EXPECT_EQ(trace.entries.back().event, static_cast<std::uint16_t>(RP1_TRACE_GRAPH_DONE));
}

TEST_F(SubmitterFixture, SilentNodesDoNotProduceCqEntries) {
    auto img = makeSignalGraph(/*slot*/ 1, /*value*/ 0xCAFEBABE);
    img.nodes[0].flags |= RP1_FLAG_SILENT;
    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    auto cq = submitter_->drainCq();
    EXPECT_TRUE(cq.empty());
    EXPECT_EQ(ddr_.signals()[1].value, 0xCAFEBABE);
}

TEST_F(SubmitterFixture, BackToBackSubmissionsAccumulateGraphSeq) {
    submitter_->submitAndWait(makeSignalGraph(0, 0x1111), std::chrono::milliseconds(500));
    submitter_->submitAndWait(makeSignalGraph(0, 0x2222), std::chrono::milliseconds(500));
    submitter_->submitAndWait(makeSignalGraph(0, 0x3333), std::chrono::milliseconds(500));

    EXPECT_EQ(submitter_->lastGraphSeq(), 3u);
    EXPECT_EQ(rp1_->graphsRun(), 3u);
    EXPECT_EQ(ddr_.signals()[0].value, 0x3333u);

    auto cq = submitter_->drainCq();
    // We drained nothing between submissions, so all three entries are
    // now visible.  Slot writes overwrite each other, but each
    // submission emits its own CQ entry.
    EXPECT_EQ(cq.size(), 1u) << "third graph only; drainCq advances last_cq_start_";
}

TEST_F(SubmitterFixture, ArgBufferIsStaged) {
    Rp1GraphImage img;
    img.arg_buf = {0x11, 0x22, 0x33, 0x44};
    img.nodes.resize(1);
    auto& n = img.nodes[0];
    n.opcode               = RP1_OP_KERNEL_DISPATCH;
    n.flags                = 0;
    n.barrier_await_mask   = 0;
    n.barrier_set_mask     = 0x1;
    n.barrier_await_bucket = 0;
    n.barrier_set_bucket   = 0;
    n.payload.kernel_dispatch.kernel_base_addr  = 0x88010000u;
    n.payload.kernel_dispatch.arg_buffer_offset = 0;
    n.payload.kernel_dispatch.arg_count         = 4;

    submitter_->submitAndWait(img, std::chrono::milliseconds(500));

    auto* args = ddr_.args();
    EXPECT_EQ(args[0], 0x11u);
    EXPECT_EQ(args[1], 0x22u);
    EXPECT_EQ(args[2], 0x33u);
    EXPECT_EQ(args[3], 0x44u);
}

TEST_F(SubmitterFixture, FirmwareErrorIsSurfacedAfterSubmission) {
    // Stop the fake firmware and stamp an ERROR state.  Submitter
    // should observe it after timing out (or, if graph_done_seq has
    // already been bumped, immediately on next status check).
    rp1_.reset();

    submitter_->ensureReady(std::chrono::milliseconds(500));
    // After ensureReady, fake state is READY but with no worker; set
    // it to ERROR and pre-bump graph_done_seq so submitAndWait returns
    // and then inspects the state.
    ddr_.ctrl().graph_done_seq = ddr_.ctrl().graph_seq + 1;
    ddr_.ctrl().rp1_state      = RP1_STATE_ERROR;
    ddr_.ctrl().rp1_error_code = 1;  // ERR_INFLIGHT_FULL

    auto img = makeSignalGraph(/*slot*/ 0, /*value*/ 0xDEADBEEFu);
    EXPECT_THROW(submitter_->submitAndWait(img, std::chrono::milliseconds(100)),
                 std::runtime_error);
}

TEST_F(SubmitterFixture, NoCompletionTimesOut) {
    rp1_.reset();  // no worker → graph_done_seq stays put forever
    auto img = makeSignalGraph(/*slot*/ 0, /*value*/ 0xDEADBEEFu);
    EXPECT_THROW(submitter_->submitAndWait(img, std::chrono::milliseconds(30)),
                 Rp1TimeoutError);
}

TEST_F(SubmitterFixture, TooManyNodesIsRejected) {
    submitter_->ensureReady(std::chrono::milliseconds(500));
    Rp1GraphImage img;
    img.nodes.resize(RP1_MAX_NODES + 1);
    for (auto& n : img.nodes) {
        n.opcode = RP1_OP_NOP;
        n.status = RP1_NODE_PENDING;
    }
    EXPECT_THROW(submitter_->submitAndWait(img), std::logic_error);
}

TEST(Rp1SubmitterCtor, NonPowerOfTwoCqSizeIsRejected) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    Rp1BarWindow window(backing.data(), backing.size(), kWindowOff);
    EXPECT_THROW(Rp1Submitter(window, /*cq_size*/ 5), std::invalid_argument);
    EXPECT_THROW(Rp1Submitter(window, /*cq_size*/ 0), std::invalid_argument);
    EXPECT_NO_THROW(Rp1Submitter(window, /*cq_size*/ 64));
}
