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

/// Host polling cadence retained from the protocol-v4 submitter.
constexpr std::chrono::microseconds kPollInterval{1000};

/// Bytes available before the default signal-array region begins.
constexpr std::size_t kDefaultArgBufferBytes =
    RP1_DEFAULT_SIG_ARRAY_OFFSET - RP1_DEFAULT_ARG_BUF_OFFSET;

/// Result flags that prove the device cannot be reused without recovery.
constexpr std::uint32_t kPoisonFlags =
    RP1_RESULT_RECOVERY_REQUIRED | RP1_RESULT_INFINITE_WORK_REMAINS;

/// Return a stable diagnostic label for a raw firmware state.
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

/// Return true when @p v is a non-zero power of two.
bool isPowerOfTwo(std::uint32_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

/// Return true when @p op is a protocol-defined condition operation.
bool isValidCondition(std::uint16_t op) noexcept {
    return op <= RP1_COP_AND_Z;
}

/**
 * @brief Return whether one phase-1 DMA endpoint fits the 32-bit DDR model.
 *
 * Phase 1 supports only word-aligned, low-word addresses. A zero-byte transfer
 * is valid; non-empty ranges may end at the top of the 32-bit address space
 * but must not wrap past it.
 */
bool isValidPhase1DmaRange(std::uint32_t lo, std::uint32_t hi,
                           std::uint32_t bytes) noexcept {
    constexpr std::uint64_t addressSpaceSize =
        std::uint64_t{1} << 32;
    return hi == 0u && ((lo | bytes) & 3u) == 0u &&
           static_cast<std::uint64_t>(lo) + bytes <= addressSpaceSize;
}

/*
 * Validation is deliberately complete before the doorbell can move. Check the
 * shared header first, then opcode-specific slots and ranges, and reject
 * undefined packets before staged bytes can become firmware-owned.
 */
void validateImage(const Rp1GraphImage& image) {
    const auto bad = [](std::size_t index, const std::string& reason) {
        throw std::logic_error(
            "Rp1Submitter: invalid node " + std::to_string(index) +
            ": " + reason);
    };
    const auto validSlot = [](std::uint32_t slot) {
        return slot < RP1_MAX_SIGNALS;
    };

    if (image.arg_buf.size() >
        kDefaultArgBufferBytes / sizeof(std::uint32_t)) {
        throw std::logic_error(
            "Rp1Submitter: argument buffer exceeds the default " +
            std::to_string(kDefaultArgBufferBytes) + "-byte region");
    }

    for (std::size_t i = 0; i < image.nodes.size(); ++i) {
        const rp1_node_t& node = image.nodes[i];
        const std::uint16_t opcode = rp1_node_get_opcode(&node);
        const std::uint16_t flags = rp1_node_get_flags(&node);
        if ((rp1_node_get_control(&node) & RP1_NODE_RESERVED_MASK) != 0u) {
            bad(i, "packed control reserved bits are non-zero");
        }
        if (rp1_node_get_status(&node) != RP1_NODE_PENDING) {
            bad(i, "initial status is not PENDING");
        }
        if ((flags & ~RP1_FLAG_INFINITE) != 0u ||
            ((flags & RP1_FLAG_INFINITE) != 0u &&
             opcode != RP1_OP_KERNEL_DISPATCH)) {
            bad(i, "flags are invalid for the opcode");
        }
        if (node.barrier_await_bucket >= RP1_MAX_BUCKETS ||
            node.barrier_set_bucket >= RP1_MAX_BUCKETS) {
            bad(i, "barrier bucket out of range");
        }
        switch (opcode) {
            case RP1_OP_NOP:
            case RP1_OP_HALT:
                break;
            case RP1_OP_SCALAR_WRITE: {
                bool terminated = false;
                for (const rp1_write_pair_t& write :
                     node.payload.scalar_write.writes) {
                    if (write.addr == 0u) {
                        terminated = true;
                    } else if (terminated || (write.addr & 3u) != 0u) {
                        bad(i, "SCALAR_WRITE pair sequence is invalid");
                    }
                }
                break;
            }
            case RP1_OP_DMA_COPY: {
                const auto& dma = node.payload.dma_copy;
                const std::uint32_t packed =
                    dma.length_types;
                const std::uint32_t length =
                    rp1_dma_get_length(packed);
                if (rp1_dma_get_src_type(packed) != 0u ||
                    rp1_dma_get_dst_type(packed) != 0u ||
                    !isValidPhase1DmaRange(
                        dma.src_addr_lo, dma.src_addr_hi, length) ||
                    !isValidPhase1DmaRange(
                        dma.dst_addr_lo, dma.dst_addr_hi, length)) {
                    bad(i, "DMA_COPY address, length, or memory type is invalid");
                }
                break;
            }
            case RP1_OP_DMA_FILL: {
                const auto& dma = node.payload.dma_fill;
                if (dma.dst_type != 0u ||
                    !isValidPhase1DmaRange(
                        dma.dst_addr_lo, dma.dst_addr_hi, dma.length)) {
                    bad(i, "DMA_FILL address, length, or memory type is invalid");
                }
                break;
            }
            case RP1_OP_SIGNAL:
                if (!validSlot(node.payload.signal.target_slot)) {
                    bad(i, "SIGNAL target slot out of range");
                }
                if (node.payload.signal.operation > RP1_SIGOP_AND) {
                    bad(i, "SIGNAL operation out of range");
                }
                break;
            case RP1_OP_WAIT:
                if (!validSlot(node.payload.wait.condition_signal)) {
                    bad(i, "WAIT condition slot out of range");
                }
                if (!isValidCondition(node.payload.wait.condition_op)) {
                    bad(i, "WAIT condition operation out of range");
                }
                break;
            case RP1_OP_SCALAR_READ:
                if (!validSlot(node.payload.scalar_read.target_slot)) {
                    bad(i, "SCALAR_READ target slot out of range");
                }
                break;
            case RP1_OP_SCALAR_COPY:
                if (!validSlot(node.payload.scalar_copy.source_slot)) {
                    bad(i, "SCALAR_COPY source slot out of range");
                }
                break;
            case RP1_OP_KERNEL_DISPATCH: {
                const auto& kernel = node.payload.kernel_dispatch;
                const std::uint64_t argEnd =
                    static_cast<std::uint64_t>(kernel.arg_buffer_offset) +
                    static_cast<std::uint64_t>(kernel.arg_count) *
                        sizeof(rp1_kernel_arg_t);
                if (kernel.kernel_base_addr == 0 ||
                    (kernel.arg_buffer_offset & 7u) != 0u ||
                    kernel.ctrl_flags != 0u ||
                    argEnd > image.arg_buf.size() * sizeof(std::uint32_t)) {
                    bad(i, "KERNEL_DISPATCH argument range is invalid");
                }
                break;
            }
            case RP1_OP_PDI_LOAD:
                break;
            case RP1_OP_LOOP: {
                const auto& loop = node.payload.loop;
                if (!validSlot(loop.condition_signal)) {
                    bad(i, "LOOP condition slot out of range");
                }
                if (!isValidCondition(loop.condition_op) ||
                    loop.loop_id >= RP1_MAX_LOOPS ||
                    loop.body_start > loop.body_end ||
                    loop.body_end >= image.nodes.size() ||
                    loop.bucket_clear_start > loop.bucket_clear_end ||
                    loop.bucket_clear_end >= RP1_MAX_BUCKETS) {
                    bad(i, "LOOP range or operation is invalid");
                }
                break;
            }
            case RP1_OP_COND: {
                const auto& cond = node.payload.cond;
                if (!validSlot(cond.condition_signal)) {
                    bad(i, "COND condition slot out of range");
                }
                const bool emptyBody = cond.body_start > cond.body_end;
                const bool emptyBuckets =
                    cond.bucket_clear_start > cond.bucket_clear_end;
                if (!isValidCondition(cond.condition_op) ||
                    cond.done_bucket >= RP1_MAX_BUCKETS ||
                    (!emptyBody && cond.body_end >= image.nodes.size()) ||
                    (!emptyBuckets &&
                     cond.bucket_clear_end >= RP1_MAX_BUCKETS)) {
                    bad(i, "COND range or operation is invalid");
                }
                break;
            }
            case RP1_OP_RERUN:
                if (node.payload.rerun.target_node >= image.nodes.size() ||
                    (node.payload.rerun.rerun_flags &
                     ~RP1_RERUN_CLEAR_STATE) != 0u ||
                    (((node.payload.rerun.rerun_flags &
                       RP1_RERUN_CLEAR_STATE) != 0u) &&
                     node.payload.rerun.loop_id >= RP1_MAX_LOOPS)) {
                    bad(i, "RERUN target or loop id is invalid");
                }
                break;
            default:
                bad(i, "opcode is not defined by protocol v6");
        }
    }
}

/*
 * Compatibility has three independent failure modes: protocol layout,
 * mandatory behavior bits, and generated IPI identity. Reporting each one
 * avoids misdiagnosing stale firmware as malformed graph data.
 */
void requireFirmwareContract(Rp1BarWindow& window) {
    const std::uint32_t version =
        window.readU32(offsetof(rp1_ctrl_t, version));
    if (version != RP1_PROTOCOL_VERSION) {
        throw std::runtime_error(
            "Rp1Submitter: RP1 firmware protocol version mismatch "
            "(firmware reports v" + std::to_string(version) +
            ", host built for v" +
            std::to_string(RP1_PROTOCOL_VERSION) +
            "); reflash rp1.elf and rebuild libvrt from the same tree "
            "before running");
    }

    const std::uint32_t capabilities =
        window.readU32(offsetof(rp1_ctrl_t, capabilities));
    const std::uint32_t missing =
        RP1_REQUIRED_CAPABILITIES & ~capabilities;
    if (missing != 0u) {
        throw std::runtime_error(
            "Rp1Submitter: RP1 firmware is missing required protocol-v6 "
            "capabilities (firmware mask=" +
            std::to_string(capabilities) + ", required mask=" +
            std::to_string(RP1_REQUIRED_CAPABILITIES) + ", missing mask=" +
            std::to_string(missing) + ")");
    }
    const std::uint32_t platform =
        window.readU32(offsetof(rp1_ctrl_t, pdi_ipi_platform_id));
    if (platform == RP1_PDI_IPI_PLATFORM_UNKNOWN) {
        throw std::runtime_error(
            "Rp1Submitter: firmware did not publish a generated/fixture "
            "PDI IPI platform id");
    }
}

}  // namespace

