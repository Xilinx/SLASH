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

/// @file debug/rp1_probe.cpp
/// @brief Implementation of the RP1 firmware bring-up probe debug commands.
///
/// Ported from the former @c examples/rp1_bringup tool.  The control block,
/// node array, and signal array live at their default DDR offsets (see
/// @c RP1_DEFAULT_*_OFFSET in rp1_protocol.h) within the host-visible BAR
/// window; the window itself begins at @c --ctrl-offset bytes into the BAR
/// (64 MiB by default, the relationship validated by the RP1 memcheck path).

#include "rp1_probe.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <slash/uapi/rp1_protocol.h>
#include <vrtd/session.hpp>

#include "../bdf.hpp"

namespace {

/*
 * A distinctive nonzero sentinel distinguishes this probe's SIGNAL side effect
 * from reset state or stale zeroes; the sequence-tagged graph result proves
 * which scanner submission completed the node.
 */
constexpr uint32_t kSignalMagic     = 0xDEADBEEFu;
/// Trace-ring capacity sufficient for the one-node probe lifecycle.
constexpr uint32_t kBringupTraceSize = 64u;
/// Absolute graph-result wait limit.
constexpr auto     kPollTimeout     = std::chrono::seconds(3);
/// Host polling cadence for sequence and heartbeat reads.
constexpr auto     kPollInterval    = std::chrono::milliseconds(1);
/// Heartbeat silence classified as a firmware stall.
constexpr auto     kStallWindow     = std::chrono::milliseconds(500);

/**
 * @brief Stable host snapshot of the protocol-v6 graph result.
 */
struct GraphResultSnapshot {
    /// Commit marker written after the result payload.
    uint32_t magic = 0;
    /// Exact accepted graph sequence.
    uint32_t graphSeq = 0;
    /// Terminal rp1_graph_outcome_t value.
    uint32_t outcome = 0;
    /// Raw RP1_RESULT_* mask.
    uint32_t flags = 0;
    /// First terminal RP1_ERR_* code.
    uint32_t errorCode = 0;
    /// Failing or HALT node.
    uint32_t terminalNode = RP1_TERMINAL_ERROR_NODE_NONE;
    /// Opcode associated with the terminal node.
    uint32_t terminalOpcode = RP1_TERMINAL_OPCODE_NONE;
    /// Error-specific primary detail.
    uint32_t errorDetail = 0;
    /// Error-specific auxiliary detail.
    uint32_t errorAux = 0;
    /// Final known image identifier.
    uint32_t activeImageId = 0;
    /// Final rp1_image_state_t value.
    uint32_t imageState = RP1_IMAGE_STATE_NONE;
    /// Successful operation executions, including loop repeats.
    uint32_t completedOperations = 0;
    /// PMU ticks through GRAPH_DONE.
    uint32_t graphElapsedTicks = 0;
    /// PMU ticks through final result preparation.
    uint32_t publishElapsedTicks = 0;
    /// Final trace producer cursor.
    uint32_t traceWriteIndex = 0;
    /// Packed terminal quiescence counts.
    uint32_t quiescence = 0;
};

/// Return true when @p text starts with a C-style hexadecimal prefix.
bool hasHexPrefix(std::string_view text) {
    return text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
}

/// Parse one decimal or prefixed-hexadecimal unsigned command-line value.
uint64_t parseUnsigned(std::string_view text, const char* fieldName) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(fieldName) + " is required");
    }

    std::string_view digits = text;
    int base = 10;
    if (hasHexPrefix(text)) {
        digits = text.substr(2);
        base = 16;
        if (digits.empty()) {
            throw std::invalid_argument(std::string(fieldName) + " has no digits after 0x prefix");
        }
    }

    uint64_t value{};
    const char* begin = digits.data();
    const char* end = begin + digits.size();
    std::from_chars_result result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc() || result.ptr != end) {
        throw std::invalid_argument(std::string("Invalid ") + fieldName + ": '" + std::string(text) + "'");
    }
    return value;
}

/// Return a diagnostic name for an RP1 firmware state.
const char* stateStr(uint32_t s) {
    switch (s) {
    case RP1_STATE_INIT:    return "INIT";
    case RP1_STATE_READY:   return "READY";
    case RP1_STATE_RUNNING: return "RUNNING";
    case RP1_STATE_ERROR:   return "ERROR";
    case RP1_STATE_HALTED:  return "HALTED";
    default:                return "?";
    }
}

/// Return a diagnostic name for a graph-result outcome.
const char* outcomeStr(uint32_t outcome) {
    switch (outcome) {
    case RP1_GRAPH_RESULT_NONE:    return "NONE";
    case RP1_GRAPH_RESULT_SUCCESS: return "SUCCESS";
    case RP1_GRAPH_RESULT_FAILED:  return "FAILED";
    case RP1_GRAPH_RESULT_HALTED:  return "HALTED";
    default:                       return "?";
    }
}

/// Return a diagnostic name for firmware image knowledge.
const char* imageStateStr(uint32_t state) {
    switch (state) {
    case RP1_IMAGE_STATE_NONE:    return "NONE";
    case RP1_IMAGE_STATE_KNOWN:   return "KNOWN";
    case RP1_IMAGE_STATE_UNKNOWN: return "UNKNOWN";
    default:                      return "?";
    }
}

