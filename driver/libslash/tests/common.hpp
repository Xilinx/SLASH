/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <gtest/gtest.h>

enum class LibSlashBackend {
    // Only relevant for hotplug, where no path equates to the driver.
    IMPLICIT_DRIVER,
    DRIVER,
    SYSEMU,
    MOCK,
};

static constexpr const char *SLASH_DRIVER_HOTPLUG_PATH = "/dev/slash_hotplug";
static constexpr const char *SLASH_SYSEMU_HOTPLUG_PATH = "/run/slash_sysemu/slash_hotplug";
static constexpr const char *SLASH_DRIVER_CTLDEV_PATH = "/dev/slash_ctl0";
static constexpr const char *SLASH_SYSEMU_CTLDEV_PATH =
    "/run/slash_sysemu/slash_ctl0";
static constexpr const char *SLASH_DRIVER_QDMA_PATH = "/dev/slash_qdma_ctl0";
static constexpr const char *SLASH_SYSEMU_QDMA_PATH =
    "/run/slash_sysemu/slash_qdma_ctl0";
 
static constexpr uint64_t DDR_BASE_ADDRESS = 0x60000000000ULL;
static constexpr uint64_t HBM_BASE_ADDRESS = 0x4000000000ULL;