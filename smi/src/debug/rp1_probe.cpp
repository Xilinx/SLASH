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

constexpr uint32_t kSignalMagic     = 0xDEADBEEFu;
constexpr uint32_t kBringupCqSize   = 64u;
constexpr auto     kPollTimeout     = std::chrono::seconds(3);
constexpr auto     kPollInterval    = std::chrono::milliseconds(1);
constexpr auto     kStallWindow     = std::chrono::milliseconds(500);

bool hasHexPrefix(std::string_view text) {
    return text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
}

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

void printCtrl(volatile rp1_ctrl_t* c) {
    std::printf("  magic            = 0x%08x (%s)\n",
                c->magic, (c->magic == RP1_CTRL_MAGIC) ? "SQR1" : "BAD");
    std::printf("  version          = %u\n",   c->version);
    std::printf("  node_count       = %u\n",   c->node_count);
    std::printf("  cq_size          = %u\n",   c->cq_size);
    std::printf("  node_base        = 0x%08x_%08x\n", c->node_base_hi, c->node_base_lo);
    std::printf("  cq_base          = 0x%08x_%08x\n", c->cq_base_hi, c->cq_base_lo);
    std::printf("  arg_buf_base     = 0x%08x_%08x\n", c->arg_buf_base_hi, c->arg_buf_base_lo);
    std::printf("  sig_array_base   = 0x%08x_%08x\n", c->sig_array_base_hi, c->sig_array_base_lo);
    std::printf("  graph_seq        = %u\n",   c->graph_seq);
    std::printf("  graph_done_seq   = %u\n",   c->graph_done_seq);
    std::printf("  cq_write_idx     = %u\n",   c->cq_write_idx);
    std::printf("  rp1_state        = %u (%s)\n", c->rp1_state, stateStr(c->rp1_state));
    std::printf("  rp1_error_code   = %u\n",   c->rp1_error_code);
    std::printf("  rp1_current_node = %u\n",   c->rp1_current_node);
    std::printf("  heartbeat        = %u\n",   c->heartbeat);
}

/// Minimum BAR length needed to reach the whole signal array.
uint64_t requiredBarLen(uint64_t ctrlOffset) {
    return ctrlOffset + RP1_DEFAULT_SIG_ARRAY_OFFSET
         + static_cast<uint64_t>(RP1_MAX_SIGNALS) * sizeof(rp1_signal_slot_t);
}

void barZero(volatile void* dst, size_t bytes) {
    auto* p = static_cast<volatile uint8_t*>(dst);
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = 0;
    }
}

void nodeSetHeader(volatile rp1_node_t* n, uint16_t opcode,
                   uint8_t awaitBucket, uint32_t awaitMask,
                   uint8_t setBucket, uint32_t setMask) {
    n->opcode               = opcode;
    n->flags                = 0;
    n->barrier_await_mask   = awaitMask;
    n->barrier_set_mask     = setMask;
    n->barrier_await_bucket = awaitBucket;
    n->barrier_set_bucket   = setBucket;
    n->status               = RP1_NODE_PENDING;
}

/// Program the control block's base-address fields for a graph of @p nodeCount nodes.
void programCtrl(volatile rp1_ctrl_t* c, uint32_t nodeCount) {
    c->node_count        = nodeCount;
    c->cq_size           = kBringupCqSize;
    c->node_base_lo      = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    c->node_base_hi      = 0;
    c->cq_base_lo        = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET);
    c->cq_base_hi        = 0;
    c->arg_buf_base_lo   = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    c->arg_buf_base_hi   = 0;
    c->sig_array_base_lo = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    c->sig_array_base_hi = 0;
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

/// Poll graph_done_seq until it reaches @p wantSeq.  Returns 0 on success,
/// -1 on timeout, -2 on a detected firmware hang (heartbeat stuck).
int waitForSeq(volatile rp1_ctrl_t* c, uint32_t wantSeq) {
    using clock = std::chrono::steady_clock;
    const auto deadline    = clock::now() + kPollTimeout;
    uint32_t   lastHb      = c->heartbeat;
    auto       lastHbTick  = clock::now();

    while (c->graph_done_seq < wantSeq) {
        const auto now = clock::now();

        const uint32_t hb = c->heartbeat;
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
                         c->graph_done_seq, wantSeq);
            return -1;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return 0;
}

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

