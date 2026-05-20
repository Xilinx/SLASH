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
 * @file allocator.c
 * @brief QDMA device memory allocation tracker for DDR and HBM regions.
 *
 * The V80 FPGA exposes two classes of device-side memory -- DDR and HBM --
 * each divided into fixed-size regions (512 MiB).  Every region is further
 * split into subregions (64 MiB) which are the minimum allocation granularity.
 *
 * Allocation tracking uses a simple bitmap-like scheme: each subregion stores
 * the owning client's connection ID (0 == free).  To allocate N bytes the
 * allocator rounds up to the next subregion boundary, then scans for a
 * contiguous run of free subregions within a single region using a sliding
 * window.  This first-fit-per-region strategy keeps the logic O(regions *
 * subregions) and avoids external fragmentation across region boundaries.
 *
 * Freeing validates that every subregion in the range is owned by the
 * requesting client before clearing ownership, preventing use-after-free
 * and cross-client interference.
 *
 * The allocator itself is stateless beyond the device_memory_map struct;
 * create/destroy simply calloc/free that struct.
 */

#define _GNU_SOURCE

#include "allocator.h"

#include <stdlib.h>

/**
 * Create a new device memory map with all subregions marked as free.
 *
 * calloc zero-initialises the struct, which means every client_id slot
 * starts at 0 (the "free" sentinel).
 *
 * @return Heap-allocated map, or NULL on allocation failure.
 */
struct device_memory_map *device_memory_map_create(void)
{
    return calloc(1, sizeof(struct device_memory_map));
}

/** Free a device memory map.  NULL-safe. */
void device_memory_map_cleanup(struct device_memory_map *map)
{
    if (map == NULL) {
        return;
    }

    free(map);
}

/**
 * Allocate contiguous device memory from a DDR or HBM region.
 *
 * The requested @size is rounded up to the next multiple of SUBREGION_SIZE
 * (64 MiB).  The allocator performs a first-fit scan: it walks subregions
 * within each candidate region, counting consecutive free slots; when the
 * run length reaches the required count, the subregions are stamped with
 * @client_id and the device-side base address is returned.
 *
 * For ALLOCATION_TYPE_HBM (non-VNOC), @arg selects the specific HBM region
 * index (0..63).  For DDR and HBM_VNOC, @arg is ignored and all regions are
 * scanned.
 *
 * @param map        The device memory map to allocate from.
 * @param type       DDR, HBM (pinned region), or HBM_VNOC (any HBM region).
 * @param size       [in/out] Requested size; updated to the rounded-up allocated size.
 * @param arg        HBM region index for ALLOCATION_TYPE_HBM; unused otherwise.
 * @param client_id  Non-zero connection ID that will own the allocation.
 * @param addr_out   [out] Device-side base address of the allocation.
 * @return           ALLOCATION_RESULT_SUCCESS, _NO_MEMORY, or _BAD_ARGUMENT.
 */
enum allocation_result device_memory_map_allocate(struct device_memory_map *map,
                                                  enum allocation_type type,
                                                  uint64_t *size,
                                                  uint64_t arg,
                                                  uint64_t client_id,
                                                  uint64_t *addr_out)

{
    if (map == NULL || size == NULL || addr_out == NULL || *size == 0) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    /* Round up to whole subregions (64 MiB granularity). */
    uint64_t num_subregions = (*size + SUBREGION_SIZE - 1) / SUBREGION_SIZE;

    if (client_id == 0) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    switch (type) {
    case ALLOCATION_TYPE_DDR: {
        if (num_subregions > SUBREGIONS_PER_REGION) {
            return ALLOCATION_RESULT_BAD_ARGUMENT;
        }
        /* Scan all DDR regions for a contiguous run of free subregions
         * (first-fit across regions, first-fit within each region). */
        for (size_t region_idx = 0; region_idx < DDR_REGIONS; region_idx++) {
            size_t contiguous_free = 0;
            for (size_t subregion_idx = 0; subregion_idx < SUBREGIONS_PER_REGION; subregion_idx++) {
                if (map->ddr_regions[region_idx].client_id[subregion_idx] == 0) {
                    contiguous_free++;
                    if (contiguous_free == num_subregions) {
                        // Found a suitable block
                        size_t start_subregion = subregion_idx + 1 - num_subregions;
                        for (size_t i = 0; i < num_subregions; i++) {
                            map->ddr_regions[region_idx].client_id[start_subregion + i] = client_id;
                        }
                        *addr_out = DDR_START_ADDRESS + (region_idx * REGION_SIZE) +
                                    (start_subregion * SUBREGION_SIZE);
                        *size = num_subregions * SUBREGION_SIZE;
                        return ALLOCATION_RESULT_SUCCESS;
                    }
                } else {
                    contiguous_free = 0;
                }
            }
        }
        // No suitable block found
        return ALLOCATION_RESULT_NO_MEMORY;
    }

    case ALLOCATION_TYPE_HBM: {
        /* HBM (non-VNOC): the caller specifies exactly which HBM region
         * to allocate from via @arg.  Useful when the FPGA design routes
         * a particular AXI master to a specific HBM pseudo-channel. */
        if (arg >= HBM_REGIONS || num_subregions > SUBREGIONS_PER_REGION) {
            return ALLOCATION_RESULT_BAD_ARGUMENT;
        }

        size_t region_idx = (size_t)arg;

        if (region_idx >= HBM_REGIONS) {
            return ALLOCATION_RESULT_BAD_ARGUMENT;
        }

        size_t contiguous_free = 0;
        for (size_t subregion_idx = 0; subregion_idx < SUBREGIONS_PER_REGION; subregion_idx++) {
            if (map->hbm_regions[region_idx].client_id[subregion_idx] == 0) {
                contiguous_free++;
                if (contiguous_free == num_subregions) {
                    size_t start_subregion = subregion_idx + 1 - num_subregions;
                    for (size_t i = 0; i < num_subregions; i++) {
                        map->hbm_regions[region_idx].client_id[start_subregion + i] = client_id;
                    }
                    *addr_out = HBM_START_ADDRESS + (region_idx * REGION_SIZE) +
                                (start_subregion * SUBREGION_SIZE);
                    *size = num_subregions * SUBREGION_SIZE;
                    return ALLOCATION_RESULT_SUCCESS;
                }
            } else {
                contiguous_free = 0;
            }
        }

        return ALLOCATION_RESULT_NO_MEMORY;
    }

    case ALLOCATION_TYPE_HBM_VNOC: {
        /* HBM via VNoC: the caller does not care which HBM region is used,
         * so we scan all HBM regions (first-fit) to find available space. */
        if (num_subregions > SUBREGIONS_PER_REGION) {
            return ALLOCATION_RESULT_BAD_ARGUMENT;
        }

        /* Scan all HBM regions for a contiguous block of free subregions. */
        for (size_t region_idx = 0; region_idx < HBM_REGIONS; region_idx++) {
            size_t contiguous_free = 0;
            for (size_t subregion_idx = 0; subregion_idx < SUBREGIONS_PER_REGION; subregion_idx++) {
                if (map->hbm_regions[region_idx].client_id[subregion_idx] == 0) {
                    contiguous_free++;
                    if (contiguous_free == num_subregions) {
                        size_t start_subregion = subregion_idx + 1 - num_subregions;
                        for (size_t i = 0; i < num_subregions; i++) {
                            map->hbm_regions[region_idx].client_id[start_subregion + i] = client_id;
                        }
                        *addr_out = HBM_START_ADDRESS + (region_idx * REGION_SIZE) +
                                    (start_subregion * SUBREGION_SIZE);
                        *size = num_subregions * SUBREGION_SIZE;
                        return ALLOCATION_RESULT_SUCCESS;
                    }
                } else {
                    contiguous_free = 0;
                }
            }
        }

        return ALLOCATION_RESULT_NO_MEMORY;
    }

    }

    return ALLOCATION_RESULT_BAD_ARGUMENT;
}