/// Return a diagnostic name for an RP1 graph opcode.
const char* opcodeStr(uint32_t opcode) {
    switch (opcode) {
    case RP1_OP_NOP:             return "NOP";
    case RP1_OP_WAIT:            return "WAIT";
    case RP1_OP_SIGNAL:          return "SIGNAL";
    case RP1_OP_KERNEL_DISPATCH: return "KERNEL_DISPATCH";
    case RP1_OP_SCALAR_WRITE:    return "SCALAR_WRITE";
    case RP1_OP_SCALAR_READ:     return "SCALAR_READ";
    case RP1_OP_SCALAR_COPY:     return "SCALAR_COPY";
    case RP1_OP_DMA_COPY:        return "DMA_COPY";
    case RP1_OP_DMA_FILL:        return "DMA_FILL";
    case RP1_OP_PDI_LOAD:        return "PDI_LOAD";
    case RP1_OP_LOOP:            return "LOOP";
    case RP1_OP_COND:            return "COND";
    case RP1_OP_RERUN:           return "RERUN";
    case RP1_OP_HALT:            return "HALT";
    case RP1_TERMINAL_OPCODE_NONE: return "NONE";
    default:                     return "?";
    }
}

/// Return a diagnostic name for one optional trace event.
const char* traceEventStr(uint32_t e) {
    switch (e) {
    case RP1_TRACE_GRAPH_START:    return "GRAPH_START";
    case RP1_TRACE_NODE_ACTIVATE:  return "NODE_ACTIVATE";
    case RP1_TRACE_KERNEL_LAUNCH:  return "KERNEL_LAUNCH";
    case RP1_TRACE_KERNEL_DONE:    return "KERNEL_DONE";
    case RP1_TRACE_KERNEL_TIMEOUT: return "KERNEL_TIMEOUT";
    case RP1_TRACE_LOOP_ITER:      return "LOOP_ITER";
    case RP1_TRACE_COND_EVAL:      return "COND_EVAL";
    case RP1_TRACE_WAIT_PARK:      return "WAIT_PARK";
    case RP1_TRACE_WAIT_WAKE:      return "WAIT_WAKE";
    case RP1_TRACE_PDI_LOAD:       return "PDI_LOAD";
    case RP1_TRACE_IMAGE_MISMATCH: return "IMAGE_MISMATCH";
    case RP1_TRACE_GRAPH_DONE:     return "GRAPH_DONE";
    case RP1_TRACE_FLUSH_START:    return "TRACE_FLUSH_START";
    case RP1_TRACE_FLUSH_END:      return "TRACE_FLUSH_END";
    default:                       return "?";
    }
}

/// Print whether firmware advertises one named capability.
void printCapability(const char* name, uint32_t capabilities, uint32_t mask) {
    std::printf("    %-29s = %s\n", name,
                (capabilities & mask) != 0u ? "yes" : "no");
}

/// Print whether one graph-result flag is present.
void printResultFlag(const char* name, uint32_t flags, uint32_t mask) {
    std::printf("    %-29s = %s\n", name,
                (flags & mask) != 0u ? "yes" : "no");
}

/*
 * Read each volatile word once after the graph_done_seq publication barrier.
 * The returned object owns its values and cannot observe a later submission.
 */
GraphResultSnapshot snapshotResult(volatile rp1_ctrl_t* c) {
    GraphResultSnapshot result;
    result.magic = c->result.magic;
    result.graphSeq = c->result.graph_seq;
    result.outcome = c->result.outcome;
    result.flags = c->result.flags;
    result.errorCode = c->result.error_code;
    result.terminalNode = c->result.terminal_node;
    result.terminalOpcode = c->result.terminal_opcode;
    result.errorDetail = c->result.error_detail;
    result.errorAux = c->result.error_aux;
    result.activeImageId = c->result.active_image_id;
    result.imageState = c->result.image_state;
    result.completedOperations = c->result.completed_operations;
    result.graphElapsedTicks = c->result.graph_elapsed_ticks;
    result.publishElapsedTicks = c->result.publish_elapsed_ticks;
    result.traceWriteIndex = c->result.trace_write_idx;
    result.quiescence = c->result.quiescence;
    return result;
}

