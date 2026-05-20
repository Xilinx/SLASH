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
 * @file allocator.h
 * @brief Device memory map allocator for HBM and DDR address ranges.
 *
 * The SLASH V80 FPGA exposes two classes of on-board memory to userspace:
 *   - @b HBM (High Bandwidth Memory) -- 64 regions starting at 0x4000000000.
 *   - @b DDR -- 64 regions starting at 0x60000000000.
 *
 * Each region is 512 MB and is subdivided into 8 subregions of 64 MB each.
 * Subregion tracking is per-client: each subregion records the connection ID
 * of the client that allocated it (0 = free).  This allows automatic cleanup
 * when a client disconnects.
 *
 * Allocation granularity is one subregion (64 MB).  Requested sizes are
 * rounded up to the next subregion boundary.  For non-VNOC HBM allocations,
 * the caller specifies a region index; for DDR and HBM-VNOC, the allocator
 * finds the first available contiguous run of subregions.
 *
 * The allocator is purely a bookkeeping structure -- it does not perform any
 * hardware I/O.  Actual DMA setup is handled by the buffer layer.
 */

#ifndef VRTD_ALLOCATOR_H
#define VRTD_ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>

/** @brief Number of HBM regions available on the device. */
// Regions are 512MB
// Subregions are 64MB
#define HBM_REGIONS 64
/** @brief Number of DDR regions available on the device. */
#define DDR_REGIONS 64
/** @brief Base device address of the HBM address space. */
#define HBM_START_ADDRESS 0x4000000000ULL
/** @brief Base device address of the DDR address space. */
#define DDR_START_ADDRESS 0x60000000000ULL
/** @brief Size of one region in bytes (512 MB). */
#define REGION_SIZE (512UL * 1024 * 1024)
/** @brief Size of one subregion in bytes (64 MB). */
#define SUBREGION_SIZE (64UL * 1024 * 1024)
/** @brief Number of subregions within each region (REGION_SIZE / SUBREGION_SIZE = 8). */
#define SUBREGIONS_PER_REGION (REGION_SIZE / SUBREGION_SIZE)

/**
 * @brief Per-region subregion ownership tracking for DDR memory.
 *
 * Each element of @c client_id tracks the owner of one 64 MB subregion.
 * A value of 0 indicates the subregion is free.
 */
struct ddr_region_data {
    /** @brief Owner connection ID for each subregion (0 = free). */
    uint64_t client_id[SUBREGIONS_PER_REGION]; // 0 if free, non-zero owner connection id if allocated
};

/**
 * @brief Per-region subregion ownership tracking for HBM memory.
 *
 * Identical layout to @c ddr_region_data but used for the HBM address space.
 */
struct hbm_region_data {
    /** @brief Owner connection ID for each subregion (0 = free). */
    uint64_t client_id[SUBREGIONS_PER_REGION]; // 0 if free, non-zero owner connection id if allocated
};

/**
 * @brief Complete memory map for one SLASH FPGA device.
 *
 * Contains subregion ownership arrays for all DDR and HBM regions.
 * One instance exists per device and is shared (non-owning) with all
 * buffers allocated on that device.
 */
struct device_memory_map {
    /** @brief Subregion ownership data for all DDR regions. */
    struct ddr_region_data ddr_regions[DDR_REGIONS];
    /** @brief Subregion ownership data for all HBM regions. */
    struct hbm_region_data hbm_regions[HBM_REGIONS];
};

/**
 * @brief Memory type selector for allocation requests.
 */
enum allocation_type {
    /** @brief Allocate from DDR memory. */
    ALLOCATION_TYPE_DDR,
    /** @brief Allocate from HBM memory (caller specifies region index). */
    ALLOCATION_TYPE_HBM,
    /** @brief Allocate from HBM memory via VNOC (auto region selection). */
    ALLOCATION_TYPE_HBM_VNOC,
};

/**
 * @brief Allocate and zero-initialize a device memory map.
 * @return Heap-allocated memory map on success, NULL on allocation failure.
 */
struct device_memory_map *device_memory_map_create(void);

/**
 * @brief Release a device memory map.
 * @param map Pointer to the memory map to free. May be NULL (no-op).
 */
void device_memory_map_cleanup(struct device_memory_map *map);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param mapp Address of a @c struct @c device_memory_map pointer.
 */
static inline
void device_memory_map_cleanupp(struct device_memory_map **mapp)
{
    device_memory_map_cleanup(*mapp);
    *mapp = NULL;
}

/**
 * @brief Result codes for allocation and free operations.
 */
enum allocation_result {
    /** @brief Operation succeeded. */
    ALLOCATION_RESULT_SUCCESS = 0,
    /** @brief Not enough contiguous subregions available. */
    ALLOCATION_RESULT_NO_MEMORY = 1,
    /** @brief Invalid argument (e.g. region index out of range, zero size). */
    ALLOCATION_RESULT_BAD_ARGUMENT = 2,
};

/**
 * @brief Allocate a contiguous range of subregions from the device memory map.
 *
 * For ALLOCATION_TYPE_HBM (non-VNOC), @p arg specifies the HBM region index (0-63).
 * For ALLOCATION_TYPE_DDR and ALLOCATION_TYPE_HBM_VNOC, @p arg is ignored and the
 * allocator searches for the first fit.
 *
 * @param map           The device memory map to allocate from.
 * @param type          Memory type (DDR, HBM, or HBM_VNOC).
 * @param[in,out] size  On input, the requested size in bytes.
 *                      On output, the actual allocated size (rounded up to the nearest
 *                      subregion boundary).
 * @param arg           Type-specific argument (HBM region index for non-VNOC HBM).
 * @param client_id     Connection ID of the allocating client (recorded for ownership).
 * @param[out] addr_out Receives the base device address of the allocation.
 * @return ALLOCATION_RESULT_SUCCESS, ALLOCATION_RESULT_NO_MEMORY, or
 *         ALLOCATION_RESULT_BAD_ARGUMENT.
 */
// For ALLOCATION_TYPE_HBM (non-VNOC), arg specifies the HBM region index (0-63).
// Otherwise, arg is ignored.
// size is input as the requested size, and output as the allocated size (rounded up to
// the nearest subregion).
enum allocation_result device_memory_map_allocate(struct device_memory_map *map,
                                                  enum allocation_type type,
                                                  uint64_t *size,
                                                  uint64_t arg,
                                                  uint64_t client_id,
                                                  uint64_t *addr_out);

/**
 * @brief Free a previously allocated range of subregions.
 *
 * Marks the subregions covered by [@p addr, @p addr + @p size) as free,
 * but only if they are currently owned by @p client_id.
 *
 * @param map       The device memory map.
 * @param type      Memory type of the original allocation.
 * @param addr      Base device address of the allocation to free.
 * @param size      Size of the allocation in bytes.
 * @param client_id Connection ID of the client that owns the allocation.
 * @return ALLOCATION_RESULT_SUCCESS or ALLOCATION_RESULT_BAD_ARGUMENT.
 */
enum allocation_result device_memory_map_free(struct device_memory_map *map,
                                              enum allocation_type type,
                                              uint64_t addr,
                                              uint64_t size,
                                              uint64_t client_id);

#endif // VRTD_ALLOCATOR_H
