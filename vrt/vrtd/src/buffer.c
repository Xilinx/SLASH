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
 * @brief Buffer lifecycle management: allocate, initialise, and free QDMA-backed
 *        device memory buffers.
 *
 * A "buffer" in vrtd ties together three resources:
 *
 *  1. A device memory allocation (DDR or HBM subregions tracked by the
 *     allocator in allocator.c).
 *  2. A QDMA queue pair (qpair) that provides the DMA channel for host<->device
 *     data movement to/from that allocation.
 *  3. A file descriptor obtained from the QDMA driver that the client can
 *     mmap() or read()/write() against.
 *
 * buffer_create() orchestrates acquiring all three; cleanup_buffer() tears
 * them down in reverse order, tolerating partial initialisation so that the
 * error path from buffer_init() can safely call it.
 */

#include "buffer.h"
#include "utils.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#include <systemd/sd-journal.h>

/* QDMA queue configuration constants. */
#define VRTD_QDMA_Q_MODE_MM 0u          /* Memory-mapped (MM) mode */
#define VRTD_QDMA_DIR_H2C (1u << 0)     /* Host-to-Card direction */
#define VRTD_QDMA_DIR_C2H (1u << 1)     /* Card-to-Host direction */
#define VRTD_QDMA_RING_SZ_IDX 0u        /* Default ring size index */

/**
 * Initialise a buffer: allocate device memory, create a QDMA queue pair,
 * start the queue, and obtain a file descriptor for host-side access.
 *
 * Initialisation proceeds in strict order:
 *  1. Allocate device memory via the allocator (sets buf->addr, buf->size).
 *  2. Translate the requested transfer direction into QDMA direction flags.
 *  3. Add a QDMA queue pair configured for memory-mapped (MM) mode with the
 *     appropriate direction mask (H2C, C2H, or both).
 *  4. Start the queue pair so DMA transfers can be issued.
 *  5. Obtain a file descriptor (O_CLOEXEC) from the QDMA driver for the
 *     queue; this fd is later passed to the client for data transfer.
 *
 * On any failure, cleanup_buffer() is called which safely tears down
 * whichever resources were successfully acquired (using the allocation_valid
 * and qpair_created flags to track partial progress).
 *
 * @return 0 on success, -1 on failure (errno set, buffer cleaned up).
 */