void appendScalarWritePackets(
    Rp1GraphImage& image,
    const std::vector<rp1_write_pair_t>& writes,
    std::uint8_t awaitBucket, std::uint32_t awaitMask,
    std::uint8_t setBucket, std::uint32_t setMask) {
    if (writes.empty()) {
        throw std::logic_error(
            "appendScalarWritePackets: write list is empty");
    }
    if (awaitBucket >= RP1_MAX_BUCKETS ||
        setBucket >= RP1_MAX_BUCKETS) {
        throw std::logic_error(
            "appendScalarWritePackets: barrier bucket is out of range");
    }
    for (const rp1_write_pair_t& write : writes) {
        if (write.addr == 0u || (write.addr & 3u) != 0u) {
            throw std::logic_error(
                "appendScalarWritePackets: address is zero or unaligned");
        }
    }

    const std::size_t packetCount =
        writes.size() / RP1_SCALAR_WRITE_MAX +
        (writes.size() % RP1_SCALAR_WRITE_MAX != 0u ? 1u : 0u);
    if (image.nodes.size() > RP1_MAX_NODES ||
        packetCount > RP1_MAX_NODES - image.nodes.size()) {
        throw std::logic_error(
            "appendScalarWritePackets: image exceeds RP1_MAX_NODES");
    }

    /*
     * Every packet shares the original await. Only the final packet publishes
     * completion, so contiguous flat-scanner order cannot expose a partial
     * register-write sequence to dependent nodes.
     */
    image.nodes.reserve(image.nodes.size() + packetCount);
    for (std::size_t first = 0; first < writes.size();
         first += RP1_SCALAR_WRITE_MAX) {
        rp1_node_t node{};
        rp1_node_set_opcode(&node, RP1_OP_SCALAR_WRITE);
        rp1_node_set_flags(&node, 0u);
        rp1_node_set_status(&node, RP1_NODE_PENDING);
        node.barrier_await_bucket = awaitBucket;
        node.barrier_await_mask = awaitMask;
        const bool final =
            first + RP1_SCALAR_WRITE_MAX >= writes.size();
        node.barrier_set_bucket = final ? setBucket : 0u;
        node.barrier_set_mask = final ? setMask : 0u;
        for (std::size_t i = 0;
             i < RP1_SCALAR_WRITE_MAX && first + i < writes.size();
             ++i) {
            node.payload.scalar_write.writes[i] = writes[first + i];
        }
        image.nodes.push_back(node);
    }
}

