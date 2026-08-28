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
 * @file rp1_submitter.hpp
 * @brief Rp1Submitter — submit and wait on a single RP1 graph image.
 *
 * `Rp1Submitter` is the second-from-the-bottom layer of the FPGA graph
 * backend. It takes a fully realised graph image (already-laid-out
 * `rp1_node_t` packets + a packed argument buffer + a list of signal
 * slots to zero) and:
 *
 *   1. On first use, waits for the firmware to publish
 *      `magic == RP1_CTRL_MAGIC` and `rp1_state == READY`, then writes
 *      the node, argument, signal, and trace base addresses at the
 *      recommended `RP1_DEFAULT_*_OFFSET` layout.
 *   2. For each `submitAndWait()`:
 *        - Copies the node array, arg buffer, and signal clears into DDR.
 *        - Memory-fences, bumps `graph_seq` by one, memory-fences again.
 *        - Polls `graph_done_seq` by exact sequence equality.
 *        - Reads and validates the committed protocol-v6 graph result.
 *
 * The submitter knows nothing about graphs, kernels, or VRT — it is a
 * mechanical adapter between a fully-realised RP1 graph image and the
 * BAR window. Higher-level code (FpgaDevice in phase 2) is responsible
 * for assembling the `Rp1GraphImage`.
 *
 * Not generally thread-safe: overlapping submissions are rejected, and other
 * operations must not race a submission. Multiple submitters against the same
 * Rp1BarWindow are also unsafe; only one client should "own" the RP1 at a
 * time. Multi-tenancy is deferred to a later phase.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP
#define VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/graph/device/fpga/rp1_bar_window.hpp>

namespace vrt::graph::fpga {

/**
 * @brief Fully-realised graph ready for submission to RP1.
 */
struct Rp1GraphImage {
    /// Node packets, contiguous, in flat scanner order.
    /// Must be ≤ @c RP1_MAX_NODES.
    std::vector<rp1_node_t> nodes;

    /// Packed 32-bit argument words referenced by KERNEL_DISPATCH
    /// payloads via `arg_buffer_offset`.  May be empty.
    std::vector<std::uint32_t> arg_buf;

    /// Signal slot indices that the submitter should zero before
    /// bumping `graph_seq` (typically: the sentinel slot + every slot
    /// the graph plans to write). Clearing the sentinel before peer
    /// queues start distinguishes this launch from stale completion
    /// state left by an earlier graph.
    std::vector<std::uint32_t> clear_signal_slots;

    /// Enable RP1 firmware trace-ring writes for this submission.
    bool trace_enable = false;

