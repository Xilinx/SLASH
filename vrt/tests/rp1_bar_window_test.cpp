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
 * @file rp1_bar_window_test.cpp
 *
 * Unit tests for vrt::graph::fpga::Rp1BarWindow with a raw, in-process
 * buffer backing store (no vrtd daemon, no hardware).
 *
 * These tests pin the wire layout against `slash/uapi/rp1_protocol.h`
 * static asserts and verify the bracketed read/write helpers move
 * bytes at the right offsets.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <slash/uapi/rp1_protocol.h>
#include <vrt/graph/device/fpga/rp1_bar_window.hpp>

using vrt::graph::fpga::Rp1BarWindow;

namespace {

/// Sized backing buffer that satisfies the V80 layout: 64 MiB window
/// at offset 64 MiB inside a 128 MiB "BAR".
class WindowFixture : public ::testing::Test {
   protected:
    void SetUp() override {
        backing_.assign(kBarSize, std::byte{0});
        window_ = std::make_unique<Rp1BarWindow>(
            backing_.data(), backing_.size(), kWindowOff);
    }

    static constexpr std::size_t   kBarSize   = 128ULL << 20;
    static constexpr std::uint64_t kWindowOff = 64ULL << 20;

    std::vector<std::byte>        backing_;
    std::unique_ptr<Rp1BarWindow> window_;
};

}  // namespace

TEST(Rp1BarWindowProtocol, WireSizesMatchHeader) {
    // These are also asserted by RP1_STATIC_ASSERT in rp1_protocol.h, but
    // mirroring them in a runtime test makes regressions easy to spot in CI.
    EXPECT_EQ(sizeof(rp1_node_t),        std::size_t{64});
    EXPECT_EQ(sizeof(rp1_ctrl_t),        std::size_t{0x1000});
    EXPECT_EQ(sizeof(rp1_signal_slot_t), std::size_t{16});
    EXPECT_EQ(sizeof(rp1_cq_entry_t),    std::size_t{16});
    EXPECT_EQ(sizeof(rp1_trace_entry_t), std::size_t{16});
    EXPECT_EQ(RP1_PROTOCOL_VERSION, 4u);
    EXPECT_EQ(offsetof(rp1_ctrl_t, capabilities), std::size_t{0x64});
    EXPECT_EQ(offsetof(rp1_ctrl_t, pdi_ipi_platform_id),
              std::size_t{0x68});
    EXPECT_EQ(offsetof(rp1_ctrl_t, terminal_error_node),
              std::size_t{0x6C});
    EXPECT_EQ(offsetof(rp1_ctrl_t, terminal_error_detail),
              std::size_t{0x70});
    EXPECT_EQ(offsetof(rp1_ctrl_t, terminal_error_aux),
              std::size_t{0x74});
}

TEST_F(WindowFixture, MappedLengthAndWindowOffsetExposed) {
    EXPECT_EQ(window_->mappedLength(), kBarSize);
    EXPECT_EQ(window_->windowOffset(), kWindowOff);
}

TEST_F(WindowFixture, NullRawBufferIsRejected) {
    EXPECT_THROW(Rp1BarWindow(nullptr, 16, 0), std::invalid_argument);
}

TEST_F(WindowFixture, WindowOffsetBeyondBufferIsRejected) {
    EXPECT_THROW(Rp1BarWindow(backing_.data(), 16, 32), std::invalid_argument);
}

TEST_F(WindowFixture, ControlBlockReadWriteRoundTrip) {
    rp1_ctrl_t out{};
    out.magic            = RP1_CTRL_MAGIC;
    out.version          = RP1_PROTOCOL_VERSION;
    out.node_count       = 5;
    out.cq_size          = 64;
    out.node_base_lo     = 0x30001000u;
    out.cq_base_lo       = 0x30041000u;
    out.arg_buf_base_lo  = 0x30051000u;
    out.sig_array_base_lo = 0x30151000u;
    out.graph_seq        = 1;
    out.graph_done_seq   = 1;
    out.rp1_state        = RP1_STATE_READY;
    out.capabilities     = RP1_REQUIRED_CAPABILITIES;
    out.pdi_ipi_platform_id = RP1_PDI_IPI_PLATFORM_UNKNOWN;
    out.terminal_error_node = RP1_TERMINAL_ERROR_NODE_NONE;

    window_->writeCtrl(out);

    // Bytes land at backing_[kWindowOff + 0 .. kWindowOff + sizeof(rp1_ctrl_t)].
    rp1_ctrl_t echoed{};
    std::memcpy(&echoed, backing_.data() + kWindowOff, sizeof(echoed));
    EXPECT_EQ(echoed.magic, RP1_CTRL_MAGIC);
    EXPECT_EQ(echoed.node_count, 5u);
    EXPECT_EQ(echoed.rp1_state, RP1_STATE_READY);

    rp1_ctrl_t roundtrip{};
    window_->readCtrl(roundtrip);
    EXPECT_EQ(roundtrip.magic, RP1_CTRL_MAGIC);
    EXPECT_EQ(roundtrip.cq_size, 64u);
    EXPECT_EQ(roundtrip.sig_array_base_lo, 0x30151000u);
    EXPECT_EQ(roundtrip.capabilities, RP1_REQUIRED_CAPABILITIES);
    EXPECT_EQ(roundtrip.terminal_error_node,
              RP1_TERMINAL_ERROR_NODE_NONE);
}

