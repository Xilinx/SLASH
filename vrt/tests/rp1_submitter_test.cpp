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
 * Protocol-v6 submitter tests use a heap-backed BAR and a fake RP1 publisher.
 * The fake commits the result payload, magic, terminal state, and exact
 * graph_done_seq in firmware order so publication and corruption paths can be
 * exercised without vrtd or hardware.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <slash/uapi/rp1_protocol.h>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>

using vrt::graph::fpga::Rp1BarWindow;
using vrt::graph::fpga::Rp1GraphImage;
using vrt::graph::fpga::Rp1GraphOutcome;
using vrt::graph::fpga::Rp1ImageState;
using vrt::graph::fpga::Rp1Submitter;
using vrt::graph::fpga::Rp1TimeoutError;
using vrt::graph::fpga::appendScalarWritePackets;

namespace {

/// Size of the fake BAR mapping containing one 64 MiB RP1 window.
constexpr std::size_t kBarSize = 128ULL << 20;
/// BAR-relative origin of the fake RP1 window.
constexpr std::uint64_t kWindowOffset = 64ULL << 20;

/**
 * @brief Direct firmware-side view of the fake shared DDR window.
 *
 * The view does not own @c base; the fixture's backing vector outlives every
 * fake publisher and host accessor using it.
 */
struct DdrView {
    /// Start of the caller-owned fake BAR mapping.
    std::byte* base = nullptr;

    /// Return the shared RP1 control block.
    rp1_ctrl_t& ctrl() const {
        return *reinterpret_cast<rp1_ctrl_t*>(base + kWindowOffset);
    }