/// Print every field and decoded flag in a graph-result snapshot.
void printGraphResult(const GraphResultSnapshot& result) {
    std::printf("  result.magic              = 0x%08x (%s)\n",
                result.magic,
                result.magic == RP1_GRAPH_RESULT_MAGIC ? "RSLT" : "UNCOMMITTED");
    std::printf("  result.graph_seq          = %u\n", result.graphSeq);
    std::printf("  result.outcome            = %u (%s)\n",
                result.outcome, outcomeStr(result.outcome));
    std::printf("  result.flags              = 0x%08x\n", result.flags);
    printResultFlag("recovery_required", result.flags,
                    RP1_RESULT_RECOVERY_REQUIRED);
    printResultFlag("effects_may_be_partial", result.flags,
                    RP1_RESULT_EFFECTS_MAY_BE_PARTIAL);
    printResultFlag("infinite_work_remains", result.flags,
                    RP1_RESULT_INFINITE_WORK_REMAINS);
    printResultFlag("trace_enabled", result.flags,
                    RP1_RESULT_TRACE_ENABLED);
    printResultFlag("trace_overflow", result.flags,
                    RP1_RESULT_TRACE_OVERFLOW);
    printResultFlag("unreached_nodes", result.flags,
                    RP1_RESULT_UNREACHED_NODES);
    std::printf("  result.error_code         = %u\n", result.errorCode);
    if (result.terminalNode == RP1_TERMINAL_ERROR_NODE_NONE) {
        std::printf("  result.terminal_node      = 0x%08x (none)\n",
                    result.terminalNode);
    } else {
        std::printf("  result.terminal_node      = %u\n",
                    result.terminalNode);
    }
    std::printf("  result.terminal_opcode    = 0x%08x (%s)\n",
                result.terminalOpcode, opcodeStr(result.terminalOpcode));
    std::printf("  result.error_detail       = 0x%08x\n",
                result.errorDetail);
    std::printf("  result.error_aux          = 0x%08x\n",
                result.errorAux);
    std::printf("  result.active_image_id    = %u\n",
                result.activeImageId);
    std::printf("  result.image_state        = %u (%s)\n",
                result.imageState, imageStateStr(result.imageState));
    std::printf("  result.completed_operations = %u\n",
                result.completedOperations);
    std::printf("  result.graph_elapsed_ticks  = %u\n",
                result.graphElapsedTicks);
    std::printf("  result.publish_elapsed_ticks = %u\n",
                result.publishElapsedTicks);
    std::printf("  result.trace_write_idx      = %u\n",
                result.traceWriteIndex);
    std::printf("  result.quiescence           = 0x%08x\n",
                result.quiescence);
    std::printf("    finite_done/finite_timeout/infinite = %u/%u/%u\n",
                (result.quiescence >> RP1_QUIESCE_FINITE_DONE_SHIFT) &
                    RP1_QUIESCE_COUNT_MASK,
                (result.quiescence >> RP1_QUIESCE_FINITE_TIMEOUT_SHIFT) &
                    RP1_QUIESCE_COUNT_MASK,
                (result.quiescence >> RP1_QUIESCE_INFINITE_SHIFT) &
                    RP1_QUIESCE_COUNT_MASK);
}

/*
 * A successful one-node probe has no terminal record, partial effects,
 * recovery requirement, outstanding work, trace overflow, or unreached node.
 * Image identity may be NONE or KNOWN because the probe does not reprogram.
 */
bool validateProbeResult(const GraphResultSnapshot& result, uint32_t wantSeq,
                         bool traceExpected) {
    constexpr uint32_t kKnownFlags =
        RP1_RESULT_RECOVERY_REQUIRED |
        RP1_RESULT_EFFECTS_MAY_BE_PARTIAL |
        RP1_RESULT_INFINITE_WORK_REMAINS |
        RP1_RESULT_TRACE_ENABLED |
        RP1_RESULT_TRACE_OVERFLOW |
        RP1_RESULT_UNREACHED_NODES;
    constexpr uint32_t kFailureFlags =
        RP1_RESULT_RECOVERY_REQUIRED |
        RP1_RESULT_EFFECTS_MAY_BE_PARTIAL |
        RP1_RESULT_INFINITE_WORK_REMAINS |
        RP1_RESULT_TRACE_OVERFLOW |
        RP1_RESULT_UNREACHED_NODES;

    bool valid = true;
    if (result.magic != RP1_GRAPH_RESULT_MAGIC) {
        std::fprintf(stderr,
                     "FAIL: graph result magic=0x%08x, expected 0x%08x\n",
                     result.magic,
                     static_cast<uint32_t>(RP1_GRAPH_RESULT_MAGIC));
        valid = false;
    }
    if (result.graphSeq != wantSeq) {
        std::fprintf(stderr,
                     "FAIL: graph result seq=%u, expected %u\n",
                     result.graphSeq, wantSeq);
        valid = false;
    }
    if (result.outcome != RP1_GRAPH_RESULT_SUCCESS) {
        std::fprintf(stderr,
                     "FAIL: graph outcome=%s(%u), expected SUCCESS(%u)\n",
                     outcomeStr(result.outcome), result.outcome,
                     static_cast<uint32_t>(RP1_GRAPH_RESULT_SUCCESS));
        valid = false;
    }
    if ((result.flags & ~kKnownFlags) != 0u ||
        (result.flags & kFailureFlags) != 0u) {
        std::fprintf(stderr,
                     "FAIL: graph result has invalid success flags 0x%08x\n",
                     result.flags);
        valid = false;
    }
    const bool traceFlag =
        (result.flags & RP1_RESULT_TRACE_ENABLED) != 0u;
    if (traceFlag != traceExpected ||
        (traceExpected && result.traceWriteIndex == 0u) ||
        (!traceExpected && result.traceWriteIndex != 0u)) {
        std::fprintf(stderr,
                     "FAIL: graph trace result flag=%u write_idx=%u, "
                     "expected enabled=%u\n",
                     traceFlag ? 1u : 0u, result.traceWriteIndex,
                     traceExpected ? 1u : 0u);
        valid = false;
    }
    if (result.errorCode != 0u ||
        result.terminalNode != RP1_TERMINAL_ERROR_NODE_NONE ||
        result.terminalOpcode != RP1_TERMINAL_OPCODE_NONE ||
        result.errorDetail != 0u || result.errorAux != 0u) {
        std::fprintf(stderr,
                     "FAIL: successful graph result carries terminal error data\n");
        valid = false;
    }
    const bool imageConsistent =
        (result.imageState == RP1_IMAGE_STATE_NONE &&
         result.activeImageId == 0u) ||
        (result.imageState == RP1_IMAGE_STATE_KNOWN &&
         result.activeImageId != 0u);
    if (!imageConsistent) {
        std::fprintf(stderr,
                     "FAIL: successful graph result has image=%s(%u):%u\n",
                     imageStateStr(result.imageState), result.imageState,
                     result.activeImageId);
        valid = false;
    }
    if (result.completedOperations != 1u) {
        std::fprintf(stderr,
                     "FAIL: graph result completed %u operations, expected 1\n",
                     result.completedOperations);
        valid = false;
    }
    if (result.publishElapsedTicks < result.graphElapsedTicks) {
        std::fprintf(stderr,
                     "FAIL: publish ticks %u precede graph ticks %u\n",
                     result.publishElapsedTicks, result.graphElapsedTicks);
        valid = false;
    }
    if (result.quiescence != 0u) {
        std::fprintf(stderr,
                     "FAIL: successful probe required quiescence 0x%08x\n",
                     result.quiescence);
        valid = false;
    }
    return valid;
}

