/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 graph dispatch loop — flat scanner + inflight kernel tracker.
 *
 * See ARCHITECTURE.md section D for the full specification.
 */

#include "rp1_loop.h"
#include "rp1_cycles.h"
#include "rp1_hal.h"
#include "rp1_pdi.h"
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

static uint32_t g_cq_blocked;

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

/*
 * Producer and consumer cursors are monotonic, so unsigned subtraction remains
 * valid across wrap. A full ring backpressures every non-silent side effect;
 * occupancy beyond the ring is corruption, while silent nodes need no slot.
 */
static int cq_can_write(uint16_t flags)
{
    if (flags & RP1_FLAG_SILENT)
        return 1;

    uint32_t used = g_ctrl->cq_write_idx - g_ctrl->cq_read_idx;
    if (used > g_ctrl->cq_size) {
        rp1_latch_error(RP1_ERR_CQ_CORRUPT,
                        RP1_TERMINAL_ERROR_NODE_NONE,
                        used, g_ctrl->cq_size);
        return -1;
    }
    return used != g_ctrl->cq_size;
}

/*
 * Fill the selected slot completely before publishing cq_write_idx. The host
 * advances cq_read_idx only after copying that slot, making the two cursors the
 * ownership handoff for reusable ring storage.
 */
static int write_cq_entry(uint16_t flags, uint32_t node_index,
                          uint32_t status, uint32_t error_detail)
{
    int available = cq_can_write(flags);
    if (available <= 0)
        return available;
    if (flags & RP1_FLAG_SILENT)
        return 1;

    uint32_t idx = g_ctrl->cq_write_idx & (g_ctrl->cq_size - 1u);
    g_cq[idx].node_index   = node_index;
    g_cq[idx].status       = status;
    g_cq[idx].error_detail = error_detail;
    g_cq[idx].timestamp    = rp1_cycles() - g_graph_start_cycles;
    rp1_barrier();
    g_ctrl->cq_write_idx++;
    rp1_barrier();
    return 1;
}

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
    rp1_barrier();
    g_ctrl->trace_write_idx++;
    rp1_barrier();
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
    slot->timeout_start     = rp1_cycles();
    slot->timeout_cycles    = kd->timeout_cycles ?
                              kd->timeout_cycles :
                              RP1_DEFAULT_KERNEL_TIMEOUT_TICKS;
    slot->infinite = (node->flags & RP1_FLAG_INFINITE) ? 1 : 0;
    slot->settle_polls = 0;
}

static void remove_inflight(uint32_t idx)
{
    g_inflight_count--;
    if (idx < g_inflight_count)
        g_inflight[idx] = g_inflight[g_inflight_count];
}