TEST_F(WindowFixture, SingleWordHotPathAccessors) {
    window_->writeGraphSeq(0xDEADBEEFu);
    EXPECT_EQ(window_->readGraphSeq(), 0xDEADBEEFu);

    // Underlying byte layout matches the field offset.
    std::uint32_t direct = 0;
    std::memcpy(&direct,
                backing_.data() + kWindowOff + offsetof(rp1_ctrl_t, graph_seq),
                sizeof(direct));
    EXPECT_EQ(direct, 0xDEADBEEFu);

    window_->writeNodeCount(7);
    EXPECT_EQ(window_->readU32(offsetof(rp1_ctrl_t, node_count)), 7u);

    window_->writeTraceEnable(1);
    window_->writeTraceBase(0x30152000u, 0);
    window_->writeTraceSize(128);
    window_->writeU32(offsetof(rp1_ctrl_t, trace_write_idx), 9);
    EXPECT_EQ(window_->readU32(offsetof(rp1_ctrl_t, trace_enable)), 1u);
    EXPECT_EQ(window_->readU32(offsetof(rp1_ctrl_t, trace_base_lo)), 0x30152000u);
    EXPECT_EQ(window_->readU32(offsetof(rp1_ctrl_t, trace_size)), 128u);
    EXPECT_EQ(window_->readTraceWriteIdx(), 9u);
}

TEST_F(WindowFixture, WriteNodesUsesDefaultNodeArrayOffset) {
    constexpr std::size_t kCount = 3;
    rp1_node_t nodes[kCount] = {};
    nodes[0].opcode               = RP1_OP_KERNEL_DISPATCH;
    nodes[0].barrier_await_mask   = 0x1;
    nodes[0].barrier_set_mask     = 0x2;
    nodes[0].barrier_await_bucket = 0;
    nodes[0].barrier_set_bucket   = 0;
    nodes[0].payload.kernel_dispatch.kernel_base_addr  = 0x88010000u;
    nodes[0].payload.kernel_dispatch.arg_buffer_offset = 0;
    nodes[0].payload.kernel_dispatch.arg_count         = 3;

    nodes[1].opcode = RP1_OP_SIGNAL;
    nodes[1].payload.signal.target_slot = 1;
    nodes[1].payload.signal.value       = 0xCAFEBABE;

    nodes[2].opcode = RP1_OP_NOP;

    window_->writeNodes(nodes, kCount);

    rp1_node_t echoed[kCount] = {};
    std::memcpy(echoed,
                backing_.data() + kWindowOff + RP1_DEFAULT_NODE_ARRAY_OFFSET,
                sizeof(echoed));
    EXPECT_EQ(echoed[0].opcode, RP1_OP_KERNEL_DISPATCH);
    EXPECT_EQ(echoed[0].payload.kernel_dispatch.kernel_base_addr, 0x88010000u);
    EXPECT_EQ(echoed[1].payload.signal.value, 0xCAFEBABE);
    EXPECT_EQ(echoed[2].opcode, RP1_OP_NOP);
}

TEST_F(WindowFixture, WriteArgsLandsAtDefaultArgBuffer) {
    const std::uint32_t args[] = {0xAAAAu, 0xBBBBu, 0xCCCCu};
    window_->writeArgs(args, std::size(args));

    std::uint32_t echoed[3] = {};
    std::memcpy(echoed,
                backing_.data() + kWindowOff + RP1_DEFAULT_ARG_BUF_OFFSET,
                sizeof(echoed));
    EXPECT_EQ(echoed[0], 0xAAAAu);
    EXPECT_EQ(echoed[1], 0xBBBBu);
    EXPECT_EQ(echoed[2], 0xCCCCu);
}

TEST_F(WindowFixture, ClearAndReadSignalSlot) {
    // Pre-stamp slot 3 with junk.
    rp1_signal_slot_t junk{};
    junk.value            = 0x12345678u;
    junk.last_writer_node = 42;
    junk.flags            = 0xFu;
    std::memcpy(backing_.data() + kWindowOff + RP1_DEFAULT_SIG_ARRAY_OFFSET
                    + 3 * sizeof(rp1_signal_slot_t),
                &junk, sizeof(junk));

    window_->clearSignal(/*slot*/ 3);

    rp1_signal_slot_t out{};
    window_->readSignal(/*slot*/ 3, out);
    EXPECT_EQ(out.value, 0u);
    EXPECT_EQ(out.last_writer_node, 0u);
    EXPECT_EQ(out.flags, 0u);
}

