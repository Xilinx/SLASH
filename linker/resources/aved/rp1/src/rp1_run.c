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
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

static inline void dsb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

#if !defined(QEMU_SEMIHOSTING) && !defined(RP1_POLLING_BRINGUP)
static inline void wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}
#endif

static void trace(uint16_t event, uint32_t node_index, uint32_t aux0, uint32_t aux1)
{
    if (!g_trace_enable || !g_trace || !g_trace_size)
        return;

    uint32_t idx = g_ctrl->trace_write_idx % g_trace_size;
    g_trace[idx].timestamp  = rp1_cycles() - g_graph_start_cycles;
    g_trace[idx].event      = event;
    g_trace[idx].node_index = (uint16_t)node_index;
    g_trace[idx].aux0       = aux0;
    g_trace[idx].aux1       = aux1;
    g_ctrl->trace_write_idx++;
    dsb();
}

#ifdef QEMU_SEMIHOSTING
int rp1_run(const rp1_hooks_t *hooks)
#else
int rp1_run(void)
#endif
{
    rp1_pmu_init();

    g_ctrl->magic          = RP1_CTRL_MAGIC;
    g_ctrl->version        = RP1_PROTOCOL_VERSION;
    g_ctrl->rp1_state      = RP1_STATE_READY;
    g_ctrl->graph_done_seq = 0;
    g_ctrl->cq_write_idx   = 0;
    g_ctrl->heartbeat      = 0;
    dsb();

    for (;;) {
        if (g_ctrl->graph_seq == g_ctrl->graph_done_seq) {
            g_ctrl->heartbeat++;

#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_idle) {
                if (hooks->on_idle())
                    return 0;
            }
#elif !defined(RP1_POLLING_BRINGUP)
            wfi();
#endif
            continue;
        }

        /* New graph submitted — resolve DDR pointers and reset state. */
        rp1_store_init();
        g_graph_start_cycles = rp1_cycles();
        trace(RP1_TRACE_GRAPH_START, 0xFFFFu, g_ctrl->graph_seq, g_ctrl->node_count);
        g_ctrl->rp1_state      = RP1_STATE_RUNNING;
        g_ctrl->rp1_error_code = 0;
        dsb();

#ifdef QEMU_SEMIHOSTING
        int result = rp1_loop(hooks);
#else
        int result = rp1_loop();
#endif

        trace(RP1_TRACE_GRAPH_DONE, 0xFFFFu, (uint32_t)result, g_ctrl->graph_seq);
        g_ctrl->graph_done_seq = g_ctrl->graph_seq;

        if (result == -1)
            g_ctrl->rp1_state = RP1_STATE_ERROR;
        else if (result == -2)
            g_ctrl->rp1_state = RP1_STATE_HALTED;
        else
            g_ctrl->rp1_state = RP1_STATE_READY;
        dsb();

#ifdef QEMU_SEMIHOSTING
        if (hooks && hooks->on_graph_done) {
            if (hooks->on_graph_done(result))
                return result;
        }
#endif
        /* TODO: ring GCQ doorbell (S01_AXI/0x000) when block design is wired. */
    }
}
