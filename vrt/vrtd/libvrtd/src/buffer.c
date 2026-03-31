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
 * the FPGA.  Each buffer is backed by an anonymous mmap (preferring
 * 2 MB hugepages for TLB efficiency, with automatic fallback to
 * regular pages) and associated with a QDMA queue pair fd for
 * performing the actual H2C / C2H transfers.
 *
 * Sync operations (sync_to_device / sync_from_device) transfer data
 * between the host buffer and FPGA memory in TRANSFER_STEP_SIZE (4 KB)
 * chunks using positional I/O on the QDMA qpair fd.
 *
 * Buffer lifecycle:
 *   1. vrtd_buffer_open()          -- daemon allocates, returns qpair fd
 *   2. vrtd_buffer_create_raw()    -- client mmaps host memory
 *   3. vrtd_buffer_sync_to/from_device() -- DMA transfers
 *   4. vrtd_buffer_close()         -- tells daemon to free, unmaps locally
 */

#define _GNU_SOURCE

#include <vrtd/vrtd.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>


#include <stdio.h>

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21UL << MAP_HUGE_SHIFT)
#endif

#define TRANSFER_STEP_SIZE (4ULL * 1024ULL) // 4K

enum vrtd_ret vrtd_buffer_create_raw(
    int sock_fd,
    uint32_t dev,
    uint32_t alloc_type,
    uint32_t alloc_dir,
    uint64_t alloc_arg,
    uint64_t size,
    uint64_t phys_addr,
    int qpair_fd,
    struct vrtd_buffer **buffer_out
) {
    if (buffer_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_buffer *buffer = (struct vrtd_buffer *) malloc(sizeof(struct vrtd_buffer));
    if (buffer == NULL) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    buffer->buf = mmap(
        NULL, /* address (let the kernel choose) */
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB | MAP_POPULATE,
        -1, /* fd */
        0   /* offset */
    );
    if (buffer->buf == MAP_FAILED) {
        // Huge pages are an optimization, not a hard requirement.
        // Fall back to normal anonymous mapping when hugepage mmap fails.
        buffer->buf = mmap(
            NULL, /* address (let the kernel choose) */
            size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
            -1, /* fd */
            0   /* offset */
        );
        if (buffer->buf == MAP_FAILED) {
            free(buffer);
            return VRTD_RET_INTERNAL_ERROR;
        }
    }

    buffer->sock_fd    = sock_fd;
    buffer->dev        = dev;
    buffer->alloc_type = alloc_type;
    buffer->alloc_dir  = alloc_dir;
    buffer->alloc_arg  = alloc_arg;
    buffer->size       = size;
    buffer->phys_addr  = phys_addr;
    buffer->qpair_fd   = qpair_fd;

    *buffer_out = buffer;

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_destroy(
    struct vrtd_buffer *buffer
) {
    if (buffer == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    if (buffer->qpair_fd >= 0) {
        (void) close(buffer->qpair_fd);
    }

    if (buffer->buf != NULL) {
        (void) munmap(buffer->buf, buffer->size);
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

    assert(buffer->qpair_fd >= 0);
    assert(buffer->buf != NULL);
    assert(buffer->size % TRANSFER_STEP_SIZE == 0);
    assert(buffer->phys_addr % TRANSFER_STEP_SIZE == 0);

    uint64_t effective_offset = offset - (offset % TRANSFER_STEP_SIZE);
    uint64_t end_offset = offset + size;

    off_t ret = lseek(buffer->qpair_fd, buffer->phys_addr + effective_offset, SEEK_SET);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    for (uint64_t curr_offset = effective_offset; curr_offset < end_offset; curr_offset += TRANSFER_STEP_SIZE) {
        ssize_t bytes_written = 0;
        while (bytes_written < TRANSFER_STEP_SIZE) {
            ssize_t bw = write(buffer->qpair_fd,
                               (uint8_t *) buffer->buf + curr_offset + bytes_written,
                               TRANSFER_STEP_SIZE - bytes_written);
            if (bw == -1) {
                return VRTD_RET_INTERNAL_ERROR;
            }
            bytes_written += bw;
        }
    }

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

    assert(buffer->qpair_fd >= 0);
    assert(buffer->buf != NULL);
    assert(buffer->size % TRANSFER_STEP_SIZE == 0);
    assert(buffer->phys_addr % TRANSFER_STEP_SIZE == 0);

    uint64_t effective_offset = offset - (offset % TRANSFER_STEP_SIZE);
    uint64_t end_offset = offset + size;

    off_t ret = lseek(buffer->qpair_fd, buffer->phys_addr + effective_offset, SEEK_SET);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    for (uint64_t curr_offset = effective_offset; curr_offset < end_offset; curr_offset += TRANSFER_STEP_SIZE) {
        ssize_t bytes_read = 0;
        while (bytes_read < TRANSFER_STEP_SIZE) {
            ssize_t br = read(buffer->qpair_fd,
                              (uint8_t *) buffer->buf + curr_offset + bytes_read,
                              TRANSFER_STEP_SIZE - bytes_read);
            if (br == -1) {
                return VRTD_RET_INTERNAL_ERROR;
            }
            bytes_read += br;
        }
    }

    return VRTD_RET_OK;
}
