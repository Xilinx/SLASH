/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 static storage definitions and initialisation.
 */

#include "rp1_cycles.h"
#include "rp1_hal.h"
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
rp1_node_t    g_nodes[RP1_MAX_NODES]       BTCM_SECTION;
uint32_t      g_node_count                 BTCM_SECTION;
uint32_t      g_loop_iters[RP1_MAX_LOOPS]  BTCM_SECTION;
rp1_inflight_t g_inflight[RP1_MAX_INFLIGHT] BTCM_SECTION;
uint32_t      g_inflight_count             BTCM_SECTION;
uint32_t      g_graph_start_cycles         BTCM_SECTION;
uint32_t      g_trace_size                 BTCM_SECTION;
uint32_t      g_trace_enable               BTCM_SECTION;
uint32_t      g_completed_operations       BTCM_SECTION;
uint32_t      g_operation_started          BTCM_SECTION;
uint32_t      g_quiesce_finite_done         BTCM_SECTION;
uint32_t      g_quiesce_finite_timeout      BTCM_SECTION;
uint32_t      g_quiesce_infinite            BTCM_SECTION;
uint32_t      g_recovery_required           BTCM_SECTION;
uint32_t      g_terminal_opcode             BTCM_SECTION;
/* Fixed on-chip trace page; only explicit flushes touch the DDR trace ring. */
static rp1_trace_entry_t
    g_trace_staging[RP1_TRACE_STAGING_ENTRIES] BTCM_SECTION;
/* Number of valid entries at the front of g_trace_staging. */
static uint32_t g_trace_staging_count       BTCM_SECTION;

/* Persistent physical reconfiguration state; zeroed only at boot. */
uint32_t      g_active_image_id            BTCM_SECTION;
uint32_t      g_active_image_state         BTCM_SECTION;

_Static_assert(sizeof(g_trace_staging) == RP1_TRACE_STAGING_BYTES,
               "BTCM trace staging must occupy exactly 4 KB");
_Static_assert(sizeof(g_nodes) == 32u * RP1_MAX_NODES,
               "BTCM node snapshot must occupy exactly 32 KB");

/* -------------------------------------------------------------------------
 * DDR-backed pointer table (set by rp1_store_init)
 * ---------------------------------------------------------------------- */

rp1_ctrl_t       *g_ctrl    = (rp1_ctrl_t *)RP1_CTRL_PHYS_ADDR;
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

/*
 * GCC's may_alias word keeps this hot snapshot defined under -O2 strict
 * aliasing while preserving one aligned NoC transaction per 32-bit word.
 */
typedef uint32_t rp1_alias_u32_t __attribute__((may_alias));

/*
 * Copy each source word exactly once after the caller validates and orders the
 * host-owned range. The volatile source prevents later scanner reads from
 * folding into DDR; all execution subsequently uses the mutable BTCM copy.
 */
static void snapshot_nodes(uint32_t source_lo, uint32_t source_hi,
                           uint32_t node_count)
{
    const volatile rp1_alias_u32_t *source =
        (const volatile rp1_alias_u32_t *)
            (uintptr_t)make64(source_lo, source_hi);
    rp1_alias_u32_t *destination = (rp1_alias_u32_t *)(void *)g_nodes;
    uint32_t words =
        node_count * (uint32_t)sizeof(rp1_node_t) / sizeof(uint32_t);

    rp1_dmb_sy();
    for (uint32_t i = 0u; i < words; i++)
        destination[i] = source[i];

    /*
     * Node state is firmware-owned. Ignore stale host status while retaining
     * opcode, flags, and reserved control bits from the submitted snapshot.
     */
    for (uint32_t i = 0u; i < node_count; i++)
        rp1_node_set_status(&g_nodes[i], RP1_NODE_PENDING);
    rp1_dmb_sy();
}

/*
 * Stage one event without testing the flush threshold. Callers reserve space
 * first so marker insertion cannot recurse or overrun the fixed BTCM page.
 */
static void trace_stage(uint16_t event, uint32_t node_index,
                        uint32_t aux0, uint32_t aux1)
{
    rp1_trace_entry_t *entry = &g_trace_staging[g_trace_staging_count++];
    entry->timestamp = rp1_cycles() - g_graph_start_cycles;
    entry->event = event;
    entry->node_index = (uint16_t)node_index;
    entry->aux0 = aux0;
    entry->aux1 = aux1;
}

/*
 * Copy the staged prefix into the monotonic DDR ring and publish its producer
 * cursor only after every entry is visible. Ring wrap deliberately overwrites
 * the oldest records, matching the existing host-side overflow contract.
 */
static void trace_flush_staged(void)
{
    uint32_t count = g_trace_staging_count;
    if (count == 0u)
        return;

    uint32_t write = g_ctrl->trace_write_idx;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (write + i) % g_trace_size;
        g_trace[idx].timestamp = g_trace_staging[i].timestamp;
        g_trace[idx].event = g_trace_staging[i].event;
        g_trace[idx].node_index = g_trace_staging[i].node_index;
        g_trace[idx].aux0 = g_trace_staging[i].aux0;
        g_trace[idx].aux1 = g_trace_staging[i].aux1;
    }
    rp1_dmb_st();
    g_ctrl->trace_write_idx = write + count;
    rp1_dsb_st();
    g_trace_staging_count = 0u;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * Configuration validation checks bounded counts and aligned ranges wholly
 * inside the shared window. No host address becomes a pointer until every
 * field needed by the scanner has passed.
 */
