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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * rp1_bringup_kernel.hip — single-thread HIP kernel that talks to RP1
 * over PCIe peer-to-peer DMA through the BAR4 dma-buf imported into HSA.
 *
 * Functionally equivalent to the `signal` subcommand of the host-side
 * examples/rp1_bringup/rp1_bringup.c, but every BAR write originates on
 * the GPU. The host CPU writes nothing to BAR4 in the hot path.
 *
 * The kernel:
 *   1. Sanity-checks the RP1 control-block magic.
 *   2. Builds a one-node SIGNAL graph at the node array.
 *   3. Programs the control block's base addresses + node_count.
 *   4. Bumps graph_seq.
 *   5. Polls graph_done_seq until it reaches the new value (or times out).
 *   6. Reads back signal slot 0 and reports it via the result struct.
 *
 * Memory ordering:
 *   - The bar mapping is host-side write-combine (BAR4 is prefetchable).
 *     Every store goes through the GPU's WC buffer.
 *   - __threadfence_system() flushes prior stores to system memory and
 *     invalidates loads, providing acquire/release ordering w.r.t. the
 *     CPU/FPGA. We fence:
 *       * before the graph_seq bump (so RP1 cannot see graph_seq advance
 *         before nodes / ctrl have landed in DDR),
 *       * after the graph_seq bump,
 *       * each iteration of the polling loop (so each graph_done_seq
 *         read is fresh from DDR rather than the GPU's last-cached
 *         value).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include <slash/uapi/rp1_protocol.h>

#include "rp1_bringup_result.h"

/*
 * Host-relative byte offset of the RP1 64 MiB DDR aperture inside BAR4
 * on V80. Mirrors the BAR_CTRL_OFFSET conventions in
 * examples/rp1_bringup/rp1_bringup.c and examples/07_rp1_memcheck/07_rp1_memcheck.c.
 */
static constexpr uint64_t kBarCtrlOffset = 64ULL * 1024ULL * 1024ULL;

/*
 * Polling deadline expressed in GPU clock cycles via clock64(). The GPU
 * reference clock is typically 1–2 GHz on gfx90a / gfx10 / gfx11, so
 * 6e9 cycles caps the wall-clock at roughly 3–6 s, comfortably above
 * the host-side rp1_bringup POLL_TIMEOUT_NS = 3 s.
 */
static constexpr long long kPollDeadlineCycles =
    6LL * 1000LL * 1000LL * 1000LL;

/* Marker the SIGNAL node writes into slot 0 — same as the host signal
 * subcommand so a developer eyeballing dmesg / strace sees the same
 * value. */
static constexpr uint32_t kSignalMagic = 0xDEADBEEFu;

/**
 * @brief GPU-driven RP1 SIGNAL bringup.
 *
 * @param bar         BAR4 mapped into the GPU's address space (volatile
 *                    so the compiler cannot cache reads/writes).
 * @param bar_len     Length of the GPU mapping returned by
 *                    hsa_amd_interop_map_buffer().
 * @param result_out  Device-memory output struct, populated on exit.
 *
 * Launch geometry: <<<1, 1>>>. The protocol is inherently sequential
 * per graph; only thread 0 of block 0 does any work.
 */