    /// Return the staged RP1 node array.
    rp1_node_t* nodes() const {
        return reinterpret_cast<rp1_node_t*>(
            base + kWindowOffset + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    }

    /// Return the staged RP1 argument buffer.
    std::uint32_t* args() const {
        return reinterpret_cast<std::uint32_t*>(
            base + kWindowOffset + RP1_DEFAULT_ARG_BUF_OFFSET);
    }

    /// Return the shared RP1 signal array.
    rp1_signal_slot_t* signals() const {
        return reinterpret_cast<rp1_signal_slot_t*>(
            base + kWindowOffset + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    }

    /// Return the optional shared trace ring.
    rp1_trace_entry_t* trace() const {
        return reinterpret_cast<rp1_trace_entry_t*>(
            base + kWindowOffset + RP1_DEFAULT_TRACE_OFFSET);
    }
};

/**
 * @brief Publish a complete protocol-v6 readiness contract.
 */
void primeAsReady(DdrView ddr) {
    rp1_ctrl_t& ctrl = ddr.ctrl();
    ctrl.version = RP1_PROTOCOL_VERSION;
    ctrl.capabilities = RP1_REQUIRED_CAPABILITIES;
    ctrl.pdi_ipi_platform_id = 0x51454D55u;
    ctrl.graph_seq = 0u;
    ctrl.graph_done_seq = 0u;
    ctrl.rp1_state = RP1_STATE_READY;
    ctrl.heartbeat = 1u;
    ctrl.magic = RP1_CTRL_MAGIC;
}

/**
 * @brief Wire fields and publication behavior selected for one fake result.
 *
 * Optional sequence/state overrides deliberately create corrupt terminal
 * publications. @c publishDone false models an accepted graph that never
 * reaches the protocol release point.
 */
struct FakeResultSpec {
    /// Commit magic written before graph_done_seq.
    std::uint32_t magic = RP1_GRAPH_RESULT_MAGIC;
    /// Raw graph outcome, including invalid values used by corruption tests.
    std::uint32_t outcome = RP1_GRAPH_RESULT_SUCCESS;
    /// Raw result flags supplied by the fake.
    std::uint32_t flags = 0u;
    /// First terminal error code.
    std::uint32_t errorCode = 0u;
    /// Failing or HALT node.
    std::uint32_t terminalNode = RP1_TERMINAL_ERROR_NODE_NONE;
    /// Failing or HALT opcode.
    std::uint32_t terminalOpcode = RP1_TERMINAL_OPCODE_NONE;
    /// Error-specific primary detail.
    std::uint32_t errorDetail = 0u;
    /// Error-specific auxiliary detail.
    std::uint32_t errorAux = 0u;
    /// Final active-image id.
    std::uint32_t activeImageId = 0u;
    /// Raw final image state.
    std::uint32_t imageState = RP1_IMAGE_STATE_NONE;
    /// Optional successful-operation count override.
    std::optional<std::uint32_t> completedOperations;
    /// Graph work duration in protocol PMU ticks.
    std::uint32_t graphElapsedTicks = 101u;
    /// Full publication duration in protocol PMU ticks.
    std::uint32_t publishElapsedTicks = 123u;
    /// Packed terminal quiescence counters.
    std::uint32_t quiescence = 0u;
    /// Optional result sequence override.
    std::optional<std::uint32_t> resultSequence;
    /// Optional terminal-state override.
    std::optional<std::uint32_t> terminalState;
    /// Delay between terminal state and graph_done_seq publication.
    std::chrono::milliseconds doneDelay{0};
    /// Whether the fake publishes graph_done_seq.
    bool publishDone = true;
};

/**
 * @brief Minimal firmware publisher for one or more sequential graph images.
 *
 * Nodes execute synchronously. SIGNAL side effects and one optional trace entry
 * are modeled; terminal classification comes from @c FakeResultSpec so host
 * result validation can be tested independently from scanner behavior.
 */
class FakeRp1 {
   public:
    /// Start the fake publisher over @p ddr using @p spec for every graph.
    FakeRp1(DdrView ddr, FakeResultSpec spec = {})
        : ddr_(ddr), spec_(std::move(spec)),
          thread_([this] { run(); }) {}

    /// Stop the publisher before its backing mapping is destroyed.
    ~FakeRp1() {
        stop_.store(true, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
    }

    /// Return the number of graph_done_seq values published by this fake.
    std::uint32_t graphsRun() const noexcept {
        return graphsRun_.load(std::memory_order_relaxed);
    }

   private:
    /// Derive the control-block terminal state unless the test overrides it.
    std::uint32_t terminalState() const {
        if (spec_.terminalState) return *spec_.terminalState;
        switch (spec_.outcome) {
            case RP1_GRAPH_RESULT_FAILED: return RP1_STATE_ERROR;
            case RP1_GRAPH_RESULT_HALTED: return RP1_STATE_HALTED;
            default:                      return RP1_STATE_READY;
        }
    }

    /// Execute immediate fake node effects and return the completion count.
    std::uint32_t processGraph() {
        rp1_ctrl_t& ctrl = ddr_.ctrl();
        std::uint32_t completed = 0u;
        ctrl.trace_write_idx = 0u;
        for (std::uint32_t i = 0; i < ctrl.node_count; ++i) {
            const rp1_node_t& node = ddr_.nodes()[i];
            if (rp1_node_get_opcode(&node) == RP1_OP_SIGNAL) {
                const auto& signal = node.payload.signal;
                ddr_.signals()[signal.target_slot].value = signal.value;
                ddr_.signals()[signal.target_slot].last_writer_node = i;
            }
            ++completed;
        }
        if (ctrl.trace_enable != 0u && ctrl.trace_size != 0u) {
            rp1_trace_entry_t& entry = ddr_.trace()[0];
            entry.timestamp = 17u;
            entry.event = RP1_TRACE_GRAPH_DONE;
            entry.node_index = 0xFFFFu;
            entry.aux0 = spec_.outcome;
            entry.aux1 = ctrl.graph_seq;
            ctrl.trace_write_idx = 1u;
        }
        return completed;
    }

    /// Publish one committed result in the firmware's required write order.
    void publishResult(std::uint32_t sequence,
                       std::uint32_t completed) {
        rp1_ctrl_t& ctrl = ddr_.ctrl();
        rp1_graph_result_t& result = ctrl.result;
        result.magic = 0u;
        result.graph_seq =
            spec_.resultSequence.value_or(sequence);
        result.outcome = spec_.outcome;
        result.flags = spec_.flags |
            (ctrl.trace_enable != 0u ? RP1_RESULT_TRACE_ENABLED : 0u);
        result.error_code = spec_.errorCode;
        result.terminal_node = spec_.terminalNode;
        result.terminal_opcode = spec_.terminalOpcode;
        result.error_detail = spec_.errorDetail;
        result.error_aux = spec_.errorAux;
        result.active_image_id = spec_.activeImageId;
        result.image_state = spec_.imageState;
        result.completed_operations =
            spec_.completedOperations.value_or(completed);
        result.graph_elapsed_ticks = spec_.graphElapsedTicks;
        result.publish_elapsed_ticks = spec_.publishElapsedTicks;
        result.trace_write_idx = ctrl.trace_write_idx;
        result.quiescence = spec_.quiescence;

        ctrl.rp1_error_code = spec_.errorCode;
        ctrl.terminal_error_node = spec_.terminalNode;
        ctrl.terminal_error_detail = spec_.errorDetail;
        ctrl.terminal_error_aux = spec_.errorAux;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        result.magic = spec_.magic;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ctrl.rp1_state = terminalState();
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (spec_.doneDelay.count() != 0) {
            std::this_thread::sleep_for(spec_.doneDelay);
        }
        if (spec_.publishDone) {
            ctrl.graph_done_seq = sequence;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            graphsRun_.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    /// Watch graph_seq and publish each newly accepted graph.
    void run() {
        while (!stop_.load(std::memory_order_relaxed)) {
            rp1_ctrl_t& ctrl = ddr_.ctrl();
            if (ctrl.graph_seq != ctrl.graph_done_seq &&
                ctrl.rp1_state != RP1_STATE_ERROR &&
                ctrl.rp1_state != RP1_STATE_HALTED) {
                const std::uint32_t sequence = ctrl.graph_seq;
                ctrl.rp1_state = RP1_STATE_RUNNING;
                const std::uint32_t completed = processGraph();
                publishResult(sequence, completed);
            }
            ctrl.heartbeat = ctrl.heartbeat + 1u;
            std::this_thread::sleep_for(
                std::chrono::microseconds(200));
        }
    }

    /// Shared DDR view; non-owning.
    DdrView ddr_;
    /// Immutable result behavior for this fake instance.
    FakeResultSpec spec_;
    /// Cooperative thread-stop flag.
    std::atomic<bool> stop_{false};
    /// Count of completed result publications.
    std::atomic<std::uint32_t> graphsRun_{0u};
    /// Background firmware-emulation thread.
    std::thread thread_;
};

/**
 * @brief Common heap-backed submitter fixture with a successful fake RP1.
 */
class SubmitterFixture : public ::testing::Test {
   protected:
    /// Allocate the BAR, publish readiness, and start the default fake.
    void SetUp() override {
        backing_.assign(kBarSize, std::byte{0});
        ddr_ = DdrView{backing_.data()};
        primeAsReady(ddr_);
        window_ = std::make_unique<Rp1BarWindow>(
            backing_.data(), backing_.size(), kWindowOffset);
        submitter_ = std::make_unique<Rp1Submitter>(*window_);
        fake_ = std::make_unique<FakeRp1>(ddr_);
    }

    /// Stop the fake before releasing its mapping and host accessors.
    void TearDown() override {
        fake_.reset();
        submitter_.reset();
        window_.reset();
    }

    /// Replace the fake before any graph is accepted in the current test.
    void restartFake(FakeResultSpec spec) {
        fake_.reset();
        fake_ = std::make_unique<FakeRp1>(ddr_, std::move(spec));
    }

    /// Build one valid SIGNAL graph with a cleared output slot.
    Rp1GraphImage makeSignalGraph(
        std::uint32_t slot = 2u,
        std::uint32_t value = 0xDEADBEEFu) {
        Rp1GraphImage image;
        image.nodes.resize(1u);
        rp1_node_t& node = image.nodes.front();
        rp1_node_set_opcode(&node, RP1_OP_SIGNAL);
        rp1_node_set_status(&node, RP1_NODE_PENDING);
        node.barrier_set_mask = 1u;
        node.payload.signal.target_slot =
            static_cast<std::uint8_t>(slot);
        node.payload.signal.value = value;
        node.payload.signal.operation = RP1_SIGOP_SET;
        image.clear_signal_slots.push_back(slot);
        return image;
    }

    /// Build one zero-initialized pending node with @p opcode.
    Rp1GraphImage makeOpcodeGraph(std::uint16_t opcode) {
        Rp1GraphImage image;
        image.nodes.resize(1u);
        rp1_node_set_opcode(&image.nodes.front(), opcode);
        rp1_node_set_status(&image.nodes.front(), RP1_NODE_PENDING);
        return image;
    }

    /// Caller-owned fake BAR storage.
    std::vector<std::byte> backing_;
    /// Firmware-side typed view of @c backing_.
    DdrView ddr_;
    /// Host-side typed BAR accessor.
    std::unique_ptr<Rp1BarWindow> window_;
    /// Submitter under test.
    std::unique_ptr<Rp1Submitter> submitter_;
    /// Active fake firmware publisher.
    std::unique_ptr<FakeRp1> fake_;
};

}  // namespace

TEST_F(SubmitterFixture, EnsureReadyProgramsV6Configuration) {
    ddr_.ctrl()._reserved_cq_size = 64u;
    ddr_.ctrl()._reserved_cq_base_lo = 0x30041000u;
    ddr_.ctrl()._reserved_cq_base_hi = 1u;

    submitter_->ensureReady(std::chrono::milliseconds(500));

    const rp1_ctrl_t& ctrl = ddr_.ctrl();
    EXPECT_EQ(ctrl._reserved_cq_size, 0u);
    EXPECT_EQ(ctrl._reserved_cq_base_lo, 0u);
    EXPECT_EQ(ctrl._reserved_cq_base_hi, 0u);
    EXPECT_EQ(
        ctrl.node_base_lo,
        static_cast<std::uint32_t>(
            RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET));
    EXPECT_EQ(
        ctrl.arg_buf_base_lo,
        static_cast<std::uint32_t>(
            RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET));
    EXPECT_EQ(
        ctrl.sig_array_base_lo,
        static_cast<std::uint32_t>(
            RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET));
    EXPECT_EQ(
        ctrl.trace_base_lo,
        static_cast<std::uint32_t>(
            RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET));
    EXPECT_EQ(ctrl.trace_size, vrt::graph::fpga::kDefaultTraceSize);
    EXPECT_EQ(ctrl.trace_enable, 0u);
}

TEST_F(SubmitterFixture, MissingMagicTimesOut) {
    ddr_.ctrl().magic = 0u;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        Rp1TimeoutError);
}

TEST_F(SubmitterFixture, ProtocolV5FirmwareIsRejected) {
    ddr_.ctrl().version = 5u;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, GraphResultCapabilityIsRequired) {
    ddr_.ctrl().capabilities &= ~RP1_CAP_GRAPH_RESULT;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, CachedReadinessRevalidatesCapabilities) {
    ASSERT_NO_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(500)));
    ddr_.ctrl().capabilities &= ~RP1_CAP_GRAPH_RESULT;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, UnknownPdiPlatformIsRejected) {
    ddr_.ctrl().pdi_ipi_platform_id =
        RP1_PDI_IPI_PLATFORM_UNKNOWN;
    EXPECT_THROW(
        submitter_->ensureReady(std::chrono::milliseconds(20)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, SuccessReturnsTypedResult) {
    FakeResultSpec spec;
    spec.activeImageId = 7u;
    spec.imageState = RP1_IMAGE_STATE_KNOWN;
    spec.completedOperations = 19u;
    spec.graphElapsedTicks = 200u;
    spec.publishElapsedTicks = 240u;
    restartFake(spec);

    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(result.outcome, Rp1GraphOutcome::Success);
    EXPECT_EQ(result.sequence, 1u);
    EXPECT_FALSE(result.terminal.has_value());
    EXPECT_EQ(result.imageState, Rp1ImageState::Known);
    EXPECT_EQ(result.activeImageId, 7u);
    EXPECT_EQ(result.completedOperations, 19u);
    EXPECT_EQ(result.graphElapsedTicks, 200u);
    EXPECT_EQ(result.publishElapsedTicks, 240u);
    EXPECT_EQ(result.quiescence.finiteDone, 0u);
    EXPECT_EQ(result.quiescence.finiteTimeout, 0u);
    EXPECT_EQ(result.quiescence.infinite, 0u);
    EXPECT_EQ(ddr_.signals()[2].value, 0xDEADBEEFu);
    EXPECT_EQ(
        rp1_node_get_status(&ddr_.nodes()[0]),
        RP1_NODE_PENDING);
    EXPECT_EQ(submitter_->lastGraphSeq(), 1u);
    EXPECT_EQ(submitter_->submissionSerial(), 1u);
}

TEST_F(SubmitterFixture, FailedResultIsReturnedWithTerminalRecord) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_FAILED;
    spec.flags = RP1_RESULT_RECOVERY_REQUIRED |
                 RP1_RESULT_EFFECTS_MAY_BE_PARTIAL |
                 RP1_RESULT_UNREACHED_NODES;
    spec.errorCode = RP1_ERR_PDI_FAILED;
    spec.terminalNode = 4u;
    spec.terminalOpcode = RP1_OP_PDI_LOAD;
    spec.errorDetail = 0x1234u;
    spec.errorAux = 0x5678u;
    spec.imageState = RP1_IMAGE_STATE_UNKNOWN;
    spec.quiescence = RP1_QUIESCE_PACK(3u, 0u, 0u);
    restartFake(spec);

    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));

