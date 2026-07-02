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

#include <gtest/gtest.h>

#include <cstdint>

#include "../libvrtd/src/v80_policy.h"

namespace {

constexpr uint64_t STEP = 4096;
constexpr uint64_t MiB = 1024ULL * 1024ULL;
constexpr uint64_t GiB = 1024ULL * MiB;

// A single available queue always carries the whole transfer on fds[0].
TEST(V80Plan, SingleQueueIsWhole) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint32_t n = vrtd_plan_v80(VRTD_V80_HBM_BASE, 0, 512 * MiB, STEP, 1, segs);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].offset, 0u);
    EXPECT_EQ(segs[0].size, 512 * MiB);
}

// DDR has a single NSU, so the range is split in half across both channels.
TEST(V80Plan, DdrSplitsInHalf) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint32_t n = vrtd_plan_v80(VRTD_V80_DDR_BASE, 0, 512 * MiB, STEP, 2, segs);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].offset, 0u);
    EXPECT_EQ(segs[0].size, 256 * MiB);
    EXPECT_EQ(segs[1].qpair_index, 1u);
    EXPECT_EQ(segs[1].offset, 256 * MiB);
    EXPECT_EQ(segs[1].size, 256 * MiB);
}

// A DDR transfer too small to halve along the step boundary stays on fds[0].
TEST(V80Plan, DdrTinyTransferStaysOnPrimary) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint32_t n = vrtd_plan_v80(VRTD_V80_DDR_BASE, 0, STEP, STEP, 2, segs);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].size, STEP);
}

// An HBM buffer entirely below the half-boundary uses channel 0 only.
TEST(V80Plan, HbmLowerHalfChannel0) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint32_t n = vrtd_plan_v80(VRTD_V80_HBM_BASE, 0, 512 * MiB, STEP, 2, segs);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].offset, 0u);
    EXPECT_EQ(segs[0].size, 512 * MiB);
}

// An HBM buffer entirely at/above the half-boundary uses channel 1 only.
TEST(V80Plan, HbmUpperHalfChannel1) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint64_t base = VRTD_V80_HBM_BASE + VRTD_V80_HBM_HALF + 4 * GiB;
    uint32_t n = vrtd_plan_v80(base, 0, 512 * MiB, STEP, 2, segs);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(segs[0].qpair_index, 1u);
    EXPECT_EQ(segs[0].offset, 0u);
    EXPECT_EQ(segs[0].size, 512 * MiB);
}

// A buffer sitting exactly on the boundary belongs to the upper half.
TEST(V80Plan, HbmOnBoundaryIsUpperHalf) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint64_t base = VRTD_V80_HBM_BASE + VRTD_V80_HBM_HALF;
    uint32_t n = vrtd_plan_v80(base, 0, 256 * MiB, STEP, 2, segs);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(segs[0].qpair_index, 1u);
}

// An HBM range straddling the boundary splits exactly at it.
TEST(V80Plan, HbmSpanningSplitsAtBoundary) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint64_t base = VRTD_V80_HBM_BASE + VRTD_V80_HBM_HALF - 256 * MiB;
    uint32_t n = vrtd_plan_v80(base, 0, 512 * MiB, STEP, 2, segs);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].offset, 0u);
    EXPECT_EQ(segs[0].size, 256 * MiB);
    EXPECT_EQ(segs[1].qpair_index, 1u);
    EXPECT_EQ(segs[1].offset, 256 * MiB);
    EXPECT_EQ(segs[1].size, 256 * MiB);
}

// The split point is computed from the absolute device address, so a non-zero
// buffer offset that crosses the boundary is honoured.
TEST(V80Plan, HbmSpanningWithOffset) {
    vrtd_xfer_seg segs[VRTD_V80_MAX_SEGS];
    uint64_t offset = VRTD_V80_HBM_HALF - STEP;  // crosses boundary STEP into the range
    uint32_t n = vrtd_plan_v80(VRTD_V80_HBM_BASE, offset, 2 * STEP, STEP, 2, segs);
    ASSERT_EQ(n, 2u);
    EXPECT_EQ(segs[0].qpair_index, 0u);
    EXPECT_EQ(segs[0].offset, offset);
    EXPECT_EQ(segs[0].size, STEP);
    EXPECT_EQ(segs[1].qpair_index, 1u);
    EXPECT_EQ(segs[1].offset, offset + STEP);
    EXPECT_EQ(segs[1].size, STEP);
}

}  // namespace