/// Print the complete RP1 control and graph-result diagnostics.
void printCtrl(volatile rp1_ctrl_t* c) {
    std::printf("  magic            = 0x%08x (%s)\n",
                c->magic, (c->magic == RP1_CTRL_MAGIC) ? "SQR1" : "BAD");
    std::printf("  version          = %u\n",   c->version);
    std::printf("  node_count       = %u\n",   c->node_count);
    std::printf("  reserved_0x0c    = 0x%08x\n", c->_reserved_cq_size);
    std::printf("  node_base        = 0x%08x_%08x\n", c->node_base_hi, c->node_base_lo);
    std::printf("  reserved_0x18    = 0x%08x\n", c->_reserved_cq_base_lo);
    std::printf("  reserved_0x1c    = 0x%08x\n", c->_reserved_cq_base_hi);
    std::printf("  arg_buf_base     = 0x%08x_%08x\n", c->arg_buf_base_hi, c->arg_buf_base_lo);
    std::printf("  sig_array_base   = 0x%08x_%08x\n", c->sig_array_base_hi, c->sig_array_base_lo);
    std::printf("  trace_enable     = %u\n",   c->trace_enable);
    std::printf("  trace_base       = 0x%08x_%08x\n", c->trace_base_hi, c->trace_base_lo);
    std::printf("  trace_size       = %u\n",   c->trace_size);
    std::printf("  trace_write_idx  = %u\n",   c->trace_write_idx);
    const uint32_t capabilities = c->capabilities;
    std::printf("  capabilities     = 0x%08x\n", capabilities);
    printCapability("platform_pdi_ipi_config", capabilities,
                    RP1_CAP_PLATFORM_PDI_IPI_CONFIG);
    printCapability("pmu_cycle_timeouts", capabilities,
                    RP1_CAP_PMU_CYCLE_TIMEOUTS);
    printCapability("structured_pdi_response", capabilities,
                    RP1_CAP_STRUCTURED_PDI_RESPONSE);
    printCapability("latched_terminal_errors", capabilities,
                    RP1_CAP_LATCHED_TERMINAL_ERRORS);
    printCapability("btcm_trace_staging", capabilities,
                    RP1_CAP_BTCM_TRACE_STAGING);
    printCapability("graph_result", capabilities,
                    RP1_CAP_GRAPH_RESULT);
    std::printf("  required_capabilities = 0x%08x\n",
                static_cast<uint32_t>(RP1_REQUIRED_CAPABILITIES));
    std::printf("  missing_capabilities  = 0x%08x\n",
                static_cast<uint32_t>(RP1_REQUIRED_CAPABILITIES) & ~capabilities);
    std::printf("  pdi_ipi_platform_id   = 0x%08x (generated platform/IPI identity)\n",
                c->pdi_ipi_platform_id);
    std::printf("  graph_seq        = %u\n",   c->graph_seq);
    std::printf("  graph_done_seq   = %u\n",   c->graph_done_seq);
    std::printf("  reserved_0x28    = 0x%08x\n", c->_reserved_cq_write_idx);
    std::printf("  reserved_0x2c    = 0x%08x\n", c->_reserved_cq_read_idx);
    std::printf("  rp1_state        = %u (%s)\n", c->rp1_state, stateStr(c->rp1_state));
    std::printf("  rp1_error_code   = %u\n",   c->rp1_error_code);
    std::printf("  rp1_current_node = %u\n",   c->rp1_current_node);
    if (c->terminal_error_node == RP1_TERMINAL_ERROR_NODE_NONE) {
        std::printf("  terminal_error_node   = 0x%08x (none)\n",
                    c->terminal_error_node);
    } else {
        std::printf("  terminal_error_node   = %u\n", c->terminal_error_node);
    }
    std::printf("  terminal_error_detail = 0x%08x\n", c->terminal_error_detail);
    std::printf("  terminal_error_aux    = 0x%08x\n", c->terminal_error_aux);
    std::printf("  heartbeat        = %u\n",   c->heartbeat);
    std::printf("Graph result:\n");
    printGraphResult(snapshotResult(c));
}