    EXPECT_EQ(result.outcome, Rp1GraphOutcome::Failed);
    EXPECT_FALSE(result.succeeded());
    ASSERT_TRUE(result.terminal.has_value());
    EXPECT_EQ(result.terminal->code, RP1_ERR_PDI_FAILED);
    EXPECT_EQ(result.terminal->node, 4u);
    EXPECT_EQ(result.terminal->opcode,
              static_cast<std::uint32_t>(RP1_OP_PDI_LOAD));
    EXPECT_EQ(result.terminal->detail, 0x1234u);
    EXPECT_EQ(result.terminal->aux, 0x5678u);
    EXPECT_TRUE(result.hasFlags(RP1_RESULT_RECOVERY_REQUIRED));
    EXPECT_EQ(result.imageState, Rp1ImageState::Unknown);
    EXPECT_EQ(result.quiescence.finiteDone, 3u);
    EXPECT_TRUE(submitter_->poisoned());
    EXPECT_THROW(submitter_->clearSignalSlots({2u}), std::runtime_error);
}

TEST_F(SubmitterFixture, InfiniteWorkResultPoisonsLaterSubmission) {
    FakeResultSpec spec;
    spec.flags = RP1_RESULT_INFINITE_WORK_REMAINS;
    restartFake(spec);

    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));

    EXPECT_TRUE(result.succeeded());
    EXPECT_TRUE(result.hasFlags(RP1_RESULT_INFINITE_WORK_REMAINS));
    EXPECT_TRUE(submitter_->poisoned());
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, FiniteTimeoutEvidencePoisonsWithoutHazardFlag) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_FAILED;
    spec.errorCode = RP1_ERR_KERNEL_TIMEOUT;
    spec.terminalNode = 0u;
    spec.terminalOpcode = RP1_OP_KERNEL_DISPATCH;
    spec.quiescence = RP1_QUIESCE_PACK(0u, 1u, 0u);
    restartFake(spec);

    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
    EXPECT_TRUE(submitter_->poisoned());
}

