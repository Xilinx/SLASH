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

#ifndef VRTD_RESET_H
#define VRTD_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include <vrtd/wire.h>

#include "flash.h"

struct device;
struct device_ptr_array;

int shell_boot_partition(enum vrtd_shell_type shell, uint32_t *partition_out);
bool shell_reset_required(enum vrtd_shell_type current_shell, enum vrtd_shell_type required_shell);
bool shell_switch_blocked_by_jtag(
    enum vrtd_shell_type current_shell,
    enum vrtd_shell_type required_shell,
    bool jtag
);

/**
 * @brief Outcome of one post-reset readiness poll.
 */
enum reset_ready_state {
    /** @brief The device is back, fully opened, and running the expected shell. */
    RESET_READY_OK,
    /** @brief Not there yet. The condition may still become true with more time. */
    RESET_READY_WAIT,
    /** @brief The build-ID register answered with a shell other than the one the
     *  boot partition selected. More time cannot change this. */
    RESET_READY_SHELL_MISMATCH,
};

/**
 * @brief Decide what one post-reset readiness sample means.
 *
 * Separates the two reasons a build-ID read can fail to confirm the shell.
 * build_id_read_shell() answers @c VRTD_SHELL_UNKNOWN both when the BAR is
 * absent or unmapped and when the register reads back all-ones -- in either
 * case the device is still coming up and the caller should keep waiting.  A
 * register that answers with a *different* known shell is a different thing
 * entirely: the boot partition did not take effect, no amount of waiting will
 * change it, and the caller must fail immediately rather than spend its whole
 * deadline re-reading a register that is already telling the truth.
 *
 * @param device_present    Whether re-discovery has produced the target device.
 * @param fully_initialized Whether that device has all its resources open
 *                          (see device_is_fully_initialized()).
 * @param reported_shell    Shell read from the build-ID register, or
 *                          @c VRTD_SHELL_UNKNOWN when it could not be read.
 * @param booted_shell      Shell the selected boot partition should have loaded.
 * @return The state this sample implies.
 */
enum reset_ready_state reset_ready_classify(
    bool device_present,
    bool fully_initialized,
    enum vrtd_shell_type reported_shell,
    enum vrtd_shell_type booted_shell
);
uint16_t reset_with_ami(
    struct device *device,
    struct device_ptr_array *devices,
    enum vrtd_shell_type target_shell
);
uint16_t reset_with_ami_partition(
    struct device *device,
    struct device_ptr_array *devices,
    uint32_t partition
);
uint16_t reset_with_ami_partition_progress(
    struct device *device,
    struct device_ptr_array *devices,
    uint32_t partition,
    cfgmem_progress_callback progress_cb,
    void *progress_ctx
);

#endif /* VRTD_RESET_H */
