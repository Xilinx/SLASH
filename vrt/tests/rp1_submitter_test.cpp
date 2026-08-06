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
#include <future>
#include <limits>
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
    c.version      = RP1_PROTOCOL_VERSION;
    c.capabilities = RP1_REQUIRED_CAPABILITIES;
    c.pdi_ipi_platform_id = 0x51454D55u;
    c.rp1_state    = RP1_STATE_READY;
    c.heartbeat    = 1;
    c.graph_seq      = 0;
    c.graph_done_seq = 0;
    c.magic          = RP1_CTRL_MAGIC;
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

    void setCqResult(std::uint32_t status, std::uint32_t detail) {
        cq_status_.store(status, std::memory_order_relaxed);
        cq_detail_.store(detail, std::memory_order_relaxed);
    }

    void setTerminalResult(std::uint32_t state, std::uint32_t code,
                           std::uint32_t node, std::uint32_t detail,
                           std::uint32_t aux,
                           std::chrono::milliseconds publicationDelay = {}) {
        terminal_state_.store(state, std::memory_order_relaxed);
        terminal_code_.store(code, std::memory_order_relaxed);
        terminal_node_.store(node, std::memory_order_relaxed);
        terminal_detail_.store(detail, std::memory_order_relaxed);
        terminal_aux_.store(aux, std::memory_order_relaxed);
        terminal_delay_ms_.store(
            static_cast<std::uint32_t>(publicationDelay.count()),
            std::memory_order_relaxed);
    }

   private:
    void run() {
        while (!stop_.load(std::memory_order_relaxed)) {
            auto& c = ddr_.ctrl();
            const std::uint32_t seq      = c.graph_seq;
            const std::uint32_t done_seq = c.graph_done_seq;
            if (seq != done_seq &&
                c.rp1_state != RP1_STATE_ERROR &&
                c.rp1_state != RP1_STATE_HALTED) {
                processGraph();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const std::uint32_t terminal =
                    terminal_state_.load(std::memory_order_relaxed);
                c.rp1_error_code =
                    terminal_code_.load(std::memory_order_relaxed);
                c.terminal_error_node =
                    terminal_node_.load(std::memory_order_relaxed);
                c.terminal_error_detail =
                    terminal_detail_.load(std::memory_order_relaxed);
                c.terminal_error_aux =
                    terminal_aux_.load(std::memory_order_relaxed);
                c.rp1_state = terminal;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const std::uint32_t delay =
                    terminal_delay_ms_.load(std::memory_order_relaxed);
                if (delay != 0u) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(delay));
                }
                c.graph_done_seq = seq;
                std::atomic_thread_fence(std::memory_order_seq_cst);
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
                while (!stop_.load(std::memory_order_relaxed) &&
                       c.cq_write_idx - c.cq_read_idx == cq_size) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(50));
                }
                if (stop_.load(std::memory_order_relaxed))
                    return;
                const std::uint32_t idx = c.cq_write_idx & (cq_size - 1u);
                rp1_cq_entry_t& entry = ddr_.cq()[idx];
                entry.node_index   = i;
                entry.status       =
                    cq_status_.load(std::memory_order_relaxed);
                entry.error_detail =
                    cq_detail_.load(std::memory_order_relaxed);
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
    std::atomic<std::uint32_t> cq_status_{RP1_CQ_OK};
    std::atomic<std::uint32_t> cq_detail_{0};
    std::atomic<std::uint32_t> terminal_state_{RP1_STATE_READY};
    std::atomic<std::uint32_t> terminal_code_{0};
    std::atomic<std::uint32_t> terminal_node_{
        RP1_TERMINAL_ERROR_NODE_NONE};
    std::atomic<std::uint32_t> terminal_detail_{0};
    std::atomic<std::uint32_t> terminal_aux_{0};
    std::atomic<std::uint32_t> terminal_delay_ms_{0};
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

    Rp1GraphImage makeNopGraph(std::size_t count) {
        Rp1GraphImage img;
        img.nodes.resize(count);
        for (auto& node : img.nodes) {
            node.opcode = RP1_OP_NOP;
            node.status = RP1_NODE_PENDING;
        }
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

TEST_F(SubmitterFixture, WrongProtocolVersionIsRejected) {
    ddr_.ctrl().version = RP1_PROTOCOL_VERSION - 1u;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, MissingRequiredCapabilityIsRejected) {
    ddr_.ctrl().capabilities &=
        ~RP1_CAP_LATCHED_TERMINAL_ERRORS;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, UnknownPlatformConfigIsRejected) {
    ddr_.ctrl().pdi_ipi_platform_id = RP1_PDI_IPI_PLATFORM_UNKNOWN;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
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
    EXPECT_EQ(submitter_->readSignalValue(2), 0xDEADBEEFu);
}

TEST_F(SubmitterFixture, NonOkCompletionEntryIsRejected) {
    rp1_->setCqResult(RP1_CQ_TIMEOUT, 0x1234u);
    submitter_->submitAndWait(
        makeSignalGraph(2, 0xDEADBEEFu),
        std::chrono::milliseconds(500));

    EXPECT_THROW(
        {
            auto cq = submitter_->drainCq();
            (void)cq;
        },
        std::runtime_error);
    EXPECT_EQ(ddr_.ctrl().cq_read_idx, ddr_.ctrl().cq_write_idx);
}

TEST_F(SubmitterFixture, RawDrainPreservesLaterErrorEvidence) {
    rp1_->setCqResult(RP1_CQ_TIMEOUT, 0x1234u);
    submitter_->submitAndWait(
        makeSignalGraph(2, 0xDEADBEEFu),
        std::chrono::milliseconds(500));

    const auto cq = submitter_->drainCqRaw();
    ASSERT_EQ(cq.size(), 1u);
    EXPECT_EQ(
        cq.front().status,
        static_cast<std::uint32_t>(RP1_CQ_TIMEOUT));
    EXPECT_EQ(cq.front().error_detail, 0x1234u);
    EXPECT_THROW(
        Rp1Submitter::validateCq(cq),
        std::runtime_error);
}

TEST_F(SubmitterFixture, CompletionQueueOverflowIsRejected) {
    submitter_->submitAndWait(
        makeSignalGraph(2, 0xDEADBEEFu),
        std::chrono::milliseconds(500));
    ddr_.ctrl().cq_write_idx =
        ddr_.ctrl().cq_write_idx +
        vrt::graph::fpga::kDefaultCqSize + 1u;

    EXPECT_THROW(
        {
            auto cq = submitter_->drainCq();
            (void)cq;
        },
        std::runtime_error);
}

TEST_F(SubmitterFixture, CompletionQueueBackpressureDrainsIncrementally) {
    rp1_.reset();
    submitter_.reset();
    primeAsReady(ddr_);
    submitter_ = std::make_unique<Rp1Submitter>(*window_, 4u);
    rp1_ = std::make_unique<FakeRp1>(ddr_, 4u);

    rp1_cq_entry_t& canary = ddr_.cq()[4];
    canary.node_index = 0xA1A2A3A4u;
    canary.status = 0xB1B2B3B4u;
    canary.error_detail = 0xC1C2C3C4u;
    canary.timestamp = 0xD1D2D3D4u;

    submitter_->submitAndWait(
        makeNopGraph(20), std::chrono::milliseconds(500));
    const auto cq = submitter_->drainCq();

    ASSERT_EQ(cq.size(), 20u);
    for (std::uint32_t i = 0; i < cq.size(); ++i) {
        EXPECT_EQ(cq[i].node_index, i);
    }
    EXPECT_EQ(canary.node_index, 0xA1A2A3A4u);
    EXPECT_EQ(canary.status, 0xB1B2B3B4u);
    EXPECT_EQ(canary.error_detail, 0xC1C2C3C4u);
    EXPECT_EQ(canary.timestamp, 0xD1D2D3D4u);
}

TEST_F(SubmitterFixture, CompletionQueueCursorWrapIsLossless) {
    rp1_.reset();
    submitter_.reset();
    ddr_.ctrl().cq_write_idx =
        std::numeric_limits<std::uint32_t>::max() - 1u;
    ddr_.ctrl().cq_read_idx = ddr_.ctrl().cq_write_idx;
    submitter_ = std::make_unique<Rp1Submitter>(*window_, 4u);
    rp1_ = std::make_unique<FakeRp1>(ddr_, 4u);

    submitter_->submitAndWait(
        makeNopGraph(3), std::chrono::milliseconds(500));
    const auto cq = submitter_->drainCq();

    ASSERT_EQ(cq.size(), 3u);
    EXPECT_EQ(cq[0].node_index, 0u);
    EXPECT_EQ(cq[1].node_index, 1u);
    EXPECT_EQ(cq[2].node_index, 2u);
    EXPECT_EQ(ddr_.ctrl().cq_write_idx, 1u);
    EXPECT_EQ(ddr_.ctrl().cq_read_idx, 1u);
}

TEST_F(SubmitterFixture, GraphSequenceWrapUsesEquality) {
    rp1_.reset();
    submitter_.reset();
    ddr_.ctrl().graph_seq = std::numeric_limits<std::uint32_t>::max();
    ddr_.ctrl().graph_done_seq = ddr_.ctrl().graph_seq;
    submitter_ = std::make_unique<Rp1Submitter>(*window_);
    rp1_ = std::make_unique<FakeRp1>(
        ddr_, vrt::graph::fpga::kDefaultCqSize);

    submitter_->submitAndWait(
        makeNopGraph(1), std::chrono::milliseconds(500));
    EXPECT_EQ(submitter_->lastGraphSeq(), 0u);
    EXPECT_EQ(ddr_.ctrl().graph_done_seq, 0u);
    EXPECT_EQ(submitter_->drainCq().size(), 1u);
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

TEST_F(SubmitterFixture,
       BackToBackSubmissionsRetainOnlyLatestCqEvidence) {
    submitter_->submitAndWait(makeSignalGraph(0, 0x1111), std::chrono::milliseconds(500));
    submitter_->submitAndWait(makeSignalGraph(0, 0x2222), std::chrono::milliseconds(500));
    submitter_->submitAndWait(makeSignalGraph(0, 0x3333), std::chrono::milliseconds(500));

    EXPECT_EQ(submitter_->lastGraphSeq(), 3u);
    EXPECT_EQ(rp1_->graphsRun(), 3u);
    EXPECT_EQ(ddr_.signals()[0].value, 0x3333u);

    auto cq = submitter_->drainCq();
    ASSERT_EQ(cq.size(), 1u);
    EXPECT_EQ(cq.front().node_index, 0u);
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
    n.payload.kernel_dispatch.arg_count         = 2;

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

TEST_F(SubmitterFixture, TerminalErrorSurfacesBeforeDoneWithFullRecord) {
    rp1_->setCqResult(RP1_CQ_ERROR, 0x80002001u);
    rp1_->setTerminalResult(
        RP1_STATE_ERROR,
        RP1_ERR_PDI_FAILED | RP1_ERR_RECOVERY_REQUIRED,
        0u, 0x80002001u, 0xDEADCAFEu,
        std::chrono::milliseconds(200));

    try {
        submitter_->submitAndWait(
            makeSignalGraph(2, 0xDEADBEEFu),
            std::chrono::milliseconds(500));
        FAIL() << "terminal firmware error was not surfaced";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("node=0"), std::string::npos);
        EXPECT_NE(message.find("detail=2147491841"), std::string::npos);
        EXPECT_NE(message.find("aux=3735931646"), std::string::npos);
        EXPECT_NE(message.find("recovery_required=1"), std::string::npos);
    }

    EXPECT_EQ(ddr_.ctrl().rp1_state, RP1_STATE_ERROR);
    EXPECT_NE(ddr_.ctrl().graph_done_seq, submitter_->lastGraphSeq())
        << "host should observe terminal state before delayed done publication";
    const auto evidence = submitter_->drainCqRaw();
    ASSERT_EQ(evidence.size(), 1u);
    EXPECT_EQ(evidence[0].error_detail, 0x80002001u);

    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(2, 1u), std::chrono::milliseconds(50)),
        std::runtime_error);
    EXPECT_EQ(rp1_->graphsRun(), 0u)
        << "terminal fake firmware has not accepted a later graph";
}

