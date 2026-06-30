/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 outer loop — graph submission polling and lifecycle.
 */

#ifndef RP1_RUN_H
#define RP1_RUN_H

#include "rp1_loop.h"

#ifdef QEMU_SEMIHOSTING
int rp1_run(const rp1_hooks_t *hooks);
#else
int rp1_run(void);
#endif

#endif /* RP1_RUN_H */
