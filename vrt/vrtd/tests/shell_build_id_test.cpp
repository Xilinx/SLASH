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
#include <cstring>
#include <vector>

extern "C" {
#include "shell_build_id.h"
}

namespace {

// A BAR mapping large enough to hold the build-ID register, with the high word
// preset to `hi`. The low word is left at zero; nothing under test reads it.
class FakeBar {
  public:
    explicit FakeBar(uint32_t hi, size_t len = BUILD_ID_REG_HI + sizeof(uint32_t))
        : storage_(len, 0) {
        if (len >= BUILD_ID_REG_HI + sizeof(uint32_t)) {
            std::memcpy(&storage_[BUILD_ID_REG_HI], &hi, sizeof(hi));
        }
        bar_.map = storage_.data();
        bar_.len = storage_.size();
        bar_.fd = -1;
        bar_.mock = true;
        bar_.mock_path = nullptr;
    }

    const struct slash_bar_file *get() const { return &bar_; }

  private:
    std::vector<uint8_t> storage_;
    struct slash_bar_file bar_{};
};

// Build-ID high words as the two shells emit them: bits[27:0] carry the top of
// the commit hash, bit[28] the shell variant, bit[31] the dirty flag.
constexpr uint32_t kServiceHi = 0x0abcdef0u;
constexpr uint32_t kComputeHi = 0x1abcdef0u;

}  // namespace

TEST(BuildIdDecodeTest, DecodesBothShellVariants) {
    EXPECT_EQ(build_id_decode_shell(kServiceHi), VRTD_SHELL_SERVICE);
    EXPECT_EQ(build_id_decode_shell(kComputeHi), VRTD_SHELL_COMPUTE);
}

TEST(BuildIdDecodeTest, DirtyFlagDoesNotAffectTheShellVariant) {
    EXPECT_EQ(build_id_decode_shell(kServiceHi | 0x80000000u), VRTD_SHELL_SERVICE);
    EXPECT_EQ(build_id_decode_shell(kComputeHi | 0x80000000u), VRTD_SHELL_COMPUTE);
}

TEST(BuildIdDecodeTest, AnInteractiveBuildWithNoHashStillIdentifiesItsShell) {
    // create_project.tcl forces bit[28] regardless of SLASH_BUILD_ID_HI, so the
    // variant is valid even when the hash bits are zero.
    EXPECT_EQ(build_id_decode_shell(0x00000000u), VRTD_SHELL_SERVICE);
    EXPECT_EQ(build_id_decode_shell(0x10000000u), VRTD_SHELL_COMPUTE);
}

TEST(BuildIdDecodeTest, RejectsWordsThatCannotBeABuildId) {
    // An MMIO read that does not reach the device returns all ones.
    EXPECT_EQ(build_id_decode_shell(0xFFFFFFFFu), VRTD_SHELL_UNKNOWN);
    // No shell sets the reserved bits.
    EXPECT_EQ(build_id_decode_shell(kServiceHi | 0x20000000u), VRTD_SHELL_UNKNOWN);
    EXPECT_EQ(build_id_decode_shell(kComputeHi | 0x40000000u), VRTD_SHELL_UNKNOWN);
}

TEST(BuildIdReadTest, ReportsUnknownWithoutAUsableMapping) {
    EXPECT_EQ(build_id_read_shell(nullptr), VRTD_SHELL_UNKNOWN);

    FakeBar truncated(kComputeHi, BUILD_ID_REG_HI);
    EXPECT_EQ(build_id_read_shell(truncated.get()), VRTD_SHELL_UNKNOWN);
}

TEST(BuildIdReadTest, ReadsTheVariantFromTheMapping) {
    FakeBar service(kServiceHi);
    FakeBar compute(kComputeHi);

    EXPECT_EQ(build_id_read_shell(service.get()), VRTD_SHELL_SERVICE);
    EXPECT_EQ(build_id_read_shell(compute.get()), VRTD_SHELL_COMPUTE);
}

TEST(BuildIdCheckTest, PassesWhenHardwareAgrees) {
    FakeBar service(kServiceHi);
    EXPECT_EQ(build_id_check_shell(service.get(), VRTD_SHELL_SERVICE, "test"), 0);
}

TEST(BuildIdCheckTest, FailsWhenHardwareReportsTheOtherShell) {
    // Regression for SLASH issue 207: vrtd drove the device believing it was
    // running a shell the hardware was not running.
    FakeBar service(kServiceHi);
    EXPECT_EQ(build_id_check_shell(service.get(), VRTD_SHELL_COMPUTE, "test"), -1);
}

TEST(BuildIdCheckTest, FailsWhenTheRegisterDoesNotRespond) {
    FakeBar dead(0xFFFFFFFFu);
    EXPECT_EQ(build_id_check_shell(dead.get(), VRTD_SHELL_SERVICE, "test"), -1);
    EXPECT_EQ(build_id_check_shell(nullptr, VRTD_SHELL_COMPUTE, "test"), -1);
}

TEST(BuildIdCheckTest, PassesWhenNoShellHasBeenClaimed) {
    // Nothing to contradict: an unknown expectation is not a mismatch, even
    // when the register itself is unreadable.
    FakeBar dead(0xFFFFFFFFu);
    EXPECT_EQ(build_id_check_shell(dead.get(), VRTD_SHELL_UNKNOWN, "test"), 0);
    EXPECT_EQ(build_id_check_shell(nullptr, VRTD_SHELL_UNKNOWN, "test"), 0);
}