TEST_F(SubmitterFixture, InfiniteQuiescenceRequiresBothHazardFlags) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_FAILED;
    spec.errorCode = RP1_ERR_KERNEL_TIMEOUT;
    spec.terminalNode = 0u;
    spec.terminalOpcode = RP1_OP_KERNEL_DISPATCH;
    spec.flags = RP1_RESULT_RECOVERY_REQUIRED;
    spec.quiescence = RP1_QUIESCE_PACK(0u, 0u, 1u);
    restartFake(spec);

    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
    EXPECT_TRUE(submitter_->poisoned());
}

TEST_F(SubmitterFixture, HaltedResultIsReturnedWithHaltNode) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_HALTED;
    spec.terminalNode = 2u;
    spec.terminalOpcode = RP1_OP_HALT;
    restartFake(spec);

    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));

    EXPECT_EQ(result.outcome, Rp1GraphOutcome::Halted);
    ASSERT_TRUE(result.terminal.has_value());
    EXPECT_EQ(result.terminal->code, 0u);
    EXPECT_EQ(result.terminal->node, 2u);
    EXPECT_EQ(result.terminal->opcode,
              static_cast<std::uint32_t>(RP1_OP_HALT));
}

TEST_F(SubmitterFixture, TerminalStateDoesNotBeatDelayedDonePublication) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_FAILED;
    spec.errorCode = RP1_ERR_KERNEL_TIMEOUT;
    spec.terminalNode = 0u;
    spec.terminalOpcode = RP1_OP_KERNEL_DISPATCH;
    spec.doneDelay = std::chrono::milliseconds(50);
    restartFake(spec);

    const auto start = std::chrono::steady_clock::now();
    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(result.outcome, Rp1GraphOutcome::Failed);
    EXPECT_GE(
        elapsed,
        std::chrono::milliseconds(40));
}

