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
#include "reset.h"
}

TEST(ResetShellTest, ShellBootPartitionMapsKnownShells) {
    uint32_t partition = UINT32_MAX;

    EXPECT_EQ(shell_boot_partition(VRTD_SHELL_SERVICE, &partition), 0);
    EXPECT_EQ(partition, 0u);

    partition = UINT32_MAX;
    EXPECT_EQ(shell_boot_partition(VRTD_SHELL_COMPUTE, &partition), 0);
    EXPECT_EQ(partition, 1u);
}

TEST(ResetShellTest, ShellBootPartitionRejectsUnknownShell) {
    uint32_t partition = UINT32_MAX;

    EXPECT_NE(shell_boot_partition(VRTD_SHELL_UNKNOWN, &partition), 0);
    EXPECT_EQ(partition, UINT32_MAX);
}

TEST(ResetShellTest, ShellResetRequiredForUnknownOrMismatch) {
    EXPECT_TRUE(shell_reset_required(VRTD_SHELL_UNKNOWN, VRTD_SHELL_SERVICE));
    EXPECT_TRUE(shell_reset_required(VRTD_SHELL_SERVICE, VRTD_SHELL_COMPUTE));
    EXPECT_TRUE(shell_reset_required(VRTD_SHELL_COMPUTE, VRTD_SHELL_SERVICE));
}

TEST(ResetShellTest, ShellResetNotRequiredForMatch) {
    EXPECT_FALSE(shell_reset_required(VRTD_SHELL_SERVICE, VRTD_SHELL_SERVICE));
    EXPECT_FALSE(shell_reset_required(VRTD_SHELL_COMPUTE, VRTD_SHELL_COMPUTE));
}

TEST(ResetShellTest, JtagBlocksShellSwitchResetOnlyWhenResetWouldBeRequired) {
    EXPECT_TRUE(shell_switch_blocked_by_jtag(
        VRTD_SHELL_SERVICE, VRTD_SHELL_COMPUTE, true));
    EXPECT_TRUE(shell_switch_blocked_by_jtag(
        VRTD_SHELL_UNKNOWN, VRTD_SHELL_SERVICE, true));

    EXPECT_FALSE(shell_switch_blocked_by_jtag(
        VRTD_SHELL_SERVICE, VRTD_SHELL_SERVICE, true));
    EXPECT_FALSE(shell_switch_blocked_by_jtag(
        VRTD_SHELL_SERVICE, VRTD_SHELL_COMPUTE, false));
}
