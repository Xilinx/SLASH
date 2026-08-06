/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 firmware entry point (non-semihosting / real hardware).
 */

#include "rp1_run.h"

void rp1_main(void)
{
    rp1_run();
}