/*
 * A usable probe requires four independent publications: magic, exact protocol
 * layout, every mandatory behavior bit, and a non-unknown IPI identity.
 * Keep this stricter than a heartbeat-only liveness check.
 */
constexpr bool contractFieldsCompatible(
    uint32_t magic, uint32_t version, uint32_t capabilities,
    uint32_t platform) {
    const uint32_t missing =
        static_cast<uint32_t>(RP1_REQUIRED_CAPABILITIES) & ~capabilities;
    return magic == RP1_CTRL_MAGIC &&
           version == RP1_PROTOCOL_VERSION &&
           missing == 0u &&
           platform != RP1_PDI_IPI_PLATFORM_UNKNOWN;
}

static_assert(
    !contractFieldsCompatible(
        RP1_CTRL_MAGIC, 5u, RP1_REQUIRED_CAPABILITIES, 1u),
    "protocol-v5 firmware must be rejected");

/// Return true only when the published firmware contract matches this host.
bool contractCompatible(volatile rp1_ctrl_t* c) {
    return contractFieldsCompatible(
        c->magic, c->version, c->capabilities,
        c->pdi_ipi_platform_id);
}

/// Diagnose an incompatible firmware contract before any probe BAR mutation.
bool validateFirmwareContract(volatile rp1_ctrl_t* c) {
    if (contractCompatible(c)) {
        return true;
    }
    std::fprintf(
        stderr,
        "ERROR: incompatible RP1 firmware contract "
        "(magic=0x%08x, version=%u, capabilities=0x%08x, "
        "platform_id=0x%08x); expected protocol v%u and capabilities 0x%08x.\n",
        c->magic, c->version, c->capabilities, c->pdi_ipi_platform_id,
        static_cast<uint32_t>(RP1_PROTOCOL_VERSION),
        static_cast<uint32_t>(RP1_REQUIRED_CAPABILITIES));
    return false;
}

/// Minimum BAR length needed to reach the bring-up trace ring.
uint64_t requiredBarLen(uint64_t ctrlOffset) {
    return ctrlOffset + RP1_DEFAULT_TRACE_OFFSET
         + static_cast<uint64_t>(kBringupTraceSize) * sizeof(rp1_trace_entry_t);
}

/// Zero @p bytes through a volatile BAR mapping.
void barZero(volatile void* dst, size_t bytes) {
    auto* p = static_cast<volatile uint8_t*>(dst);
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = 0;
    }
}

/// Copy @p bytes into a volatile BAR mapping without dropping writes.
void barWrite(volatile void* dst, const void* src, size_t bytes) {
    auto* out = static_cast<volatile uint8_t*>(dst);
    const auto* in = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = in[i];
    }
}

/// Initialize the common header of one staged node packet.
void nodeSetHeader(rp1_node_t* n, uint16_t opcode,
                   uint8_t awaitBucket, uint32_t awaitMask,
                   uint8_t setBucket, uint32_t setMask) {
    rp1_node_set_opcode(n, opcode);
    rp1_node_set_flags(n, 0u);
    rp1_node_set_status(n, RP1_NODE_PENDING);
    n->barrier_await_mask   = awaitMask;
    n->barrier_set_mask     = setMask;
    n->barrier_await_bucket = awaitBucket;
    n->barrier_set_bucket   = setBucket;
}

/// Program the control block's base-address fields for a graph of @p nodeCount nodes.
void programCtrl(volatile rp1_ctrl_t* c, uint32_t nodeCount) {
    c->node_count        = nodeCount;
    c->_reserved_cq_size = 0;
    c->node_base_lo      = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    c->node_base_hi      = 0;
    c->_reserved_cq_base_lo = 0;
    c->_reserved_cq_base_hi = 0;
    c->arg_buf_base_lo   = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    c->arg_buf_base_hi   = 0;
    c->sig_array_base_lo = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    c->sig_array_base_hi = 0;
    c->trace_enable      = 0;
    c->trace_base_lo     = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET);
    c->trace_base_hi     = 0;
    c->trace_size        = kBringupTraceSize;
}