int Rp1Probe::dump(const Options& options) {
    const uint64_t ctrlOffset = parseUnsigned(options.ctrlOffsetText, "ctrl-offset");

    vrtd::Session session;
    vrtd::BarFile barFile = openRp1Bar(options, session, ctrlOffset, "debug rp1-dump");

    auto base = barFile.getPtr<uint8_t>(vrtd::BarFile::Direction::Read, static_cast<size_t>(ctrlOffset));
    auto* c = reinterpret_cast<volatile rp1_ctrl_t*>(base.get());

    std::printf("RP1 control block @ R5 0x%08lx (BAR%u + 0x%lx):\n",
                static_cast<unsigned long>(RP1_CTRL_PHYS_ADDR),
                options.bar, static_cast<unsigned long>(ctrlOffset));
    printCtrl(c);

    const uint32_t hb1 = c->heartbeat;
    std::this_thread::sleep_for(kStallWindow);
    const uint32_t hb2 = c->heartbeat;
    if (hb2 != hb1) {
        std::printf("Liveness: heartbeat advanced %u -> %u (running)\n", hb1, hb2);
    } else {
        std::printf("Liveness: heartbeat unchanged at %u (stuck or not loaded)\n", hb1);
    }
    return 0;
}

int Rp1Probe::ping(const Options& options) {
    const uint64_t ctrlOffset = parseUnsigned(options.ctrlOffsetText, "ctrl-offset");

    vrtd::Session session;
    vrtd::BarFile barFile = openRp1Bar(options, session, ctrlOffset, "debug rp1-ping");

    // Hold a single write session over the whole submit + poll sequence and
    // reach every sub-region by offsetting from the control-block base.
    auto base = barFile.getPtr<uint8_t>(vrtd::BarFile::Direction::Write, static_cast<size_t>(ctrlOffset));
    volatile uint8_t* basePtr = base.get();
    auto* c     = reinterpret_cast<volatile rp1_ctrl_t*>(basePtr);
    auto* nodes = reinterpret_cast<volatile rp1_node_t*>(basePtr + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    auto* sigs  = reinterpret_cast<volatile rp1_signal_slot_t*>(basePtr + RP1_DEFAULT_SIG_ARRAY_OFFSET);

    if (!checkFirmwareReady(c)) {
        return 1;
    }

    // One-node SIGNAL graph: slot 0 <- kSignalMagic.
    barZero(&nodes[0], sizeof(rp1_node_t));
    nodeSetHeader(&nodes[0], RP1_OP_SIGNAL, /*await*/ 0, 0x0, /*set*/ 0, 0x1);
    nodes[0].payload.signal.target_slot = 0;
    nodes[0].payload.signal.value       = kSignalMagic;
    nodes[0].payload.signal.operation   = RP1_SIGOP_SET;

    sigs[0].value            = 0;
    sigs[0].last_writer_node = 0;
    sigs[0].flags            = 0;

    programCtrl(c, /*nodeCount*/ 1);

    const uint32_t wantSeq = c->graph_done_seq + 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    c->graph_seq = wantSeq;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::printf("rp1-ping: submitted seq=%u, polling...\n", wantSeq);
    if (waitForSeq(c, wantSeq) != 0) {
        printCtrl(c);
        return 1;
    }

    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint32_t observed = sigs[0].value;
    if (observed != kSignalMagic) {
        std::fprintf(stderr, "FAIL: signal slot 0 = 0x%08x, expected 0x%08x\n",
                     observed, kSignalMagic);
        printCtrl(c);
        return 1;
    }

    std::printf("PASS: slot[0] = 0x%08x, cq_write_idx=%u, state=%s\n",
                observed, c->cq_write_idx, stateStr(c->rp1_state));
    return 0;
}
