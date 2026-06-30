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
 * @file rp1_bar_window.hpp
 * @brief Rp1BarWindow — typed accessors over the BAR-visible RP1 control region.
 *
 * The RP1 firmware exposes its host-shared state through a 64 MiB DDR
 * aperture that lives at R5 physical address @c RP1_CTRL_PHYS_ADDR
 * (`0x3000_0000`). On V80 the host reaches that aperture through BAR4 at
 * a fixed 64 MiB byte offset (matching the convention validated by
 * `examples/07_rp1_memcheck` and `examples/rp1_bringup`).
 *
 * @c Rp1BarWindow owns the BAR mapping and provides bracketed, typed
 * access to the protocol structs defined in
 * `driver/libslash/include/slash/uapi/rp1_protocol.h`. It is the only
 * place in the FPGA graph backend that talks to vrtd directly.
 *
 * Two construction paths are supported:
 *
 *  - **Production:** pass a @c vrtd::BarFile obtained via
 *    @c Session::getDeviceByBdf(...).getBar(4).openBarFile().  Every
 *    public operation brackets exactly one read or write through
 *    @c BarFile::getPtr<>(Direction, offset) so the dma-buf
 *    SYNC_START/SYNC_END contract is honoured.
 *  - **Tests / in-process:** pass a raw, caller-owned mapping pointer +
 *    length.  No bracketing is performed; semantics are otherwise
 *    identical.  Used by `rp1_bar_window_test` (no daemon required) and
 *    by the @c FpgaDevice unit tests' fake-RP1 thread.
 *
 * The class is move-only and not thread-safe.  Callers that need
 * concurrent access from multiple threads must serialise externally.
 */

#ifndef VRT_GRAPH_DEVICE_FPGA_RP1_BAR_WINDOW_HPP
#define VRT_GRAPH_DEVICE_FPGA_RP1_BAR_WINDOW_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

#include <slash/uapi/rp1_protocol.h>
#include <vrtd/bar_file.hpp>

namespace vrt::graph::fpga {

/**
 * @brief Owning, typed accessor for the RP1 host-visible DDR window.
 *
 * Offsets passed to the @c readAt / @c writeAt / @c readU32 / @c writeU32
 * methods are **window-relative** (i.e. zero is the control block;
 * @c RP1_DEFAULT_NODE_ARRAY_OFFSET is the start of the node array). The
 * window's BAR-relative origin is supplied at construction time
 * (defaulting to 64 MiB on V80) and added internally before talking to
 * the underlying mapping.
 */
class Rp1BarWindow {
   public:
    /**
     * @brief Host-side BAR offset of the RP1 64 MiB DDR aperture on V80.
     *
     * Mirrors @c BAR_CTRL_OFFSET in @c examples/rp1_bringup/rp1_bringup.c
     * and the convention validated by `examples/07_rp1_memcheck`.
     */
    static constexpr std::uint64_t kDefaultWindowOffset = 64ULL << 20;

    /**
     * @brief Logical size of the host-visible window (64 MiB).
     *
     * Used internally for bounds checking; the underlying BAR mapping is
     * typically larger.
     */
    static constexpr std::uint64_t kWindowSize = 64ULL << 20;

    /**
     * @brief Production constructor: own a vrtd::BarFile.
     *
     * The mapping must cover at least @p window_offset + @c kWindowSize
     * bytes. Throws @c std::invalid_argument if it does not.
     */
    explicit Rp1BarWindow(vrtd::BarFile bar_file,
                          std::uint64_t window_offset = kDefaultWindowOffset);

    /**
     * @brief Test / in-process constructor: window over a raw mapping.
     *
     * The buffer at @p base must remain valid for the lifetime of this
     * object. @p length is the size of the caller-owned mapping, of
     * which @p window_offset bytes are reserved for non-RP1 use.
     */
    Rp1BarWindow(void* base, std::size_t length, std::uint64_t window_offset = 0);

    ~Rp1BarWindow();

    Rp1BarWindow(const Rp1BarWindow&)            = delete;
    Rp1BarWindow& operator=(const Rp1BarWindow&) = delete;
    Rp1BarWindow(Rp1BarWindow&&) noexcept;
    Rp1BarWindow& operator=(Rp1BarWindow&&) noexcept;

    // ---- Bulk access (window-relative byte offsets) ------------------

    /// Read @p n bytes from window-relative @p offset into @p dst.
    void readAt(std::uint32_t offset, void* dst, std::size_t n);