Rp1Submitter::Rp1Submitter(Rp1BarWindow& window) : window_(&window) {}

void Rp1Submitter::requireUsable() const {
    if (poisoned()) {
        throw std::runtime_error(
            "Rp1Submitter: device is poisoned after an unsafe terminal "
            "submission; reset/recover the device and construct a new "
            "runtime device before submitting again");
    }
}

/*
 * Cached readiness has two cases: intact publication can be revalidated;
 * missing magic or READY means firmware restarted and the full handshake must
 * run. Host-owned addresses are written only after the contract is visible.
 */
void Rp1Submitter::ensureReady(std::chrono::milliseconds timeout) {
    requireUsable();
    if (ready_) {
        // Re-verify: state should still be READY and magic intact.
        const std::uint32_t magic = window_->readMagic();
        const std::uint32_t state = window_->readState();
        if (magic == RP1_CTRL_MAGIC && state == RP1_STATE_READY) {
            requireFirmwareContract(*window_);
            return;
        }
        // Firmware has restarted under us; re-do the handshake.
        ready_ = false;
    }

    waitForMagic(timeout);
    waitForState(RP1_STATE_READY, timeout);

    // Magic and READY are the firmware's publication barrier for version and
    // capabilities. Reject a half-deployed firmware/host mix only after both
    // publication fields are visible.
    requireFirmwareContract(*window_);

    /*
     * Program host-owned words individually. A bulk control-block write would
     * race firmware-owned heartbeat, state, sequence, result, and diagnostics.
     * Former CQ configuration words are zero-only protocol-v6 reservations.
     */
    window_->writeU32(offsetof(rp1_ctrl_t, _reserved_cq_size), 0u);
    window_->writeU32(offsetof(rp1_ctrl_t, node_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, node_base_hi),  0);
    window_->writeU32(offsetof(rp1_ctrl_t, _reserved_cq_base_lo), 0u);
    window_->writeU32(offsetof(rp1_ctrl_t, _reserved_cq_base_hi), 0u);
    window_->writeU32(offsetof(rp1_ctrl_t, arg_buf_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, arg_buf_base_hi), 0);
    window_->writeU32(offsetof(rp1_ctrl_t, sig_array_base_lo),
                       static_cast<std::uint32_t>(
                           RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET));
    window_->writeU32(offsetof(rp1_ctrl_t, sig_array_base_hi), 0);
    window_->writeTraceBase(static_cast<std::uint32_t>(
                                RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET),
                            0);
    window_->writeTraceSize(kDefaultTraceSize);
    window_->writeTraceEnable(0);
    last_trace_size_ = kDefaultTraceSize;

    std::atomic_thread_fence(std::memory_order_seq_cst);
    last_graph_seq_ = window_->readGraphSeq();

    ready_ = true;
}

