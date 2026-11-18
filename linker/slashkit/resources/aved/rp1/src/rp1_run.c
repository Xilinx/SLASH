/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 outer loop — polls graph_seq for new submissions, resets state,
 * runs the flat scanner via rp1_loop(), and updates graph_done_seq.
 *
 * See ARCHITECTURE.md section D (rp1_main pseudocode).
 */

#include "rp1_run.h"
#include "rp1_cycles.h"
#include "rp1_hal.h"
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

/* Return non-zero when a successful graph leaves an infinite kernel running. */
static uint32_t has_infinite_work(void)
{
    for (uint32_t i = 0u; i < g_inflight_count; i++) {
        if (g_inflight[i].infinite)
            return 1u;
    }
    return 0u;
}

/*
 * Report nodes never reached before terminal publication. A configuration
 * failure cannot safely inspect node storage, but any submitted node is then
 * necessarily unreached.
 */
static uint32_t has_unreached_nodes(uint32_t node_count, int store_ready)
{
    if (!store_ready)
        return node_count != 0u;

    for (uint32_t i = 0u; i < node_count; i++) {
        uint8_t status = rp1_node_get_status(&g_nodes[i]);
        if (status == RP1_NODE_PENDING ||
            status == RP1_NODE_DISPATCHED ||
            status == RP1_NODE_WAITING)
            return 1u;
    }
    return 0u;
}

/* Initialize every result word while its commit magic is invalid. */
static void initialize_result(void)
{
    g_ctrl->result.magic = 0u;
    g_ctrl->result.graph_seq = 0u;
    g_ctrl->result.outcome = RP1_GRAPH_RESULT_NONE;
    g_ctrl->result.flags = 0u;
    g_ctrl->result.error_code = 0u;
    g_ctrl->result.terminal_node = RP1_TERMINAL_ERROR_NODE_NONE;
    g_ctrl->result.terminal_opcode = RP1_TERMINAL_OPCODE_NONE;
    g_ctrl->result.error_detail = 0u;
    g_ctrl->result.error_aux = 0u;
    g_ctrl->result.active_image_id = 0u;
    g_ctrl->result.image_state = RP1_IMAGE_STATE_NONE;
    g_ctrl->result.completed_operations = 0u;
    g_ctrl->result.graph_elapsed_ticks = 0u;
    g_ctrl->result.publish_elapsed_ticks = 0u;
    g_ctrl->result.trace_write_idx = 0u;
    g_ctrl->result.quiescence = 0u;
}

/*
 * Fill the uncommitted graph result after trace finalization. The caller
 * publishes magic only after this function returns and a barrier completes.
 */
static void populate_result(uint32_t accepted_seq, int result,
                            int store_ready, uint32_t node_count,
                            uint32_t graph_elapsed)
{
    uint32_t outcome = result == -1 ? RP1_GRAPH_RESULT_FAILED :
                       result == -2 ? RP1_GRAPH_RESULT_HALTED :
                                      RP1_GRAPH_RESULT_SUCCESS;
    uint32_t flags = 0u;
    uint32_t trace_write = 0u;

    if (g_recovery_required)
        flags |= RP1_RESULT_RECOVERY_REQUIRED;
    if (outcome != RP1_GRAPH_RESULT_SUCCESS && g_operation_started)
        flags |= RP1_RESULT_EFFECTS_MAY_BE_PARTIAL;
    if (g_quiesce_infinite != 0u || has_infinite_work())
        flags |= RP1_RESULT_INFINITE_WORK_REMAINS;
    if (store_ready && g_trace_enable) {
        flags |= RP1_RESULT_TRACE_ENABLED;
        trace_write = g_ctrl->trace_write_idx;
        if (trace_write > g_trace_size)
            flags |= RP1_RESULT_TRACE_OVERFLOW;
    }
    if (has_unreached_nodes(node_count, store_ready))
        flags |= RP1_RESULT_UNREACHED_NODES;

    g_ctrl->result.graph_seq = accepted_seq;
    g_ctrl->result.outcome = outcome;
    g_ctrl->result.flags = flags;
    g_ctrl->result.error_code = g_ctrl->rp1_error_code;
    g_ctrl->result.terminal_node = g_ctrl->terminal_error_node;
    g_ctrl->result.terminal_opcode = g_terminal_opcode;
    g_ctrl->result.error_detail = g_ctrl->terminal_error_detail;
    g_ctrl->result.error_aux = g_ctrl->terminal_error_aux;
    g_ctrl->result.active_image_id =
        g_active_image_state == RP1_IMAGE_STATE_KNOWN ?
        g_active_image_id : 0u;
    g_ctrl->result.image_state = g_active_image_state;
    g_ctrl->result.completed_operations = g_completed_operations;
    g_ctrl->result.graph_elapsed_ticks = graph_elapsed;
    g_ctrl->result.trace_write_idx = trace_write;
    g_ctrl->result.quiescence =
        RP1_QUIESCE_PACK(g_quiesce_finite_done,
                         g_quiesce_finite_timeout,
                         g_quiesce_infinite);
    /* This is the final payload word sampled before the commit barrier. */
    g_ctrl->result.publish_elapsed_ticks =
        rp1_cycles() - g_graph_start_cycles;
}