/// Refuse to submit unless the firmware is alive and idle.  Prints a
/// diagnostic and returns false when it is not.
bool checkFirmwareReady(volatile rp1_ctrl_t* c) {
    if (c->magic != RP1_CTRL_MAGIC) {
        std::fprintf(stderr,
                     "ERROR: firmware magic = 0x%08x, expected 0x%08x (\"SQR1\").\n"
                     "       RP1 firmware not loaded -- load rp1.elf onto R5-1 via xsdb.\n",
                     c->magic, static_cast<uint32_t>(RP1_CTRL_MAGIC));
        return false;
    }
    if (c->graph_seq != c->graph_done_seq || c->rp1_state != RP1_STATE_READY) {
        std::fprintf(stderr,
                     "ERROR: firmware not READY for a new submission.\n"
                     "       rp1_state=%u (%s), graph_seq=%u, graph_done_seq=%u, heartbeat=%u\n"
                     "       The firmware is either mid-processing or wedged (hung AXI access).\n"
                     "       Re-read a moment later; if heartbeat is not advancing,\n"
                     "       reload rp1.elf onto R5-1 via xsdb to reset the state.\n",
                     c->rp1_state, stateStr(c->rp1_state),
                     c->graph_seq, c->graph_done_seq, c->heartbeat);
        return false;
    }
    return true;
}

/*
 * The wait has three exits: exact sequence completion, a live-but-slow absolute
 * timeout, or a heartbeat stall indicating the R5 stopped making progress.
 * Separating the latter gives bring-up users an actionable recovery path.
 */
/// Poll graph_done_seq until it equals @p wantSeq. Returns 0 on success,
/// -1 on timeout, -2 on a detected firmware hang (heartbeat stuck).
int waitForSeq(vrtd::BarFile& barFile, size_t ctrlOffset, uint32_t wantSeq) {
    using clock = std::chrono::steady_clock;
    const auto deadline    = clock::now() + kPollTimeout;
    uint32_t lastHb;
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read, ctrlOffset);
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());
        lastHb = c->heartbeat;
    }
    auto       lastHbTick  = clock::now();

    while (true) {
        uint32_t done;
        uint32_t hb;
        {
            /*
             * End each DMA-BUF read transaction before sleeping. Re-entering
             * the bracket makes the next firmware publication visible.
             */
            auto base = barFile.getPtr<uint8_t>(
                vrtd::BarFile::Direction::Read, ctrlOffset);
            auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());
            done = c->graph_done_seq;
            hb = c->heartbeat;
        }
        if (done == wantSeq) {
            return 0;
        }
        const auto now = clock::now();

        if (hb != lastHb) {
            lastHb = hb;
            lastHbTick = now;
        } else if (now - lastHbTick > kStallWindow) {
            std::fprintf(stderr,
                         "STALLED: heartbeat=%u has not advanced in 500 ms -- "
                         "R5 is hung on an AXI access.\n"
                         "         Reload rp1.elf onto R5-1 via xsdb to recover.\n",
                         hb);
            return -2;
        }

        if (now > deadline) {
            std::fprintf(stderr, "TIMEOUT: graph_done_seq=%u (want %u)\n",
                         done, wantSeq);
            return -1;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return 0;
}

/*
 * Validate enough BAR length for every probe subregion up front. A short map
 * must fail before pointer arithmetic can turn a diagnostic into a bad access.
 */
/// Open, validate, and map the RP1 BAR window for @p options.
///
/// The returned BarFile borrows nothing from @p session beyond the mapping
/// it owns, but the caller must keep @p session alive for its lifetime.
vrtd::BarFile openRp1Bar(const Rp1Probe::Options& options,
                         const vrtd::Session& session,
                         uint64_t ctrlOffset,
                         const char* cmdName) {
    const std::string bdf = resolveBoardBdf(options.bdf, cmdName);

    auto device = session.getDeviceByBdf(bdf);
    auto bar = device.getBar(static_cast<uint8_t>(options.bar));
    if (!bar.isUsable()) {
        throw std::runtime_error("Requested BAR is not usable");
    }

    vrtd::BarFile barFile = bar.openBarFile();
    const uint64_t len = static_cast<uint64_t>(barFile.getLen());
    const uint64_t need = requiredBarLen(ctrlOffset);
    if (len < need) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "BAR%u length 0x%lx < required 0x%lx (control block at offset 0x%lx)",
                      options.bar, static_cast<unsigned long>(len),
                      static_cast<unsigned long>(need),
                      static_cast<unsigned long>(ctrlOffset));
        throw std::runtime_error(msg);
    }
    return barFile;
}

} // namespace

/*
 * dump is intentionally passive: snapshot the published contract, then sample
 * heartbeat over a stall window. It diagnoses compatibility and liveness
 * without changing sequence, graph result, or graph storage.
 */
int Rp1Probe::dump(const Options& options) {
    const uint64_t ctrlOffset = parseUnsigned(options.ctrlOffsetText, "ctrl-offset");

    vrtd::Session session;
    vrtd::BarFile barFile = openRp1Bar(options, session, ctrlOffset, "debug rp1-dump");

    bool compatible;
    uint32_t hb1;
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());

        std::printf("RP1 control block @ R5 0x%08lx (BAR%u + 0x%lx):\n",
                    static_cast<unsigned long>(RP1_CTRL_PHYS_ADDR),
                    options.bar, static_cast<unsigned long>(ctrlOffset));
        printCtrl(c);
        compatible = contractCompatible(c);
        std::printf("Protocol contract: %s\n",
                    compatible ? "compatible" : "INCOMPATIBLE");
        hb1 = c->heartbeat;
    }
    std::this_thread::sleep_for(kStallWindow);
    uint32_t hb2;
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());
        hb2 = c->heartbeat;
    }
    if (hb2 != hb1) {
        std::printf("Liveness: heartbeat advanced %u -> %u (running)\n", hb1, hb2);
    } else {
        std::printf("Liveness: heartbeat unchanged at %u (stuck or not loaded)\n", hb1);
    }
    if (!compatible) {
        std::fprintf(stderr,
                     "ERROR: RP1 must report magic SQR1, protocol v%u, all required "
                     "capabilities, and a non-zero platform/IPI identity.\n",
                     static_cast<uint32_t>(RP1_PROTOCOL_VERSION));
        return 1;
    }
    return 0;
}

