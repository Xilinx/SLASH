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

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

extern "C" {
#include "device.h"
#include "flash_worker.h"
}

TEST(FlashWorkerTest, UnknownJobReturnsEnoent) {
    struct flash_worker *worker = flash_worker_create();
    ASSERT_NE(worker, nullptr);

    struct vrtd_cfgmem_program_status status {};
    errno = 0;
    EXPECT_EQ(flash_worker_poll_status(worker, 1, &status), -1);
    EXPECT_EQ(errno, ENOENT);

    cleanup_flash_worker(worker);
}

TEST(FlashWorkerTest, WrongOwnerStatusReturnsEacces) {
    struct flash_worker *worker = flash_worker_create();
    ASSERT_NE(worker, nullptr);

    struct device device {};
    struct device_ptr_array devices = device_ptr_array_init();
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);

    uint64_t job_id = 0;
    ASSERT_EQ(flash_worker_submit_async(
        worker,
        &device,
        &devices,
        fd,
        0,
        0,
        111,
        &job_id
    ), 0);
    fd = -1; // Worker owns the fd after successful submit.

    struct vrtd_cfgmem_program_status status {};
    errno = 0;
    EXPECT_EQ(flash_worker_poll_status_for_owner(worker, job_id, 222, &status), -1);
    EXPECT_EQ(errno, EACCES);

    bool terminal = false;
    for (int i = 0; i < 1000; i++) {
        errno = 0;
        ASSERT_EQ(flash_worker_poll_status_for_owner(worker, job_id, 111, &status), 0);
        if (status.state == VRTD_CFGMEM_PROGRAM_STATE_DONE ||
            status.state == VRTD_CFGMEM_PROGRAM_STATE_FAILED) {
            terminal = true;
            break;
        }
        usleep(1000);
    }
    EXPECT_TRUE(terminal);

    cleanup_flash_worker(worker);
    device_ptr_array_free(&devices);
}

TEST(FlashWorkerTest, ResetJobCompletesWithoutInputFd) {
    struct flash_worker *worker = flash_worker_create();
    ASSERT_NE(worker, nullptr);

    struct device device {};
    struct device_ptr_array devices = device_ptr_array_init();

    uint64_t job_id = 0;
    ASSERT_EQ(flash_worker_submit_reset_async(
        worker,
        &device,
        &devices,
        0,
        111,
        &job_id
    ), 0);
    EXPECT_NE(job_id, 0u);

    bool done = false;
    uint16_t result = VRTD_RET_OK;
    for (int i = 0; i < 1000; i++) {
        ASSERT_EQ(flash_worker_poll_result(worker, &done, &result), 0);
        if (done) {
            break;
        }
        usleep(1000);
    }

    EXPECT_TRUE(done);
    EXPECT_EQ(result, VRTD_RET_INTERNAL_ERROR);

    cleanup_flash_worker(worker);
    device_ptr_array_free(&devices);
}