TEST_F(SubmitterFixture, SequenceWrapUsesExactEquality) {
    ddr_.ctrl().graph_seq =
        std::numeric_limits<std::uint32_t>::max();
    ddr_.ctrl().graph_done_seq =
        std::numeric_limits<std::uint32_t>::max();

    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));

    EXPECT_EQ(result.sequence, 0u);
    EXPECT_EQ(ddr_.ctrl().graph_done_seq, 0u);
    EXPECT_EQ(submitter_->lastGraphSeq(), 0u);
}

TEST_F(SubmitterFixture, HostTimeoutPoisonsAllLaterMutation) {
    fake_.reset();

    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(20)),
        Rp1TimeoutError);
    EXPECT_TRUE(submitter_->poisoned());
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(20)),
        std::runtime_error);
    EXPECT_THROW(
        submitter_->clearSignalSlots({2u}),
        std::runtime_error);
}

TEST_F(SubmitterFixture, CorruptResultMagicIsRejected) {
    FakeResultSpec spec;
    spec.magic = 0u;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
    EXPECT_FALSE(submitter_->poisoned());
}

TEST_F(SubmitterFixture, StaleResultSequenceIsRejected) {
    FakeResultSpec spec;
    spec.resultSequence = 99u;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, NonTerminalOutcomeIsRejected) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_NONE;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, OutcomeAndTerminalStateMustAgree) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_SUCCESS;
    spec.terminalState = RP1_STATE_ERROR;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, InvalidImageStateIsRejected) {
    FakeResultSpec spec;
    spec.imageState = 99u;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, KnownImageRequiresNonzeroId) {
    FakeResultSpec spec;
    spec.imageState = RP1_IMAGE_STATE_KNOWN;
    spec.activeImageId = 0u;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, NonKnownImageRejectsActiveId) {
    FakeResultSpec spec;
    spec.imageState = RP1_IMAGE_STATE_UNKNOWN;
    spec.activeImageId = 1u;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, SuccessCannotCarryTerminalErrorCode) {
    FakeResultSpec spec;
    spec.errorCode = RP1_ERR_INVALID_NODE;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, SuccessCannotRequireRecovery) {
    FakeResultSpec spec;
    spec.flags = RP1_RESULT_RECOVERY_REQUIRED;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
    EXPECT_TRUE(submitter_->poisoned());
}

