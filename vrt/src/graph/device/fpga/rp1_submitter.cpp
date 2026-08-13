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

bool isValidCqSize(std::uint32_t size) noexcept {
    return isPowerOfTwo(size) && size <= RP1_MAX_CQ_ENTRIES;
}

bool isValidCondition(std::uint16_t op) noexcept {
    return op <= RP1_COP_AND_Z;
}

/*
 * Validation is deliberately complete before the doorbell can move. Check the
 * shared header first, then opcode-specific slots and ranges, and reject
 * undefined packets or PDI loads that suppress durable completion evidence.
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

    for (std::size_t i = 0; i < image.nodes.size(); ++i) {
        const rp1_node_t& node = image.nodes[i];
        if (node.barrier_await_bucket >= RP1_MAX_BUCKETS ||
            node.barrier_set_bucket >= RP1_MAX_BUCKETS) {
            bad(i, "barrier bucket out of range");
        }
        switch (node.opcode) {
            case RP1_OP_NOP:
            case RP1_OP_SCALAR_WRITE:
            case RP1_OP_DMA_COPY:
            case RP1_OP_DMA_FILL:
            case RP1_OP_HALT:
                break;
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
                    argEnd > image.arg_buf.size() * sizeof(std::uint32_t)) {
                    bad(i, "KERNEL_DISPATCH argument range is invalid");
                }
                break;
            }
            case RP1_OP_PDI_LOAD:
                if ((node.flags & RP1_FLAG_SILENT) != 0u) {
                    bad(i, "PDI_LOAD must retain CQ evidence");
                }
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
                    (((node.payload.rerun.rerun_flags &
                       RP1_RERUN_CLEAR_STATE) != 0u) &&
                     node.payload.rerun.loop_id >= RP1_MAX_LOOPS)) {
                    bad(i, "RERUN target or loop id is invalid");
                }
                break;
            default:
                bad(i, "opcode is not defined by protocol v4");
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
            "Rp1Submitter: RP1 firmware is missing required protocol-v4 "
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

Rp1Submitter::Rp1Submitter(Rp1BarWindow& window, std::uint32_t cq_size)
    : window_(&window), cq_size_(cq_size) {
    if (!isValidCqSize(cq_size_)) {
        throw std::invalid_argument(
            "Rp1Submitter: cq_size must be a power of 2 in [1, " +
            std::to_string(RP1_MAX_CQ_ENTRIES) + "], got " +
            std::to_string(cq_size_));
    }
}

void Rp1Submitter::requireUsable() const {
    if (poisoned()) {
        throw std::runtime_error(
            "Rp1Submitter: device is poisoned after an indeterminate "
            "submission timeout; reset/recover the device and construct "
            "a new runtime device before submitting again");
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
    window_->writeTraceBase(static_cast<std::uint32_t>(
                                RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET),
                            0);
    window_->writeTraceSize(kDefaultTraceSize);
    window_->writeTraceEnable(0);
    last_trace_size_ = kDefaultTraceSize;

    // Synchronize the host-owned CQ consumer cursor to the producer's current
    // position. Other sequence/state fields remain firmware-owned.
    cq_cursor_ = window_->readCqWriteIdx();
    window_->writeCqReadIdx(cq_cursor_);
    pending_cq_.clear();

    std::atomic_thread_fence(std::memory_order_seq_cst);

    // Seed last_cq_start_ so callers that immediately drainCq() see an
    // empty range.
    last_cq_start_ = cq_cursor_;
    last_graph_seq_ = window_->readGraphSeq();

    ready_ = true;
}

/*
 * A submission has five ordered phases: claim and validate, establish READY,
 * retire prior CQ evidence, stage the image, then publish graph_seq and wait.
 * Only publication transfers ownership of the staged bytes to RP1.
 */