int rp1_store_init(uint32_t *detail, uint32_t *aux)
{
    uint32_t node_count = g_ctrl->node_count;
    uint32_t node_base_lo = g_ctrl->node_base_lo;
    uint32_t node_base_hi = g_ctrl->node_base_hi;

    *detail = 0u;
    *aux = 0u;
    g_node_count = 0u;
    if (node_count == 0u || node_count > RP1_MAX_NODES) {
        *detail = RP1_CONFIG_NODE_COUNT;
        *aux = node_count;
        return -1;
    }
    uint32_t reserved_cq =
        (g_ctrl->_reserved_cq_size != 0u ? 1u << 0 : 0u) |
        (g_ctrl->_reserved_cq_base_lo != 0u ? 1u << 1 : 0u) |
        (g_ctrl->_reserved_cq_base_hi != 0u ? 1u << 2 : 0u) |
        (g_ctrl->_reserved_cq_write_idx != 0u ? 1u << 3 : 0u) |
        (g_ctrl->_reserved_cq_read_idx != 0u ? 1u << 4 : 0u);
    if (reserved_cq != 0u) {
        *detail = RP1_CONFIG_RESERVED_CQ;
        *aux = reserved_cq;
        return -1;
    }
    if (!valid_window_range(node_base_lo, node_base_hi,
                            node_count * (uint32_t)sizeof(rp1_node_t), 4u)) {
        *detail = RP1_CONFIG_NODE_BASE;
        *aux = node_base_lo;
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
     * Phase 2: all ranges are now proven 32-bit, aligned, and in-window.
     * Resolve non-node stores, reset stale graph state, then take the sole DDR
     * node read before publishing the valid BTCM prefix.
     */
    g_signals = (rp1_signal_slot_t *)
                    (uintptr_t)make64(g_ctrl->sig_array_base_lo, g_ctrl->sig_array_base_hi);
    g_arg_buf = (uint32_t *)
                    (uintptr_t)make64(g_ctrl->arg_buf_base_lo, g_ctrl->arg_buf_base_hi);
    g_trace   = (rp1_trace_entry_t *)
                    (uintptr_t)make64(g_ctrl->trace_base_lo, g_ctrl->trace_base_hi);
    g_trace_size = g_ctrl->trace_size;
    g_trace_enable = g_ctrl->trace_enable;
    g_ctrl->trace_write_idx = 0;
    g_trace_staging_count = 0u;

    rp1_store_reset_graph();
    snapshot_nodes(node_base_lo, node_base_hi, node_count);
    g_node_count = node_count;
    return 0;
}

void rp1_store_reset_graph(void)
{
    memzero(g_barriers,    sizeof(g_barriers));
    memzero(g_loop_iters,  sizeof(g_loop_iters));
    memzero(g_inflight,    sizeof(g_inflight));
    g_node_count = 0u;
    g_inflight_count = 0u;
    g_completed_operations = 0u;
    g_operation_started = 0u;
    g_quiesce_finite_done = 0u;
    g_quiesce_finite_timeout = 0u;
    g_quiesce_infinite = 0u;
    g_recovery_required = 0u;
    g_terminal_opcode = RP1_TERMINAL_OPCODE_NONE;
}

/*
 * A normal event may consume at most the penultimate slot. The final slot
 * names the synchronous flush, and the first entry staged afterward records
 * when that blocking copy returned.
 */
void rp1_trace_emit(uint16_t event, uint32_t node_index,
                    uint32_t aux0, uint32_t aux1)
{
    if (!g_trace_enable || !g_trace || !g_trace_size)
        return;

    trace_stage(event, node_index, aux0, aux1);
    if (RP1_TRACE_STAGING_ENTRIES - g_trace_staging_count != 1u)
        return;

    uint32_t write = g_ctrl->trace_write_idx;
    trace_stage(RP1_TRACE_FLUSH_START, 0xFFFFu,
                RP1_TRACE_STAGING_ENTRIES, write);
    trace_flush_staged();
    trace_stage(RP1_TRACE_FLUSH_END, 0xFFFFu,
                RP1_TRACE_STAGING_ENTRIES,
                g_ctrl->trace_write_idx);
}

void rp1_trace_flush_final(void)
{
    if (!g_trace_enable || !g_trace || !g_trace_size)
        return;
    trace_flush_staged();
}

void rp1_clear_error_latch(void)
{
    g_ctrl->rp1_error_code = 0u;
    g_ctrl->terminal_error_node = RP1_TERMINAL_ERROR_NODE_NONE;
    g_ctrl->terminal_error_detail = 0u;
    g_ctrl->terminal_error_aux = 0u;
    g_terminal_opcode = RP1_TERMINAL_OPCODE_NONE;
}

/*
 * The first error owns node/opcode/detail/aux for the whole graph. Quiescence
 * records recovery separately so it cannot alter the initiating cause.
 */
void rp1_latch_error(uint32_t code, uint32_t node,
                     uint32_t detail, uint32_t aux)
{
    if (g_ctrl->rp1_error_code != 0u)
        return;
    g_ctrl->rp1_error_code = code;
    g_ctrl->terminal_error_node = node;
    g_ctrl->terminal_error_detail = detail;
    g_ctrl->terminal_error_aux = aux;
    if (node != RP1_TERMINAL_ERROR_NODE_NONE && node < g_node_count)
        g_terminal_opcode = rp1_node_get_opcode(&g_nodes[node]);
}

void rp1_mark_recovery_required(void)
{
    g_recovery_required = 1u;
}
