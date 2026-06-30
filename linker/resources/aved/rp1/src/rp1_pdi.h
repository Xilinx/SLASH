/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 partial PDI reconfiguration helper.
 *
 * Wraps the canonical Versal "RPU asks PMC to load a PDI from DDR" IPI
 * sequence: write a 4-word command block into the PMC scratch area at
 * 0xFF3F0A40, then poke IPI channel 3 (0xFF360000) and poll its
 * observation register (0xFF360004) until the PMC has acknowledged the
 * request.  All registers are on the LPD and reachable from the R5 via
 * M_AXI_LPD; no fabric path is required.
 *
 * The symbol is declared with weak linkage so the QEMU semihosting test
 * harness can supply a recorder that validates the arguments without
 * actually issuing the IPI.
 */

#ifndef RP1_PDI_H
#define RP1_PDI_H

#include <stdint.h>

/**
 * rp1_pdi_load() — trigger a partial PDI reload from DDR via the PMC.
 *
 * @param addr_lo         Low 32 bits of the partial PDI's DDR physical address.
 * @param addr_hi         High 32 bits of the partial PDI's DDR physical address.
 * @param timeout_cycles  Poll budget for the IPI observation register.
 *                        0 selects the default (10,000,000 iterations).
 *
 * @return 0 on success (PMC ACKed the IPI), 1 on timeout.
 */
int rp1_pdi_load(uint32_t addr_lo, uint32_t addr_hi, uint32_t timeout_cycles);

#endif /* RP1_PDI_H */
