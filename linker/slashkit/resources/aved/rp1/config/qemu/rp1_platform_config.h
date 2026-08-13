/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Explicit unit/QEMU fixture. Hardware builds are forbidden from using this
 * file and must consume a header generated from their R5_1 BSP metadata.
 */

#ifndef RP1_PLATFORM_CONFIG_H
#define RP1_PLATFORM_CONFIG_H

#define RP1_PLATFORM_CONFIG_GENERATED 0u
#define RP1_PLATFORM_CONFIG_FIXTURE   1u

#define RP1_PLATFORM_ID               0x51454D55u /* "QEMU" */
#define RP1_R5_FREQ_HZ                100000000u

/* Deliberately models an R5-owned IPI3 source. This is test data, not an AVED
 * hardware claim. The generated hardware header may select a different IPI. */
#define RP1_PDI_IPI_SOURCE_BASE       0xFF360000u
#define RP1_PDI_IPI_SOURCE_MASK       0x00000020u
#define RP1_PDI_IPI_SOURCE_BUF_INDEX  5u
#define RP1_PDI_IPI_TARGET_MASK       0x00000002u
#define RP1_PDI_IPI_TARGET_BUF_INDEX  1u
#define RP1_PDI_IPI_REQUEST_BASE      0xFF3F0A40u
#define RP1_PDI_IPI_RESPONSE_BASE     0xFF3F0A60u
#define RP1_PDI_IPI_TRIGGER_REG       0xFF360000u
#define RP1_PDI_IPI_OBSERVATION_REG   0xFF360004u

#endif /* RP1_PLATFORM_CONFIG_H */