void Rp1Submitter::submitAndWait(const Rp1GraphImage& image,
                                  std::chrono::milliseconds timeout) {
    requireUsable();
    bool expected = false;
    if (!submission_active_.compare_exchange_strong(
            expected, true, std::memory_order_acquire,
            std::memory_order_relaxed)) {
        throw std::runtime_error(
            "Rp1Submitter: a graph submission is already active");
    }
    struct ActiveSubmission {
        std::atomic_bool& active;
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

    /*
     * Phase 2: establish the firmware contract and reject terminal or busy
     * state before this submission takes ownership of shared storage.
     */
    if (!ready_) {
        ensureReady(kDefaultReadyTimeout);
    }
    const std::uint32_t preflightState = window_->readState();
    if (preflightState != RP1_STATE_READY) {
        if (preflightState == RP1_STATE_ERROR ||
            preflightState == RP1_STATE_HALTED) {
            throwTerminalError(preflightState, window_->readGraphSeq());
        }
        throw std::runtime_error(
            "Rp1Submitter: firmware is not READY before submission "
            "(state=" + std::string(stateName(preflightState)) + ")");
    }

    /*
     * Phase 3: retire old CQ records, then stage arguments, signals, nodes,
     * and host-owned configuration while firmware still sees the old sequence.
     */
    drainAvailableCq();
    // CQ records carry no graph sequence. Retain evidence for exactly this
    // submission so an earlier image can never be interpreted as the next one.
    pending_cq_.clear();
    last_cq_start_ = cq_cursor_;

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
    last_cq_start_ = cq_cursor_;
    const std::uint32_t prev_seq = window_->readGraphSeq();
    const std::uint32_t want_seq = prev_seq + 1u;

    // Program node_count and (optionally) cq_size *before* bumping
    // graph_seq.  RP1's flat scanner reads node_count when it observes
    // graph_seq advancing, so this must be visible first.
    window_->writeNodeCount(static_cast<std::uint32_t>(image.nodes.size()));
    if (image.cq_size_override != 0) {
        if (!isValidCqSize(image.cq_size_override)) {
            throw std::invalid_argument(
                "Rp1Submitter: cq_size_override must be a power of 2 in [1, " +
                std::to_string(RP1_MAX_CQ_ENTRIES) + "], got " +
                std::to_string(image.cq_size_override));
        }
        // Persist for subsequent submissions too.
        cq_size_ = image.cq_size_override;
        window_->writeU32(offsetof(rp1_ctrl_t, cq_size), cq_size_);
    }

    const std::uint32_t trace_size =
        image.trace_size_override != 0 ? image.trace_size_override : kDefaultTraceSize;
    if (!isPowerOfTwo(trace_size) ||
        trace_size > RP1_MAX_TRACE_ENTRIES) {
        throw std::invalid_argument(
            "Rp1Submitter: trace_size must be a power of 2 in [1, " +
            std::to_string(RP1_MAX_TRACE_ENTRIES) + "], got " +
            std::to_string(trace_size));
    }
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
     * Phase 5: drain while waiting so a full CQ cannot stall firmware, then
     * translate the terminal publication into the host-visible result.
     */
    try {
        waitForGraphDone(want_seq, timeout);
    } catch (const Rp1TimeoutError&) {
        /*
         * The doorbell is irreversible: RP1 may still consume the image or
         * publish CQ after timeout. Poisoning prevents a second image racing it.
         */
        poisoned_.store(true, std::memory_order_release);
        throw;
    }

    // After completion, surface explicit firmware-side errors so the
    // caller doesn't have to remember to inspect rp1_state.
    const std::uint32_t state = window_->readState();
    if (state == RP1_STATE_ERROR || state == RP1_STATE_HALTED) {
        throwTerminalError(state, want_seq);
    }
}

void Rp1Submitter::clearSignalSlots(const std::vector<std::uint32_t>& slots) {
    requireUsable();
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

std::vector<rp1_cq_entry_t> Rp1Submitter::drainCq() {
    std::vector<rp1_cq_entry_t> out = drainCqRaw();
    validateCq(out);
    return out;
}

std::vector<rp1_cq_entry_t> Rp1Submitter::drainCqRaw() {
    drainAvailableCq();
    std::vector<rp1_cq_entry_t> out;
    out.swap(pending_cq_);
    last_cq_start_ = cq_cursor_;
    return out;
}

/*
 * Monotonic uint32_t subtraction remains valid across wrap while delta does
 * not exceed the ring; the power-of-two mask then selects physical slots.
 * Copy before advancing read_idx so firmware cannot overwrite unretained
 * evidence. Draining in the wait loop also relieves producer backpressure.
 */
void Rp1Submitter::drainAvailableCq() {
    const std::uint32_t end = window_->readCqWriteIdx();
    const std::uint32_t delta = end - cq_cursor_;
    if (delta > cq_size_) {
        throw std::runtime_error(
            "Rp1Submitter::drainCq: corrupt completion queue cursors (" +
            std::to_string(delta) + " entries for a " +
            std::to_string(cq_size_) + "-entry ring)");
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    pending_cq_.reserve(pending_cq_.size() + delta);
    for (std::uint32_t i = 0; i < delta; ++i) {
        const std::uint32_t idx = (cq_cursor_ + i) & (cq_size_ - 1u);
        rp1_cq_entry_t entry{};
        window_->readCq(idx, entry);
        pending_cq_.push_back(entry);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    window_->writeCqReadIdx(end);
    cq_cursor_ = end;
}

void Rp1Submitter::validateCq(
    const std::vector<rp1_cq_entry_t>& entries) {
    for (const rp1_cq_entry_t& entry : entries) {
        if (entry.status == RP1_CQ_OK) continue;
        const char* status =
            entry.status == RP1_CQ_TIMEOUT ? "timeout" : "error";
        throw std::runtime_error(
            "Rp1Submitter: node " +
            std::to_string(entry.node_index) + " reported " + status +
            " (status=" + std::to_string(entry.status) +
            ", detail=" + std::to_string(entry.error_detail) + ")");
    }
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
            throwTerminalError(s, window_->readGraphSeq());
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
 * Each poll first frees CQ space, then handles terminal, exact completion, or
 * timeout. Completion gets a final drain and state recheck because firmware
 * publishes CQ and errors before graph_done_seq, but BAR reads are separate.
 */
void Rp1Submitter::waitForGraphDone(std::uint32_t want_seq,
                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        drainAvailableCq();
        std::uint32_t state = window_->readState();
        if (state == RP1_STATE_ERROR || state == RP1_STATE_HALTED) {
            throwTerminalError(state, want_seq);
        }
        const std::uint32_t done = window_->readGraphDoneSeq();
        if (done == want_seq) {
            drainAvailableCq();
            state = window_->readState();
            if (state == RP1_STATE_ERROR || state == RP1_STATE_HALTED) {
                throwTerminalError(state, want_seq);
            }
            return;
        }
        if (std::chrono::steady_clock::now() > deadline) {
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

/*
 * Terminal state publishes the first-error-wins record after quiescence.
 * Read every field only after observing that state so diagnostics describe
 * the accepted graph rather than a partially written failure.
 */
void Rp1Submitter::throwTerminalError(
    std::uint32_t state, std::uint32_t graph_seq) const {
    const std::uint32_t code = window_->readErrorCode();
    const std::uint32_t node =
        window_->readU32(offsetof(rp1_ctrl_t, terminal_error_node));
    const std::uint32_t detail =
        window_->readU32(offsetof(rp1_ctrl_t, terminal_error_detail));
    const std::uint32_t aux =
        window_->readU32(offsetof(rp1_ctrl_t, terminal_error_aux));
    throw std::runtime_error(
        std::string("Rp1Submitter: firmware reported terminal state ") +
        stateName(state) + " for graph seq " + std::to_string(graph_seq) +
        " (error_code=" + std::to_string(code) +
        ", base_error=" + std::to_string(code & RP1_ERR_CODE_MASK) +
        ", recovery_required=" +
        std::to_string((code & RP1_ERR_RECOVERY_REQUIRED) != 0u) +
        ", node=" + std::to_string(node) +
        ", detail=" + std::to_string(detail) +
        ", aux=" + std::to_string(aux) + ")");
}

}  // namespace vrt::graph::fpga