    /// Optional override of trace_size. Zero uses kDefaultTraceSize.
    std::uint32_t trace_size_override = 0;
};

/**
 * @brief Append a complete SCALAR_WRITE operation as compact packets.
 *
 * Protocol v6 carries at most @c RP1_SCALAR_WRITE_MAX pairs per packet.
 * This helper emits enough contiguous packets for every pair, applies
 * @p awaitBucket / @p awaitMask to the whole sequence, and publishes
 * @p setBucket / @p setMask only from the final packet. The flat RP1 scanner
 * therefore performs every write before dependent work can activate.
 *
 * @throws std::logic_error for an empty list, a zero/unaligned address,
 *         an invalid bucket, or a sequence that would exceed
 *         @c RP1_MAX_NODES. The image is unchanged on error.
 */
void appendScalarWritePackets(
    Rp1GraphImage& image,
    const std::vector<rp1_write_pair_t>& writes,
    std::uint8_t awaitBucket, std::uint32_t awaitMask,
    std::uint8_t setBucket, std::uint32_t setMask);

/**
 * @brief Default optional trace-ring size programmed by Rp1Submitter.
 */
constexpr std::uint32_t kDefaultTraceSize = 256u;

/**
 * @brief Valid terminal outcomes returned by protocol-v6 firmware.
 */
enum class Rp1GraphOutcome : std::uint32_t {
    /// Every reachable finite operation completed without a fatal error.
    Success = RP1_GRAPH_RESULT_SUCCESS,
    /// Firmware stopped activation after the first fatal graph error.
    Failed = RP1_GRAPH_RESULT_FAILED,
    /// An explicit @c RP1_OP_HALT terminated the graph.
    Halted = RP1_GRAPH_RESULT_HALTED,
};

/**
 * @brief Firmware's final knowledge of the programmed user image.
 */
enum class Rp1ImageState : std::uint32_t {
    /// No image identity has been established by RP1.
    None = RP1_IMAGE_STATE_NONE,
    /// @c activeImageId names the image RP1 knows is installed.
    Known = RP1_IMAGE_STATE_KNOWN,
    /// A failed or timed-out reconfiguration made the image indeterminate.
    Unknown = RP1_IMAGE_STATE_UNKNOWN,
};

/**
 * @brief Terminal-node record carried by FAILED and HALTED results.
 *
 * HALTED uses a zero @c code and identifies its explicit HALT packet through
 * @c node and @c opcode. FAILED uses the first-error-wins firmware record.
 */
struct Rp1TerminalError {
    /// First terminal @c RP1_ERR_* code, or zero for explicit HALT.
    std::uint32_t code = 0;
    /// Failing or HALT node, or @c RP1_TERMINAL_ERROR_NODE_NONE.
    std::uint32_t node = RP1_TERMINAL_ERROR_NODE_NONE;
    /// Terminal opcode, or @c RP1_TERMINAL_OPCODE_NONE when unavailable.
    std::uint32_t opcode = RP1_TERMINAL_OPCODE_NONE;
    /// Error-specific primary detail.
    std::uint32_t detail = 0;
    /// Error-specific auxiliary detail.
    std::uint32_t aux = 0;
};

/**
 * @brief Counts recorded while firmware quiesces in-flight kernels.
 */
struct Rp1Quiescence {
    /// Finite kernels that completed during terminal quiescence.
    std::uint32_t finiteDone = 0;
    /// Finite kernels that timed out during terminal quiescence.
    std::uint32_t finiteTimeout = 0;
    /// Infinite kernels that remain active after terminal publication.
    std::uint32_t infinite = 0;
};

/**
 * @brief Validated, sequence-tagged terminal result for one graph.
 *
 * This host-owned value contains no volatile BAR references and remains valid
 * after another graph is submitted. FAILED and HALTED are determinate return
 * values; transport/protocol corruption and host-side timeouts are exceptions.
 */
struct Rp1GraphResult {
    /// Exact graph sequence accepted and completed by firmware.
    std::uint32_t sequence = 0;
    /// Terminal classification for the graph.
    Rp1GraphOutcome outcome = Rp1GraphOutcome::Success;
    /// Raw @c RP1_RESULT_* bit mask.
    std::uint32_t flags = 0;
    /// First terminal record for FAILED or HALTED; absent on SUCCESS.
    std::optional<Rp1TerminalError> terminal;
    /// Final known image id, or zero when @c imageState is not Known.
    std::uint32_t activeImageId = 0;
    /// Firmware's final image-identity state.
    Rp1ImageState imageState = Rp1ImageState::None;
    /// Number of successful operation executions, including loop repeats.
    std::uint32_t completedOperations = 0;
    /// PMU ticks spent executing graph work through GRAPH_DONE.
    std::uint32_t graphElapsedTicks = 0;
    /// PMU ticks through final trace flush and result preparation.
    std::uint32_t publishElapsedTicks = 0;
    /// Final monotonic trace producer cursor.
    std::uint32_t traceWriteIndex = 0;
    /// Decoded terminal quiescence counters.
    Rp1Quiescence quiescence;

    /// Return true only for @c Rp1GraphOutcome::Success.
    bool succeeded() const noexcept {
        return outcome == Rp1GraphOutcome::Success;
    }

    /// Return true when every bit in @p mask is present in @c flags.
    bool hasFlags(std::uint32_t mask) const noexcept {
        return (flags & mask) == mask;
    }
};

/**
 * @brief Trace entries captured after an RP1 graph submission.
 */
struct Rp1TraceCapture {
    /// Readable trace entries, in chronological order.
    std::vector<rp1_trace_entry_t> entries;

    /// Firmware's raw trace_write_idx value after graph completion.
    std::uint32_t written = 0;

    /// True when the firmware wrote more entries than fit in the ring.
    bool overflow = false;
};

/**
 * @brief Polled-completion timeouts the firmware reasonably honours.
 */
constexpr std::chrono::milliseconds kDefaultReadyTimeout  {1000};
constexpr std::chrono::milliseconds kDefaultSubmitTimeout {3000};

/**
 * @brief Error thrown when a wait operation times out.
 */
class Rp1TimeoutError : public std::runtime_error {
   public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Single-flight protocol-v6 graph submitter over one RP1 BAR window.
 *
 * The referenced window must outlive the submitter. A post-doorbell timeout or
 * a terminal result reporting recovery-required or remaining infinite work
 * permanently poisons the object; no later shared-state mutation is allowed.
 */
class Rp1Submitter {
   public:
    /// Bind one submitter to the exclusively owned RP1 BAR window @p window.
    explicit Rp1Submitter(Rp1BarWindow& window);

