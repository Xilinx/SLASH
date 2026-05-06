/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <gtest/gtest.h>

extern "C" {
#include "allocator.h"
}

class AllocatorTest : public ::testing::Test {
   protected:
    struct device_memory_map *map = nullptr;

    void SetUp() override {
        map = device_memory_map_create();
        ASSERT_NE(map, nullptr);
    }

    void TearDown() override {
        device_memory_map_cleanup(map);
    }
};

// --- Create / Destroy ---

TEST_F(AllocatorTest, CreateReturnsNonNull) {
    EXPECT_NE(map, nullptr);
}

TEST_F(AllocatorTest, FreshMapHasAllRegionsFree) {
    for (int r = 0; r < DDR_REGIONS; r++) {
        for (int s = 0; s < (int)SUBREGIONS_PER_REGION; s++) {
            EXPECT_EQ(map->ddr_regions[r].client_id[s], 0u);
        }
    }
    for (int r = 0; r < HBM_REGIONS; r++) {
        for (int s = 0; s < (int)SUBREGIONS_PER_REGION; s++) {
            EXPECT_EQ(map->hbm_regions[r].client_id[s], 0u);
        }
    }
}

TEST_F(AllocatorTest, CleanupNullIsSafe) {
    device_memory_map_cleanup(nullptr);
}

// --- Argument validation ---

TEST_F(AllocatorTest, AllocateNullMap) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(nullptr, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, AllocateNullSize) {
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, nullptr, 0, 1, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, AllocateNullAddrOut) {
    uint64_t size = SUBREGION_SIZE;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, nullptr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, AllocateZeroSize) {
    uint64_t size = 0;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, AllocateZeroClientId) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 0, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeNullMap) {
    EXPECT_EQ(device_memory_map_free(nullptr, ALLOCATION_TYPE_DDR, DDR_START_ADDRESS, SUBREGION_SIZE, 1),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeZeroSize) {
    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, DDR_START_ADDRESS, 0, 1),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeZeroClientId) {
    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, DDR_START_ADDRESS, SUBREGION_SIZE, 0),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

// --- DDR allocation ---

TEST_F(AllocatorTest, DdrSingleSubregion) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, DDR_START_ADDRESS);
    EXPECT_EQ(size, SUBREGION_SIZE);
}

TEST_F(AllocatorTest, DdrSizeRoundsUp) {
    uint64_t size = 1;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(size, SUBREGION_SIZE);
}

TEST_F(AllocatorTest, DdrMultiSubregion) {
    uint64_t size = 3 * SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, DDR_START_ADDRESS);
    EXPECT_EQ(size, 3 * SUBREGION_SIZE);

    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(map->ddr_regions[0].client_id[i], 1u);
    }
    for (int i = 3; i < (int)SUBREGIONS_PER_REGION; i++) {
        EXPECT_EQ(map->ddr_regions[0].client_id[i], 0u);
    }
}

TEST_F(AllocatorTest, DdrFullRegion) {
    uint64_t size = SUBREGIONS_PER_REGION * SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, DDR_START_ADDRESS);
}

TEST_F(AllocatorTest, DdrExceedsSingleRegion) {
    uint64_t size = (SUBREGIONS_PER_REGION + 1) * SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, DdrSecondAllocationFollowsFirst) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr1, addr2;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr1),
              ALLOCATION_RESULT_SUCCESS);
    size = SUBREGION_SIZE;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr2),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr2, DDR_START_ADDRESS + SUBREGION_SIZE);
}

TEST_F(AllocatorTest, DdrSpillsToNextRegion) {
    uint64_t size = SUBREGIONS_PER_REGION * SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);

    size = SUBREGION_SIZE;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 2, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, DDR_START_ADDRESS + REGION_SIZE);
}

TEST_F(AllocatorTest, DdrExhaustAllRegions) {
    for (int r = 0; r < DDR_REGIONS; r++) {
        uint64_t size = SUBREGIONS_PER_REGION * SUBREGION_SIZE;
        uint64_t addr;
        ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
                  ALLOCATION_RESULT_SUCCESS);
    }

    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_NO_MEMORY);
}

// --- HBM allocation (pinned region) ---

TEST_F(AllocatorTest, HbmPinnedRegion) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM, &size, 5, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, HBM_START_ADDRESS + 5 * REGION_SIZE);
}

TEST_F(AllocatorTest, HbmRegionOutOfRange) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM, &size, HBM_REGIONS, 1, &addr),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, HbmPinnedRegionExhaustion) {
    for (int s = 0; s < (int)SUBREGIONS_PER_REGION; s++) {
        uint64_t size = SUBREGION_SIZE;
        uint64_t addr;
        ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM, &size, 0, 1, &addr),
                  ALLOCATION_RESULT_SUCCESS);
    }

    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM, &size, 0, 1, &addr),
              ALLOCATION_RESULT_NO_MEMORY);
}

// --- HBM_VNOC allocation (auto-region) ---

TEST_F(AllocatorTest, HbmVnocAutoSelection) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    EXPECT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM_VNOC, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, HBM_START_ADDRESS);
}

TEST_F(AllocatorTest, HbmVnocFirstFitAcrossRegions) {
    for (int s = 0; s < (int)SUBREGIONS_PER_REGION; s++) {
        uint64_t size = SUBREGION_SIZE;
        uint64_t addr;
        ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM_VNOC, &size, 0, 1, &addr),
                  ALLOCATION_RESULT_SUCCESS);
    }

    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM_VNOC, &size, 0, 2, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, HBM_START_ADDRESS + REGION_SIZE);
}

// --- Free ---

TEST_F(AllocatorTest, FreeAndReallocate) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    ASSERT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, addr, size, 1),
              ALLOCATION_RESULT_SUCCESS);

    EXPECT_EQ(map->ddr_regions[0].client_id[0], 0u);

    size = SUBREGION_SIZE;
    uint64_t addr2;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 2, &addr2),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr2, addr);
}

TEST_F(AllocatorTest, FreeWrongClientIdDenied) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);

    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, addr, size, 99),
              ALLOCATION_RESULT_BAD_ARGUMENT);

    EXPECT_EQ(map->ddr_regions[0].client_id[0], 1u);
}

TEST_F(AllocatorTest, FreeUnalignedAddress) {
    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, DDR_START_ADDRESS + 1, SUBREGION_SIZE, 1),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeAddressOutOfRange) {
    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, 0, SUBREGION_SIZE, 1),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeCrossRegionSpan) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr = DDR_START_ADDRESS + (SUBREGIONS_PER_REGION - 1) * SUBREGION_SIZE;
    uint64_t free_size = 2 * SUBREGION_SIZE;
    EXPECT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, addr, free_size, 1),
              ALLOCATION_RESULT_BAD_ARGUMENT);
}

TEST_F(AllocatorTest, FreeHbm) {
    uint64_t size = 2 * SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM, &size, 3, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    ASSERT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_HBM, addr, size, 1),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(map->hbm_regions[3].client_id[0], 0u);
    EXPECT_EQ(map->hbm_regions[3].client_id[1], 0u);
}

// --- Multi-client isolation ---

TEST_F(AllocatorTest, MultiClientIsolation) {
    uint64_t size = SUBREGION_SIZE;
    uint64_t addr1, addr2;

    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 100, &addr1),
              ALLOCATION_RESULT_SUCCESS);
    size = SUBREGION_SIZE;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 200, &addr2),
              ALLOCATION_RESULT_SUCCESS);

    EXPECT_EQ(map->ddr_regions[0].client_id[0], 100u);
    EXPECT_EQ(map->ddr_regions[0].client_id[1], 200u);

    ASSERT_EQ(device_memory_map_free(map, ALLOCATION_TYPE_DDR, addr1, SUBREGION_SIZE, 100),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(map->ddr_regions[0].client_id[0], 0u);
    EXPECT_EQ(map->ddr_regions[0].client_id[1], 200u);
}

// --- Address math verification ---

TEST_F(AllocatorTest, DdrAddressMath) {
    for (int r = 0; r < 3; r++) {
        uint64_t size = SUBREGIONS_PER_REGION * SUBREGION_SIZE;
        uint64_t addr;
        ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_DDR, &size, 0, 1, &addr),
                  ALLOCATION_RESULT_SUCCESS);
        EXPECT_EQ(addr, DDR_START_ADDRESS + (uint64_t)r * REGION_SIZE);
    }
}

TEST_F(AllocatorTest, HbmVnocAddressMath) {
    uint64_t size = 2 * SUBREGION_SIZE;
    uint64_t addr;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM_VNOC, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, HBM_START_ADDRESS);

    size = 2 * SUBREGION_SIZE;
    ASSERT_EQ(device_memory_map_allocate(map, ALLOCATION_TYPE_HBM_VNOC, &size, 0, 1, &addr),
              ALLOCATION_RESULT_SUCCESS);
    EXPECT_EQ(addr, HBM_START_ADDRESS + 2 * SUBREGION_SIZE);
}