    /// Write @p n bytes from @p src to window-relative @p offset.
    void writeAt(std::uint32_t offset, const void* src, std::size_t n);

    /// Fill @p n bytes at window-relative @p offset with zero.
    void zeroAt(std::uint32_t offset, std::size_t n);

    // ---- Single 32-bit word access (hot path for polling) ------------

    /// Read a single 32-bit word at window-relative @p offset.
    std::uint32_t readU32(std::uint32_t offset);

    /// Write a single 32-bit word at window-relative @p offset.
    void writeU32(std::uint32_t offset, std::uint32_t value);

    // ---- Typed convenience wrappers ----------------------------------

    /// Full read of the 4 KiB control block at window offset 0.
    void readCtrl(rp1_ctrl_t& out) {
        readAt(0, &out, sizeof(out));
    }

    /// Full write of the 4 KiB control block at window offset 0.
    void writeCtrl(const rp1_ctrl_t& in) {
        writeAt(0, &in, sizeof(in));
    }

    /// Copy @p n consecutive 64-byte node packets to the node array at
    /// @c (RP1_DEFAULT_NODE_ARRAY_OFFSET + index * sizeof(rp1_node_t)).
    /// @p node_array_offset is the window-relative byte offset of the
    /// node array (defaults to @c RP1_DEFAULT_NODE_ARRAY_OFFSET).
    void writeNodes(const rp1_node_t* src, std::size_t n,
                    std::uint32_t node_array_offset = RP1_DEFAULT_NODE_ARRAY_OFFSET) {
        writeAt(node_array_offset, src, n * sizeof(rp1_node_t));
    }

    /// Stage @p words 32-bit argument words into the argument buffer.
    void writeArgs(const std::uint32_t* src, std::size_t words,
                   std::uint32_t arg_buf_offset = RP1_DEFAULT_ARG_BUF_OFFSET) {
        writeAt(arg_buf_offset, src, words * sizeof(std::uint32_t));
    }

    /// Reset signal slot @p slot to value=0, last_writer_node=0, flags=0.
    void clearSignal(std::uint32_t slot,
                     std::uint32_t sig_array_offset = RP1_DEFAULT_SIG_ARRAY_OFFSET);

    /// Read signal slot @p slot.
    void readSignal(std::uint32_t slot, rp1_signal_slot_t& out,
                    std::uint32_t sig_array_offset = RP1_DEFAULT_SIG_ARRAY_OFFSET);

    /// Read completion-queue entry @p idx.
    void readCq(std::uint32_t idx, rp1_cq_entry_t& out,
                std::uint32_t cq_offset = RP1_DEFAULT_CQ_OFFSET);

    // ---- Single-field accessors (cheap polling) ----------------------

    std::uint32_t readMagic()        { return readU32(offsetof(rp1_ctrl_t, magic)); }
    std::uint32_t readGraphSeq()     { return readU32(offsetof(rp1_ctrl_t, graph_seq)); }
    std::uint32_t readGraphDoneSeq() { return readU32(offsetof(rp1_ctrl_t, graph_done_seq)); }
    std::uint32_t readCqWriteIdx()   { return readU32(offsetof(rp1_ctrl_t, cq_write_idx)); }
    std::uint32_t readState()        { return readU32(offsetof(rp1_ctrl_t, rp1_state)); }
    std::uint32_t readErrorCode()    { return readU32(offsetof(rp1_ctrl_t, rp1_error_code)); }
    std::uint32_t readHeartbeat()    { return readU32(offsetof(rp1_ctrl_t, heartbeat)); }

    void writeGraphSeq(std::uint32_t value) {
        writeU32(offsetof(rp1_ctrl_t, graph_seq), value);
    }
    void writeNodeCount(std::uint32_t value) {
        writeU32(offsetof(rp1_ctrl_t, node_count), value);
    }
    void writeCqReadIdx(std::uint32_t value) {
        writeU32(offsetof(rp1_ctrl_t, cq_read_idx), value);
    }

    // ---- Diagnostics --------------------------------------------------

    /// Total length of the underlying mapping in bytes.
    std::size_t mappedLength() const noexcept;

    /// BAR-relative byte offset of window-offset zero.
    std::uint64_t windowOffset() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vrt::graph::fpga

#endif  // VRT_GRAPH_DEVICE_FPGA_RP1_BAR_WINDOW_HPP
