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
 * @file v80_policy.h
 * @brief Client-side V80 placement-aware channel policy for QDMA transfers.
 *
 * The kernel returns the opaque SLASH_QDMA_TRANSFER_HINT_V80 marker on buffer
 * registration; the actual decision of how to spread a transfer across the
 * available QDMA queues lives here, where the buffer's device address is known.
 *
 * On the V80 a transfer takes two independent NoC paths: the host-side ingress
 * master (NMU) is chosen by the queue's mm-channel, while the memory-side
 * egress endpoint (NSU / HBM pseudo-channel) is chosen by the device address.
 * Sustaining both NMUs requires also spreading across two NSUs.  The policy:
 *
 *   - DDR (single NSU): split the range in half so both NMUs stay busy.
 *   - HBM below the 16 GiB half-boundary: channel 0 only.
 *   - HBM at/above the half-boundary:     channel 1 only.
 *   - HBM spanning the boundary:          split there (below -> ch0, above -> ch1).
 *
 * The qpair-to-channel mapping is the wire contract from vrtd: qpair_index 0 is
 * pinned to channel 0 and qpair_index 1 to channel 1 (see vrtd_resp_buffer_open).
 */

#ifndef VRTD_V80_POLICY_H
#define VRTD_V80_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * V80 device-memory geometry (mirrors vrt/vrtd/src/allocator.h and the
 * memory-model docs).  HBM and DDR are each 64 x 512 MiB = 32 GiB; the HBM
 * half-boundary at +16 GiB separates the two NoC slave-unit (NSU) regions.
 */
#define VRTD_V80_HBM_BASE 0x4000000000ULL
#define VRTD_V80_HBM_SIZE (64ULL * 512ULL * 1024ULL * 1024ULL)
#define VRTD_V80_HBM_HALF (VRTD_V80_HBM_SIZE / 2ULL)
#define VRTD_V80_DDR_BASE 0x60000000000ULL
#define VRTD_V80_DDR_SIZE (64ULL * 512ULL * 1024ULL * 1024ULL)

/** @brief Maximum segments a transfer is split into (one per mm-channel). */
#define VRTD_V80_MAX_SEGS 2u

/** @brief One contiguous sub-transfer routed to a specific qpair. */
struct vrtd_xfer_seg {
    uint32_t qpair_index;  /**< Index into the fd's bound qpairs (== channel). */
    uint64_t offset;       /**< Buffer-relative byte offset. */
    uint64_t size;         /**< Byte count. */
};

/**
 * @brief Compute the V80 transfer plan for a buffer range.
 *
 * Plans the transfer of [@p offset, @p offset + @p size) within a buffer based
 * at device address @p phys_addr across @p qpair_count available queue pairs
 * (qpair_index 0 == channel 0, qpair_index 1 == channel 1).  Split points are
 * aligned down to @p step so every emitted segment stays page-aligned.  With
 * fewer than two queue pairs (or a zero step) the whole range is assigned to
 * qpair_index 0.
 *
 * @param phys_addr   Device base address of the buffer.
 * @param offset      Buffer-relative start of the transfer.
 * @param size        Transfer length in bytes (assumed a multiple of step).
 * @param step        Transfer/page granule used to align split points.
 * @param qpair_count Number of available queue pairs (1 or 2).
 * @param segs        [out] Receives up to VRTD_V80_MAX_SEGS segments.
 * @return Number of segments written to @p segs (1 or 2).
 */
static inline uint32_t vrtd_plan_v80(uint64_t phys_addr,
                                     uint64_t offset,
                                     uint64_t size,
                                     uint64_t step,
                                     uint32_t qpair_count,
                                     struct vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS])
{
    if (qpair_count < 2u || step == 0u) {
        segs[0].qpair_index = 0u;
        segs[0].offset = offset;
        segs[0].size = size;
        return 1u;
    }

    uint64_t start = phys_addr + offset;
    uint64_t end = start + size;
    bool is_ddr = (phys_addr >= VRTD_V80_DDR_BASE &&
                   phys_addr < VRTD_V80_DDR_BASE + VRTD_V80_DDR_SIZE);

    uint64_t lo_len;
    if (is_ddr) {
        /* Single DDR NSU: just split the range to drive both NMUs. */
        lo_len = size / 2u;
    } else {
        /* HBM: route by the 16 GiB half-memory boundary (NSU split). */
        uint64_t boundary = VRTD_V80_HBM_BASE + VRTD_V80_HBM_HALF;
        if (end <= boundary) {
            lo_len = size;             /* entirely in the lower half -> ch0 */
        } else if (start >= boundary) {
            segs[0].qpair_index = 1u;  /* entirely in the upper half -> ch1 */
            segs[0].offset = offset;
            segs[0].size = size;
            return 1u;
        } else {
            lo_len = boundary - start;  /* spans the boundary */
        }
    }

    lo_len -= lo_len % step;        /* keep both segments page-aligned */

    if (lo_len == 0u || lo_len >= size) {
        segs[0].qpair_index = 0u;
        segs[0].offset = offset;
        segs[0].size = size;
        return 1u;
    }

    segs[0].qpair_index = 0u;
    segs[0].offset = offset;
    segs[0].size = lo_len;
    segs[1].qpair_index = 1u;
    segs[1].offset = offset + lo_len;
    segs[1].size = size - lo_len;
    return 2u;
}

#endif /* VRTD_V80_POLICY_H */