static int buffer_init(struct buffer *buf,
                       struct slash_qdma *qdma,
                       struct device_memory_map *map,
                       enum allocation_type alloc_type,
                       enum vrtd_alloc_dir alloc_dir,
                       uint64_t size,
                       uint64_t alloc_arg,
                       uint64_t client_id,
                       const struct slash_qdma_qpair_add *qpair_params)
{
    if (buf == NULL) {
        errno = EINVAL;
        LOG(LOG_ERR, "Failed to initialize buffer: invalid output pointer");
        return -1;
    }

    /* Zero-initialise all fields so cleanup_buffer() can safely inspect
     * the flags (allocation_valid, qpair_created) on early failure. */
    *buf = (struct buffer) {
        .qdma = qdma,
        .map = map,
        .alloc_type = alloc_type,
        .alloc_arg = alloc_arg,
        .alloc_dir = alloc_dir,
        .client_id = client_id,
        .addr = 0,
        .size = 0,
        .qid = 0,
        .fd = -1,
        .allocation_valid = false,
        .qpair_created = false,
    };

    if (qdma == NULL || map == NULL || size == 0 || client_id == 0) {
        errno = EINVAL;
        LOG(
            LOG_ERR,
            "Failed to initialize buffer: invalid arguments (qdma=%p map=%p size=%llu client_id=%llu)",
            (void *)qdma,
            (void *)map,
            (unsigned long long)size,
            (unsigned long long)client_id
        );
        goto fail;
    }

    /* Map the vrtd allocation direction enum to QDMA hardware direction flags. */
    uint32_t dir_mask = 0;
    switch (alloc_dir) {
    case VRTD_ALLOC_DIR_BIDIRECTIONAL:
        dir_mask = VRTD_QDMA_DIR_H2C | VRTD_QDMA_DIR_C2H;
        break;
    case VRTD_ALLOC_DIR_HOST_TO_DEVICE:
        dir_mask = VRTD_QDMA_DIR_H2C;
        break;
    case VRTD_ALLOC_DIR_DEVICE_TO_HOST:
        dir_mask = VRTD_QDMA_DIR_C2H;
        break;
    default:
        errno = EINVAL;
        LOG(
            LOG_ERR,
            "Failed to initialize buffer: invalid allocation direction %u",
            (unsigned int)alloc_dir
        );
        goto fail;
    }

    /* Step 1: Reserve device memory subregions from the allocator.
     * alloc_size may be rounded up to the next 64 MiB boundary. */
    uint64_t alloc_size = size;
    uint64_t alloc_addr = 0;
    enum allocation_result ares = device_memory_map_allocate(
        map,
        alloc_type,
        &alloc_size,
        alloc_arg,
        client_id,
        &alloc_addr
    );
    if (ares != ALLOCATION_RESULT_SUCCESS) {
        errno = (ares == ALLOCATION_RESULT_NO_MEMORY) ? ENOMEM : EINVAL;
        LOG(
            LOG_ERR,
            "Failed to allocate device memory for buffer (result=%d alloc_type=%u size=%llu alloc_arg=%llu client_id=%llu): %m",
            (int)ares,
            (unsigned int)alloc_type,
            (unsigned long long)size,
            (unsigned long long)alloc_arg,
            (unsigned long long)client_id
        );
        goto fail;
    }

    buf->addr = alloc_addr;
    buf->size = alloc_size;
    buf->allocation_valid = true;

    /* Step 2: Configure and create a QDMA queue pair.  If the caller
     * supplied custom qpair parameters (e.g. streaming mode), use those;
     * otherwise default to memory-mapped mode with the smallest ring size. */
    struct slash_qdma_qpair_add qpair = {0};
    if (qpair_params != NULL) {
        qpair = *qpair_params;
    } else {
        qpair.mode = VRTD_QDMA_Q_MODE_MM;
        qpair.h2c_ring_sz = VRTD_QDMA_RING_SZ_IDX;
        qpair.c2h_ring_sz = VRTD_QDMA_RING_SZ_IDX;
        qpair.cmpt_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    }
    qpair.dir_mask = dir_mask;
    qpair.size = sizeof(qpair);

    if (slash_qdma_qpair_add(qdma, &qpair) != 0) {
        LOG(LOG_ERR, "Failed to add buffer qpair: %m");
        goto fail;
    }

    buf->qid = qpair.qid;
    buf->qpair_created = true;

    /* Step 3: Start the queue pair so DMA transfers can be issued. */
    if (slash_qdma_qpair_start(qdma, buf->qid) != 0) {
        LOG(LOG_ERR, "Failed to start buffer qpair %u: %m", buf->qid);
        goto fail;
    }

    /* Step 4: Obtain a file descriptor for the queue.  The client will use
     * this fd (passed over the Unix socket via SCM_RIGHTS) to perform
     * read/write/mmap against the QDMA queue. */
    int fd = slash_qdma_qpair_get_fd(qdma, buf->qid, O_CLOEXEC);
    if (fd < 0) {
        LOG(LOG_ERR, "Failed to get fd for buffer qpair %u: %m", buf->qid);
        goto fail;
    }
    buf->fd = fd;

    LOG(LOG_DEBUG, "Buffer initialized addr=0x%llx size=%llu qid=%u", (unsigned long long)buf->addr, (unsigned long long)buf->size, buf->qid);
    return 0;

fail:
    cleanup_buffer(buf);
    return -1;
}

/**
 * Allocate and fully initialise a new buffer.
 *
 * This is the primary public entry point.  It heap-allocates a struct buffer,
 * then delegates to buffer_init() which acquires the device memory, QDMA
 * qpair, and file descriptor.  On failure the buffer is freed and NULL is
 * returned.
 *
 * @return Heap-allocated, fully initialised buffer, or NULL on failure.
 */
struct buffer *buffer_create(struct slash_qdma *qdma,
                             struct device_memory_map *map,
                             enum allocation_type alloc_type,
                             enum vrtd_alloc_dir alloc_dir,
                             uint64_t size,
                             uint64_t alloc_arg,
                             uint64_t client_id,
                             const struct slash_qdma_qpair_add *qpair_params)
{
    struct buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        LOG(LOG_ERR, "Failed to allocate buffer: %m");
        return NULL;
    }

    if (buffer_init(buf, qdma, map, alloc_type, alloc_dir, size, alloc_arg, client_id, qpair_params) != 0) {
        LOG(LOG_ERR, "Failed to initialize buffer: %m");
        return NULL;
    }

    return buf;
}

/**
 * Allocate a buffer at a caller-specified device address, bypassing the allocator.
 *
 * Skips device_memory_map_allocate() and goes directly to QDMA queue pair creation
 * with the provided address and size.  Sets allocation_valid=false so cleanup_buffer()
 * will not attempt to free anything from the memory map.
 *
 * @return Heap-allocated buffer on success, NULL on failure (errno set).
 */
struct buffer *buffer_create_raw(struct slash_qdma *qdma,
                                 uint64_t phys_addr,
                                 uint64_t size,
                                 enum vrtd_alloc_dir alloc_dir)
{
    if (qdma == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }

    uint32_t dir_mask = 0;
    switch (alloc_dir) {
    case VRTD_ALLOC_DIR_BIDIRECTIONAL:
        dir_mask = VRTD_QDMA_DIR_H2C | VRTD_QDMA_DIR_C2H;
        break;
    case VRTD_ALLOC_DIR_HOST_TO_DEVICE:
        dir_mask = VRTD_QDMA_DIR_H2C;
        break;
    case VRTD_ALLOC_DIR_DEVICE_TO_HOST:
        dir_mask = VRTD_QDMA_DIR_C2H;
        break;
    default:
        errno = EINVAL;
        LOG(LOG_ERR, "buffer_create_raw: invalid allocation direction %u", (unsigned int)alloc_dir);
        return NULL;
    }