TEST_F(SubmitterFixture, SuccessPreservesUnreachedNodesFlag) {
    FakeResultSpec spec;
    spec.flags = RP1_RESULT_UNREACHED_NODES;
    restartFake(spec);
    const auto result = submitter_->submitAndWait(
        makeSignalGraph(), std::chrono::milliseconds(500));
    EXPECT_TRUE(result.succeeded());
    EXPECT_TRUE(result.hasFlags(RP1_RESULT_UNREACHED_NODES));
}

TEST_F(SubmitterFixture, FailedOutcomeRequiresErrorCode) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_FAILED;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, HaltedOutcomeRequiresHaltOpcode) {
    FakeResultSpec spec;
    spec.outcome = RP1_GRAPH_RESULT_HALTED;
    spec.terminalNode = 0u;
    spec.terminalOpcode = RP1_OP_NOP;
    restartFake(spec);
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
}

TEST_F(SubmitterFixture, OverlappingSubmissionIsRejected) {
    FakeResultSpec spec;
    spec.doneDelay = std::chrono::milliseconds(75);
    restartFake(spec);

    auto first = std::async(std::launch::async, [this] {
        return submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500));
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (ddr_.ctrl().graph_seq == 0u &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_THROW(
        submitter_->submitAndWait(
            makeSignalGraph(), std::chrono::milliseconds(500)),
        std::runtime_error);
    EXPECT_TRUE(first.get().succeeded());
}

TEST_F(SubmitterFixture, EmptyGraphIsRejectedBeforeDoorbell) {
    Rp1GraphImage image;
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
    EXPECT_EQ(submitter_->submissionSerial(), 0u);
}

TEST_F(SubmitterFixture, Exactly1024NodesAreAccepted) {
    static_assert(RP1_MAX_NODES == 1024u);
    Rp1GraphImage image;
    image.nodes.resize(RP1_MAX_NODES);

    const auto result = submitter_->submitAndWait(
        image, std::chrono::milliseconds(500));

    EXPECT_TRUE(result.succeeded());
    EXPECT_EQ(ddr_.ctrl().node_count, 1024u);
}

TEST_F(SubmitterFixture, GraphWith1025NodesIsRejectedBeforeDoorbell) {
    static_assert(RP1_MAX_NODES == 1024u);
    Rp1GraphImage image;
    image.nodes.resize(1025u);
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, AllProtocolV6OpcodesStageWithCompactPayloads) {
    constexpr rp1_opcode_t opcodes[] = {
        RP1_OP_NOP, RP1_OP_WAIT, RP1_OP_SIGNAL,
        RP1_OP_KERNEL_DISPATCH, RP1_OP_SCALAR_WRITE,
        RP1_OP_SCALAR_READ, RP1_OP_SCALAR_COPY, RP1_OP_DMA_COPY,
        RP1_OP_DMA_FILL, RP1_OP_PDI_LOAD, RP1_OP_LOOP, RP1_OP_COND,
        RP1_OP_RERUN, RP1_OP_HALT};
    Rp1GraphImage image;
    image.nodes.resize(std::size(opcodes));
    for (std::size_t i = 0; i < std::size(opcodes); ++i) {
        rp1_node_set_opcode(&image.nodes[i], opcodes[i]);
        rp1_node_set_status(&image.nodes[i], RP1_NODE_PENDING);
    }

    image.nodes[1].payload.wait.condition_signal = 255u;
    image.nodes[1].payload.wait.condition_op = RP1_COP_EQ;
    image.nodes[2].payload.signal.target_slot = 254u;
    image.nodes[2].payload.signal.operation = RP1_SIGOP_SET;
    image.nodes[3].payload.kernel_dispatch.kernel_base_addr =
        0x88010000u;
    image.nodes[3].payload.kernel_dispatch.arg_count = 1u;
    image.arg_buf = {0x10u, 0x1234u};
    image.nodes[4].payload.scalar_write.writes[0] =
        rp1_write_pair_t{0x88010010u, 7u};
    image.nodes[5].payload.scalar_read.source_addr = 0x88010010u;
    image.nodes[5].payload.scalar_read.target_slot = 253u;
    image.nodes[6].payload.scalar_copy.dest_addr = 0x88020010u;
    image.nodes[6].payload.scalar_copy.source_slot = 253u;
    image.nodes[7].payload.dma_copy.length_types =
        rp1_dma_pack(4096u, 0u, 0u);
    image.nodes[8].payload.dma_fill.length = 4u;
    image.nodes[10].payload.loop.body_start = 0u;
    image.nodes[10].payload.loop.body_end = 0u;
    image.nodes[10].payload.loop.condition_signal = 252u;
    image.nodes[10].payload.loop.condition_op = RP1_COP_AND_Z;
    image.nodes[11].payload.cond.body_start = 1u;
    image.nodes[11].payload.cond.body_end = 0u;
    image.nodes[11].payload.cond.condition_signal = 251u;
    image.nodes[11].payload.cond.condition_op = RP1_COP_NE;
    image.nodes[11].payload.cond.bucket_clear_start = 1u;
    image.nodes[11].payload.cond.bucket_clear_end = 0u;
    image.nodes[12].payload.rerun.target_node = 0u;

    EXPECT_NO_THROW(
        submitter_->submitAndWait(
            image, std::chrono::milliseconds(500)));
    ASSERT_EQ(ddr_.ctrl().node_count, std::size(opcodes));
    for (std::size_t i = 0; i < std::size(opcodes); ++i) {
        EXPECT_EQ(rp1_node_get_opcode(&ddr_.nodes()[i]), opcodes[i]);
    }
    EXPECT_EQ(ddr_.nodes()[1].payload.wait.condition_signal, 255u);
    EXPECT_EQ(ddr_.nodes()[10].payload.loop.body_end, 0u);
    EXPECT_EQ(
        rp1_dma_get_length(
            ddr_.nodes()[7].payload.dma_copy.length_types),
        4096u);
}

TEST_F(SubmitterFixture, ScalarWritesSplitWithoutDroppingPairs) {
    const std::vector<rp1_write_pair_t> writes = {
        {0x88010010u, 1u}, {0x88010014u, 2u},
        {0x88010018u, 3u}, {0x8801001Cu, 4u},
        {0x88010020u, 5u}};
    Rp1GraphImage image;
    appendScalarWritePackets(
        image, writes, /*awaitBucket=*/2u, /*awaitMask=*/0x10u,
        /*setBucket=*/3u, /*setMask=*/0x20u);

    ASSERT_EQ(image.nodes.size(), 3u);
    std::vector<rp1_write_pair_t> lowered;
    for (std::size_t i = 0; i < image.nodes.size(); ++i) {
        const rp1_node_t& node = image.nodes[i];
        EXPECT_EQ(rp1_node_get_opcode(&node), RP1_OP_SCALAR_WRITE);
        EXPECT_EQ(node.barrier_await_bucket, 2u);
        EXPECT_EQ(node.barrier_await_mask, 0x10u);
        EXPECT_EQ(node.barrier_set_mask, i == 2u ? 0x20u : 0u);
        for (const rp1_write_pair_t& write :
             node.payload.scalar_write.writes) {
            if (write.addr != 0u) lowered.push_back(write);
        }
    }
    ASSERT_EQ(lowered.size(), writes.size());
    for (std::size_t i = 0; i < writes.size(); ++i) {
        EXPECT_EQ(lowered[i].addr, writes[i].addr);
        EXPECT_EQ(lowered[i].value, writes[i].value);
    }
    EXPECT_NO_THROW(
        submitter_->submitAndWait(
            image, std::chrono::milliseconds(500)));
}

TEST_F(SubmitterFixture, OversizedArgumentBufferIsRejectedBeforeBarMutation) {
    auto image = makeSignalGraph();
    constexpr std::size_t capacityWords =
        (RP1_DEFAULT_SIG_ARRAY_OFFSET - RP1_DEFAULT_ARG_BUF_OFFSET) /
        sizeof(std::uint32_t);
    image.arg_buf.resize(capacityWords + 1u, 0xA5A5A5A5u);
    ddr_.ctrl()._reserved_cq_size = 0x55u;
    ddr_.args()[0] = 0x12345678u;

    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
    EXPECT_EQ(ddr_.ctrl()._reserved_cq_size, 0x55u);
    EXPECT_EQ(ddr_.args()[0], 0x12345678u);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
    EXPECT_EQ(submitter_->submissionSerial(), 0u);
}

TEST_F(SubmitterFixture, InvalidBarrierBucketIsRejected) {
    auto image = makeSignalGraph();
    image.nodes.front().barrier_set_bucket = RP1_MAX_BUCKETS;
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
}

TEST_F(SubmitterFixture, NonPendingInitialStatusIsRejected) {
    auto image = makeSignalGraph();
    rp1_node_set_status(&image.nodes.front(), RP1_NODE_DONE);
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
}

TEST_F(SubmitterFixture, HighestPackedSignalSlotIsAccepted) {
    auto image = makeSignalGraph();
    image.nodes.front().payload.signal.target_slot =
        static_cast<std::uint8_t>(RP1_MAX_SIGNALS - 1u);
    EXPECT_NO_THROW(
        submitter_->submitAndWait(
            image, std::chrono::milliseconds(500)));
}

TEST_F(SubmitterFixture, InvalidClearSlotIsRejected) {
    auto image = makeSignalGraph();
    image.clear_signal_slots.push_back(RP1_MAX_SIGNALS);
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
}

TEST_F(SubmitterFixture, InvalidTraceSizeIsRejectedBeforeDoorbell) {
    auto image = makeSignalGraph();
    image.trace_size_override = 3u;
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::invalid_argument);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, ReservedNodeFlagsAreRejectedByHostValidation) {
    auto image = makeSignalGraph();
    rp1_node_set_flags(&image.nodes.front(), 0x2u);
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
}

