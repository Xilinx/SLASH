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

#include <cerrno>
#include <cstring>

extern "C" {
#include "hotplug.h"
}

// --- pci_bdf_prefix ---

TEST(PciBdfPrefixTest, StripsFunctionSuffix) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_prefix("0000:65:00.2", out), 0);
    EXPECT_STREQ(out, "0000:65:00");
}

TEST(PciBdfPrefixTest, AlreadyBoardLevel) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_prefix("0000:65:00", out), 0);
    EXPECT_STREQ(out, "0000:65:00");
}

TEST(PciBdfPrefixTest, NullBdf) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_prefix(nullptr, out), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfPrefixTest, NullOutput) {
    EXPECT_EQ(pci_bdf_prefix("0000:65:00.2", nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfPrefixTest, EmptyString) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_prefix("", out), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfPrefixTest, DotAtStart) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_prefix(".2", out), 0);
    EXPECT_STREQ(out, ".2");
}

// --- pci_bdf_set_function ---

TEST(PciBdfSetFunctionTest, ReplacesFunction) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("0000:65:00.0", 2, out), 0);
    EXPECT_STREQ(out, "0000:65:00.2");
}

TEST(PciBdfSetFunctionTest, BoardLevelInput) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("0000:65:00", 3, out), 0);
    EXPECT_STREQ(out, "0000:65:00.3");
}

TEST(PciBdfSetFunctionTest, FunctionZero) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("0000:65:00.7", 0, out), 0);
    EXPECT_STREQ(out, "0000:65:00.0");
}

TEST(PciBdfSetFunctionTest, FunctionSeven) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("0000:65:00.0", 7, out), 0);
    EXPECT_STREQ(out, "0000:65:00.7");
}

TEST(PciBdfSetFunctionTest, FunctionTooLarge) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("0000:65:00.0", 8, out), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfSetFunctionTest, NullBdf) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function(nullptr, 0, out), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfSetFunctionTest, NullOutput) {
    EXPECT_EQ(pci_bdf_set_function("0000:65:00.0", 0, nullptr), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(PciBdfSetFunctionTest, EmptyString) {
    char out[VRTD_PCI_BDF_LEN];
    EXPECT_EQ(pci_bdf_set_function("", 0, out), -1);
    EXPECT_EQ(errno, EINVAL);
}

// --- hotplug_errno_to_vrtd_ret ---

TEST(HotplugErrnoTest, Einval) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(EINVAL), VRTD_RET_INVALID_ARGUMENT);
}

TEST(HotplugErrnoTest, Enodev) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(ENODEV), VRTD_RET_NOEXIST);
}

TEST(HotplugErrnoTest, Ebusy) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(EBUSY), VRTD_RET_BUSY);
}

TEST(HotplugErrnoTest, Eperm) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(EPERM), VRTD_RET_AUTH_ERROR);
}

TEST(HotplugErrnoTest, Eacces) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(EACCES), VRTD_RET_AUTH_ERROR);
}

TEST(HotplugErrnoTest, UnknownErrno) {
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(ENOMEM), VRTD_RET_INTERNAL_ERROR);
    EXPECT_EQ(hotplug_errno_to_vrtd_ret(EIO), VRTD_RET_INTERNAL_ERROR);
}