    struct buffer *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        LOG(LOG_ERR, "buffer_create_raw: failed to allocate buffer struct: %m");
        return NULL;
    }

    *buf = (struct buffer) {
        .qdma = qdma,
        .map = NULL,
        .alloc_type = 0,
        .alloc_arg = 0,
        .alloc_dir = alloc_dir,
        .client_id = 0,
        .addr = phys_addr,
        .size = size,
        .qid = 0,
        .fd = -1,
        .allocation_valid = false, /* no allocator reservation to free */
        .qpair_created = false,
    };

    struct slash_qdma_qpair_add qpair = {0};
    qpair.mode = VRTD_QDMA_Q_MODE_MM;
    qpair.h2c_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    qpair.c2h_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    qpair.cmpt_ring_sz = VRTD_QDMA_RING_SZ_IDX;
    qpair.dir_mask = dir_mask;
    qpair.size = sizeof(qpair);

    if (slash_qdma_qpair_add(qdma, &qpair) != 0) {
        LOG(LOG_ERR, "buffer_create_raw: failed to add qpair: %m");
        free(buf);
        return NULL;
    }

    buf->qid = qpair.qid;
    buf->qpair_created = true;

    if (slash_qdma_qpair_start(qdma, buf->qid) != 0) {
        LOG(LOG_ERR, "buffer_create_raw: failed to start qpair %u: %m", buf->qid);
        cleanup_buffer(buf);
        return NULL;
    }

    int fd = slash_qdma_qpair_get_fd(qdma, buf->qid, O_CLOEXEC);
    if (fd < 0) {
        LOG(LOG_ERR, "buffer_create_raw: failed to get fd for qpair %u: %m", buf->qid);
        cleanup_buffer(buf);
        return NULL;
    }
    buf->fd = fd;

    LOG(LOG_DEBUG, "Raw buffer created phys_addr=0x%llx size=%llu qid=%u",
        (unsigned long long)phys_addr, (unsigned long long)size, buf->qid);
    return buf;
}

/**
 * Tear down a buffer and release all associated resources.
 *
 * Resources are released in reverse acquisition order:
 *  1. Close the file descriptor (if open).
 *  2. Stop and delete the QDMA queue pair (if created).
 *  3. Free the device memory allocation (if valid).
 *  4. Zero all fields and free the struct.
 *
 * Each step is guarded by its corresponding flag (fd >= 0,
 * qpair_created, allocation_valid) so this function is safe to call
 * after partial initialisation.  NULL-safe.
 */
void cleanup_buffer(struct buffer *buf)
{
    if (buf == NULL) {
        return;
    }

    LOG(LOG_DEBUG, "Freeing buffer addr=0x%llx size=%llu qid=%u", (unsigned long long)buf->addr, (unsigned long long)buf->size, buf->qid);

    /* Close the QDMA queue fd first, before stopping the queue. */
    if (buf->fd >= 0) {
        (void) close(buf->fd);
        buf->fd = -1;
    }

    /* Stop and delete the QDMA queue pair.  Errors are logged but
     * otherwise ignored -- we are on the teardown path and must continue
     * releasing remaining resources. */
    if (buf->qpair_created && buf->qdma != NULL) {
        if (slash_qdma_qpair_stop(buf->qdma, buf->qid) != 0) {
            LOG(
                LOG_WARNING,
                "Error stopping buffer qpair %u: %m (ignored)",
                buf->qid
            );
        }
        if (slash_qdma_qpair_del(buf->qdma, buf->qid) != 0) {
            LOG(
                LOG_WARNING,
                "Error deleting buffer qpair %u: %m (ignored)",
                buf->qid
            );
        }
    }

    /* Return the device memory subregions to the allocator. */
    if (buf->allocation_valid && buf->map != NULL) {
        if (device_memory_map_free(
                buf->map,
                buf->alloc_type,
                buf->addr,
                buf->size,
                buf->client_id
            ) != ALLOCATION_RESULT_SUCCESS) {
            LOG(
                LOG_WARNING,
                "Error freeing buffer allocation (addr=0x%llx size=%llu): %m (ignored)",
                (unsigned long long)buf->addr,
                (unsigned long long)buf->size
            );
        }
    }

    /* Zero all fields so a stale pointer dereference is more likely to
     * crash cleanly rather than silently corrupt state. */
    buf->qdma = NULL;
    buf->map = NULL;
    buf->qpair_created = false;
    buf->allocation_valid = false;
    buf->addr = 0;
    buf->size = 0;
    buf->qid = 0;
    buf->fd = -1;

    free(buf);
}
