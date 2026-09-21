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

/**
 * @file shell_build_id.c
 * @brief Implementation of the shell build-ID shell-variant check.
 */

#include "shell_build_id.h"

#include <errno.h>
#include <stddef.h>
#include <syslog.h>

#include <systemd/sd-journal.h>

#include "utils.h"

/*
 * An MMIO read that does not reach the device completes with every bit set.
 * The build-ID high word never holds this value: its reserved bits are always
 * clear.
 */
#define BUILD_ID_REG_UNRESPONSIVE 0xFFFFFFFFu

const char *build_id_shell_name(enum vrtd_shell_type shell)
{
    switch (shell) {
    case VRTD_SHELL_SERVICE:
        return "service";
    case VRTD_SHELL_COMPUTE:
        return "compute";
    default:
        return "unknown";
    }
}

enum vrtd_shell_type build_id_decode_shell(uint32_t hi)
{
    if (hi == BUILD_ID_REG_UNRESPONSIVE || (hi & BUILD_ID_HI_RESERVED_MASK) != 0u) {
        return VRTD_SHELL_UNKNOWN;
    }

    return (hi & BUILD_ID_HI_SHELL_MASK) != 0u ? VRTD_SHELL_COMPUTE : VRTD_SHELL_SERVICE;
}

enum vrtd_shell_type build_id_read_shell(const struct slash_bar_file *bar)
{
    if (bar == NULL || bar->map == NULL || bar->len < BUILD_ID_REG_HI + sizeof(uint32_t)) {
        return VRTD_SHELL_UNKNOWN;
    }

    const volatile uint32_t *regs = (const volatile uint32_t *) bar->map;
    return build_id_decode_shell(regs[BUILD_ID_REG_HI / sizeof(uint32_t)]);
}

int build_id_check_shell(
    const struct slash_bar_file *bar,
    enum vrtd_shell_type expected,
    const char *context
)
{
    if (expected == VRTD_SHELL_UNKNOWN) {
        return 0;
    }

    enum vrtd_shell_type reported = build_id_read_shell(bar);
    if (reported == VRTD_SHELL_UNKNOWN) {
        LOG(
            LOG_ERR,
            "%s: shell build-ID register at BAR%d+0x%x did not respond; cannot confirm "
            "the %s shell is loaded",
            context,
            BUILD_ID_BAR_NUMBER,
            BUILD_ID_REG_HI,
            build_id_shell_name(expected)
        );
        errno = EIO;
        return -1;
    }

    if (reported != expected) {
        LOG(
            LOG_ERR,
            "%s: hardware reports the %s shell but vrtd expected the %s shell",
            context,
            build_id_shell_name(reported),
            build_id_shell_name(expected)
        );
        errno = EIO;
        return -1;
    }

    return 0;
}