TEST_F(SubmitterFixture, ReservedControlBitsAreRejectedByHostValidation) {
    auto image = makeSignalGraph();
    rp1_node_set_control(
        &image.nodes.front(),
        rp1_node_get_control(&image.nodes.front()) |
            RP1_NODE_RESERVED_MASK);
    EXPECT_THROW(
        submitter_->submitAndWait(image),
        std::logic_error);
}

TEST_F(SubmitterFixture, Phase1DmaCopyRejectsUnsupportedEndpoints) {
    auto image = makeOpcodeGraph(RP1_OP_DMA_COPY);
    auto& dma = image.nodes.front().payload.dma_copy;
    dma.src_addr_lo = 0x1000u;
    dma.dst_addr_lo = 0x2000u;
    dma.length_types = rp1_dma_pack(8u, 0u, 0u);

    dma.src_addr_hi = 1u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    dma.src_addr_hi = 0u;

    dma.length_types = rp1_dma_pack(8u, 1u, 0u);
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    dma.length_types = rp1_dma_pack(8u, 0u, 0u);

    dma.src_addr_lo = 0xFFFFFFFCu;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    dma.src_addr_lo = 0x1002u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, Phase1DmaFillRejectsUnsupportedEndpoint) {
    auto image = makeOpcodeGraph(RP1_OP_DMA_FILL);
    auto& dma = image.nodes.front().payload.dma_fill;
    dma.dst_addr_lo = 0x2000u;
    dma.length = 8u;

    dma.dst_addr_hi = 1u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    dma.dst_addr_hi = 0u;

    dma.dst_type = 1u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    dma.dst_type = 0u;

    dma.dst_addr_lo = 0xFFFFFFFCu;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, DispatchControlFlagsAreRejected) {
    auto image = makeOpcodeGraph(RP1_OP_KERNEL_DISPATCH);
    auto& dispatch = image.nodes.front().payload.kernel_dispatch;
    dispatch.kernel_base_addr = 0x88010000u;
    dispatch.ctrl_flags = 1u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, UnknownRerunFlagsAreRejected) {
    auto image = makeOpcodeGraph(RP1_OP_RERUN);
    image.nodes.front().payload.rerun.target_node = 0u;
    image.nodes.front().payload.rerun.rerun_flags = 0x2u;
    EXPECT_THROW(submitter_->submitAndWait(image), std::logic_error);
    EXPECT_EQ(ddr_.ctrl().graph_seq, 0u);
}