static void set_node_status(uint32_t node_index, uint16_t status)
{
    g_node_status[node_index] = (uint8_t)status;
    g_nodes[node_index].status = status;
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
    (void)rp1_mmio_read32(kd->kernel_base_addr + 0x00);
    rp1_barrier();

    for (uint16_t i = 0; i < kd->arg_count; i++)
        rp1_mmio_write32(kd->kernel_base_addr + args[i].reg_offset,
                         args[i].value);

    rp1_barrier();
    rp1_mmio_write32(kd->kernel_base_addr + 0x00, 0x01); /* ap_start */
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
            rp1_mmio_write32(p->writes[w].addr, p->writes[w].value);
        }
        rp1_barrier();
        break;
    }

    case RP1_OP_SCALAR_READ: {
        const rp1_payload_scalar_read_t *p = &node->payload.scalar_read;
        g_signals[p->target_slot].value = rp1_mmio_read32(p->source_addr);
        g_signals[p->target_slot].last_writer_node = node_index;
        break;
    }

    case RP1_OP_SCALAR_COPY: {
        const rp1_payload_scalar_copy_t *p = &node->payload.scalar_copy;
        rp1_mmio_write32(p->dest_addr, g_signals[p->source_slot].value);
        rp1_barrier();
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
        rp1_barrier();
        break;
    }

    case RP1_OP_DMA_FILL: {
        const rp1_payload_dma_fill_t *p = &node->payload.dma_fill;
        uint32_t *dst = (uint32_t *)(uintptr_t)p->dst_addr_lo;
        uint32_t words = p->length / 4;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = p->pattern;
        rp1_barrier();
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Packet validation
 * ---------------------------------------------------------------------- */

static uint32_t valid_condition(uint16_t op)
{
    return op <= RP1_COP_AND_Z;
}

static uint32_t valid_signal_slot(uint32_t slot)
{
    return slot < RP1_MAX_SIGNALS;
}

/*
 * Packet validation has two phases per node: common barrier bounds, then
 * opcode-specific slots, operations, address ranges, and control ranges.
 * The whole graph passes before activation, so rejection has no fabric effect.
 */
static int validate_nodes(uint32_t node_count, uint32_t *bad_node,
                          uint32_t *detail, uint32_t *aux)
{
    uint32_t arg_available =
        RP1_CTRL_PHYS_ADDR + RP1_CTRL_WINDOW_SIZE - g_ctrl->arg_buf_base_lo;

    for (uint32_t i = 0; i < node_count; i++) {
        const rp1_node_t *node = &g_nodes[i];
        /*
         * Phase 1: every opcode shares barrier buckets, so reject an invalid
         * header before interpreting its payload union.
         */
        if (node->barrier_await_bucket >= RP1_MAX_BUCKETS ||
            node->barrier_set_bucket >= RP1_MAX_BUCKETS) {
            *bad_node = i;
            *detail = RP1_NODE_BAD_BARRIER;
            *aux = ((uint32_t)node->barrier_await_bucket << 8) |
                   node->barrier_set_bucket;
            return -1;
        }

        /*
         * Phase 2: validate only the active union member. Simple opcodes have
         * no indexed fields; the remaining cases prove every later dereference.
         */
        switch (node->opcode) {
        case RP1_OP_NOP:
        case RP1_OP_SCALAR_WRITE:
        case RP1_OP_DMA_COPY:
        case RP1_OP_DMA_FILL:
        case RP1_OP_HALT:
            break;
        case RP1_OP_SIGNAL:
            if (!valid_signal_slot(node->payload.signal.target_slot)) {
                *detail = RP1_NODE_BAD_SIGNAL_SLOT;
                *aux = node->payload.signal.target_slot;
            } else if (node->payload.signal.operation > RP1_SIGOP_AND) {
                *detail = RP1_NODE_BAD_OPERATION;
                *aux = node->payload.signal.operation;
            } else {
                break;
            }
            *bad_node = i;
            return -1;
        case RP1_OP_WAIT:
            if (!valid_signal_slot(node->payload.wait.condition_signal)) {
                *detail = RP1_NODE_BAD_SIGNAL_SLOT;
                *aux = node->payload.wait.condition_signal;
            } else if (!valid_condition(node->payload.wait.condition_op)) {
                *detail = RP1_NODE_BAD_OPERATION;
                *aux = node->payload.wait.condition_op;
            } else {
                break;
            }
            *bad_node = i;
            return -1;
        case RP1_OP_SCALAR_READ:
            if (valid_signal_slot(node->payload.scalar_read.target_slot))
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_SIGNAL_SLOT;
            *aux = node->payload.scalar_read.target_slot;
            return -1;
        case RP1_OP_SCALAR_COPY:
            if (valid_signal_slot(node->payload.scalar_copy.source_slot))
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_SIGNAL_SLOT;
            *aux = node->payload.scalar_copy.source_slot;
            return -1;
        case RP1_OP_KERNEL_DISPATCH: {
            const rp1_payload_kernel_dispatch_t *kd =
                &node->payload.kernel_dispatch;
            uint32_t bytes = (uint32_t)kd->arg_count *
                             (uint32_t)sizeof(rp1_kernel_arg_t);
            if (kd->kernel_base_addr != 0u &&
                (kd->arg_buffer_offset & 7u) == 0u &&
                kd->arg_buffer_offset <= arg_available &&
                bytes <= arg_available - kd->arg_buffer_offset)
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_ARGUMENTS;
            *aux = kd->arg_buffer_offset;
            return -1;
        }
        case RP1_OP_PDI_LOAD:
            if ((node->flags & RP1_FLAG_SILENT) == 0u)
                break;
            *bad_node = i;
            *detail = RP1_NODE_PDI_WITHOUT_CQ;
            *aux = 0u;
            return -1;
        case RP1_OP_LOOP: {
            const rp1_payload_loop_t *loop = &node->payload.loop;
            if (valid_signal_slot(loop->condition_signal) &&
                valid_condition(loop->condition_op) &&
                loop->loop_id < RP1_MAX_LOOPS &&
                loop->body_start <= loop->body_end &&
                loop->body_end < node_count &&
                loop->bucket_clear_start <= loop->bucket_clear_end &&
                loop->bucket_clear_end < RP1_MAX_BUCKETS)
                break;
            *bad_node = i;
            *detail = valid_signal_slot(loop->condition_signal) ?
                      RP1_NODE_BAD_LOOP_CONFIG :
                      RP1_NODE_BAD_SIGNAL_SLOT;
            *aux = valid_signal_slot(loop->condition_signal) ?
                   loop->body_end : loop->condition_signal;
            return -1;
        }
        case RP1_OP_COND: {
            const rp1_payload_cond_t *cond = &node->payload.cond;
            uint32_t empty_body = cond->body_start > cond->body_end;
            uint32_t empty_buckets =
                cond->bucket_clear_start > cond->bucket_clear_end;
            if (valid_signal_slot(cond->condition_signal) &&
                valid_condition(cond->condition_op) &&
                cond->done_bucket < RP1_MAX_BUCKETS &&
                (empty_body || cond->body_end < node_count) &&
                (empty_buckets ||
                 cond->bucket_clear_end < RP1_MAX_BUCKETS))
                break;
            *bad_node = i;
            *detail = valid_signal_slot(cond->condition_signal) ?
                      RP1_NODE_BAD_LOOP_CONFIG :
                      RP1_NODE_BAD_SIGNAL_SLOT;
            *aux = valid_signal_slot(cond->condition_signal) ?
                   cond->body_end : cond->condition_signal;
            return -1;
        }
        case RP1_OP_RERUN:
            if (node->payload.rerun.target_node < node_count &&
                ((node->payload.rerun.rerun_flags &
                  RP1_RERUN_CLEAR_STATE) == 0u ||
                 node->payload.rerun.loop_id < RP1_MAX_LOOPS))
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_TARGET;
            *aux = node->payload.rerun.target_node;
            return -1;
        default:
            *bad_node = i;
            *detail = RP1_NODE_BAD_OPERATION;
            *aux = node->opcode;
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Inflight kernel polling
 * ---------------------------------------------------------------------- */

/*
 * Cases are complete, expired, or still pending. A full CQ parks finite work
 * before reading sticky/clear-on-read ap_done, preserving evidence until it
 * can be published; infinite work needs no eventual completion entry.
 *
 * Returns 1 for progress, 0 for none, and -1 for a fatal timeout/corruption.
 * PMU elapsed ticks, not scan passes, define every timeout.
 */
static int check_inflight(void)
{
    uint32_t i = 0;
    int made_progress = 0;

    while (i < g_inflight_count) {
        rp1_inflight_t *k = &g_inflight[i];
        uint16_t flags = g_nodes[k->node_index].flags;
        int cq_available = cq_can_write(flags);
        if (cq_available < 0)
            return -1;
        if (!k->infinite && cq_available == 0) {
            g_cq_blocked = 1u;
            i++;
            continue;
        }

        uint32_t ctrl = rp1_mmio_read32(k->base_addr + 0x00);

        if (ctrl & 0x2) { /* ap_done */
            if (!k->infinite) {
                set_node_status(k->node_index, RP1_NODE_DONE);
                g_barriers[k->set_bucket] |= k->set_mask;
                if (write_cq_entry(flags, k->node_index,
                                   RP1_CQ_OK, 0) < 0)
                    return -1;
            }
            trace(RP1_TRACE_KERNEL_DONE, k->node_index, k->base_addr, k->infinite);
            remove_inflight(i);
            made_progress = 1;
            /* don't increment i — slot was replaced by swap */
        } else {
            uint32_t now = rp1_cycles();
            if (rp1_timeout_elapsed(k->timeout_start,
                                    k->timeout_cycles, now)) {
                if (cq_available == 0) {
                    g_cq_blocked = 1u;
                    i++;
                    continue;
                }
                set_node_status(k->node_index, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_KERNEL_TIMEOUT, k->node_index,
                                k->base_addr, k->timeout_cycles);
                if (write_cq_entry(flags, k->node_index,
                                   RP1_CQ_TIMEOUT, k->base_addr) < 0)
                    return -1;
                trace(RP1_TRACE_KERNEL_TIMEOUT, k->node_index,
                      k->base_addr, k->timeout_cycles);

                if (flags & RP1_FLAG_HALT_ON_ERROR) {
                    /* Keep the timed-out kernel tracked for fatal quiesce. */
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
        /*
         * Reserve CQ capacity before changing WAIT status or barriers; once a
         * dependent can run, its wake evidence cannot be reconstructed.
         */
        int cq_available = cq_can_write(node->flags);
        if (cq_available < 0)
            return -1;
        if (cq_available == 0) {
            g_cq_blocked = 1u;
            continue;
        }
        uint32_t sig_val = g_signals[w->condition_signal].value;
        if (compare(sig_val, w->condition_op, w->condition_value)) {
            set_node_status(i, RP1_NODE_DONE);
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                return -1;
            trace(RP1_TRACE_WAIT_WAKE, i, w->condition_signal, sig_val);
            made_progress = 1;
        }
    }

    return made_progress;
}

/* -------------------------------------------------------------------------
 * Node activation (one full scan pass)
 *
 * Eligible packets split into asynchronous kernel work, synchronous PDI work,
 * scanner control, and immediate operations. CQ capacity is checked before
 * side effects because non-silent completion evidence cannot be recreated.
 *
 * Returns 1 for progress, 0 for none, -1 for error, and -2 for HALT.
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

        int cq_available = cq_can_write(node->flags);
        if (cq_available < 0)
            return -1;
        if (cq_available == 0) {
            g_cq_blocked = 1u;
            continue;
        }

        g_ctrl->rp1_current_node = i;
        trace(RP1_TRACE_NODE_ACTIVATE, i, node->opcode, node->flags);

        /*
         * Phase 1: launch asynchronous fabric work or perform the serialized
         * platform-image transition; both may establish later dispatch state.
         */
        switch (node->opcode) {

        case RP1_OP_KERNEL_DISPATCH: {
            /* Expected-image guard: a dispatch that names an image (non-zero)
             * must match the image last installed by PDI_LOAD. Fail fast
             * instead of poking an absent kernel and hanging. */
            const rp1_payload_kernel_dispatch_t *kd = &node->payload.kernel_dispatch;
            if (kd->expected_image_id != 0 &&
                kd->expected_image_id != g_active_image_id) {
                set_node_status(i, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_IMAGE_MISMATCH, i,
                                kd->expected_image_id, g_active_image_id);
                if (write_cq_entry(node->flags, i, RP1_CQ_ERROR,
                                   g_active_image_id) < 0)
                    return -1;
                trace(RP1_TRACE_IMAGE_MISMATCH, i,
                      kd->expected_image_id, g_active_image_id);
                if (node->flags & RP1_FLAG_HALT_ON_ERROR)
                    return -1;
                /* Non-fatal: set barriers so dependents can proceed. */
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                made_progress = 1;
                break;
            }
            if (g_inflight_count >= RP1_MAX_INFLIGHT) {
                set_node_status(i, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_INFLIGHT_FULL, i,
                                g_inflight_count, RP1_MAX_INFLIGHT);
                if (write_cq_entry(node->flags, i, RP1_CQ_ERROR,
                                   g_inflight_count) < 0)
                    return -1;
                return -1;
            }
            launch_kernel(node);
            trace(RP1_TRACE_KERNEL_LAUNCH, i,
                  kd->kernel_base_addr, kd->arg_count);
            if (node->flags & RP1_FLAG_INFINITE) {
                set_node_status(i, RP1_NODE_DONE);
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                    return -1;
            } else {
                set_node_status(i, RP1_NODE_DISPATCHED);
            }
            add_inflight(node, i);
            made_progress = 1;
            break;
        }

        case RP1_OP_PDI_LOAD: {
            const rp1_payload_pdi_load_t *p = &node->payload.pdi_load;
            rp1_pdi_result_t result =
                rp1_pdi_load(p->pdi_addr_lo, p->pdi_addr_hi,
                             p->timeout_cycles);
            trace(RP1_TRACE_PDI_LOAD, i, result.status, result.detail);

            if (result.outcome == RP1_PDI_RESULT_OK) {
                /* Record the now-active image for the dispatch guard. */
                g_active_image_id = p->image_id;
                set_node_status(i, RP1_NODE_DONE);
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                    return -1;
            } else {
                set_node_status(i, RP1_NODE_ERROR);
                if (result.outcome == RP1_PDI_RESULT_TIMEOUT) {
                    uint32_t timeout = p->timeout_cycles ?
                                       p->timeout_cycles :
                                       RP1_DEFAULT_PDI_TIMEOUT_TICKS;
                    rp1_latch_error(RP1_ERR_PDI_TIMEOUT, i, 0u, timeout);
                    if (write_cq_entry(node->flags, i,
                                       RP1_CQ_TIMEOUT, 0) < 0)
                        return -1;
                } else {
                    rp1_latch_error(RP1_ERR_PDI_FAILED, i,
                                    result.status, result.detail);
                    if (write_cq_entry(node->flags, i, RP1_CQ_ERROR,
                                       result.status) < 0)
                        return -1;
                }

                if (node->flags & RP1_FLAG_HALT_ON_ERROR)
                    return -1;

                /* Non-fatal: set barriers so dependents can proceed. */
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            }
            made_progress = 1;
            break;
        }

        /*
         * Phase 2: control packets re-arm ranges, publish conditional barriers,
         * redirect scanning, or park until an external signal becomes true.
         */
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
            trace(RP1_TRACE_LOOP_ITER, i, lp->loop_id,
                  (g_loop_iters[lp->loop_id] << 1) | (uint32_t)exit_loop);

            if (exit_loop) {
                set_node_status(i, RP1_NODE_DONE);
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                    return -1;
            } else {
                for (uint8_t b = lp->bucket_clear_start;
                     b <= lp->bucket_clear_end; b++)
                    g_barriers[b] = 0;
                for (uint32_t n = lp->body_start; n <= lp->body_end; n++)
                    set_node_status(n, RP1_NODE_PENDING);
                set_node_status(i, RP1_NODE_DONE);
                /* Do NOT set barrier_set — body + RERUN must fire first. */
            }
            made_progress = 1;
            break;
        }

        case RP1_OP_COND: {
            const rp1_payload_cond_t *cd = &node->payload.cond;
            uint32_t sig_val = g_signals[cd->condition_signal].value;
            uint32_t cond_met = compare(sig_val, cd->condition_op, cd->condition_value);

            trace(RP1_TRACE_COND_EVAL, i, cd->condition_signal, cond_met);
            if (cond_met) {
                /* Condition met — set done barriers. */
                g_barriers[cd->done_bucket] |= cd->done_mask;
            } else {
                /* Condition not met — clear body for execution. */
                for (uint8_t b = cd->bucket_clear_start;
                     b <= cd->bucket_clear_end; b++)
                    g_barriers[b] = 0;
                for (uint32_t n = cd->body_start; n <= cd->body_end; n++)
                    set_node_status(n, RP1_NODE_PENDING);
            }
            set_node_status(i, RP1_NODE_DONE);
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                return -1;
            made_progress = 1;
            break;
        }

        case RP1_OP_RERUN: {
            const rp1_payload_rerun_t *rr = &node->payload.rerun;
            set_node_status(rr->target_node, RP1_NODE_PENDING);
            if (rr->rerun_flags & RP1_RERUN_CLEAR_STATE)
                g_loop_iters[rr->loop_id] = 0;
            set_node_status(i, RP1_NODE_DONE);
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                return -1;
            made_progress = 1;
            break;
        }

        case RP1_OP_WAIT: {
            const rp1_payload_wait_t *w = &node->payload.wait;
            if (compare(g_signals[w->condition_signal].value,
                        w->condition_op, w->condition_value)) {
                set_node_status(i, RP1_NODE_DONE);
                g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
                if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                    return -1;
                made_progress = 1;
            } else {
                /* Park the node; check_waits() re-polls the slot each pass. */
                set_node_status(i, RP1_NODE_WAITING);
                trace(RP1_TRACE_WAIT_PARK, i,
                      w->condition_signal, w->condition_value);
            }
            break;
        }

        case RP1_OP_HALT:
            set_node_status(i, RP1_NODE_DONE);
            if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                return -1;
            return -2;

        /*
         * Phase 3: remaining packets complete synchronously, so side effects,
         * status, barriers, and CQ publication all happen in this scan pass.
         */
        default: /* NOP, SIGNAL, SCALAR_*, DMA_* */
            execute_immediate(node, i);
            set_node_status(i, RP1_NODE_DONE);
            g_barriers[node->barrier_set_bucket] |= node->barrier_set_mask;
            if (write_cq_entry(node->flags, i, RP1_CQ_OK, 0) < 0)
                return -1;
            made_progress = 1;
            break;
        }
    }

    return made_progress;
}

/*
 * Fatal quiescence schedules nothing new, then classifies tracked work:
 * completed finite kernels retain CQ evidence; pending finite kernels are
 * polled to their deadline; infinite or expired work requires device recovery.
 * CQ-full work waits for host draining, and no completion releases barriers.
 */
#ifdef QEMU_SEMIHOSTING
static void quiesce_inflight(const rp1_hooks_t *hooks)
#else
static void quiesce_inflight(void)
#endif
{
    while (g_inflight_count != 0u) {
        uint32_t i = 0u;

        while (i < g_inflight_count) {
            rp1_inflight_t *kernel = &g_inflight[i];
            uint16_t flags = g_nodes[kernel->node_index].flags;

            if (kernel->infinite) {
                rp1_mark_recovery_required();
                remove_inflight(i);
                continue;
            }

            int available = cq_can_write(flags);
            if (available < 0) {
                rp1_mark_recovery_required();
                remove_inflight(i);
                continue;
            }
            if (available == 0) {
                i++;
                continue;
            }

            uint32_t control =
                rp1_mmio_read32(kernel->base_addr + 0x00u);
            if ((control & 0x2u) != 0u) {
                if (g_node_status[kernel->node_index] != RP1_NODE_ERROR) {
                    set_node_status(kernel->node_index, RP1_NODE_DONE);
                    if (write_cq_entry(flags, kernel->node_index,
                                       RP1_CQ_OK, 0u) < 0)
                        rp1_mark_recovery_required();
                }
                trace(RP1_TRACE_KERNEL_DONE, kernel->node_index,
                      kernel->base_addr, 0u);
                remove_inflight(i);
                continue;
            }

            if (rp1_timeout_elapsed(kernel->timeout_start,
                                    kernel->timeout_cycles,
                                    rp1_cycles())) {
                if (g_node_status[kernel->node_index] != RP1_NODE_ERROR) {
                    set_node_status(kernel->node_index, RP1_NODE_ERROR);
                    if (write_cq_entry(flags, kernel->node_index,
                                       RP1_CQ_TIMEOUT,
                                       kernel->base_addr) < 0)
                        rp1_mark_recovery_required();
                }
                rp1_mark_recovery_required();
                remove_inflight(i);
                continue;
            }
            i++;
        }

#ifdef QEMU_SEMIHOSTING
        if (hooks && hooks->on_scan_pass)
            hooks->on_scan_pass();
#endif
        g_ctrl->heartbeat++;
    }
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
    uint32_t bad_node = RP1_TERMINAL_ERROR_NODE_NONE;
    uint32_t detail = 0u;
    uint32_t aux = 0u;

    /*
     * Validation is an all-or-nothing phase before activation. Invalid packets
     * publish one forced CQ error when cursor state is usable; a merely full
     * ring waits for space instead of dropping node-level evidence.
     */
    if (validate_nodes(node_count, &bad_node, &detail, &aux) != 0) {
        rp1_latch_error(RP1_ERR_INVALID_NODE, bad_node, detail, aux);
        set_node_status(bad_node, RP1_NODE_ERROR);
        while (write_cq_entry(0u, bad_node,
                              RP1_CQ_ERROR, detail) == 0) {
#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_scan_pass)
                hooks->on_scan_pass();
#endif
            g_ctrl->heartbeat++;
        }
        return -1;
    }

    while (1) {
        g_cq_blocked = 0u;
        /*
         * Each pass activates ready nodes, harvests asynchronous kernels, then
         * wakes parked waits. Any fatal phase quiesces tracked work before
         * returning; CQ backpressure alone always retries.
         */
        int activated = activate_nodes(node_count);
        if (activated < 0) {
#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_scan_pass)
                hooks->on_scan_pass();
            quiesce_inflight(hooks);
#else
            quiesce_inflight();
#endif
            return activated;
        }

        int inflight_progress = check_inflight();
        if (inflight_progress < 0) {
#ifdef QEMU_SEMIHOSTING
            if (hooks && hooks->on_scan_pass)
                hooks->on_scan_pass();
            quiesce_inflight(hooks);
#else
            quiesce_inflight();
#endif
            return inflight_progress;
        }

        int wait_progress = check_waits(node_count);
        if (wait_progress < 0) {
#ifdef QEMU_SEMIHOSTING
            quiesce_inflight(hooks);
#else
            quiesce_inflight();
#endif
            return -1;
        }

#ifdef QEMU_SEMIHOSTING
        if (hooks && hooks->on_scan_pass)
            hooks->on_scan_pass();
#endif

        if (!activated && !inflight_progress && !wait_progress &&
            !g_cq_blocked) {
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
        }

        g_ctrl->heartbeat++;
    }
}