/*
 * ping bypasses the graph runtime with one SIGNAL packet. It validates the BAR
 * layout, startup contract, doorbell ordering, scanner side effect, exact
 * sequence publication, and committed graph result as one minimal transaction.
 */
int Rp1Probe::ping(const Options& options) {
    const uint64_t ctrlOffset = parseUnsigned(options.ctrlOffsetText, "ctrl-offset");

    vrtd::Session session;
    vrtd::BarFile barFile = openRp1Bar(options, session, ctrlOffset, "debug rp1-ping");

    /*
     * Phase 1: validate the full protocol and idle state in a read transaction.
     * No pointer from this bracket may escape into the later write transaction.
     */
    uint32_t wantSeq;
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());
        if (!validateFirmwareContract(c) || !checkFirmwareReady(c)) {
            return 1;
        }
        wantSeq = c->graph_done_seq + 1u;
    }

    /*
     * Phase 2: stage one SIGNAL node and clear its sentinel before publishing
     * a sequence change, so success cannot be inherited from an older run.
     */
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Write,
            static_cast<size_t>(ctrlOffset));
        volatile uint8_t* basePtr = base.get();
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(basePtr);
        auto* nodes = reinterpret_cast<volatile rp1_node_t*>(
            basePtr + RP1_DEFAULT_NODE_ARRAY_OFFSET);
        auto* sigs = reinterpret_cast<volatile rp1_signal_slot_t*>(
            basePtr + RP1_DEFAULT_SIG_ARRAY_OFFSET);

        rp1_node_t node{};
        nodeSetHeader(
            &node, RP1_OP_SIGNAL, /*await*/ 0, 0x0,
            /*set*/ 0, 0x1);
        node.payload.signal.target_slot = 0;
        node.payload.signal.value       = kSignalMagic;
        node.payload.signal.operation   = RP1_SIGOP_SET;
        barWrite(&nodes[0], &node, sizeof(node));

        sigs[0].value            = 0;
        sigs[0].last_writer_node = 0;
        sigs[0].flags            = 0;
        programCtrl(c, /*nodeCount*/ 1);

        /*
         * Fences make all staged bytes visible before graph_seq, the sole
         * firmware doorbell and exact completion value.
         */
        std::atomic_thread_fence(std::memory_order_seq_cst);
        c->graph_seq = wantSeq;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    std::printf("rp1-ping: submitted seq=%u, polling...\n", wantSeq);
    if (waitForSeq(
            barFile, static_cast<size_t>(ctrlOffset), wantSeq) != 0) {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        printCtrl(reinterpret_cast<volatile rp1_ctrl_t*>(base.get()));
        return 1;
    }

    /*
     * Phase 4: graph_done_seq releases both the sentinel side effect and the
     * sequence-tagged result. Validate both before reporting probe success.
     */
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        volatile uint8_t* basePtr = base.get();
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(basePtr);
        auto* sigs = reinterpret_cast<volatile rp1_signal_slot_t*>(
            basePtr + RP1_DEFAULT_SIG_ARRAY_OFFSET);

        std::atomic_thread_fence(std::memory_order_seq_cst);
        const GraphResultSnapshot result = snapshotResult(c);
        if (!validateProbeResult(result, wantSeq, /*traceExpected=*/false) ||
            c->rp1_state != RP1_STATE_READY) {
            printCtrl(c);
            return 1;
        }
        const uint32_t observed = sigs[0].value;
        if (observed != kSignalMagic) {
            std::fprintf(
                stderr,
                "FAIL: signal slot 0 = 0x%08x, expected 0x%08x\n",
                observed, kSignalMagic);
            printCtrl(c);
            return 1;
        }

        std::printf("Graph result:\n");
        printGraphResult(result);
        std::printf("PASS: slot[0] = 0x%08x, result_seq=%u, outcome=%s, "
                    "graph_ticks=%u, publish_ticks=%u, state=%s\n",
                    observed, result.graphSeq, outcomeStr(result.outcome),
                    result.graphElapsedTicks, result.publishElapsedTicks,
                    stateStr(c->rp1_state));
    }
    return 0;
}

/*
 * trace-ping repeats the sentinel transaction with tracing enabled, then reads
 * the committed result and trace ring. They prove externally visible graph
 * completion and the firmware's internal event ordering.
 */