TEST_F(SubmitterFixture, NoCompletionTimesOut) {
    rp1_.reset();  // no worker → graph_done_seq stays put forever
    auto img = makeSignalGraph(/*slot*/ 0, /*value*/ 0xDEADBEEFu);
    EXPECT_THROW(submitter_->submitAndWait(img, std::chrono::milliseconds(30)),
                 Rp1TimeoutError);
    EXPECT_TRUE(submitter_->poisoned());
}

TEST_F(SubmitterFixture, OverlappingSubmissionIsRejected) {
    rp1_.reset();
    auto first = std::async(
        std::launch::async, [&] {
            submitter_->submitAndWait(
                makeSignalGraph(0, 0x1111u),
                std::chrono::milliseconds(200));
        });

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (submitter_->submissionSerial() == 0u &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(submitter_->submissionSerial(), 1u);

    try {
        submitter_->submitAndWait(
            makeSignalGraph(0, 0x2222u),
            std::chrono::milliseconds(30));
        FAIL() << "overlapping submission was accepted";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(
                "submission is already active"),
            std::string::npos);
    }
    EXPECT_THROW(first.get(), Rp1TimeoutError);
}

TEST_F(SubmitterFixture,
       LateCqAfterTimeoutCannotCrossSubmissionBoundary) {
    rp1_.reset();
    auto first =
        makeSignalGraph(/*slot*/ 0, /*value*/ 0x1111u);
    EXPECT_THROW(
        submitter_->submitAndWait(
            first, std::chrono::milliseconds(30)),
        Rp1TimeoutError);
    ASSERT_TRUE(submitter_->poisoned());
    const std::uint32_t timedOutSequence =
        ddr_.ctrl().graph_seq;

    const std::uint32_t write = ddr_.ctrl().cq_write_idx;
    rp1_cq_entry_t& late =
        ddr_.cq()[write &
                  (vrt::graph::fpga::kDefaultCqSize - 1u)];
    late.node_index = 0u;
    late.status = RP1_CQ_OK;
    late.error_detail = 0xA11CEu;
    late.timestamp = 1234u;
    ddr_.ctrl().cq_write_idx = write + 1u;

    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(0, 0x2222u),
            std::chrono::milliseconds(30)),
        std::runtime_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, timedOutSequence)
        << "a poisoned submitter must not ring a second doorbell";

    const auto evidence = submitter_->drainCqRaw();
    ASSERT_EQ(evidence.size(), 1u);
    EXPECT_EQ(evidence.front().error_detail, 0xA11CEu);
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

