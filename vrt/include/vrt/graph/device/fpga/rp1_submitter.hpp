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
 *      the control-block base addresses (node array, CQ, arg buffer,
 *      signal array) at the recommended `RP1_DEFAULT_*_OFFSET` layout.
 *   2. For each `submitAndWait()`:
 *        - Copies the node array, arg buffer, and signal clears into DDR.
 *        - Records the monotonic CQ cursor before incrementing `graph_seq`.
 *        - Memory-fences, bumps `graph_seq` by one, memory-fences again.
 *        - Polls by exact sequence equality and incrementally drains CQ so
 *          firmware can make progress through a full ring.
 *        - Surfaces ERROR/HALTED immediately with the complete terminal record.
 *   3. `drainCq()` returns the retained CQ evidence for final validation.
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

    /// Optional override of cq_size. Must be a power of 2 <= 4096. Zero means
    /// "leave whatever was already programmed" (or use the submitter's
    /// default on the first submission).
    std::uint32_t cq_size_override = 0;

    /// Enable RP1 firmware trace-ring writes for this submission.
    bool trace_enable = false;

    /// Optional override of trace_size. Zero uses kDefaultTraceSize.
    std::uint32_t trace_size_override = 0;
};

/**
 * @brief Default CQ size for first submission (matches the `v80-smi debug
 *        rp1-ping` probe).
 */
constexpr std::uint32_t kDefaultCqSize = 64u;

/**
 * @brief Default optional trace-ring size programmed by Rp1Submitter.
 */
constexpr std::uint32_t kDefaultTraceSize = 256u;

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

class Rp1Submitter {
   public:
    explicit Rp1Submitter(Rp1BarWindow& window,
                          std::uint32_t cq_size = kDefaultCqSize);

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
     * @throws Rp1TimeoutError if @p timeout elapses without completion.
     * @throws std::logic_error if @p image violates the protocol caps.
     * @throws std::runtime_error if the firmware reports an error state.
     */
    void submitAndWait(const Rp1GraphImage& image,
                       std::chrono::milliseconds timeout = kDefaultSubmitTimeout);

    /**
     * @brief Read the CQ entries written by the most recent
     *        @c submitAndWait() invocation (excluding any silent nodes).
     *
     * Entries drained incrementally while the graph was running are retained
     * here for final validation.
     *
     * @throws std::runtime_error if the CQ cursors are corrupt or any entry
     *         reports an RP1 node error or timeout.
     */
    std::vector<rp1_cq_entry_t> drainCq();

    /**
     * @brief Drain CQ records without interpreting node status.
     *
     * This lets higher layers reconcile side effects that completed before a
     * later node or transport failure. Ring/cursor corruption still throws.
     */
    std::vector<rp1_cq_entry_t> drainCqRaw();

    /**
     * @brief Validate statuses from a previously drained raw CQ batch.
     */
    static void validateCq(
        const std::vector<rp1_cq_entry_t>& entries);

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
     * @brief True after an in-flight submission times out indeterminately.
     *
     * A poisoned submitter rejects further BAR mutations and submissions.
     * Callers must reset/recover the device and construct a new submitter.
     */
    bool poisoned() const noexcept {
        return poisoned_.load(std::memory_order_acquire);
    }

    /**
     * @brief CQ cursor at the latest submission/final drain boundary.
     */
    std::uint32_t lastCqStart() const noexcept { return last_cq_start_; }

   private:
    Rp1BarWindow* window_;
    std::uint32_t cq_size_;
    bool          ready_       = false;
    std::uint32_t last_graph_seq_ = 0;
    std::uint32_t last_cq_start_  = 0;
    std::uint32_t last_trace_size_ = kDefaultTraceSize;
    std::atomic_uint64_t submission_serial_{0};
    /*
     * cq_cursor_ follows the monotonic firmware producer cursor; pending_cq_
     * retains incrementally copied records until the caller consumes the graph.
     */
    std::uint32_t cq_cursor_ = 0;
    std::vector<rp1_cq_entry_t> pending_cq_;
    /*
     * Poison closes an indeterminate post-doorbell session permanently;
     * submission_active_ rejects only overlap and clears during stack unwind.
     */
    std::atomic_bool poisoned_{false};
    std::atomic_bool submission_active_{false};

    void requireUsable() const;
    void waitForMagic(std::chrono::milliseconds timeout);
    void waitForState(std::uint32_t target, std::chrono::milliseconds timeout);
    void waitForGraphDone(std::uint32_t want_seq, std::chrono::milliseconds timeout);
    void drainAvailableCq();
    [[noreturn]] void throwTerminalError(
        std::uint32_t state, std::uint32_t graph_seq) const;
};

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP
