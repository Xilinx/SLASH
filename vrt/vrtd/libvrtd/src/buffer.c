/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file buffer.c
 *
 * DMA buffer lifecycle management for the vrtd C client library.
 *
 * Buffers are host-side memory regions used for DMA transfers to/from
 * the FPGA.  Each buffer is backed by an anonymous mmap of 4 KiB base pages
 * (transparent hugepages disabled) and associated with a QDMA queue pair fd
 * for performing the actual H2C / C2H transfers.
 *
 * Sync operations (sync_to_device / sync_from_device) accept arbitrary
 * in-buffer ranges. Internally, the QDMA fd requires page-aligned transfer
 * ranges, so libvrtd expands partial requests to the mapping granule and uses
 * a staging buffer when needed to preserve host-side partial-range semantics.
 *
 * Buffer lifecycle:
 *   1. vrtd_buffer_open()          -- daemon allocates, returns qpair fd
 *   2. vrtd_buffer_create_raw()    -- client mmaps host memory
 *   3. vrtd_buffer_sync_to/from_device() -- DMA transfers
 *   4. vrtd_buffer_close()         -- tells daemon to free, unmaps locally
 */

#define _GNU_SOURCE

#include <vrtd/vrtd.h>

#include <slash/qdma.h>

#include "v80_policy.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>


#include <stdio.h>

#define BASE_TRANSFER_STEP_SIZE (4ULL * 1024ULL)              // 4K

/*
 * Per-sync timing instrumentation.
 *
 * When SLASH_QDMA_TIMING is non-zero (compile-time flag, e.g. built with
 * -DSLASH_QDMA_TIMING=1), the sync_to/from_device paths log the wall-clock
 * cost of each transfer ioctl plus the aggregate per-sync time and
 * effective bandwidth.  This is the userspace counterpart to the kernel's
 * SLASH_QDMA_TIMING breakdown.
 */
#ifndef SLASH_QDMA_TIMING
#define SLASH_QDMA_TIMING 0
#endif

#if SLASH_QDMA_TIMING
static inline uint64_t vrtd_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}
#endif

/*
 * Issue a buffer transfer of [offset, offset + size) as a single batched ioctl
 * per round, fanning the range across the fd's queue pairs (channels) according
 * to the placement policy so both NoC channels run concurrently in-kernel.
 *
 * The QDMA transfer descriptor's length is a 32-bit byte count, so each
 * segment is chunked to stay within that limit while preserving step alignment;
 * every chunk round issues one ioctl covering all active channels.
 */
static int vrtd_transfer_registered(
    int qpair_fd,
    uint32_t qpair_count,
    enum slash_qdma_transfer_hint transfer_hint,
    int buf_fd,
    uint64_t phys_addr,
    uint64_t offset,
    uint64_t size,
    uint64_t step,
    bool to_device
) {
    uint32_t direction = to_device ? SLASH_QDMA_XFER_H2C : SLASH_QDMA_XFER_C2H;

    if (size == 0) {
        return 0;
    }

    if (qpair_fd < 0 || qpair_count == 0) {
        return -EINVAL;
    }

    if (step == 0 || (offset % step) != 0 || (size % step) != 0) {
        return -EINVAL;
    }

    /*
     * Decide how the transfer maps onto the available queue pairs.  V80 applies
     * the placement-aware policy (DDR halved, HBM routed by the half-memory
     * boundary); any other hint keeps everything on the primary qpair.
     */
    struct vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint32_t nseg;

    if (transfer_hint == SLASH_QDMA_TRANSFER_HINT_V80) {
        nseg = vrtd_plan_v80(phys_addr, offset, size, step, qpair_count, segs);
    } else {
        segs[0].qpair_index = 0;
        segs[0].offset = offset;
        segs[0].size = size;
        nseg = 1;
    }

    /* Clamp any planned qpair_index to the qpairs the fd actually owns. */
    for (uint32_t i = 0; i < nseg; ++i) {
        if (segs[i].qpair_index >= qpair_count) {
            segs[i].qpair_index = 0;
        }
    }

    /* Per-channel descriptor length is 32-bit; keep chunks step-aligned. */
    uint64_t max_chunk = 0xFFFFF000ULL;
    max_chunk -= max_chunk % step;
    if (max_chunk == 0) {
        return -EINVAL;
    }

    uint64_t done[VRTD_V80_MAX_SEGS] = {0};
    for (;;) {
        struct slash_qdma_subxfer xfers[VRTD_V80_MAX_SEGS];
        uint32_t map_seg[VRTD_V80_MAX_SEGS];
        uint32_t count = 0;

        for (uint32_t i = 0; i < nseg; ++i) {
            uint64_t remaining = segs[i].size - done[i];
            uint64_t chunk;
            uint64_t xfer_offset;

            if (remaining == 0) {
                continue;
            }
            chunk = remaining > max_chunk ? max_chunk : remaining;
            xfer_offset = segs[i].offset + done[i];

            memset(&xfers[count], 0, sizeof(xfers[count]));
            xfers[count].qpair_index = segs[i].qpair_index;
            xfers[count].direction = direction;
            xfers[count].buf_fd = buf_fd;
            xfers[count].buf_offset = xfer_offset;
            xfers[count].dev_addr = phys_addr + xfer_offset;
            xfers[count].length = chunk;
            map_seg[count] = i;
            count++;
        }

        if (count == 0) {
            break;
        }

        ssize_t ret = slash_qdma_qpair_transfer_batch(qpair_fd, xfers, count);
        if (ret < 0) {
            return -EIO;
        }

        for (uint32_t c = 0; c < count; ++c) {
            done[map_seg[c]] += xfers[c].length;
        }
    }

    return 0;
}

/*
 * Transfer [0, size) of a separate kernel buffer (@bounce) against the device
 * starting at @phys_addr.  Used for partial-range read-modify-write staging.
 */
static int vrtd_bounce_transfer(
    const struct vrtd_buffer *buffer,
    const struct slash_qdma_buffer *bounce,
    uint64_t phys_addr,
    uint64_t size,
    bool to_device
) {
    if (buffer == NULL || bounce == NULL || buffer->qpair_count == 0 ||
        buffer->qpair_fd < 0) {
        return -EINVAL;
    }

    return vrtd_transfer_registered(buffer->qpair_fd, buffer->qpair_count,
                                    buffer->transfer_hint, bounce->fd,
                                    phys_addr, 0, size,
                                    BASE_TRANSFER_STEP_SIZE, to_device);
}

enum vrtd_ret vrtd_buffer_create_raw(
    int sock_fd,
    uint32_t dev,
    uint32_t alloc_type,
    uint32_t alloc_dir,
    uint64_t alloc_arg,
    uint64_t size,
    uint64_t phys_addr,
    int qpair_fd,
    uint32_t qpair_count,
    struct vrtd_buffer **buffer_out
) {
    if (buffer_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_buffer *buffer = (struct vrtd_buffer *) malloc(sizeof(struct vrtd_buffer));
    if (buffer == NULL) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    buffer->buf = NULL;
    buffer->transfer_step_size = BASE_TRANSFER_STEP_SIZE;
    buffer->qpair_fd = -1;
    buffer->qpair_count = 0;
    buffer->buffer_fd = -1;
    buffer->transfer_hint = SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR;

    if (qpair_fd < 0 || qpair_count == 0 || qpair_count > 2) {
        free(buffer);
        return VRTD_RET_BAD_LIB_CALL;
    }

    /*
     * The kernel owns the DMA buffer: it allocates 4 KiB base pages, builds the
     * SGL, and DMA-maps everything once at create time, then hands back a
     * mappable fd.  We mmap that fd for CPU access (buffer->buf).
     */
    struct slash_qdma_buffer sbuf;
    memset(&sbuf, 0, sizeof(sbuf));
    if (slash_qdma_qpair_buffer_create(qpair_fd, size, &sbuf) != 0) {
        free(buffer);
        return VRTD_RET_INTERNAL_ERROR;
    }

    buffer->buf           = sbuf.addr;
    buffer->buffer_fd     = sbuf.fd;
    buffer->transfer_hint = sbuf.transfer_hint;
    buffer->transfer_step_size = BASE_TRANSFER_STEP_SIZE;
#if SLASH_QDMA_TIMING
    syslog(
        LOG_INFO,
        "libvrtd: buffer kernel mapping size=%llu phys_addr=0x%llx step=%llu",
        (unsigned long long)size,
        (unsigned long long)phys_addr,
        (unsigned long long)buffer->transfer_step_size
    );
#endif

    buffer->sock_fd    = sock_fd;
    buffer->dev        = dev;
    buffer->alloc_type = alloc_type;
    buffer->alloc_dir  = alloc_dir;
    buffer->alloc_arg  = alloc_arg;
    buffer->size       = size;
    buffer->phys_addr  = phys_addr;
    buffer->qpair_fd    = qpair_fd;
    buffer->qpair_count = qpair_count;

    *buffer_out = buffer;

    return VRTD_RET_OK;
}

static enum vrtd_ret vrtd_buffer_prepare_sync_range(
    const struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size,
    uint64_t *aligned_offset_out,
    uint64_t *aligned_size_out,
    bool *needs_bounce_out
) {
    uint64_t step;
    uint64_t end;
    uint64_t aligned_offset;
    uint64_t aligned_end;

    if (buffer == NULL || aligned_offset_out == NULL ||
        aligned_size_out == NULL || needs_bounce_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    step = buffer->transfer_step_size;
    if (step == 0) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (offset > buffer->size || size > buffer->size - offset) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (size == 0) {
        *aligned_offset_out = offset;
        *aligned_size_out = 0;
        *needs_bounce_out = false;
        return VRTD_RET_OK;
    }

    if ((buffer->size % step) != 0 || (buffer->phys_addr % step) != 0) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    end = offset + size;
    aligned_offset = offset - (offset % step);
    if (end > UINT64_MAX - (step - 1)) {
        return VRTD_RET_INVALID_ARGUMENT;
    }
    aligned_end = ((end + step - 1) / step) * step;
    if (aligned_end > buffer->size) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    *aligned_offset_out = aligned_offset;
    *aligned_size_out = aligned_end - aligned_offset;
    *needs_bounce_out = (aligned_offset != offset || aligned_end != end);

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_destroy(
    struct vrtd_buffer *buffer
) {
    if (buffer == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    if (buffer->buf != NULL && buffer->size != 0) {
        (void) munmap(buffer->buf, buffer->size);
        buffer->buf = NULL;
    }

    if (buffer->buffer_fd >= 0) {
        (void) close(buffer->buffer_fd);
        buffer->buffer_fd = -1;
    }

    if (buffer->qpair_fd >= 0) {
        (void) close(buffer->qpair_fd);
        buffer->qpair_fd = -1;
    }

    free(buffer);

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_close(
    struct vrtd_buffer *buffer
)
{
    if (buffer == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_buffer_close req = {
        .dev_number = buffer->dev,
        .phys_addr = buffer->phys_addr,
        .size = buffer->size,
    };
    struct vrtd_resp_buffer_close resp = {0};

    enum vrtd_ret ret = vrtd_raw_request(
        buffer->sock_fd,
        VRTD_REQ_BUFFER_CLOSE,
        &req,
        sizeof(req),
        &resp,
        sizeof(resp),
        NULL,
        NULL
    );

    enum vrtd_ret destroy_ret = vrtd_buffer_destroy(buffer);
    if (ret != VRTD_RET_OK) {
        return ret;
    }
    return destroy_ret;
}

enum vrtd_ret vrtd_buffer_sync_to_device(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
) {
    if (buffer == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    if (buffer->alloc_dir == VRTD_ALLOC_DIR_DEVICE_TO_HOST) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    assert(buffer->qpair_count > 0);
    assert(buffer->qpair_fd >= 0);
    assert(buffer->buf != NULL);
    uint64_t aligned_offset = 0;
    uint64_t aligned_size = 0;
    bool needs_bounce = false;
    enum vrtd_ret range_ret = vrtd_buffer_prepare_sync_range(
        buffer, offset, size, &aligned_offset, &aligned_size, &needs_bounce);
    if (range_ret != VRTD_RET_OK) {
        return range_ret;
    }
    if (aligned_size == 0) {
        return VRTD_RET_OK;
    }

    uint64_t step = buffer->transfer_step_size;
#if SLASH_QDMA_TIMING
    uint64_t sync_start_ns = vrtd_now_ns();
#endif

    int transfer_ret;
    if (needs_bounce && buffer->alloc_dir == VRTD_ALLOC_DIR_BIDIRECTIONAL) {
        struct slash_qdma_buffer bounce;
        memset(&bounce, 0, sizeof(bounce));
        if (slash_qdma_qpair_buffer_create(buffer->qpair_fd, aligned_size,
                                           &bounce) != 0) {
            return VRTD_RET_INTERNAL_ERROR;
        }

        transfer_ret = vrtd_bounce_transfer(
            buffer, &bounce, buffer->phys_addr + aligned_offset,
            aligned_size, false);
        if (transfer_ret == 0) {
            memcpy(
                (uint8_t *)bounce.addr + (offset - aligned_offset),
                (uint8_t *)buffer->buf + offset,
                size
            );
            transfer_ret = vrtd_bounce_transfer(
                buffer, &bounce, buffer->phys_addr + aligned_offset,
                aligned_size, true);
        }
        (void) slash_qdma_buffer_destroy(&bounce);
    } else {
        /*
         * Host-to-device-only buffers cannot read the surrounding device
         * granule for a read-modify-write, so keep the historical behavior:
         * expand partial syncs to the backing DMA granule.
         */
        transfer_ret = vrtd_transfer_registered(
            buffer->qpair_fd, buffer->qpair_count, buffer->transfer_hint,
            buffer->buffer_fd, buffer->phys_addr,
            aligned_offset, aligned_size, step, true);
    }
    if (transfer_ret != 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

#if SLASH_QDMA_TIMING
    {
        uint64_t total_ns = vrtd_now_ns() - sync_start_ns;
        double mb = (double) size / (1024.0 * 1024.0);
        double sec = (double) total_ns / 1e9;
        syslog(LOG_INFO,
               "libvrtd: timing H2C sync offset=%llu size=%llu aligned_offset=%llu aligned_size=%llu step=%llu total=%llu ns (%.1f MB/s)",
               (unsigned long long) offset, (unsigned long long) size,
               (unsigned long long) aligned_offset, (unsigned long long) aligned_size,
               (unsigned long long) step, (unsigned long long) total_ns,
               sec > 0.0 ? mb / sec : 0.0);
    }
#endif

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_sync_from_device(
    struct vrtd_buffer *buffer,
    uint64_t offset,
    uint64_t size
) {
    if (buffer == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    if (buffer->alloc_dir == VRTD_ALLOC_DIR_HOST_TO_DEVICE) {
        return VRTD_RET_INVALID_ARGUMENT;
    }

    assert(buffer->qpair_count > 0);
    assert(buffer->qpair_fd >= 0);
    assert(buffer->buf != NULL);
    uint64_t aligned_offset = 0;
    uint64_t aligned_size = 0;
    bool needs_bounce = false;
    enum vrtd_ret range_ret = vrtd_buffer_prepare_sync_range(
        buffer, offset, size, &aligned_offset, &aligned_size, &needs_bounce);
    if (range_ret != VRTD_RET_OK) {
        return range_ret;
    }
    if (aligned_size == 0) {
        return VRTD_RET_OK;
    }

    uint64_t step = buffer->transfer_step_size;
#if SLASH_QDMA_TIMING
    uint64_t sync_start_ns = vrtd_now_ns();
#endif

    int transfer_ret;
    if (needs_bounce) {
        struct slash_qdma_buffer bounce;
        memset(&bounce, 0, sizeof(bounce));
        if (slash_qdma_qpair_buffer_create(buffer->qpair_fd, aligned_size,
                                           &bounce) != 0) {
            return VRTD_RET_INTERNAL_ERROR;
        }

        transfer_ret = vrtd_bounce_transfer(
            buffer, &bounce, buffer->phys_addr + aligned_offset,
            aligned_size, false);
        if (transfer_ret == 0) {
            memcpy(
                (uint8_t *)buffer->buf + offset,
                (uint8_t *)bounce.addr + (offset - aligned_offset),
                size
            );
        }
        (void) slash_qdma_buffer_destroy(&bounce);
    } else {
        transfer_ret = vrtd_transfer_registered(
            buffer->qpair_fd, buffer->qpair_count, buffer->transfer_hint,
            buffer->buffer_fd, buffer->phys_addr,
            aligned_offset, aligned_size, step, false);
    }
    if (transfer_ret != 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

#if SLASH_QDMA_TIMING
    {
        uint64_t total_ns = vrtd_now_ns() - sync_start_ns;
        double mb = (double) size / (1024.0 * 1024.0);
        double sec = (double) total_ns / 1e9;
        syslog(LOG_INFO,
               "libvrtd: timing C2H sync offset=%llu size=%llu aligned_offset=%llu aligned_size=%llu step=%llu total=%llu ns (%.1f MB/s)",
               (unsigned long long) offset, (unsigned long long) size,
               (unsigned long long) aligned_offset, (unsigned long long) aligned_size,
               (unsigned long long) step, (unsigned long long) total_ns,
               sec > 0.0 ? mb / sec : 0.0);
    }
#endif

    return VRTD_RET_OK;
}
