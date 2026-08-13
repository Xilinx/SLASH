/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef RP1_HAL_H
#define RP1_HAL_H

#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

#include "rp1_platform_config.h"

typedef struct {
    uint32_t (*read32)(uintptr_t address, void *context);
    void (*write32)(uintptr_t address, uint32_t value, void *context);
    void (*barrier)(void *context);
    uint32_t (*cycles)(void *context);
    void *context;
} rp1_hal_hooks_t;

void rp1_hal_set_hooks(const rp1_hal_hooks_t *hooks);
void rp1_hal_reset_hooks(void);

uint32_t rp1_mmio_read32(uintptr_t address);
void rp1_mmio_write32(uintptr_t address, uint32_t value);
void rp1_barrier(void);
void rp1_pmu_init(void);
uint32_t rp1_cycles(void);

/*
 * Unsigned elapsed subtraction is wrap-safe for protocol deadlines shorter
 * than one PMU counter period. Capturing start once makes a true deadline,
 * rather than a retry budget whose duration changes with scanner work.
 */
static inline uint32_t rp1_timeout_elapsed(uint32_t start,
                                           uint32_t timeout,
                                           uint32_t now)
{
    return (uint32_t)(now - start) >= timeout;
}

/*
 * PMCCNTR advances once per divisor CPU cycles. Round milliseconds upward so
 * frequency conversion can never expire a kernel or PDI request early.
 */
#define RP1_PMU_TICKS_PER_SECOND \
    ((RP1_R5_FREQ_HZ + RP1_PMU_CYCLE_DIVISOR - 1u) / RP1_PMU_CYCLE_DIVISOR)
#define RP1_TIMEOUT_TICKS(milliseconds)                                   \
    ((uint32_t)(((uint64_t)RP1_PMU_TICKS_PER_SECOND * (milliseconds) +   \
                 999u) / 1000u))
#define RP1_DEFAULT_KERNEL_TIMEOUT_TICKS \
    RP1_TIMEOUT_TICKS(RP1_DEFAULT_KERNEL_TIMEOUT_MS)
#define RP1_DEFAULT_PDI_TIMEOUT_TICKS \
    RP1_TIMEOUT_TICKS(RP1_DEFAULT_PDI_TIMEOUT_MS)

#endif /* RP1_HAL_H */