/**
 * Release a previously allocated device memory region.
 *
 * The function validates that every subregion in the [addr, addr+size) range
 * is owned by @client_id before clearing any ownership.  This two-pass
 * approach (verify first, then clear) prevents partial frees when the
 * caller provides mismatched parameters.
 *
 * @param map        The device memory map.
 * @param type       Memory type (DDR or HBM/HBM_VNOC).
 * @param addr       Device-side base address returned by allocate.
 * @param size       Size of the allocation to free.
 * @param client_id  The owning connection ID; must match all subregions.
 * @return           ALLOCATION_RESULT_SUCCESS or _BAD_ARGUMENT.
 */
enum allocation_result device_memory_map_free(struct device_memory_map *map,
                                              enum allocation_type type,
                                              uint64_t addr,
                                              uint64_t size,
                                              uint64_t client_id)
{
    if (map == NULL || size == 0 || client_id == 0) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    uint64_t base;
    uint64_t max_size;
    struct ddr_region_data *ddr_regions = NULL;
    struct hbm_region_data *hbm_regions = NULL;

    switch (type) {
    case ALLOCATION_TYPE_DDR:
        base = DDR_START_ADDRESS;
        max_size = DDR_REGIONS * REGION_SIZE;
        ddr_regions = map->ddr_regions;
        break;
    case ALLOCATION_TYPE_HBM:
    case ALLOCATION_TYPE_HBM_VNOC:
        base = HBM_START_ADDRESS;
        max_size = HBM_REGIONS * REGION_SIZE;
        hbm_regions = map->hbm_regions;
        break;
    default:
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    if (addr < base || addr >= base + max_size) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    /* Compute the offset relative to the memory class base and verify
     * it is subregion-aligned (addresses must have been returned by allocate). */
    uint64_t offset = addr - base;
    if ((offset % SUBREGION_SIZE) != 0) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    uint64_t num_subregions = (size + SUBREGION_SIZE - 1) / SUBREGION_SIZE;
    size_t region_idx = (size_t)(offset / REGION_SIZE);
    size_t start_subregion = (size_t)((offset % REGION_SIZE) / SUBREGION_SIZE);

    if (num_subregions > SUBREGIONS_PER_REGION ||
        start_subregion + num_subregions > SUBREGIONS_PER_REGION) {
        return ALLOCATION_RESULT_BAD_ARGUMENT;
    }

    /* Two-pass free: first verify all subregions belong to this client,
     * then clear ownership.  This avoids a partial free if any subregion
     * has been freed already or belongs to a different client. */
    if (ddr_regions != NULL) {
        for (size_t i = 0; i < num_subregions; i++) {
            if (ddr_regions[region_idx].client_id[start_subregion + i] != client_id) {
                return ALLOCATION_RESULT_BAD_ARGUMENT;
            }
        }
        for (size_t i = 0; i < num_subregions; i++) {
            ddr_regions[region_idx].client_id[start_subregion + i] = 0;
        }
        return ALLOCATION_RESULT_SUCCESS;
    }

    for (size_t i = 0; i < num_subregions; i++) {
        if (hbm_regions[region_idx].client_id[start_subregion + i] != client_id) {
            return ALLOCATION_RESULT_BAD_ARGUMENT;
        }
    }
    for (size_t i = 0; i < num_subregions; i++) {
        hbm_regions[region_idx].client_id[start_subregion + i] = 0;
    }
    return ALLOCATION_RESULT_SUCCESS;
}
