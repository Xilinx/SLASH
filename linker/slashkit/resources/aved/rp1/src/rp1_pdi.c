/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 partial PDI reconfiguration helper (hardware implementation).
 *
 * The generated platform header supplies the R5_1-owned source agent,
 * request/response buffers, trigger/observation registers, and PMC target
 * mask. The function blocks until the observation bit clears or a true PMU
 * deadline expires.
 */

#include "rp1_pdi.h"
#include "rp1_hal.h"

#include <stdint.h>

#define RP1_PDI_CMD_LOAD    0x00030701u
#define RP1_PDI_SRC_DDR     0x0000000Fu

/*
 * The transaction has three outcomes: observation never clears, PLM returns
 * success, or PLM returns a structured error. Request data must be visible
 * before the trigger, and response words are valid only after acknowledgement.
 */
rp1_pdi_result_t rp1_pdi_load(uint32_t addr_lo, uint32_t addr_hi,
                              uint32_t timeout_cycles)
{
    rp1_pdi_result_t result = {
        .outcome = RP1_PDI_RESULT_TIMEOUT,
        .status = 0,
        .detail = 0,
    };

    /*
     * Phase 1: populate the source/target message-RAM request and order all
     * four words before ringing the generated source-agent trigger register.
     */
    rp1_mmio_write32(RP1_PDI_IPI_REQUEST_BASE + 0u, RP1_PDI_CMD_LOAD);
    rp1_mmio_write32(RP1_PDI_IPI_REQUEST_BASE + 4u, RP1_PDI_SRC_DDR);
    rp1_mmio_write32(RP1_PDI_IPI_REQUEST_BASE + 8u, addr_hi);
    rp1_mmio_write32(RP1_PDI_IPI_REQUEST_BASE + 12u, addr_lo);
    rp1_barrier();

    rp1_mmio_write32(RP1_PDI_IPI_TRIGGER_REG, RP1_PDI_IPI_TARGET_MASK);
    rp1_barrier();

    /*
     * Phase 2: the target observation bit owns the request until it clears.
     * Measure a PMU deadline so polling speed cannot redefine the timeout.
     */
    uint32_t timeout = timeout_cycles ? timeout_cycles
                                      : RP1_DEFAULT_PDI_TIMEOUT_TICKS;
    uint32_t start = rp1_cycles();
    while ((rp1_mmio_read32(RP1_PDI_IPI_OBSERVATION_REG) &
            RP1_PDI_IPI_TARGET_MASK) != 0u) {
        if (rp1_timeout_elapsed(start, timeout, rp1_cycles()))
            return result;
    }
    rp1_barrier();

    /*
     * Phase 3: acknowledgement releases both unsigned response words. Preserve
     * them verbatim so CQ and terminal diagnostics retain PLM-specific detail.
     */
    result.status = rp1_mmio_read32(RP1_PDI_IPI_RESPONSE_BASE + 0u);
    result.detail = rp1_mmio_read32(RP1_PDI_IPI_RESPONSE_BASE + 4u);
    result.outcome = result.status == 0u ? RP1_PDI_RESULT_OK
                                        : RP1_PDI_RESULT_PLM_ERROR;
    return result;
}

_Static_assert((RP1_PDI_IPI_REQUEST_BASE & 31u) == 0u,
               "PDI request buffer must be 32-byte aligned");
_Static_assert(RP1_PDI_IPI_RESPONSE_BASE == RP1_PDI_IPI_REQUEST_BASE + 32u,
               "PDI response must follow request buffer");
_Static_assert(RP1_PDI_IPI_TARGET_MASK != 0u,
               "PMC target mask must be non-zero");