/*
 * A submission has four ordered phases: claim and validate, establish READY,
 * stage the image, then publish graph_seq and wait. Only publication transfers
 * ownership of the staged bytes to RP1.
 */
Rp1GraphResult Rp1Submitter::submitAndWait(
    const Rp1GraphImage& image, std::chrono::milliseconds timeout) {
    requireUsable();
    bool expected = false;
    if (!submission_active_.compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        throw std::runtime_error(
            "Rp1Submitter: a graph submission is already active");
    }
    /// Clear the single-flight claim during every stack-unwind path.
    struct ActiveSubmission {
        /// Submitter flag exclusively owned by this scope guard.
        std::atomic_bool& active;
        /// Release the single-flight claim after result or exception.
        ~ActiveSubmission() {
            active.store(false, std::memory_order_release);
        }
    } activeSubmission{submission_active_};

    /*
     * Phase 1: reject overlap and every malformed packet before touching
     * shared graph storage, so validation failure leaves no partial image.
     */
    if (image.nodes.empty()) {
        throw std::logic_error("Rp1Submitter: empty graph image");
    }
    if (image.nodes.size() > RP1_MAX_NODES) {
        throw std::logic_error(
            "Rp1Submitter: graph image has " + std::to_string(image.nodes.size()) +
            " nodes, max is " + std::to_string(RP1_MAX_NODES));
    }
    validateImage(image);
    for (std::uint32_t slot : image.clear_signal_slots) {
        if (slot >= RP1_MAX_SIGNALS) {
            throw std::logic_error(
                "Rp1Submitter: signal slot " + std::to_string(slot) +
                " exceeds RP1_MAX_SIGNALS (" + std::to_string(RP1_MAX_SIGNALS) + ")");
        }
    }
    const std::uint32_t trace_size =
        image.trace_size_override != 0 ?
            image.trace_size_override : kDefaultTraceSize;
    if (!isPowerOfTwo(trace_size) ||
        trace_size > RP1_MAX_TRACE_ENTRIES) {
        throw std::invalid_argument(
            "Rp1Submitter: trace_size must be a power of 2 in [1, " +
            std::to_string(RP1_MAX_TRACE_ENTRIES) + "], got " +
            std::to_string(trace_size));
    }

    /*
     * Phase 2: establish the firmware contract and reject terminal or busy
     * state before this submission takes ownership of shared storage.
     */
    ensureReady(kDefaultReadyTimeout);
    const std::uint32_t preflightState = window_->readState();
    if (preflightState != RP1_STATE_READY) {
        throw std::runtime_error(
            "Rp1Submitter: firmware is not READY before submission "
            "(state=" + std::string(stateName(preflightState)) + ")");
    }

    /*
     * Phase 3: stage arguments, signals, nodes, and host-owned configuration
     * while firmware still sees the old sequence.
     */
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

    const std::uint32_t prev_seq = window_->readGraphSeq();
    const std::uint32_t want_seq = prev_seq + 1u;

    // Program node_count before bumping graph_seq. RP1 snapshots it only after
    // observing the new sequence, so the count must be visible first.
    window_->writeNodeCount(static_cast<std::uint32_t>(image.nodes.size()));

    window_->writeTraceBase(static_cast<std::uint32_t>(
                                RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET),
                            0);
    window_->writeTraceSize(trace_size);
    window_->writeTraceEnable(image.trace_enable ? 1u : 0u);
    last_trace_size_ = trace_size;

    /*
     * Phase 4: order every staged byte before graph_seq, the sole doorbell.
     * Firmware snapshots configuration only after observing this new value.
     */
    std::atomic_thread_fence(std::memory_order_seq_cst);
    window_->writeGraphSeq(want_seq);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    last_graph_seq_ = want_seq;
    ++submission_serial_;

    /*
     * Completion: exact sequence equality releases the committed graph result.
     * A timeout after the doorbell is indeterminate and permanently poisons
     * this submitter so no later image can overwrite firmware-owned storage.
     */
    try {
        return waitForGraphDone(want_seq, timeout);
    } catch (const Rp1TimeoutError&) {
        // The doorbell is irreversible: RP1 may still consume the staged image.
        poisoned_.store(true, std::memory_order_release);
        throw;
    }
}