#ifdef QEMU_SEMIHOSTING
int rp1_run(const rp1_hooks_t *hooks)
#else
int rp1_run(void)
#endif
{
#ifdef QEMU_SEMIHOSTING
    int terminal_result = 0;
#endif

    rp1_pmu_init();

    /*
     * Startup publishes in dependency order: invalidate magic, initialize the
     * complete contract and READY state, barrier, then publish magic last.
     * Hosts may therefore treat visible magic as proof the fixed fields exist.
     */
    g_ctrl->magic                 = 0;
    g_ctrl->rp1_state             = RP1_STATE_INIT;
    g_ctrl->version               = RP1_PROTOCOL_VERSION;
    g_ctrl->capabilities          = RP1_REQUIRED_CAPABILITIES;
    g_ctrl->pdi_ipi_platform_id   = RP1_PLATFORM_ID;
    /* BTCM is NOLOAD, so establish reset-only persistent state explicitly. */
    g_active_image_id             = 0u;
    g_active_image_state          = RP1_IMAGE_STATE_NONE;
    rp1_cu_tracking_reset();
    rp1_clear_error_latch();
    /*
     * Firmware owns the doorbell only while magic is invalid. Reset both
     * sequence words to the same idle baseline before advertising readiness,
     * so a graph left in DDR by an earlier firmware instance cannot replay.
     */
    g_ctrl->graph_seq             = 0;
    g_ctrl->graph_done_seq        = 0;
    g_ctrl->_reserved_cq_write_idx = 0u;
    g_ctrl->_reserved_cq_read_idx = 0u;
    g_ctrl->heartbeat             = 0;
    initialize_result();
    g_ctrl->rp1_state             = RP1_STATE_READY;
    rp1_dmb_st();
    g_ctrl->magic = RP1_CTRL_MAGIC;
    rp1_dsb_st();

    /*
     * The outer loop has three cases: terminal firmware remains quiescent,
     * equal sequences mean idle, and a new sequence starts one accepted graph.
     * Equality, rather than ordering, keeps uint32_t sequence wrap harmless.
     */
    for (;;) {
        if (g_ctrl->rp1_state == RP1_STATE_ERROR ||
            g_ctrl->rp1_state == RP1_STATE_HALTED) {
            /* Terminal states are reset-only. A later graph_seq must never
             * reactivate work against an unproven hardware state. */
            g_ctrl->heartbeat++;
#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_idle && hooks->on_idle())
                return terminal_result;
#endif
            continue;
        }

        if (g_ctrl->graph_seq == g_ctrl->graph_done_seq) {
            g_ctrl->heartbeat++;

#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_idle) {
                if (hooks->on_idle())
                    return 0;
            }
#endif
            continue;
        }

        uint32_t accepted_seq = g_ctrl->graph_seq;
        uint32_t submitted_node_count = g_ctrl->node_count;
        uint32_t config_detail = 0u;
        uint32_t config_aux = 0u;
        int store_ready = 0;

        /*
         * Phase 1: latch the accepted sequence, enter RUNNING, then validate
         * every host-owned range before resolving or dereferencing DDR pointers.
         */
        g_ctrl->result.magic = 0u;
        rp1_dmb_st();
        rp1_clear_error_latch();
        rp1_store_reset_graph();
        g_ctrl->rp1_current_node = RP1_TERMINAL_ERROR_NODE_NONE;
        g_ctrl->rp1_state = RP1_STATE_RUNNING;
        rp1_dsb_st();
        g_graph_start_cycles = rp1_cycles();
        if (rp1_store_init(&config_detail, &config_aux) != 0) {
            rp1_latch_error(RP1_ERR_INVALID_CONFIG,
                            RP1_TERMINAL_ERROR_NODE_NONE,
                            config_detail, config_aux);
        } else {
            store_ready = 1;
        }
        if (store_ready)
            rp1_trace_emit(RP1_TRACE_GRAPH_START, 0xFFFFu,
                           accepted_seq, g_node_count);

        /*
         * Phase 2: run only a fully initialized store. Configuration failure
         * skips the scanner so an invalid host pointer can have no side effect.
         */
        int result;
        if (!store_ready) {
            result = -1;
        } else {
#ifdef QEMU_SEMIHOSTING
            result = rp1_loop(hooks);
#else
            result = rp1_loop();
#endif
        }

        uint32_t graph_elapsed;
        if (store_ready) {
            rp1_trace_emit(RP1_TRACE_GRAPH_DONE, 0xFFFFu,
                           (uint32_t)result, accepted_seq);
            graph_elapsed = rp1_cycles() - g_graph_start_cycles;
            /*
             * Publish the final partial BTCM page after graph work ends.
             * Periodic flushes are measured by FLUSH_START/END; this terminal
             * drain intentionally adds no recursive marker pair.
             */
            rp1_trace_flush_final();
        } else {
            graph_elapsed = rp1_cycles() - g_graph_start_cycles;
        }

        /*
         * Phase 3: classify scanner completion without clearing its first-error
         * record; ERROR and HALTED remain reset-only in the next outer pass.
         */
        uint32_t terminal_state;
        if (result == -1)
            terminal_state = RP1_STATE_ERROR;
        else if (result == -2)
            terminal_state = RP1_STATE_HALTED;
        else
            terminal_state = RP1_STATE_READY;

        /*
         * Commit the complete result before terminal state, then publish state
         * before graph_done_seq. Exact sequence completion is the host's
         * release point for the committed sequence-tagged result.
         */
        populate_result(accepted_seq, result, store_ready,
                        store_ready ? g_node_count : submitted_node_count,
                        graph_elapsed);
        rp1_dmb_st();
        g_ctrl->result.magic = RP1_GRAPH_RESULT_MAGIC;
        rp1_dmb_st();
        g_ctrl->rp1_state = terminal_state;
        rp1_dmb_st();
        g_ctrl->graph_done_seq = accepted_seq;
        rp1_dsb_st();
#ifdef QEMU_SEMIHOSTING
        terminal_result = result;
#endif

#ifdef QEMU_SEMIHOSTING
        if (hooks && hooks->on_graph_done) {
            if (hooks->on_graph_done(result))
                return result;
        }
#endif
        /* TODO: ring GCQ doorbell (S01_AXI/0x000) when block design is wired. */
    }
}
