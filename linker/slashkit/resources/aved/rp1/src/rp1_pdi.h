/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 partial PDI reconfiguration helper.
 *
 * Addresses and masks come from rp1_platform_config.h, generated from the
 * AVED R5_1 BSP's IPI ownership metadata. All accesses use the injectable
 * RP1 HAL so unit tests can verify the exact transaction sequence.
 */

#ifndef RP1_PDI_H
#define RP1_PDI_H

#include <stdint.h>

typedef enum {
    RP1_PDI_RESULT_OK = 0,
    RP1_PDI_RESULT_TIMEOUT = 1,
    RP1_PDI_RESULT_PLM_ERROR = 2,
} rp1_pdi_outcome_t;

/*
 * outcome separates transport timeout from a completed PLM rejection.
 * status/detail remain unsigned and unmodified for CQ and terminal evidence.
 */
typedef struct {
    rp1_pdi_outcome_t outcome;
    uint32_t status;
    uint32_t detail;
} rp1_pdi_result_t;

/**
 * rp1_pdi_load() — trigger a partial PDI reload from DDR via the PMC.
 *
 * @param addr_lo         Low 32 bits of the partial PDI's DDR physical address.
 * @param addr_hi         High 32 bits of the partial PDI's DDR physical address.
 * @param timeout_cycles  PMU-tick deadline for the IPI observation register.
 *                        0 selects the frequency-derived protocol default.
 *
 * @return Structured outcome preserving both unsigned PLM response words.
 */
rp1_pdi_result_t rp1_pdi_load(uint32_t addr_lo, uint32_t addr_hi,
                              uint32_t timeout_cycles);

#endif /* RP1_PDI_H */