void Rp1Submitter::clearSignalSlots(const std::vector<std::uint32_t>& slots) {
    requireUsable();
    for (std::uint32_t slot : slots) {
        if (slot >= RP1_MAX_SIGNALS) {
            throw std::logic_error(
                "Rp1Submitter: signal slot " + std::to_string(slot) +
                " exceeds RP1_MAX_SIGNALS (" + std::to_string(RP1_MAX_SIGNALS) + ")");
        }
    }
    if (!ready_) {
        ensureReady(kDefaultReadyTimeout);
    }
    for (std::uint32_t slot : slots) {
        window_->clearSignal(slot);
    }
}

std::uint32_t Rp1Submitter::readSignalValue(std::uint32_t slot) const {
    if (slot >= RP1_MAX_SIGNALS) {
        throw std::logic_error(
            "Rp1Submitter: signal slot " + std::to_string(slot) +
            " exceeds RP1_MAX_SIGNALS (" +
            std::to_string(RP1_MAX_SIGNALS) + ")");
    }
    rp1_signal_slot_t value{};
    window_->readSignal(slot, value);
    return value.value;
}

Rp1TraceCapture Rp1Submitter::drainTrace() {
    const std::uint32_t written = window_->readTraceWriteIdx();
    const std::uint32_t count =
        written > last_trace_size_ ? last_trace_size_ : written;
    const bool overflow = written > last_trace_size_;

    Rp1TraceCapture capture;
    capture.written = written;
    capture.overflow = overflow;
    capture.entries.reserve(count);

    const std::uint32_t start = overflow ? (written % last_trace_size_) : 0u;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t idx = (start + i) % last_trace_size_;
        rp1_trace_entry_t entry{};
        window_->readTrace(idx, entry);
        capture.entries.push_back(entry);
    }

    return capture;
}

