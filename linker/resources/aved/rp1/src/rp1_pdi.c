/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 partial PDI reconfiguration helper (hardware implementation).
 *
 * Registers (Versal LPD, reachable from R5 via M_AXI_LPD):
 *
 *   PMC scratch area for the XLoader PDI-load command:
 *     0xFF3F0A40  command word           (PDI_LOAD = 0x30701)
 *     0xFF3F0A44  source flags           (DDR      = 0x0F)
 *     0xFF3F0A48  PDI physical addr, hi 32
 *     0xFF3F0A4C  PDI physical addr, lo 32
 *
 *   IPI channel 3 (the RPU -> PMC channel):
 *     0xFF360000  trigger  -- write 0x02 to fire the IPI to the PMC
 *     0xFF360004  observation -- non-zero while the IPI is pending,
 *                                clears once the PMC has consumed it
 *
 * The function blocks until the observation register reads back zero or
 * the caller-supplied timeout budget is exhausted.  It does NOT wait for
 * the PDI load itself to finish on the PMC side beyond that ACK -- the
 * graph creator is responsible for sequencing any work that depends on
 * the new fabric content.
 */

#include "rp1_pdi.h"

#include <stdint.h>

#define RP1_PDI_CMD_REG     0xFF3F0A40u
#define RP1_PDI_SRC_REG     0xFF3F0A44u
#define RP1_PDI_ADDR_HI_REG 0xFF3F0A48u
#define RP1_PDI_ADDR_LO_REG 0xFF3F0A4Cu

#define RP1_PDI_CMD_LOAD    0x00030701u
#define RP1_PDI_SRC_DDR     0x0000000Fu

#define RP1_PDI_IPI_TRIG    0xFF360000u
#define RP1_PDI_IPI_OBS     0xFF360004u
#define RP1_PDI_IPI_RPU     0x00000002u

#define RP1_PDI_DEFAULT_TIMEOUT 10000000u

static inline uint32_t axi_read32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void axi_write32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

__attribute__((weak))
int rp1_pdi_load(uint32_t addr_lo, uint32_t addr_hi, uint32_t timeout_cycles)
{
    axi_write32(RP1_PDI_CMD_REG,     RP1_PDI_CMD_LOAD);
    axi_write32(RP1_PDI_SRC_REG,     RP1_PDI_SRC_DDR);
    axi_write32(RP1_PDI_ADDR_HI_REG, addr_hi);
    axi_write32(RP1_PDI_ADDR_LO_REG, addr_lo);
    dsb();

    axi_write32(RP1_PDI_IPI_TRIG, RP1_PDI_IPI_RPU);

    uint32_t budget = (timeout_cycles == 0u) ? RP1_PDI_DEFAULT_TIMEOUT
                                             : timeout_cycles;
    while (axi_read32(RP1_PDI_IPI_OBS) != 0u) {
        if (--budget == 0u)
            return 1;
    }
    return 0;
}
