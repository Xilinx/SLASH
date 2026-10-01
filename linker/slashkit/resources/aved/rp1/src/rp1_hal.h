/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#ifndef RP1_HAL_H
#define RP1_HAL_H

#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

#include "rp1_platform_config.h"

#ifdef QEMU_SEMIHOSTING

/** Barrier instruction selected by a firmware ordering or completion point. */
typedef enum {
    /** Complete prior stores before later instructions execute. */
    RP1_BARRIER_DSB_ST,
    /** Complete all prior explicit accesses before later instructions execute. */
    RP1_BARRIER_DSB_SY,
    /** Order prior stores before later stores. */
    RP1_BARRIER_DMB_ST,
    /** Order all prior explicit accesses before later explicit accesses. */
    RP1_BARRIER_DMB_SY,
} rp1_barrier_kind_t;

typedef struct {
    uint32_t (*read32)(uintptr_t address, void *context);
    void (*write32)(uintptr_t address, uint32_t value, void *context);
    void (*barrier)(rp1_barrier_kind_t kind, void *context);
    uint32_t (*cycles)(void *context);
    void *context;
} rp1_hal_hooks_t;

void rp1_hal_set_hooks(const rp1_hal_hooks_t *hooks);
void rp1_hal_reset_hooks(void);

uint32_t rp1_mmio_read32(uintptr_t address);
void rp1_mmio_write32(uintptr_t address, uint32_t value);
void rp1_dsb_st(void);
void rp1_dsb_sy(void);
void rp1_dmb_st(void);
void rp1_dmb_sy(void);
uint32_t rp1_cycles(void);

#else

/** Read one 32-bit device register. */
static inline uint32_t rp1_mmio_read32(uintptr_t address)
{
    return *(volatile uint32_t *)address;
}

/** Write one 32-bit device register. */
static inline void rp1_mmio_write32(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

/** Complete prior stores before executing later instructions. */
static inline void rp1_dsb_st(void)
{
    __asm__ volatile("dsb st" ::: "memory");
}

/** Complete all prior explicit accesses before executing later instructions. */
static inline void rp1_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/** Order prior stores before later stores without requiring completion. */
static inline void rp1_dmb_st(void)
{
    __asm__ volatile("dmb st" ::: "memory");
}

/** Order explicit accesses across the barrier without requiring completion. */
static inline void rp1_dmb_sy(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

/** Read the hardware PMU cycle counter. */
static inline uint32_t rp1_cycles(void)
{
    uint32_t cycles;

    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cycles));
    return cycles;
}

#endif /* QEMU_SEMIHOSTING */

void rp1_pmu_init(void);

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