// ---- Polling helpers -----------------------------------------------------

/*
 * Firmware writes magic last, after READY and the fixed contract fields.
 * Waiting on it first prevents the host from validating a half-published boot.
 */
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
            throw std::runtime_error(
                "Rp1Submitter: firmware entered terminal state " +
                std::string(stateName(s)) +
                " before becoming READY; reset/recover the device");
        }
        if (std::chrono::steady_clock::now() > deadline) {
            throw Rp1TimeoutError(
                std::string("Rp1Submitter: timed out waiting for state ") +
                stateName(target) + " (current=" + stateName(s) + ")");
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

/*
 * graph_done_seq is the sole release point for a terminal publication.
 * Firmware deliberately writes result magic and terminal state first, so
 * neither field may short-circuit this exact-sequence wait.
 */
Rp1GraphResult Rp1Submitter::waitForGraphDone(
    std::uint32_t want_seq, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const std::uint32_t done = window_->readGraphDoneSeq();
        if (done == want_seq) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            return readGraphResult(want_seq);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            const std::uint32_t state = window_->readState();
            const std::uint32_t cur = window_->readU32(
                offsetof(rp1_ctrl_t, rp1_current_node));
            const std::uint32_t err = window_->readErrorCode();
            rp1_graph_result_t result{};
            window_->readGraphResult(result);
            throw Rp1TimeoutError(
                "Rp1Submitter: timed out waiting for graph_done_seq=" +
                std::to_string(want_seq) +
                " (got " + std::to_string(done) +
                ", state=" + stateName(state) +
                ", current_node=" + std::to_string(cur) +
                ", error_code=" + std::to_string(err) +
                ", result_magic=" + std::to_string(result.magic) +
                ", result_seq=" + std::to_string(result.graph_seq) +
                ", result_outcome=" + std::to_string(result.outcome) + ")");
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

/*
 * Convert the stable wire record only after validating every discriminant.
 * State/outcome agreement catches a reordered or torn terminal publication;
 * image-id agreement prevents higher layers from trusting a malformed image.
 */
Rp1GraphResult Rp1Submitter::readGraphResult(
    std::uint32_t want_seq) {
    rp1_graph_result_t wire{};
    window_->readGraphResult(wire);
    const std::uint32_t state = window_->readState();

    const auto corrupt = [want_seq](const std::string& reason) {
        throw std::runtime_error(
            "Rp1Submitter: corrupt graph result for sequence " +
            std::to_string(want_seq) + ": " + reason);
    };
    if (wire.magic != RP1_GRAPH_RESULT_MAGIC) {
        corrupt("magic=" + std::to_string(wire.magic) +
                ", expected=" + std::to_string(RP1_GRAPH_RESULT_MAGIC));
    }
    if (wire.graph_seq != want_seq) {
        corrupt("result sequence=" + std::to_string(wire.graph_seq) +
                ", expected=" + std::to_string(want_seq));
    }
    /*
     * A committed hazardous result is enough to close the session even when
     * another field is corrupt. Firmware may still own staged addresses.
     */
    if ((wire.flags & kPoisonFlags) != 0u) {
        poisoned_.store(true, std::memory_order_release);
    }
    const std::uint32_t finiteDone =
        (wire.quiescence >> RP1_QUIESCE_FINITE_DONE_SHIFT) &
        RP1_QUIESCE_COUNT_MASK;
    const std::uint32_t finiteTimeout =
        (wire.quiescence >> RP1_QUIESCE_FINITE_TIMEOUT_SHIFT) &
        RP1_QUIESCE_COUNT_MASK;
    const std::uint32_t infinite =
        (wire.quiescence >> RP1_QUIESCE_INFINITE_SHIFT) &
        RP1_QUIESCE_COUNT_MASK;
    if (finiteTimeout != 0u || infinite != 0u) {
        /*
         * Counters are direct evidence that firmware may retain access even
         * when a corrupt result omitted the matching hazard flags.
         */
        poisoned_.store(true, std::memory_order_release);
    }
    if (finiteTimeout != 0u &&
        (wire.flags & RP1_RESULT_RECOVERY_REQUIRED) == 0u) {
        corrupt("finite timeout quiescence lacks recovery-required flag");
    }
    if (infinite != 0u &&
        (wire.flags & (RP1_RESULT_RECOVERY_REQUIRED |
                       RP1_RESULT_INFINITE_WORK_REMAINS)) !=
            (RP1_RESULT_RECOVERY_REQUIRED |
             RP1_RESULT_INFINITE_WORK_REMAINS)) {
        corrupt("infinite quiescence lacks recovery/infinite flags");
    }

    Rp1GraphOutcome outcome = Rp1GraphOutcome::Success;
    std::uint32_t expectedState = RP1_STATE_READY;
    switch (wire.outcome) {
        case RP1_GRAPH_RESULT_SUCCESS:
            outcome = Rp1GraphOutcome::Success;
            expectedState = RP1_STATE_READY;
            if (wire.error_code != 0u) {
                corrupt("SUCCESS carries error_code=" +
                        std::to_string(wire.error_code));
            }
            if ((wire.flags &
                 (RP1_RESULT_RECOVERY_REQUIRED |
                  RP1_RESULT_EFFECTS_MAY_BE_PARTIAL)) != 0u) {
                corrupt("SUCCESS carries terminal-only flags=" +
                        std::to_string(wire.flags));
            }
            break;
        case RP1_GRAPH_RESULT_FAILED:
            outcome = Rp1GraphOutcome::Failed;
            expectedState = RP1_STATE_ERROR;
            if (wire.error_code == 0u) {
                corrupt("FAILED carries no terminal error code");
            }
            break;
        case RP1_GRAPH_RESULT_HALTED:
            outcome = Rp1GraphOutcome::Halted;
            expectedState = RP1_STATE_HALTED;
            if (wire.error_code != 0u ||
                wire.terminal_opcode != RP1_OP_HALT) {
                corrupt("HALTED terminal record is inconsistent");
            }
            break;
        default:
            corrupt("outcome=" + std::to_string(wire.outcome) +
                    " is not terminal");
    }
    if (state != expectedState) {
        corrupt("state=" + std::string(stateName(state)) +
                " does not match outcome=" +
                std::to_string(wire.outcome));
    }

    Rp1ImageState imageState = Rp1ImageState::None;
    switch (wire.image_state) {
        case RP1_IMAGE_STATE_NONE:
            imageState = Rp1ImageState::None;
            break;
        case RP1_IMAGE_STATE_KNOWN:
            imageState = Rp1ImageState::Known;
            break;
        case RP1_IMAGE_STATE_UNKNOWN:
            imageState = Rp1ImageState::Unknown;
            break;
        default:
            corrupt("image_state=" + std::to_string(wire.image_state) +
                    " is invalid");
    }
    if ((imageState == Rp1ImageState::Known) !=
        (wire.active_image_id != 0u)) {
        corrupt("active_image_id=" +
                std::to_string(wire.active_image_id) +
                " disagrees with image_state=" +
                std::to_string(wire.image_state));
    }

    Rp1GraphResult result;
    result.sequence = wire.graph_seq;
    result.outcome = outcome;
    result.flags = wire.flags;
    if (outcome != Rp1GraphOutcome::Success) {
        result.terminal = Rp1TerminalError{
            wire.error_code,
            wire.terminal_node,
            wire.terminal_opcode,
            wire.error_detail,
            wire.error_aux};
    }
    result.activeImageId = wire.active_image_id;
    result.imageState = imageState;
    result.completedOperations = wire.completed_operations;
    result.graphElapsedTicks = wire.graph_elapsed_ticks;
    result.publishElapsedTicks = wire.publish_elapsed_ticks;
    result.traceWriteIndex = wire.trace_write_idx;
    result.quiescence.finiteDone = finiteDone;
    result.quiescence.finiteTimeout = finiteTimeout;
    result.quiescence.infinite = infinite;
    return result;
}

}  // namespace vrt::graph::fpga