TEST_F(WindowFixture, ReadCqEntryAtIndex) {
    rp1_cq_entry_t entry{};
    entry.node_index   = 4;
    entry.status       = RP1_CQ_OK;
    entry.error_detail = 0;
    entry.timestamp    = 0xABCDu;
    std::memcpy(backing_.data() + kWindowOff + RP1_DEFAULT_CQ_OFFSET
                    + 7 * sizeof(rp1_cq_entry_t),
                &entry, sizeof(entry));

    rp1_cq_entry_t out{};
    window_->readCq(/*idx*/ 7, out);
    EXPECT_EQ(out.node_index, 4u);
    EXPECT_EQ(out.status,     static_cast<std::uint32_t>(RP1_CQ_OK));
    EXPECT_EQ(out.timestamp,  0xABCDu);
}

TEST_F(WindowFixture, ReadTraceEntryAtIndex) {
    rp1_trace_entry_t entry{};
    entry.timestamp  = 0x1234u;
    entry.event      = RP1_TRACE_KERNEL_LAUNCH;
    entry.node_index = 2;
    entry.aux0       = 0x88010000u;
    entry.aux1       = 3;
    std::memcpy(backing_.data() + kWindowOff + RP1_DEFAULT_TRACE_OFFSET
                    + 5 * sizeof(rp1_trace_entry_t),
                &entry, sizeof(entry));

    rp1_trace_entry_t out{};
    window_->readTrace(/*idx*/ 5, out);
    EXPECT_EQ(out.timestamp,  0x1234u);
    EXPECT_EQ(out.event,      static_cast<std::uint16_t>(RP1_TRACE_KERNEL_LAUNCH));
    EXPECT_EQ(out.node_index, 2u);
    EXPECT_EQ(out.aux0,       0x88010000u);
    EXPECT_EQ(out.aux1,       3u);
}

TEST_F(WindowFixture, OutOfRangeWriteIsRejected) {
    // window_offset is at 64 MiB inside a 128 MiB buffer => 64 MiB usable.
    // Writing past the end should throw.
    std::vector<std::uint8_t> payload(8, 0xFFu);
    const std::uint32_t   near_end = static_cast<std::uint32_t>((64ULL << 20) - 4);
    EXPECT_THROW(window_->writeAt(near_end, payload.data(), payload.size()),
                 std::out_of_range);
}

TEST_F(WindowFixture, ZeroByteRequestIsNoop) {
    // Should not throw even at a value that would otherwise overflow.
    const std::uint32_t at_end = static_cast<std::uint32_t>(64ULL << 20);
    EXPECT_NO_THROW(window_->readAt (at_end, nullptr, 0));
    EXPECT_NO_THROW(window_->writeAt(at_end, nullptr, 0));
    EXPECT_NO_THROW(window_->zeroAt (at_end, 0));
}

TEST_F(WindowFixture, ZeroAtClearsRegion) {
    // Pre-fill a 1 KiB region with 0xAB.
    constexpr std::uint32_t kOff = RP1_DEFAULT_NODE_ARRAY_OFFSET + 4096;
    constexpr std::size_t   kLen = 1024;
    std::vector<std::uint8_t> fill(kLen, 0xABu);
    window_->writeAt(kOff, fill.data(), fill.size());

    // Verify the pre-fill landed.
    std::vector<std::uint8_t> echoed(kLen, 0);
    std::memcpy(echoed.data(), backing_.data() + kWindowOff + kOff, kLen);
    for (auto b : echoed) EXPECT_EQ(b, 0xABu);

    window_->zeroAt(kOff, kLen);

    std::memcpy(echoed.data(), backing_.data() + kWindowOff + kOff, kLen);
    for (auto b : echoed) EXPECT_EQ(b, 0u);
}

TEST_F(WindowFixture, ReadAndWriteAtRespectWindowOffset) {
    // Writing at window-relative offset 0 should land at backing_[kWindowOff],
    // not backing_[0].
    const std::uint32_t pattern = 0xFEEDFACE;
    window_->writeU32(0, pattern);

    std::uint32_t before_window = 0;
    std::memcpy(&before_window, backing_.data(), sizeof(before_window));
    EXPECT_EQ(before_window, 0u) << "bytes leaked before the window";

    std::uint32_t in_window = 0;
    std::memcpy(&in_window, backing_.data() + kWindowOff, sizeof(in_window));
    EXPECT_EQ(in_window, pattern);
}
