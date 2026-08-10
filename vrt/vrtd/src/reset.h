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