TEST_F(SubmitterFixture, EverySignalBearingPacketValidatesItsSlot) {
    const std::vector<std::uint16_t> opcodes = {
        RP1_OP_SIGNAL,
        RP1_OP_WAIT,
        RP1_OP_SCALAR_READ,
        RP1_OP_SCALAR_COPY,
        RP1_OP_LOOP,
        RP1_OP_COND,
    };
    for (std::uint16_t opcode : opcodes) {
        Rp1GraphImage img;
        img.nodes.resize(1);
        rp1_node_t& node = img.nodes[0];
        node.opcode = opcode;
        node.status = RP1_NODE_PENDING;
        switch (opcode) {
            case RP1_OP_SIGNAL:
                node.payload.signal.target_slot = RP1_MAX_SIGNALS;
                node.payload.signal.operation = RP1_SIGOP_SET;
                break;
            case RP1_OP_WAIT:
                node.payload.wait.condition_signal = RP1_MAX_SIGNALS;
                node.payload.wait.condition_op = RP1_COP_EQ;
                break;
            case RP1_OP_SCALAR_READ:
                node.payload.scalar_read.target_slot = RP1_MAX_SIGNALS;
                break;
            case RP1_OP_SCALAR_COPY:
                node.payload.scalar_copy.source_slot = RP1_MAX_SIGNALS;
                break;
            case RP1_OP_LOOP:
                node.payload.loop.condition_signal = RP1_MAX_SIGNALS;
                node.payload.loop.condition_op = RP1_COP_EQ;
                node.payload.loop.body_start = 0;
                node.payload.loop.body_end = 0;
                break;
            case RP1_OP_COND:
                node.payload.cond.condition_signal = RP1_MAX_SIGNALS;
                node.payload.cond.condition_op = RP1_COP_EQ;
                node.payload.cond.body_start = 1;
                node.payload.cond.body_end = 0;
                node.payload.cond.bucket_clear_start = 1;
                node.payload.cond.bucket_clear_end = 0;
                break;
            default:
                break;
        }
        EXPECT_THROW(
            submitter_->submitAndWait(
                img, std::chrono::milliseconds(50)),
            std::logic_error)
            << "opcode " << opcode;
    }
    EXPECT_EQ(rp1_->graphsRun(), 0u);
}

