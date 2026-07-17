/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 static storage — BTCM-resident hot data and DDR pointers.
 *
 * All BTCM objects are in a dedicated .btcm section so the linker script
 * can place them there explicitly.  DDR-backed objects are accessed through
 * pointers that are initialised from the control block at startup.
 *
 * BTCM budget (of 64 KB):
 *   completed_barriers[32]    128 B
 *   node_status[4096]        4096 B
 *   loop_iterations[64]       256 B
 *   inflight[32]              768 B   (32 * sizeof(rp1_inflight_t) = 24)
 *   inflight_count              4 B
 *   stack                    4096 B   (linker script)
 *   code variables           ~1 KB
 *   ─────────────────────────────
 *   Total hot data           ~10.3 KB
 */

#ifndef RP1_STORE_H
#define RP1_STORE_H

#include <slash/uapi/rp1_protocol.h>

/* -------------------------------------------------------------------------
 * BTCM-resident hot stores
 * ---------------------------------------------------------------------- */

/* Flat barrier array: 32 buckets of 32 bits each = 1024 barrier signals. */
extern uint32_t g_barriers[RP1_MAX_BUCKETS];

/* Per-node status cache: one byte per node, mirrors rp1_node_t.status. */
extern uint8_t g_node_status[RP1_MAX_NODES];

/* Per-loop iteration counter, indexed by loop_id. */
extern uint32_t g_loop_iters[RP1_MAX_LOOPS];

/* In-flight kernel table. */
extern rp1_inflight_t g_inflight[RP1_MAX_INFLIGHT];
extern uint32_t       g_inflight_count;

/* PMU cycle counter value captured at the start of the current graph. */
extern uint32_t g_graph_start_cycles;

/*
 * Image id last installed by a successful PDI_LOAD (0 = none loaded yet).
 * This reflects physical partial-reconfiguration state, so unlike the other
 * BTCM stores it PERSISTS across graph submissions and is deliberately not
 * cleared by rp1_store_reset_graph(); only a PDI_LOAD node changes it. A
 * KERNEL_DISPATCH with a non-zero expected_image_id that does not match this
 * is failed fast instead of poking an absent kernel.
 */
extern uint32_t g_active_image_id;

/* -------------------------------------------------------------------------
 * DDR-backed stores (pointers into shared DDR, set at graph init)
 * ---------------------------------------------------------------------- */

/* Pointer to the control block (fixed at RP1_DDR_CTRL_BASE). */
extern rp1_ctrl_t *g_ctrl;

/* Pointer to the node array (from g_ctrl->node_base_lo/hi). */
extern rp1_node_t *g_nodes;

/* Pointer to the CQ ring (from g_ctrl->cq_base_lo/hi). */
extern rp1_cq_entry_t *g_cq;

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
 * rp1_store_init() — zero all BTCM stores and resolve DDR pointers from the
 * control block.  Must be called once before processing each graph.
 */
void rp1_store_init(void);

/*
 * rp1_store_reset_graph() — reset per-graph BTCM state (barriers, node
 * statuses, loop counters, inflight table) without touching the DDR pointers.
 * Called at the start of every new graph submission.
 */
void rp1_store_reset_graph(void);

#endif /* RP1_STORE_H */
