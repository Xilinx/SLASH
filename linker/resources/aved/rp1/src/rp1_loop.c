/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 graph dispatch loop — flat scanner + inflight kernel tracker.
 *
 * See ARCHITECTURE.md section D for the full specification.
 */

#include "rp1_loop.h"
#include "rp1_pdi.h"
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Hardware helpers
 * ---------------------------------------------------------------------- */

static inline uint32_t axi_read32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void axi_write32(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

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

/* -------------------------------------------------------------------------
 * Condition evaluation  (shared with LOOP, COND)
 * ---------------------------------------------------------------------- */

static uint32_t compare(uint32_t sig, uint16_t op, uint32_t val)
{
    switch (op) {
    case RP1_COP_EQ:     return sig == val;
    case RP1_COP_NE:     return sig != val;
    case RP1_COP_LT:     return sig <  val;
    case RP1_COP_GE:     return sig >= val;
    case RP1_COP_AND_NZ: return (sig & val) != 0;
    case RP1_COP_AND_Z:  return (sig & val) == 0;
    default:             return 0;
    }
}

/* -------------------------------------------------------------------------
 * Completion queue
 * ---------------------------------------------------------------------- */

static void write_cq_entry(uint16_t flags, uint32_t node_index,
                           uint32_t status, uint32_t error_detail)
{
    if (flags & RP1_FLAG_SILENT)
        return;

    uint32_t idx = g_ctrl->cq_write_idx % g_ctrl->cq_size;
    g_cq[idx].node_index   = node_index;
    g_cq[idx].status       = status;
    g_cq[idx].error_detail = error_detail;
    g_cq[idx].timestamp    = 0; /* TODO: read R5 cycle counter */
    g_ctrl->cq_write_idx++;
    dsb();
}

/* -------------------------------------------------------------------------
 * Inflight kernel management
 * ---------------------------------------------------------------------- */

static void add_inflight(const rp1_node_t *node, uint32_t node_index)
{
    const rp1_payload_kernel_dispatch_t *kd = &node->payload.kernel_dispatch;
    rp1_inflight_t *slot = &g_inflight[g_inflight_count++];

    slot->base_addr         = kd->kernel_base_addr;
    slot->node_index        = node_index;
    slot->set_bucket        = node->barrier_set_bucket;
    slot->set_mask          = node->barrier_set_mask;
    slot->timeout_remaining = kd->timeout_cycles;
    if (slot->timeout_remaining == 0)
        slot->timeout_remaining = 10000000; /* default 10M cycles */
    slot->infinite = (node->flags & RP1_FLAG_INFINITE) ? 1 : 0;
    slot->settle_polls = 0;
}

static void remove_inflight(uint32_t idx)
{
    g_inflight_count--;
    if (idx < g_inflight_count)
        g_inflight[idx] = g_inflight[g_inflight_count];
}

/* -------------------------------------------------------------------------
 * Kernel launch
 * ---------------------------------------------------------------------- */

static void launch_kernel(const rp1_node_t *node)
{
    const rp1_payload_kernel_dispatch_t *kd = &node->payload.kernel_dispatch;
    /* Protocol v2: the argument buffer is an array of (reg_offset, value)
     * pairs.  Write each value to kernel_base_addr + reg_offset so the
     * non-contiguous HLS s_axilite register map is honoured exactly. */
    const rp1_kernel_arg_t *args =
        (const rp1_kernel_arg_t *)(g_arg_buf + kd->arg_buffer_offset / 4);

    /* HLS ap_done is sticky/clear-on-read. Clear any stale completion from a
     * previous invocation before writing arguments and pulsing ap_start.
     */
    (void)axi_read32(kd->kernel_base_addr + 0x00);
    dsb();

    for (uint16_t i = 0; i < kd->arg_count; i++)
        axi_write32(kd->kernel_base_addr + args[i].reg_offset, args[i].value);

    dsb();
    axi_write32(kd->kernel_base_addr + 0x00, 0x01); /* ap_start */
}

/* -------------------------------------------------------------------------
 * Immediate-completion opcodes (NOP, SIGNAL, SCALAR_*, DMA_*)
 * ---------------------------------------------------------------------- */

static void execute_immediate(const rp1_node_t *node, uint32_t node_index)
{
    switch (node->opcode) {
    case RP1_OP_NOP:
        break;

    case RP1_OP_SIGNAL: {
        const rp1_payload_signal_t *p = &node->payload.signal;
        volatile rp1_signal_slot_t *s = &g_signals[p->target_slot];
        switch (p->operation) {
        case RP1_SIGOP_SET: s->value  = p->value; break;
        case RP1_SIGOP_ADD: s->value += p->value; break;
        case RP1_SIGOP_OR:  s->value |= p->value; break;
        case RP1_SIGOP_AND: s->value &= p->value; break;
        }
        s->last_writer_node = node_index;
        break;
    }

    case RP1_OP_SCALAR_WRITE: {
        const rp1_payload_scalar_write_t *p = &node->payload.scalar_write;
#pragma GCC unroll 6
        for (uint32_t w = 0; w < RP1_SCALAR_WRITE_MAX; w++) {
            if (!p->writes[w].addr)
                break;
            axi_write32(p->writes[w].addr, p->writes[w].value);
        }
        dsb();
        break;
    }

    case RP1_OP_SCALAR_READ: {
        const rp1_payload_scalar_read_t *p = &node->payload.scalar_read;
        g_signals[p->target_slot].value = axi_read32(p->source_addr);
        g_signals[p->target_slot].last_writer_node = node_index;
        break;
    }

    case RP1_OP_SCALAR_COPY: {
        const rp1_payload_scalar_copy_t *p = &node->payload.scalar_copy;
        axi_write32(p->dest_addr, g_signals[p->source_slot].value);
        dsb();
        break;
    }

    case RP1_OP_DMA_COPY: {
        const rp1_payload_dma_copy_t *p = &node->payload.dma_copy;
        /* Phase 1: DDR-DDR software memcpy (32-bit addresses only). */
        uint32_t *src = (uint32_t *)(uintptr_t)p->src_addr_lo;
        uint32_t *dst = (uint32_t *)(uintptr_t)p->dst_addr_lo;
        uint32_t words = p->length / 4;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = src[w];
        dsb();
        break;
    }

    case RP1_OP_DMA_FILL: {
        const rp1_payload_dma_fill_t *p = &node->payload.dma_fill;
        uint32_t *dst = (uint32_t *)(uintptr_t)p->dst_addr_lo;
        uint32_t words = p->length / 4;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = p->pattern;
        dsb();
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Inflight kernel polling
 * ---------------------------------------------------------------------- */

/*
 * Returns:  1  at least one inflight kernel completed or timed out
 *           0  no progress
 *          -1  HALT_ON_ERROR timeout fired
 *
 * Timeout: timeout_remaining is decremented once per call.  On real
 * hardware this under-counts (one call ≈ one scan pass ≈ microseconds,
 * not cycles), but the mechanism is correct and testable.  Switch to
 * the R5 cycle counter (PMCCNTR) for accurate wall-clock timeouts.
 */
static int check_inflight(void)
{
    uint32_t i = 0;
    int made_progress = 0;

    while (i < g_inflight_count) {
        rp1_inflight_t *k = &g_inflight[i];
        uint32_t ctrl = axi_read32(k->base_addr + 0x00);

        if (ctrl & 0x2) { /* ap_done */
            if (!k->infinite) {
                g_node_status[k->node_index] = RP1_NODE_DONE;
                g_barriers[k->set_bucket] |= k->set_mask;
                write_cq_entry(g_nodes[k->node_index].flags,
                               k->node_index, RP1_CQ_OK, 0);
            }
            remove_inflight(i);
            made_progress = 1;
            /* don't increment i — slot was replaced by swap */
        } else {
            k->timeout_remaining--;
            if (k->timeout_remaining == 0) {
                uint16_t flags = g_nodes[k->node_index].flags;
                g_node_status[k->node_index] = RP1_NODE_ERROR;
                g_ctrl->rp1_error_code = RP1_ERR_KERNEL_TIMEOUT;
                write_cq_entry(flags, k->node_index, RP1_CQ_TIMEOUT, 0);

                if (flags & RP1_FLAG_HALT_ON_ERROR) {
                    remove_inflight(i);
                    g_ctrl->rp1_state = RP1_STATE_ERROR;
                    return -1;
                }

                /* Non-fatal: set barriers so dependents can proceed. */
                g_barriers[k->set_bucket] |= k->set_mask;
                remove_inflight(i);
                made_progress = 1;
            } else {
                i++;
            }
        }
    }

    return made_progress;
}

/* -------------------------------------------------------------------------
 * WAIT polling
 *
 * Re-evaluates every node parked in RP1_NODE_WAITING against its signal slot.
 * A WAIT becomes DONE (raising its barrier) as soon as the condition holds,
 * which may happen because a peer queue or the host wrote the slot between
 * scan passes.  Mirrors check_inflight(): returns 1 if any wait resolved.
 * ---------------------------------------------------------------------- */

static int check_waits(uint32_t node_count)
{
    int made_progress = 0;

    for (uint32_t i = 0; i < node_count; i++) {
        if (g_node_status[i] != RP1_NODE_WAITING)
            continue;

        const rp1_node_t *node = &g_nodes[i];
        const rp1_payload_wait_t *w = &node->payload.wait;
        if (compare(g_signals[w->condition_signal].value,
                    w->condition_op, w->condition_value)) {
            g_node_status[i] = RP1_NODE_DONE;
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            made_progress = 1;
        }
    }

    return made_progress;
}

/* -------------------------------------------------------------------------
 * Node activation (one full scan pass)
 *
 * Returns:  1  at least one node was activated
 *           0  no progress
 *          -1  error (inflight full)
 *          -2  HALT opcode executed
 * ---------------------------------------------------------------------- */

static int activate_nodes(uint32_t node_count)
{
    int made_progress = 0;

    for (uint32_t i = 0; i < node_count; i++) {
        if (g_node_status[i] != RP1_NODE_PENDING)
            continue;

        const rp1_node_t *node = &g_nodes[i];

        if ((g_barriers[node->barrier_await_bucket] & node->barrier_await_mask)
                != node->barrier_await_mask)
            continue;

        g_ctrl->rp1_current_node = i;

        switch (node->opcode) {

        case RP1_OP_KERNEL_DISPATCH: {
            /* Expected-image guard: a dispatch that names an image (non-zero)
             * must match the image last installed by PDI_LOAD. Fail fast
             * instead of poking an absent kernel and hanging. */
            const rp1_payload_kernel_dispatch_t *kd = &node->payload.kernel_dispatch;
            if (kd->expected_image_id != 0 &&
                kd->expected_image_id != g_active_image_id) {
                g_node_status[i] = RP1_NODE_ERROR;
                g_ctrl->rp1_error_code = RP1_ERR_IMAGE_MISMATCH;
                write_cq_entry(node->flags, i, RP1_CQ_ERROR, g_active_image_id);
                if (node->flags & RP1_FLAG_HALT_ON_ERROR) {
                    g_ctrl->rp1_state = RP1_STATE_ERROR;
                    return -1;
                }
                /* Non-fatal: set barriers so dependents can proceed. */
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                made_progress = 1;
                break;
            }
            if (g_inflight_count >= RP1_MAX_INFLIGHT) {
                g_ctrl->rp1_error_code = RP1_ERR_INFLIGHT_FULL;
                g_ctrl->rp1_state = RP1_STATE_ERROR;
                return -1;
            }
            launch_kernel(node);
            if (node->flags & RP1_FLAG_INFINITE) {
                g_node_status[i] = RP1_NODE_DONE;
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            } else {
                g_node_status[i] = RP1_NODE_DISPATCHED;
            }
            add_inflight(node, i);
            made_progress = 1;
            break;
        }

        case RP1_OP_PDI_LOAD: {
            const rp1_payload_pdi_load_t *p = &node->payload.pdi_load;
            int rc = rp1_pdi_load(p->pdi_addr_lo, p->pdi_addr_hi,
                                  p->timeout_cycles);

            if (rc == 0) {
                /* Record the now-active image for the dispatch guard. */
                g_active_image_id = p->image_id;
                g_node_status[i] = RP1_NODE_DONE;
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            } else {
                g_node_status[i] = RP1_NODE_ERROR;
                g_ctrl->rp1_error_code = RP1_ERR_PDI_TIMEOUT;
                write_cq_entry(node->flags, i, RP1_CQ_TIMEOUT, 0);

                if (node->flags & RP1_FLAG_HALT_ON_ERROR) {
                    g_ctrl->rp1_state = RP1_STATE_ERROR;
                    return -1;
                }

                /* Non-fatal: set barriers so dependents can proceed. */
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            }
            made_progress = 1;
            break;
        }

        case RP1_OP_LOOP: {
            const rp1_payload_loop_t *lp = &node->payload.loop;
            g_loop_iters[lp->loop_id]++;

            uint32_t sig_val = g_signals[lp->condition_signal].value;
            int exit_loop = 0;

            if (lp->max_iterations > 0
                && g_loop_iters[lp->loop_id] > lp->max_iterations)
                exit_loop = 1;
            if (compare(sig_val, lp->condition_op, lp->condition_value))
                exit_loop = 1;

            if (exit_loop) {
                g_node_status[i] = RP1_NODE_DONE;
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            } else {
                for (uint8_t b = lp->bucket_clear_start;
                     b <= lp->bucket_clear_end; b++)
                    g_barriers[b] = 0;
                for (uint32_t n = lp->body_start; n <= lp->body_end; n++)
                    g_node_status[n] = RP1_NODE_PENDING;
                g_node_status[i] = RP1_NODE_DONE;
                /* Do NOT set barrier_set — body + RERUN must fire first. */
            }
            made_progress = 1;
            break;
        }

        case RP1_OP_COND: {
            const rp1_payload_cond_t *cd = &node->payload.cond;
            uint32_t sig_val = g_signals[cd->condition_signal].value;

            if (compare(sig_val, cd->condition_op, cd->condition_value)) {
                /* Condition met — set done barriers. */
                g_barriers[cd->done_bucket] |= cd->done_mask;
            } else {
                /* Condition not met — clear body for execution. */
                for (uint8_t b = cd->bucket_clear_start;
                     b <= cd->bucket_clear_end; b++)
                    g_barriers[b] = 0;
                for (uint32_t n = cd->body_start; n <= cd->body_end; n++)
                    g_node_status[n] = RP1_NODE_PENDING;
            }
            g_node_status[i] = RP1_NODE_DONE;
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            made_progress = 1;
            break;
        }

        case RP1_OP_RERUN: {
            const rp1_payload_rerun_t *rr = &node->payload.rerun;
            g_node_status[rr->target_node] = RP1_NODE_PENDING;
            if (rr->rerun_flags & RP1_RERUN_CLEAR_STATE)
                g_loop_iters[rr->loop_id] = 0;
            g_node_status[i] = RP1_NODE_DONE;
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            made_progress = 1;
            break;
        }

        case RP1_OP_WAIT: {
            const rp1_payload_wait_t *w = &node->payload.wait;
            if (compare(g_signals[w->condition_signal].value,
                        w->condition_op, w->condition_value)) {
                g_node_status[i] = RP1_NODE_DONE;
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
                made_progress = 1;
            } else {
                /* Park the node; check_waits() re-polls the slot each pass. */
                g_node_status[i] = RP1_NODE_WAITING;
            }
            break;
        }

        case RP1_OP_HALT:
            g_node_status[i] = RP1_NODE_DONE;
            write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            return -2;

        default: /* NOP, SIGNAL, SCALAR_*, DMA_* */
            execute_immediate(node, i);
            g_node_status[i] = RP1_NODE_DONE;
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            write_cq_entry(node->flags, i, RP1_CQ_OK, 0);
            made_progress = 1;
            break;
        }
    }

    return made_progress;
}

/* -------------------------------------------------------------------------
 * Main dispatch loop
 * ---------------------------------------------------------------------- */

#ifdef QEMU_SEMIHOSTING
int rp1_loop(const rp1_hooks_t *hooks)
#else
int rp1_loop(void)
#endif
{
    uint32_t node_count = g_ctrl->node_count;

    while (1) {
        int activated = activate_nodes(node_count);
        if (activated < 0)
            return activated;

        int inflight_progress = check_inflight();
        if (inflight_progress < 0)
            return inflight_progress;

        int wait_progress = check_waits(node_count);

#ifdef QEMU_SEMIHOSTING
        if (hooks && hooks->on_scan_pass)
            hooks->on_scan_pass();
#endif

        if (!activated && !inflight_progress && !wait_progress) {
            /* No scan progress — keep looping while kernels are in flight or a
             * WAIT is still gated on a signal a peer/host may yet raise. */
            uint32_t has_dispatched = 0;
            uint32_t has_waiting = 0;
            for (uint32_t i = 0; i < node_count; i++) {
                uint8_t st = g_node_status[i];
                if (st == RP1_NODE_DISPATCHED) has_dispatched = 1;
                else if (st == RP1_NODE_WAITING) has_waiting = 1;
            }
            if (!has_dispatched && !has_waiting)
                return 0; /* graph complete */

#if !defined(QEMU_SEMIHOSTING) && !defined(RP1_POLLING_BRINGUP)
            /* A pending WAIT must busy-poll: host signal writes over the BAR do
             * not raise an R5 event, so wfi() could oversleep.  Only idle the
             * core when the sole outstanding work is in-flight kernels. */
            if (!has_waiting)
                wfi();
#endif
        }

        g_ctrl->heartbeat++;
    }
}