    /**
     * @brief Wait for the firmware's READY signal and program the
     *        control-block base addresses on first use.
     *
     * Subsequent calls are cheap and idempotent (just re-verify state).
     * Throws @c Rp1TimeoutError if magic doesn't appear or state
     * doesn't reach READY in time.
     */
    void ensureReady(std::chrono::milliseconds timeout = kDefaultReadyTimeout);

    /**
     * @brief Clear signal slots before graph launch.
     *
     * Used by graph orchestration to zero rendezvous slots synchronously before
     * any peer queue starts producing signals. submitAndWait() still clears the
     * image slots for direct/standalone callers that do not use prepareLaunch().
     * The early clear is what makes a later sentinel value proof that every
     * participating queue reached the current launch's terminal node.
     */
    void clearSignalSlots(const std::vector<std::uint32_t>& slots);

    /**
     * @brief Read one signal slot after graph completion.
     */
    std::uint32_t readSignalValue(std::uint32_t slot) const;

    /**
     * @brief Submit @p image and block until graph_done_seq catches up.
     *
     * Calls @c ensureReady() if it hasn't been called yet.
     *
     * FAILED and HALTED are returned as determinate terminal results. The
     * method throws only when host validation fails, BAR transport is corrupt,
     * or @p timeout elapses without exact-sequence completion.
     *
     * @return A validated SUCCESS, FAILED, or HALTED result for this sequence.
     * @throws Rp1TimeoutError if @p timeout elapses without completion.
     * @throws std::logic_error if @p image violates the protocol contract.
     * @throws std::runtime_error if firmware publication is inconsistent.
     */
    Rp1GraphResult submitAndWait(
        const Rp1GraphImage& image,
        std::chrono::milliseconds timeout = kDefaultSubmitTimeout);

    /**
     * @brief Read trace entries from the most recent @c submitAndWait().
     *
     * RP1 resets @c trace_write_idx at graph start, so this drains the
     * per-submission range @c [0, trace_write_idx).
     */
    Rp1TraceCapture drainTrace();

    /**
     * @brief Sequence number of the most recently submitted graph.
     *
     * Useful for diagnostics; matches the value of @c graph_seq the
     * submitter wrote to the control block.
     */
    std::uint32_t lastGraphSeq() const noexcept { return last_graph_seq_; }

    /// Host-local count incremented only after a graph doorbell is written.
    std::uint64_t submissionSerial() const noexcept {
        return submission_serial_.load(
            std::memory_order_acquire);
    }

    /**
     * @brief True when the completed session cannot safely be reused.
     *
     * A timeout, recovery-required result, or remaining infinite work poisons
     * the submitter. Callers must reset/recover the device and construct a new
     * submitter before issuing another mutation.
     */
    bool poisoned() const noexcept {
        return poisoned_.load(std::memory_order_acquire);
    }

   private:
    /// Non-owning BAR accessor supplied at construction.
    Rp1BarWindow* window_;
    /// True after the current firmware boot passed readiness validation.
    bool          ready_       = false;
    /// Exact sequence written by the most recent accepted host submission.
    std::uint32_t last_graph_seq_ = 0;
    /// Ring size used to decode the most recent optional trace capture.
    std::uint32_t last_trace_size_ = kDefaultTraceSize;
    /// Host-local count advanced immediately after each graph doorbell.
    std::atomic_uint64_t submission_serial_{0};
    /*
     * Poison closes an unsafe post-doorbell session permanently;
     * submission_active_ rejects only overlap and clears during stack unwind.
     */
    std::atomic_bool poisoned_{false};
    std::atomic_bool submission_active_{false};

    /// Reject mutations after an unsafe post-doorbell terminal condition.
    void requireUsable() const;
    /// Poll for the firmware's boot-contract commit magic.
    void waitForMagic(std::chrono::milliseconds timeout);
    /// Poll for @p target while rejecting reset-only terminal firmware.
    void waitForState(std::uint32_t target, std::chrono::milliseconds timeout);
    /// Wait for exact sequence completion and return its validated result.
    Rp1GraphResult waitForGraphDone(
        std::uint32_t want_seq, std::chrono::milliseconds timeout);
    /// Snapshot and validate the committed result for @p want_seq.
    Rp1GraphResult readGraphResult(std::uint32_t want_seq);
};

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP
