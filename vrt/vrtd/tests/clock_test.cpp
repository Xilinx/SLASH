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

extern "C" {
#include "clock.h"
}

namespace {

constexpr uint32_t kOMin = 2u;
constexpr uint32_t kOMax = 511u;

}  // namespace

TEST(ClockWizardLeafTest, EncodeDecodeRoundTripsEveryDivider) {
    for (uint32_t o = kOMin; o <= kOMax; ++o) {
        uint32_t ctrl = 0;
        uint32_t counts = 0;
        clock_wizard_encode_leaf(o, &ctrl, &counts);

        EXPECT_EQ(clock_wizard_decode_leaf(ctrl, counts), o)
            << "leaf encode/decode disagree for O=" << o;
    }
}

TEST(ClockWizardLeafTest, DecodesOddAndHalfStepDividers) {
    // Regression for SLASH issue 206: O=6 decoded as 5, reporting a rate
    // exactly 1.2x the one actually programmed.
    const struct {
        uint32_t o;
        uint32_t ctrl;
        uint32_t counts;
    } kCases[] = {
        {5u, 0xBA00u, 0x0101u},
        {6u, 0x1B00u, 0x0101u},
        {9u, 0xBA00u, 0x0202u},
        {10u, 0x1B00u, 0x0202u},
        {20u, 0x1A00u, 0x0505u},  // observed on hardware: leaf0/leaf1 for O=20
    };

    for (const auto &c : kCases) {
        uint32_t ctrl = 0;
        uint32_t counts = 0;
        clock_wizard_encode_leaf(c.o, &ctrl, &counts);

        EXPECT_EQ(ctrl, c.ctrl) << "unexpected leaf flags for O=" << c.o;
        EXPECT_EQ(counts, c.counts) << "unexpected leaf counts for O=" << c.o;
        EXPECT_EQ(clock_wizard_decode_leaf(c.ctrl, c.counts), c.o);
    }
}

TEST(ClockWizardLeafTest, DecodeNeverReturnsZero) {
    EXPECT_EQ(clock_wizard_decode_leaf(0u, 0u), 1u);
}
