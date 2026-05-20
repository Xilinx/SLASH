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
 * @file buffer.h
 * @brief DMA buffer management for SLASH FPGA devices.
 *
 * A @c struct @c buffer represents a single DMA-accessible memory region
 * allocated from a device's HBM or DDR address space.  The lifecycle is:
 *
 *   1. @b Allocation -- @c buffer_create reserves address space from the
 *      device memory map, creates a QDMA queue pair for data transfer,
 *      and records ownership (the client connection ID that requested it).
 *   2. @b Use -- The client reads/writes through the QDMA queue pair fd.
 *   3. @b Deallocation -- @c cleanup_buffer tears down the QDMA queue pair,
 *      releases the address-space reservation, and closes the fd.
 *
 * When a client disconnects, all buffers owned by that connection ID are
 * automatically freed.
 */

#ifndef VRTD_BUFFER_H
#define VRTD_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include <slash/qdma.h>

#include "allocator.h"
#include "array.h"
#include "vrtd/wire.h"

/**
 * @brief A single DMA buffer allocated on a SLASH FPGA device.
 *
 * Tracks both the memory allocation metadata and the QDMA queue pair
 * used to transfer data to/from the buffer.
 */
struct buffer {
    /** @brief QDMA subsystem handle (non-owning, borrowed from the parent device). */
    struct slash_qdma *qdma; /* non-owning */
    /** @brief Device memory map used for address allocation (non-owning). */
    struct device_memory_map *map; /* non-owning */
    /** @brief Memory type of this allocation (DDR, HBM, or HBM_VNOC). */
    enum allocation_type alloc_type;
    /** @brief Type-specific allocation argument (e.g. HBM region index for non-VNOC HBM). */
    uint64_t alloc_arg;
    /** @brief DMA transfer direction (host-to-card, card-to-host, or bidirectional). */
    enum vrtd_alloc_dir alloc_dir;
    /** @brief Connection ID of the client that owns this buffer.
     *  Used for automatic cleanup on client disconnect. */
    uint64_t client_id; /* owning connection id */
    /** @brief Base device address of the allocated memory region. */
    uint64_t addr;
    /** @brief Size of the allocated memory region in bytes (rounded up to subregion granularity). */
    uint64_t size;
    /** @brief QDMA queue ID assigned to this buffer's queue pair. */
    uint32_t qid;
    /** @brief File descriptor for the QDMA queue pair character device.
     *  Passed to the client via SCM_RIGHTS for direct data transfer. */
    int fd;
    /** @brief True if the address-space allocation in the memory map is valid and must be freed. */
    bool allocation_valid;
    /** @brief True if the QDMA queue pair has been created and must be torn down on cleanup. */
    bool qpair_created;
};

/**
 * @brief Allocate a new DMA buffer on a device.
 *
 * Reserves address space from the device memory map, creates a QDMA queue
 * pair, and starts the queue pair so it is immediately usable.
 *
 * @param qdma          QDMA subsystem handle (borrowed).
 * @param map           Device memory map for address allocation (borrowed).
 * @param alloc_type    Memory type to allocate (DDR, HBM, or HBM_VNOC).
 * @param alloc_dir     DMA transfer direction.
 * @param size          Requested buffer size in bytes (may be rounded up).
 * @param alloc_arg     Type-specific argument (HBM region index for non-VNOC HBM).
 * @param client_id     Connection ID of the owning client.
 * @param qpair_params  QDMA queue pair configuration parameters.
 * @return Heap-allocated buffer on success, NULL on failure.
 */
struct buffer *buffer_create(struct slash_qdma *qdma,
                             struct device_memory_map *map,
                             enum allocation_type alloc_type,
                             enum vrtd_alloc_dir alloc_dir,
                             uint64_t size,
                             uint64_t alloc_arg,
                             uint64_t client_id,
                             const struct slash_qdma_qpair_add *qpair_params);

/**
 * @brief Allocate a new DMA buffer at a caller-specified device address (bypasses allocator).
 *
 * Creates a QDMA queue pair at the given physical address without consulting the
 * allocator.  The caller is responsible for ensuring the address is valid and not
 * in use.  @c allocation_valid is set to false so cleanup_buffer() will not
 * attempt to release anything from the memory map.
 *
 * @param qdma       QDMA subsystem handle (borrowed).
 * @param phys_addr  Caller-specified device physical address.
 * @param size       Size in bytes.
 * @param alloc_dir  DMA transfer direction.
 * @return Heap-allocated buffer on success, NULL on failure (errno set).
 */
struct buffer *buffer_create_raw(struct slash_qdma *qdma,
                                 uint64_t phys_addr,
                                 uint64_t size,
                                 enum vrtd_alloc_dir alloc_dir);

/**
 * @brief Release all resources owned by a buffer.
 *
 * Stops and deletes the QDMA queue pair, frees the address-space reservation,
 * and closes the queue pair fd.
 *
 * @param buf Pointer to the buffer to clean up. May be NULL (no-op).
 */
void cleanup_buffer(struct buffer *buf);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param bufp Address of a @c struct @c buffer pointer.
 */
static inline
void cleanup_bufferp(struct buffer **bufp)
{
    cleanup_buffer(*bufp);
    *bufp = NULL;
}

DECLARE_OWNING_PTR_ARRAY(buffer_ptr_array, struct buffer *, cleanup_buffer);

#endif // VRTD_BUFFER_H
