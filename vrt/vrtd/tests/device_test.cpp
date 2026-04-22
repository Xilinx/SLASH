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

/**
 * Tests for device.c.
 *
 * device_open() and devices_contains_path() are static, so they are not
 * directly callable from tests.  The testable public surface is:
 *
 *   cleanup_device()           — exercised by constructing struct device
 *                                instances manually with mock handles.
 *   devices_discover_and_open() — exercised only when /dev/slash_ctl* nodes
 *                                 are present; skipped otherwise.
 *
 * All cleanup tests use mock ctldev/qdma handles and are hardware-independent.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>

extern "C" {
#include <slash/ctldev.h>
#include <slash/qdma.h>
#include <fcntl.h>
#include "allocator.h"
#include "buffer.h"
#include "design_writer.h"
#include "device.h"
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

/** Allocate a zeroed struct device and set a mock path string. */
static struct device *alloc_mock_device(void)
{
    struct device *d = static_cast<struct device *>(calloc(1, sizeof(*d)));
    if (d == nullptr)
        return nullptr;
    d->path = strdup("@mock");
    d->buffers = buffer_ptr_array_init();
    return d;
}

// ─── cleanup_device() tests (always run, mock handles only) ──────────────────

TEST(DeviceCleanupTest, CleanupNull) {
    /* Must be a silent no-op. */
    cleanup_device(nullptr);
}

TEST(DeviceCleanupTest, CleanupMinimal) {
    /* A calloc'd device with only a path set — all subsystem pointers are NULL. */
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);
    /* Ownership of d passes to cleanup_device (it calls free). */
    cleanup_device(d);
}

TEST(DeviceCleanupTest, CleanupWithCtl) {
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);

    d->ctl = slash_ctldev_open("@mock");
    ASSERT_NE(d->ctl, nullptr);

    cleanup_device(d);
}

TEST(DeviceCleanupTest, CleanupWithCtlAndQdma) {
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);

    d->ctl  = slash_ctldev_open("@mock");
    ASSERT_NE(d->ctl, nullptr);
    d->qdma = slash_qdma_open("@mock");
    ASSERT_NE(d->qdma, nullptr);

    cleanup_device(d);
}

TEST(DeviceCleanupTest, CleanupWithDesignWriter) {
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);

    d->ctl  = slash_ctldev_open("@mock");
    ASSERT_NE(d->ctl, nullptr);
    d->qdma = slash_qdma_open("@mock");
    ASSERT_NE(d->qdma, nullptr);
    d->design_writer = design_writer_create(d->qdma);
    ASSERT_NE(d->design_writer, nullptr);

    cleanup_device(d);
}

TEST(DeviceCleanupTest, CleanupWithBars) {
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);

    d->ctl  = slash_ctldev_open("@mock");
    ASSERT_NE(d->ctl, nullptr);
    d->qdma = slash_qdma_open("@mock");
    ASSERT_NE(d->qdma, nullptr);

    /* Mock ctldev provides a usable BAR 0 (64 MB, backed by a temp file). */
    d->bar_info[0] = slash_bar_info_read(d->ctl, 0);
    ASSERT_NE(d->bar_info[0], nullptr);
    EXPECT_TRUE(d->bar_info[0]->usable);

    d->bar_files[0] = slash_bar_file_open(d->ctl, 0, O_CLOEXEC);
    ASSERT_NE(d->bar_files[0], nullptr);

    /* BARs 1-5 are not usable in the mock — populate bar_info only (no bar_file). */
    for (int i = 1; i < 6; ++i) {
        d->bar_info[i] = slash_bar_info_read(d->ctl, i);
        ASSERT_NE(d->bar_info[i], nullptr);
        EXPECT_FALSE(d->bar_info[i]->usable);
    }

    cleanup_device(d);
}

TEST(DeviceCleanupTest, CleanupWithBuffers) {
    struct device *d = alloc_mock_device();
    ASSERT_NE(d, nullptr);

    d->ctl  = slash_ctldev_open("@mock");
    ASSERT_NE(d->ctl, nullptr);
    d->qdma = slash_qdma_open("@mock");
    ASSERT_NE(d->qdma, nullptr);

    /* Allocate a raw buffer on the mock QDMA and hand ownership to d->buffers. */
    struct buffer *buf = buffer_create_raw(d->qdma, DDR_START_ADDRESS, 4096,
                                           VRTD_ALLOC_DIR_HOST_TO_DEVICE);
    ASSERT_NE(buf, nullptr);

    int ret = buffer_ptr_array_push_move(&d->buffers, &buf);
    ASSERT_EQ(ret, 0);
    EXPECT_EQ(buf, nullptr);     /* ownership transferred */
    EXPECT_EQ(d->buffers.len, 1u);

    cleanup_device(d);
}

// ─── devices_discover_and_open() — real hardware only ────────────────────────

TEST(DeviceDiscoveryTest, DiscoverAndOpen) {
    struct device_ptr_array devices = device_ptr_array_init();
    int ret = devices_discover_and_open(&devices);

    if (devices.len == 0) {
        device_ptr_array_free(&devices);
        GTEST_SKIP() << "No /dev/slash_ctl* devices found — skipping hardware test";
    }

    EXPECT_EQ(ret, 0);
    EXPECT_GT(devices.len, 0u);

    /* Each discovered device must have at least a control handle. */
    for (size_t i = 0; i < devices.len; ++i) {
        EXPECT_NE(devices.d[i], nullptr);
        EXPECT_NE(devices.d[i]->ctl, nullptr);
    }

    device_ptr_array_free(&devices);
}