__global__ void rp1_bringup_signal_gpu(volatile uint8_t* bar,
                                       size_t bar_len,
                                       rp1_bringup_result_t* result_out)
{
    if (threadIdx.x != 0 || blockIdx.x != 0)
        return;

    /* Bounds check: we touch ctrl + nodes + sigs, the highest of which
     * is sigs[255] at SIG_ARRAY_OFFSET + 255*16. */
    const uint64_t needed = kBarCtrlOffset + RP1_DEFAULT_SIG_ARRAY_OFFSET +
                            256u * sizeof(rp1_signal_slot_t);
    if (bar_len < needed) {
        result_out->status = RP1_BRINGUP_STATUS_NO_FW;
        return;
    }

    auto* ctrl  = reinterpret_cast<volatile rp1_ctrl_t*>(
                      bar + kBarCtrlOffset);
    auto* nodes = reinterpret_cast<volatile rp1_node_t*>(
                      bar + kBarCtrlOffset + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    auto* sigs  = reinterpret_cast<volatile rp1_signal_slot_t*>(
                      bar + kBarCtrlOffset + RP1_DEFAULT_SIG_ARRAY_OFFSET);

    /* ── 1. Verify the RP1 firmware actually wrote its magic ──────── */

    __threadfence_system();
    uint32_t magic = ctrl->magic;
    result_out->magic_seen = magic;
    if (magic != RP1_CTRL_MAGIC) {
        result_out->status = RP1_BRINGUP_STATUS_NO_FW;
        return;
    }

    /* ── 2. Build the one-node SIGNAL graph ───────────────────────── */

    /* Zero the entire 64-byte node packet (incl. payload reserved
     * bytes) before populating, matching the host bar_zero() pattern. */
    auto* node_words = reinterpret_cast<volatile uint32_t*>(&nodes[0]);
    #pragma unroll
    for (int i = 0;
         i < static_cast<int>(sizeof(rp1_node_t) / sizeof(uint32_t));
         ++i) {
        node_words[i] = 0;
    }

    nodes[0].opcode               = RP1_OP_SIGNAL;
    nodes[0].flags                = 0;
    nodes[0].barrier_await_mask   = 0;
    nodes[0].barrier_set_mask     = 0x1u;
    nodes[0].barrier_await_bucket = 0;
    nodes[0].barrier_set_bucket   = 0;
    nodes[0].status               = RP1_NODE_PENDING;
    nodes[0].payload.signal.target_slot = 0;
    nodes[0].payload.signal.value       = kSignalMagic;
    nodes[0].payload.signal.operation   = RP1_SIGOP_SET;

    /* Clear signal slot 0 so RP1's subsequent write is observable. */
    sigs[0].value            = 0;
    sigs[0].last_writer_node = 0;
    sigs[0].flags            = 0;

    /* ── 3. Program the control block (bases + counts) ────────────── */

    ctrl->node_count        = 1;
    ctrl->cq_size           = 64;
    ctrl->node_base_lo      = static_cast<uint32_t>(
        RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    ctrl->node_base_hi      = 0;
    ctrl->cq_base_lo        = static_cast<uint32_t>(
        RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET);
    ctrl->cq_base_hi        = 0;
    ctrl->arg_buf_base_lo   = static_cast<uint32_t>(
        RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    ctrl->arg_buf_base_hi   = 0;
    ctrl->sig_array_base_lo = static_cast<uint32_t>(
        RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    ctrl->sig_array_base_hi = 0;

    /* ── 4. Bump graph_seq ────────────────────────────────────────── */

    /* Snapshot graph_done_seq before publishing graph_seq so the new
     * sequence value is monotonic w.r.t. RP1's view. The fence flushes
     * all prior node + ctrl writes to DDR before graph_seq advances. */
    uint32_t want = ctrl->graph_done_seq + 1u;
    __threadfence_system();
    ctrl->graph_seq = want;
    __threadfence_system();

    /* ── 5. Poll graph_done_seq until it catches up (or we time out) ─ */

    const long long t0 = clock64();
    uint32_t polls = 0;
    uint32_t status = RP1_BRINGUP_STATUS_PENDING;
    while (true) {
        __threadfence_system();
        uint32_t gds = ctrl->graph_done_seq;
        ++polls;
        if (gds >= want)
            break;
        if (clock64() - t0 > kPollDeadlineCycles) {
            status = RP1_BRINGUP_STATUS_TIMEOUT;
            break;
        }
    }

    /* ── 6. Capture diagnostics + classify ────────────────────────── */

    __threadfence_system();
    result_out->slot0          = sigs[0].value;
    result_out->graph_done_seq = ctrl->graph_done_seq;
    result_out->rp1_state      = ctrl->rp1_state;
    result_out->polls          = polls;

    if (status == RP1_BRINGUP_STATUS_PENDING) {
        status = (result_out->slot0 == kSignalMagic)
                 ? RP1_BRINGUP_STATUS_PASS
                 : RP1_BRINGUP_STATUS_BAD_SLOT;
    }
    result_out->status = status;
}
