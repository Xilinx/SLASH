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

#include <cerrno>
#include <cstdint>
#include <vector>

extern "C" {
#include "clock.h"
}

namespace {

constexpr uint32_t kOMin = 2u;
constexpr uint32_t kOMax = 511u;

// Wizard register offsets, mirrored from clock.c so the tests exercise the
// driver through its public interface rather than its internals.
constexpr uint32_t kUserWizard = CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET;
constexpr uint32_t kServiceWizard = CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET;
constexpr uint32_t kReg1 = 0x330u;   // M edge bit
constexpr uint32_t kReg2 = 0x334u;   // M low/high counts
constexpr uint32_t kReg3 = 0x338u;   // clk_out0 leaf pair (ctrl at +0, counts at +4)
constexpr uint32_t kReg12 = 0x380u;  // D edge bit
constexpr uint32_t kReg13 = 0x384u;  // D low/high counts

constexpr uint32_t kPrimInHz = 100000000u;

// Both wizard windows plus the full 0x400-byte register file of the higher one.
constexpr size_t kBarBytes = kServiceWizard + 0x400u;

/**
 * A clock_driver backed by an ordinary buffer instead of a mapped BAR.
 *
 * The read paths touch only regs, len and prim_in_hz, so a device is not
 * needed to drive them.
 */
class FakeWizard {
public:
    explicit FakeWizard(uint32_t fill = 0u)
        : words_(kBarBytes / sizeof(uint32_t), fill) {
        clk_.regs = words_.data();
        clk_.len = kBarBytes;
        clk_.prim_in_hz = kPrimInHz;
    }

    void poke(uint32_t offset, uint32_t value) {
        words_[offset / sizeof(uint32_t)] = value;
    }

    /** Program M, D and O for one wizard the way the hardware encodes them. */
    void program(uint32_t wizard, uint32_t m, uint32_t d, uint32_t o) {
        poke(wizard + kReg1, 0u);
        poke(wizard + kReg2, (m / 2u) | ((m - m / 2u) << 8u));
        poke(wizard + kReg12, 0u);
        poke(wizard + kReg13, (d / 2u) | ((d - d / 2u) << 8u));

        uint32_t ctrl = 0;
        uint32_t counts = 0;
        clock_wizard_encode_leaf(o, &ctrl, &counts);
        poke(wizard + kReg3, ctrl);
        poke(wizard + kReg3 + 4u, counts);
    }

    struct clock_driver *get() { return &clk_; }

private:
    std::vector<uint32_t> words_;
    struct clock_driver clk_{};
};

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

TEST(ClockWizardLeafTest, DecodeReportsAnEmptyLeafPairAsInvalid) {
    // Regression for SLASH issue 207: a cleared leaf pair was clamped to a
    // divider of 1, so an unresponsive register window reported f_out = f_VCO
    // instead of being rejected.
    EXPECT_EQ(clock_wizard_decode_leaf(0u, 0u), 0u);
}

TEST(ClockDriverGetRateTest, ReportsAProgrammedRate) {
    FakeWizard wizard;
    // M=12, D=1, O=6 => 100 MHz * 12 / 1 / 6 = 200 MHz.
    wizard.program(kUserWizard, 12u, 1u, 6u);
    wizard.program(kServiceWizard, 12u, 1u, 6u);

    uint32_t rate = 0;
    ASSERT_EQ(clock_driver_get_user_region_rate_hz(wizard.get(), &rate), 0);
    EXPECT_EQ(rate, 200000000u);

    rate = 0;
    ASSERT_EQ(clock_driver_get_service_region_rate_hz(wizard.get(), &rate), 0);
    EXPECT_EQ(rate, 200000000u);
}

TEST(ClockDriverGetRateTest, ClearedRegistersReportUnconfiguredRatherThanTheReferenceClock) {
    // The state a healthy V80 is actually in before anything programs a rate:
    // the whole 0x330-0x39C range reads zero. Clamping M, D and O to 1 used to
    // collapse f_out to f_in and report a confident 100 MHz for a clock nobody
    // had set. Zero is the honest answer, and succeeds so callers can say so.
    FakeWizard wizard(0u);

    uint32_t rate = 0xdeadbeefu;
    ASSERT_EQ(clock_driver_get_user_region_rate_hz(wizard.get(), &rate), 0);
    EXPECT_EQ(rate, 0u);

    rate = 0xdeadbeefu;
    ASSERT_EQ(clock_driver_get_service_region_rate_hz(wizard.get(), &rate), 0);
    EXPECT_EQ(rate, 0u);
}

TEST(ClockDriverGetRateTest, ProgrammedMAndDWithAClearedLeafPairIsUnconfigured) {
    // The SLASH issue 207 signature: the M/D writes landed but the output
    // divider write did not. f_VCO alone is not an output rate, and reporting
    // it gave the 4 GHz reading in the bug report.
    FakeWizard wizard;
    wizard.program(kUserWizard, 40u, 1u, 6u);
    wizard.poke(kUserWizard + kReg3, 0u);
    wizard.poke(kUserWizard + kReg3 + 4u, 0u);

    uint32_t rate = 0xdeadbeefu;
    ASSERT_EQ(clock_driver_get_user_region_rate_hz(wizard.get(), &rate), 0);
    EXPECT_EQ(rate, 0u);
}

TEST(ClockDriverGetRateTest, AnUnresponsiveWindowIsAnError) {
    // A register window that answers 0xFFFFFFFF is not answering. That is a
    // fault, distinct from a wizard that simply has nothing programmed.
    FakeWizard wizard(0xFFFFFFFFu);

    uint32_t rate = 0;
    errno = 0;
    EXPECT_EQ(clock_driver_get_user_region_rate_hz(wizard.get(), &rate), -1);
    EXPECT_EQ(errno, EIO);

    errno = 0;
    EXPECT_EQ(clock_driver_get_service_region_rate_hz(wizard.get(), &rate), -1);
    EXPECT_EQ(errno, EIO);
}
