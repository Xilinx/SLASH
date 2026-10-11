/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 static storage — BTCM-resident graph data and DDR pointers.
 *
 * All BTCM objects are in a dedicated .btcm section so the linker script
 * can place them there explicitly.  DDR-backed objects are accessed through
 * pointers that are initialised from the control block at startup.
 *
 * BTCM budget (of 64 KB):
 *   nodes[1024]              32768 B
 *   completed_barriers[32]     128 B
 *   loop_iterations[64]        256 B
 *   inflight[32]               768 B   (32 * sizeof(rp1_inflight_t) = 24)
 *   clean CU cache              132 B   (32 addresses + count)
 *   scheduler fixed state      ~3.5 KB  (CSR offsets/counts + bitsets)
 *   trace staging             4096 B   (256 * sizeof(rp1_trace_entry_t))
 *   stack                     4096 B   (linker script)
 *   bookkeeping and BSS       ~6 KB
 *   ─────────────────────────────
 *   Total                    <64 KB (enforced by the linker script)
 *
 * The sparse subscriber array uses 32 KB of otherwise free ATCM.
 */

#ifndef RP1_STORE_H
#define RP1_STORE_H

#include <slash/uapi/rp1_protocol.h>

/* One fixed 4 KB BTCM page stages trace entries before a blocking DDR flush. */
#define RP1_TRACE_STAGING_BYTES   4096u
#define RP1_TRACE_STAGING_ENTRIES \
    (RP1_TRACE_STAGING_BYTES / (uint32_t)sizeof(rp1_trace_entry_t))

/* -------------------------------------------------------------------------
 * BTCM-resident hot stores
 * ---------------------------------------------------------------------- */

/* Flat barrier array: 32 buckets of 32 bits each = 1024 barrier signals. */
extern uint32_t g_barriers[RP1_MAX_BUCKETS];

/*
 * Authoritative graph snapshot. The active DDR prefix is copied here once
 * after validation; payloads and packed status are never read back from DDR.
 */
extern rp1_node_t g_nodes[RP1_MAX_NODES];

/* Valid prefix of g_nodes for the accepted graph. */
extern uint32_t g_node_count;

/* Per-loop iteration counter, indexed by loop_id. */
extern uint32_t g_loop_iters[RP1_MAX_LOOPS];

/* In-flight kernel table. */
extern rp1_inflight_t g_inflight[RP1_MAX_INFLIGHT];
extern uint32_t       g_inflight_count;

/* PMU cycle counter value captured at the start of the current graph. */
extern uint32_t g_graph_start_cycles;

/*
 * Persistent partial-reconfiguration state. A successful named PDI makes the
 * image KNOWN; any PDI timeout or rejection makes it UNKNOWN until a later
 * successful load. Both fields survive graph resets.
 */
extern uint32_t g_active_image_id;
extern uint32_t g_active_image_state;

/* Per-graph result counters and terminal metadata, reset before activation. */
extern uint32_t g_completed_operations;
extern uint32_t g_operation_started;
extern uint32_t g_quiesce_finite_done;
extern uint32_t g_quiesce_finite_timeout;
extern uint32_t g_quiesce_infinite;
extern uint32_t g_recovery_required;
extern uint32_t g_terminal_opcode;

/* -------------------------------------------------------------------------
 * DDR-backed stores (pointers into shared DDR, set at graph init)
 * ---------------------------------------------------------------------- */

/* Pointer to the control block (fixed at RP1_DDR_CTRL_BASE). */
extern rp1_ctrl_t *g_ctrl;

/* Pointer to the signal array (from g_ctrl->sig_array_base_lo/hi). */
extern rp1_signal_slot_t *g_signals;

/* Pointer to the argument buffer (from g_ctrl->arg_buf_base_lo/hi). */
extern uint32_t *g_arg_buf;

/* Optional trace ring (from g_ctrl->trace_*), enabled per graph. */
extern rp1_trace_entry_t *g_trace;
extern uint32_t g_trace_size;
extern uint32_t g_trace_enable;

/* -------------------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------------- */

/*
 * Validate host-owned control fields, snapshot the active node prefix into
 * BTCM, resolve the remaining DDR pointers, and reset per-graph state.
 *
 * @return 0 on success, -1 with protocol detail/aux populated on failure.
 */
int rp1_store_init(uint32_t *detail, uint32_t *aux);

/*
 * rp1_store_reset_graph() — reset per-graph BTCM state (barriers, counters,
 * loop state, and inflight table) without touching the node snapshot, DDR
 * pointers, or persistent image state. Called before each graph snapshot.
 */
void rp1_store_reset_graph(void);

/*
 * Append one timestamped event to the BTCM trace page. When one slot remains,
 * the implementation fills it with FLUSH_START, copies the full page to the
 * configured DDR ring, and makes FLUSH_END the first entry in the new page.
 * Disabled tracing is a no-op.
 */
void rp1_trace_emit(uint16_t event, uint32_t node_index,
                    uint32_t aux0, uint32_t aux1);

/*
 * Copy the final partial BTCM page to DDR without adding flush markers.
 * Graph completion calls this after emitting GRAPH_DONE, so no recursive
 * marker flush is needed merely to publish FLUSH_END or GRAPH_DONE.
 */
void rp1_trace_flush_final(void);

/*
 * First-error-wins diagnostic publication for the current graph. Clear only
 * before accepting a graph; quiescence recovery metadata is tracked
 * separately and must not replace the initiating failure.
 */
void rp1_clear_error_latch(void);
void rp1_latch_error(uint32_t code, uint32_t node,
                     uint32_t detail, uint32_t aux);
void rp1_mark_recovery_required(void);

#endif /* RP1_STORE_H */