int Rp1Probe::tracePing(const Options& options) {
    const uint64_t ctrlOffset = parseUnsigned(options.ctrlOffsetText, "ctrl-offset");

    vrtd::Session session;
    vrtd::BarFile barFile = openRp1Bar(options, session, ctrlOffset, "debug rp1-trace-ping");

    /*
     * Phase 1: reject an incompatible or busy firmware publication before
     * opening any BAR write transaction.
     */
    uint32_t wantSeq;
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());
        if (!validateFirmwareContract(c) || !checkFirmwareReady(c)) {
            return 1;
        }
        wantSeq = c->graph_done_seq + 1u;
    }

    /*
     * Phase 2: clear node, sentinel, and trace storage before enabling trace;
     * this makes every observed record attributable to the new sequence.
     */
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Write,
            static_cast<size_t>(ctrlOffset));
        volatile uint8_t* basePtr = base.get();
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(basePtr);
        auto* nodes = reinterpret_cast<volatile rp1_node_t*>(
            basePtr + RP1_DEFAULT_NODE_ARRAY_OFFSET);
        auto* sigs = reinterpret_cast<volatile rp1_signal_slot_t*>(
            basePtr + RP1_DEFAULT_SIG_ARRAY_OFFSET);
        auto* traces = reinterpret_cast<volatile rp1_trace_entry_t*>(
            basePtr + RP1_DEFAULT_TRACE_OFFSET);

        rp1_node_t node{};
        nodeSetHeader(
            &node, RP1_OP_SIGNAL, /*await*/ 0, 0x0,
            /*set*/ 0, 0x1);
        node.payload.signal.target_slot = 0;
        node.payload.signal.value       = kSignalMagic;
        node.payload.signal.operation   = RP1_SIGOP_SET;
        barWrite(&nodes[0], &node, sizeof(node));

        sigs[0].value            = 0;
        sigs[0].last_writer_node = 0;
        sigs[0].flags            = 0;
        barZero(traces, kBringupTraceSize * sizeof(rp1_trace_entry_t));

        programCtrl(c, /*nodeCount*/ 1);
        c->trace_enable = 1;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        c->graph_seq = wantSeq;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    std::printf("rp1-trace-ping: submitted seq=%u, polling...\n", wantSeq);
    if (waitForSeq(
            barFile, static_cast<size_t>(ctrlOffset), wantSeq) != 0) {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        printCtrl(reinterpret_cast<volatile rp1_ctrl_t*>(base.get()));
        return 1;
    }

    /*
     * Phase 4: validate the result and sentinel, then reconstruct the trace's
     * chronological suffix when its producer count has wrapped the ring.
     */
    {
        auto base = barFile.getPtr<uint8_t>(
            vrtd::BarFile::Direction::Read,
            static_cast<size_t>(ctrlOffset));
        volatile uint8_t* basePtr = base.get();
        auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(basePtr);
        auto* sigs = reinterpret_cast<volatile rp1_signal_slot_t*>(
            basePtr + RP1_DEFAULT_SIG_ARRAY_OFFSET);
        auto* traces = reinterpret_cast<volatile rp1_trace_entry_t*>(
            basePtr + RP1_DEFAULT_TRACE_OFFSET);

        std::atomic_thread_fence(std::memory_order_seq_cst);
        const GraphResultSnapshot result = snapshotResult(c);
        if (!validateProbeResult(result, wantSeq, /*traceExpected=*/true) ||
            c->rp1_state != RP1_STATE_READY) {
            printCtrl(c);
            return 1;
        }
        const uint32_t observed = sigs[0].value;
        if (observed != kSignalMagic) {
            std::fprintf(
                stderr,
                "FAIL: signal slot 0 = 0x%08x, expected 0x%08x\n",
                observed, kSignalMagic);
            printCtrl(c);
            return 1;
        }

        if (result.traceWriteIndex != c->trace_write_idx) {
            std::fprintf(
                stderr,
                "FAIL: result trace_write_idx=%u, control block reports %u\n",
                result.traceWriteIndex, c->trace_write_idx);
            return 1;
        }
        std::printf(
            "PASS: slot[0] = 0x%08x, result_seq=%u, outcome=%s, state=%s\n",
            observed, result.graphSeq, outcomeStr(result.outcome),
            stateStr(c->rp1_state));
        std::printf("Graph result:\n");
        printGraphResult(result);

        const uint32_t traceWritten = result.traceWriteIndex;
        const uint32_t traceCount = traceWritten > kBringupTraceSize
                                  ? kBringupTraceSize : traceWritten;
        const uint32_t traceStart = traceWritten > kBringupTraceSize
                                  ? traceWritten % kBringupTraceSize : 0;
        std::printf("Trace entries written=%u readable=%u%s:\n",
                    traceWritten, traceCount,
                    traceWritten > kBringupTraceSize ? " (overflow)" : "");
        for (uint32_t i = 0; i < traceCount; i++) {
            const uint32_t idx = (traceStart + i) % kBringupTraceSize;
            const auto& e = traces[idx];
            std::printf(
                "  trace[%u] t=%u event=%s(%u) node=%u "
                "aux0=0x%08x aux1=0x%08x\n",
                idx, e.timestamp, traceEventStr(e.event), e.event,
                e.node_index, e.aux0, e.aux1);
        }
    }

    return 0;
}
