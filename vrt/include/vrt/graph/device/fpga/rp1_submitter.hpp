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
 *        - Records `cq_write_idx` and `graph_done_seq` *before*
 *          incrementing `graph_seq` so callers can later compute the CQ
 *          delta of this graph.
 *        - Memory-fences, bumps `graph_seq` by one, memory-fences again.
 *        - Polls `graph_done_seq` at ~1 ms cadence until it catches up
 *          or the user-supplied timeout elapses.
 *   3. `drainCq()` reads the CQ entries written by the most recent
 *      submission (between the recorded "before" cq_write_idx and the
 *      current cq_write_idx).
 *
 * The submitter knows nothing about graphs, kernels, or VRT — it is a
 * mechanical adapter between a fully-realised RP1 graph image and the
 * BAR window. Higher-level code (FpgaDevice in phase 2) is responsible
 * for assembling the `Rp1GraphImage`.
 *
 * Not thread-safe: a single Rp1Submitter may not be driven from
 * multiple threads concurrently. Multiple submitters against the same
 * Rp1BarWindow are also unsafe; only one client should "own" the
 * RP1 at a time. Multi-tenancy is deferred to a later phase.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP
#define VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP

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
    /// the graph plans to write).
    std::vector<std::uint32_t> clear_signal_slots;

    /// Optional override of cq_size.  Must be a power of 2.  Zero means
    /// "leave whatever was already programmed" (or use the submitter's
    /// default on the first submission).
    std::uint32_t cq_size_override = 0;
};

/**
 * @brief Default CQ size for first submission (matches `examples/rp1_bringup`).
 */
constexpr std::uint32_t kDefaultCqSize = 64u;

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
     */
    void clearSignalSlots(const std::vector<std::uint32_t>& slots);

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
     */
    std::vector<rp1_cq_entry_t> drainCq();

    /**
     * @brief Sequence number of the most recently submitted graph.
     *
     * Useful for diagnostics; matches the value of @c graph_seq the
     * submitter wrote to the control block.
     */
    std::uint32_t lastGraphSeq() const noexcept { return last_graph_seq_; }

    /**
     * @brief @c cq_write_idx as it was just before the most recent
     *        submission.  @c drainCq() reads the range
     *        @c [lastCqStart(), readCqWriteIdx()].
     */
    std::uint32_t lastCqStart() const noexcept { return last_cq_start_; }

   private:
    Rp1BarWindow* window_;
    std::uint32_t cq_size_;
    bool          ready_       = false;
    std::uint32_t last_graph_seq_ = 0;
    std::uint32_t last_cq_start_  = 0;

    void waitForMagic(std::chrono::milliseconds timeout);
    void waitForState(std::uint32_t target, std::chrono::milliseconds timeout);
    void waitForGraphDone(std::uint32_t want_seq, std::chrono::milliseconds timeout);
};

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_SUBMITTER_HPP
