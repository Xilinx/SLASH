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

#include <vrt/graph/device/fpga/rp1_submitter.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace vrt::graph::fpga {

namespace {

constexpr std::chrono::microseconds kPollInterval{1000};  // 1 ms

const char* stateName(std::uint32_t s) {
    switch (s) {
        case RP1_STATE_INIT:    return "INIT";
        case RP1_STATE_READY:   return "READY";
        case RP1_STATE_RUNNING: return "RUNNING";
        case RP1_STATE_ERROR:   return "ERROR";
        case RP1_STATE_HALTED:  return "HALTED";
        default:                return "?";
    }
}

bool isPowerOfTwo(std::uint32_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

}  // namespace

Rp1Submitter::Rp1Submitter(Rp1BarWindow& window, std::uint32_t cq_size)
    : window_(&window), cq_size_(cq_size) {
    if (!isPowerOfTwo(cq_size_)) {
        throw std::invalid_argument(
            "Rp1Submitter: cq_size must be a positive power of 2, got " +
            std::to_string(cq_size_));
    }
}

void Rp1Submitter::ensureReady(std::chrono::milliseconds timeout) {
    if (ready_) {
        // Re-verify: state should still be READY and magic intact.
        const std::uint32_t magic = window_->readMagic();
        const std::uint32_t state = window_->readState();
        if (magic == RP1_CTRL_MAGIC && state == RP1_STATE_READY) {
            return;
        }
        // Firmware has restarted under us; re-do the handshake.
        ready_ = false;
    }

    waitForMagic(timeout);
    waitForState(RP1_STATE_READY, timeout);

    // Reject a half-deployed firmware/host mix.  The on-wire layout
    // (notably the KERNEL_DISPATCH argument buffer) is version-specific, so
    // running mismatched halves silently corrupts dispatched kernels.  The
    // firmware publishes its protocol version into the control block once it
    // reaches READY; assert it matches what this host was built against.
    const std::uint32_t fw_version = window_->readU32(offsetof(rp1_ctrl_t, version));
    if (fw_version != RP1_PROTOCOL_VERSION) {
        throw std::runtime_error(
            "Rp1Submitter: RP1 firmware protocol version mismatch (firmware reports v" +
            std::to_string(fw_version) + ", host built for v" +
            std::to_string(RP1_PROTOCOL_VERSION) +
            "); reflash rp1.elf and rebuild libvrt from the same tree before running");
    }

    // Program the recommended base addresses + cq_size by writing
    // host-owned fields individually.  We must NOT bulk-write the 4 KB
    // control block here: the firmware concurrently updates its own
    // fields (heartbeat, rp1_state, graph_done_seq, cq_write_idx,
    // rp1_current_node, rp1_error_code), so a read-modify-write of the
    // whole block would clobber whatever RP1 changed between our read
    // and write.  Individual 32-bit writes only touch host-owned slots.
    window_->writeU32(offsetof(rp1_ctrl_t, cq_size), cq_size_);
    window_->writeU32(offsetof(rp1_ctrl_t, node_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, node_base_hi),  0);
    window_->writeU32(offsetof(rp1_ctrl_t, cq_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, cq_base_hi),    0);
    window_->writeU32(offsetof(rp1_ctrl_t, arg_buf_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, arg_buf_base_hi), 0);
    window_->writeU32(offsetof(rp1_ctrl_t, sig_array_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, sig_array_base_hi), 0);

    // graph_seq / graph_done_seq / cq_*_idx / rp1_state are intentionally
    // left untouched: the firmware owns the read side of graph_seq and
    // exclusively owns the rest.

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Seed last_cq_start_ so callers that immediately drainCq() see an
    // empty range.
    last_cq_start_ = window_->readCqWriteIdx();
    last_graph_seq_ = window_->readGraphSeq();

    ready_ = true;
}

void Rp1Submitter::submitAndWait(const Rp1GraphImage& image,
                                  std::chrono::milliseconds timeout) {
    if (image.nodes.empty()) {
        throw std::logic_error("Rp1Submitter: empty graph image");
    }
    if (image.nodes.size() > RP1_MAX_NODES) {
        throw std::logic_error(
            "Rp1Submitter: graph image has " + std::to_string(image.nodes.size()) +
            " nodes, max is " + std::to_string(RP1_MAX_NODES));
    }
    for (std::uint32_t slot : image.clear_signal_slots) {
        if (slot >= RP1_MAX_SIGNALS) {
            throw std::logic_error(
                "Rp1Submitter: signal slot " + std::to_string(slot) +
                " exceeds RP1_MAX_SIGNALS (" + std::to_string(RP1_MAX_SIGNALS) + ")");
        }
    }

    if (!ready_) {
        ensureReady(kDefaultReadyTimeout);
    }

    // Stage args first so the kernel-dispatch packets reference valid
    // memory the moment graph_seq advances.
    if (!image.arg_buf.empty()) {
        window_->writeArgs(image.arg_buf.data(), image.arg_buf.size());
    }

    // Zero the signal slots the graph will rely on so leftover values
    // from earlier submissions can't fool the host's post-graph checks.
    for (std::uint32_t slot : image.clear_signal_slots) {
        window_->clearSignal(slot);
    }

    // Stage the node array.
    window_->writeNodes(image.nodes.data(), image.nodes.size());

    // Read pre-submission cq cursor + current graph_seq so the caller
    // can drain just this graph's CQ entries.
    last_cq_start_ = window_->readCqWriteIdx();
    const std::uint32_t prev_seq = window_->readGraphSeq();
    const std::uint32_t want_seq = prev_seq + 1u;

    // Program node_count and (optionally) cq_size *before* bumping
    // graph_seq.  RP1's flat scanner reads node_count when it observes
    // graph_seq advancing, so this must be visible first.
    window_->writeNodeCount(static_cast<std::uint32_t>(image.nodes.size()));
    if (image.cq_size_override != 0) {
        if (!isPowerOfTwo(image.cq_size_override)) {
            throw std::invalid_argument(
                "Rp1Submitter: cq_size_override must be a power of 2, got " +
                std::to_string(image.cq_size_override));
        }
        // Persist for subsequent submissions too.
        cq_size_ = image.cq_size_override;
        window_->writeU32(offsetof(rp1_ctrl_t, cq_size), cq_size_);
    }

    // Fence before bumping graph_seq so node_count etc. are visible.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    window_->writeGraphSeq(want_seq);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    last_graph_seq_ = want_seq;

    waitForGraphDone(want_seq, timeout);

    // After completion, surface explicit firmware-side errors so the
    // caller doesn't have to remember to inspect rp1_state.
    const std::uint32_t state = window_->readState();
    if (state == RP1_STATE_ERROR || state == RP1_STATE_HALTED) {
        const std::uint32_t code = window_->readErrorCode();
        throw std::runtime_error(
            std::string("Rp1Submitter: firmware reported state ") + stateName(state) +
            " (error_code=" + std::to_string(code) + ") for graph seq " +
            std::to_string(want_seq));
    }
}

void Rp1Submitter::clearSignalSlots(const std::vector<std::uint32_t>& slots) {
    if (!ready_) {
        ensureReady(kDefaultReadyTimeout);
    }
    for (std::uint32_t slot : slots) {
        if (slot >= RP1_MAX_SIGNALS) {
            throw std::logic_error(
                "Rp1Submitter: signal slot " + std::to_string(slot) +
                " exceeds RP1_MAX_SIGNALS (" + std::to_string(RP1_MAX_SIGNALS) + ")");
        }
        window_->clearSignal(slot);
    }
}

std::vector<rp1_cq_entry_t> Rp1Submitter::drainCq() {
    const std::uint32_t end = window_->readCqWriteIdx();
    if (end < last_cq_start_) {
        throw std::runtime_error(
            "Rp1Submitter::drainCq: cq_write_idx went backwards (" +
            std::to_string(end) + " < " + std::to_string(last_cq_start_) + ")");
    }
    const std::uint32_t delta = end - last_cq_start_;

    std::vector<rp1_cq_entry_t> out;
    out.reserve(delta);
    for (std::uint32_t i = 0; i < delta; ++i) {
        const std::uint32_t idx = (last_cq_start_ + i) & (cq_size_ - 1u);
        rp1_cq_entry_t entry{};
        window_->readCq(idx, entry);
        out.push_back(entry);
    }
    // Tell the firmware we've consumed them.
    window_->writeCqReadIdx(end);
    last_cq_start_ = end;
    return out;
}

// ---- Polling helpers -----------------------------------------------------

void Rp1Submitter::waitForMagic(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::uint32_t m = window_->readMagic();
        if (m == RP1_CTRL_MAGIC) return;
        if (std::chrono::steady_clock::now() > deadline) {
            throw Rp1TimeoutError(
                "Rp1Submitter: timed out waiting for control-block magic "
                "(got 0x" + std::to_string(m) + ", expected 0x53515231 'SQR1') -- "
                "is the firmware loaded?");
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

void Rp1Submitter::waitForState(std::uint32_t target,
                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::uint32_t s = window_->readState();
        if (s == target) return;
        if (s == RP1_STATE_ERROR || s == RP1_STATE_HALTED) {
            const std::uint32_t code = window_->readErrorCode();
            throw std::runtime_error(
                std::string("Rp1Submitter: firmware in unexpected state ") +
                stateName(s) + " (error_code=" + std::to_string(code) + ")");
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw Rp1TimeoutError(
                std::string("Rp1Submitter: timed out waiting for state ") +
                stateName(target) + " (current=" + stateName(s) + ")");
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

void Rp1Submitter::waitForGraphDone(std::uint32_t want_seq,
                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::uint32_t done = window_->readGraphDoneSeq();
        if (done >= want_seq) return;
        if (std::chrono::steady_clock::now() > deadline) {
            const std::uint32_t state   = window_->readState();
            const std::uint32_t cur     = window_->readU32(
                                              offsetof(rp1_ctrl_t, rp1_current_node));
            const std::uint32_t err     = window_->readErrorCode();
            const std::uint32_t cq_idx  = window_->readCqWriteIdx();
            throw Rp1TimeoutError(
                "Rp1Submitter: timed out waiting for graph_done_seq=" +
                std::to_string(want_seq) +
                " (got " + std::to_string(done) +
                ", state=" + stateName(state) +
                ", current_node=" + std::to_string(cur) +
                ", error_code=" + std::to_string(err) +
                ", cq_write_idx=" + std::to_string(cq_idx) + ")");
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

}  // namespace vrt::graph::fpga
