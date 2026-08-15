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
uint32_t      g_graph_start_cycles         BTCM_SECTION;
uint32_t      g_trace_size                 BTCM_SECTION;
uint32_t      g_trace_enable               BTCM_SECTION;

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
rp1_trace_entry_t *g_trace   = NULL;

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

static uint32_t is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static uint32_t valid_window_range(uint32_t lo, uint32_t hi,
                                   uint32_t size, uint32_t alignment)
{
    if (hi != 0u || lo < RP1_CTRL_PHYS_ADDR || size > RP1_CTRL_WINDOW_SIZE)
        return 0u;
    if (alignment != 0u && (lo & (alignment - 1u)) != 0u)
        return 0u;
    return lo - RP1_CTRL_PHYS_ADDR <= RP1_CTRL_WINDOW_SIZE - size;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * Configuration validation has three classes: bounded counts/ring shape,
 * monotonic CQ occupancy, and aligned ranges wholly inside the shared window.
 * No host-provided address becomes a pointer until every class has passed.
 */
int rp1_store_init(uint32_t *detail, uint32_t *aux)
{
    uint32_t node_count = g_ctrl->node_count;
    uint32_t cq_size = g_ctrl->cq_size;

    *detail = 0u;
    *aux = 0u;
    if (node_count == 0u || node_count > RP1_MAX_NODES) {
        *detail = RP1_CONFIG_NODE_COUNT;
        *aux = node_count;
        return -1;
    }
    if (!valid_window_range(g_ctrl->node_base_lo, g_ctrl->node_base_hi,
                            node_count * (uint32_t)sizeof(rp1_node_t), 64u)) {
        *detail = RP1_CONFIG_NODE_BASE;
        *aux = g_ctrl->node_base_lo;
        return -1;
    }
    if (!is_power_of_two(cq_size) || cq_size > RP1_MAX_CQ_ENTRIES) {
        *detail = RP1_CONFIG_CQ_SIZE;
        *aux = cq_size;
        return -1;
    }
    if (!valid_window_range(g_ctrl->cq_base_lo, g_ctrl->cq_base_hi,
                            cq_size * (uint32_t)sizeof(rp1_cq_entry_t), 16u)) {
        *detail = RP1_CONFIG_CQ_BASE;
        *aux = g_ctrl->cq_base_lo;
        return -1;
    }
    if ((uint32_t)(g_ctrl->cq_write_idx - g_ctrl->cq_read_idx) > cq_size) {
        *detail = RP1_CONFIG_CQ_CURSORS;
        *aux = g_ctrl->cq_write_idx - g_ctrl->cq_read_idx;
        return -1;
    }
    if (!valid_window_range(g_ctrl->arg_buf_base_lo,
                            g_ctrl->arg_buf_base_hi, 4u, 4u)) {
        *detail = RP1_CONFIG_ARG_BASE;
        *aux = g_ctrl->arg_buf_base_lo;
        return -1;
    }
    if (!valid_window_range(g_ctrl->sig_array_base_lo,
                            g_ctrl->sig_array_base_hi,
                            RP1_MAX_SIGNALS *
                                (uint32_t)sizeof(rp1_signal_slot_t),
                            16u)) {
        *detail = RP1_CONFIG_SIGNAL_BASE;
        *aux = g_ctrl->sig_array_base_lo;
        return -1;
    }
    if (g_ctrl->trace_enable != 0u) {
        if (!is_power_of_two(g_ctrl->trace_size) ||
            g_ctrl->trace_size > RP1_MAX_TRACE_ENTRIES ||
            !valid_window_range(
                g_ctrl->trace_base_lo, g_ctrl->trace_base_hi,
                g_ctrl->trace_size * (uint32_t)sizeof(rp1_trace_entry_t),
                16u)) {
            *detail = RP1_CONFIG_TRACE;
            *aux = g_ctrl->trace_size;
            return -1;
        }
    }

    /*
     * Phase 2: all ranges are now proven 32-bit, aligned, and in-window, so
     * resolving them cannot expose the scanner to a partially valid store.
     */
    g_nodes   = (rp1_node_t *)
                    (uintptr_t)make64(g_ctrl->node_base_lo, g_ctrl->node_base_hi);
    g_cq      = (rp1_cq_entry_t *)
                    (uintptr_t)make64(g_ctrl->cq_base_lo, g_ctrl->cq_base_hi);
    g_signals = (rp1_signal_slot_t *)
                    (uintptr_t)make64(g_ctrl->sig_array_base_lo, g_ctrl->sig_array_base_hi);
    g_arg_buf = (uint32_t *)
                    (uintptr_t)make64(g_ctrl->arg_buf_base_lo, g_ctrl->arg_buf_base_hi);
    g_trace   = (rp1_trace_entry_t *)
                    (uintptr_t)make64(g_ctrl->trace_base_lo, g_ctrl->trace_base_hi);
    g_trace_size = g_ctrl->trace_size;
    g_trace_enable = g_ctrl->trace_enable;
    g_ctrl->trace_write_idx = 0;

    /*
     * Phase 3: reset only per-graph state and status. The active image id is
     * physical reconfiguration state and deliberately survives submissions.
     */
    rp1_store_reset_graph();
    for (uint32_t i = 0; i < node_count; i++)
        g_nodes[i].status = RP1_NODE_PENDING;
    return 0;
}

void rp1_store_reset_graph(void)
{
    memzero(g_barriers,    sizeof(g_barriers));
    memzero(g_node_status, sizeof(g_node_status));
    memzero(g_loop_iters,  sizeof(g_loop_iters));
    memzero(g_inflight,    sizeof(g_inflight));
    g_inflight_count = 0;
}

void rp1_clear_error_latch(void)
{
    g_ctrl->rp1_error_code = 0u;
    g_ctrl->terminal_error_node = RP1_TERMINAL_ERROR_NODE_NONE;
    g_ctrl->terminal_error_detail = 0u;
    g_ctrl->terminal_error_aux = 0u;
}

/*
 * The first base error owns node/detail/aux for the whole graph. Later
 * quiescence may only OR recovery-required, preserving the original cause
 * while telling the host that reset is mandatory.
 */
void rp1_latch_error(uint32_t code, uint32_t node,
                     uint32_t detail, uint32_t aux)
{
    if ((g_ctrl->rp1_error_code & RP1_ERR_CODE_MASK) != 0u)
        return;
    g_ctrl->rp1_error_code =
        (g_ctrl->rp1_error_code & RP1_ERR_RECOVERY_REQUIRED) | code;
    g_ctrl->terminal_error_node = node;
    g_ctrl->terminal_error_detail = detail;
    g_ctrl->terminal_error_aux = aux;
}

void rp1_mark_recovery_required(void)
{
    g_ctrl->rp1_error_code |= RP1_ERR_RECOVERY_REQUIRED;
}