TEST_F(SubmitterFixture, TraceCaptureUsesFinalProducerCursor) {
    auto image = makeSignalGraph();
    image.trace_enable = true;
    image.trace_size_override = 8u;

    const auto result = submitter_->submitAndWait(
        image, std::chrono::milliseconds(500));
    const auto trace = submitter_->drainTrace();

    EXPECT_TRUE(result.hasFlags(RP1_RESULT_TRACE_ENABLED));
    EXPECT_EQ(result.traceWriteIndex, 1u);
    EXPECT_EQ(trace.written, 1u);
    EXPECT_FALSE(trace.overflow);
    ASSERT_EQ(trace.entries.size(), 1u);
    EXPECT_EQ(trace.entries.front().event,
              static_cast<std::uint16_t>(RP1_TRACE_GRAPH_DONE));
}

TEST_F(SubmitterFixture, ClearAndReadSignalHelpersRetainBoundsChecks) {
    ddr_.signals()[3].value = 42u;
    submitter_->clearSignalSlots({3u});
    EXPECT_EQ(submitter_->readSignalValue(3u), 0u);
    EXPECT_THROW(
        submitter_->readSignalValue(RP1_MAX_SIGNALS),
        std::logic_error);
}

TEST_F(SubmitterFixture, ClearSignalSlotsValidatesBeforeMutation) {
    ddr_.signals()[2].value = 0xA5A5A5A5u;
    EXPECT_THROW(
        submitter_->clearSignalSlots({2u, RP1_MAX_SIGNALS}),
        std::logic_error);
    EXPECT_EQ(ddr_.signals()[2].value, 0xA5A5A5A5u);
}
