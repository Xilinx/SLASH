/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 static storage definitions and initialisation.
 */

#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stddef.h>

/* All compile-time size/offset assertions for the RP1 protocol live next
 * to the type definitions in <slash/uapi/rp1_protocol.h>. */

/* -------------------------------------------------------------------------
 * BTCM-resident hot stores
 *
 * The .btcm attribute is used so the linker script can place these in the
 * BTCM region.  Under QEMU they land in BSS (zeroed by the boot stub).
 * ---------------------------------------------------------------------- */

#define BTCM_SECTION __attribute__((section(".btcm")))

uint32_t      g_barriers[RP1_MAX_BUCKETS]  BTCM_SECTION;
uint8_t       g_node_status[RP1_MAX_NODES] BTCM_SECTION;
uint32_t      g_loop_iters[RP1_MAX_LOOPS]  BTCM_SECTION;
rp1_inflight_t g_inflight[RP1_MAX_INFLIGHT] BTCM_SECTION;
uint32_t      g_inflight_count             BTCM_SECTION;

/* Persists across graphs (physical reconfig state); zeroed only at boot. */
uint32_t      g_active_image_id            BTCM_SECTION;

/* -------------------------------------------------------------------------
 * DDR-backed pointer table (set by rp1_store_init)
 * ---------------------------------------------------------------------- */

rp1_ctrl_t       *g_ctrl    = (rp1_ctrl_t *)RP1_CTRL_PHYS_ADDR;
rp1_node_t       *g_nodes   = NULL;
rp1_cq_entry_t   *g_cq      = NULL;
rp1_signal_slot_t *g_signals = NULL;
uint32_t         *g_arg_buf  = NULL;

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void memzero(void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    while (len--)
        *p++ = 0;
}

static uint64_t make64(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void rp1_store_init(void)
{
    /* Resolve DDR pointers from control block fields. */
    g_nodes   = (rp1_node_t *)
                    (uintptr_t)make64(g_ctrl->node_base_lo, g_ctrl->node_base_hi);
    g_cq      = (rp1_cq_entry_t *)
                    (uintptr_t)make64(g_ctrl->cq_base_lo, g_ctrl->cq_base_hi);
    g_signals = (rp1_signal_slot_t *)
                    (uintptr_t)make64(g_ctrl->sig_array_base_lo, g_ctrl->sig_array_base_hi);
    g_arg_buf = (uint32_t *)
                    (uintptr_t)make64(g_ctrl->arg_buf_base_lo, g_ctrl->arg_buf_base_hi);

    rp1_store_reset_graph();
}

void rp1_store_reset_graph(void)
{
    memzero(g_barriers,    sizeof(g_barriers));
    memzero(g_node_status, sizeof(g_node_status));
    memzero(g_loop_iters,  sizeof(g_loop_iters));
    memzero(g_inflight,    sizeof(g_inflight));
    g_inflight_count = 0;
}