TEST_F(SubmitterFixture, SilentPdiIsRejectedWithoutActivation) {
    Rp1GraphImage img;
    img.nodes.resize(1);
    img.nodes[0].opcode = RP1_OP_PDI_LOAD;
    img.nodes[0].flags = RP1_FLAG_SILENT;
    img.nodes[0].status = RP1_NODE_PENDING;
    EXPECT_THROW(submitter_->submitAndWait(img), std::logic_error);
    EXPECT_EQ(rp1_->graphsRun(), 0u);
}

TEST_F(SubmitterFixture, OversizeCqOverrideIsRejected) {
    auto img = makeSignalGraph(0, 1u);
    img.cq_size_override = RP1_MAX_CQ_ENTRIES * 2u;
    EXPECT_THROW(submitter_->submitAndWait(img), std::invalid_argument);
}

TEST(Rp1SubmitterCtor, NonPowerOfTwoCqSizeIsRejected) {
    std::vector<std::byte> backing(kBarSize, std::byte{0});
    Rp1BarWindow window(backing.data(), backing.size(), kWindowOff);
    EXPECT_THROW(Rp1Submitter(window, /*cq_size*/ 5), std::invalid_argument);
    EXPECT_THROW(Rp1Submitter(window, /*cq_size*/ 0), std::invalid_argument);
    EXPECT_THROW(
        Rp1Submitter(window, /*cq_size*/ RP1_MAX_CQ_ENTRIES + 1u),
        std::invalid_argument);
    EXPECT_NO_THROW(Rp1Submitter(window, /*cq_size*/ 64));
    EXPECT_NO_THROW(Rp1Submitter(window, RP1_MAX_CQ_ENTRIES));
}
