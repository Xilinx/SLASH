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
#include "rp1_scheduler.h"
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Condition evaluation  (shared with LOOP, COND)
 * ---------------------------------------------------------------------- */

static uint32_t compare(uint32_t sig, uint8_t op, uint32_t val)
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
 * Inflight kernel management
 * ---------------------------------------------------------------------- */

/*
 * A cached base has had sticky ap_done cleared before its latest launch.
 * g_inflight supplies the RUNNING state; its completion read clears ap_done
 * and makes the retained cache entry CLEAN again. The physical design exposes
 * at most 15 CUs, so the 32-entry inflight bound also bounds this cache.
 */
static uint32_t
    g_clean_cu_bases[RP1_MAX_INFLIGHT] __attribute__((section(".btcm")));

/* Number of valid entries at the front of g_clean_cu_bases. */
static uint32_t g_clean_cu_count __attribute__((section(".btcm")));

/*
 * Invalidate the persistent CU state without clearing dead entries. The count
 * is authoritative, so startup and PDI transitions remain constant-time.
 */
void rp1_cu_tracking_reset(void)
{
    g_clean_cu_count = 0u;
}

/*
 * Return non-zero while the named CU already has a tracked invocation.
 * Dispatches to one physical control port must serialize even when their graph
 * barriers are independent.
 */
static uint32_t cu_is_inflight(uint32_t base_addr)
{
    for (uint32_t i = 0u; i < g_inflight_count; i++) {
        if (g_inflight[i].base_addr == base_addr)
            return 1u;
    }
    return 0u;
}

/*
 * Clear an unrecognized CU's sticky completion before first use. Known CUs
 * skip both the NoC read and its ordering barrier; the completion read which
 * removed their preceding inflight entry already performed the clear.
 *
 * Cache exhaustion is a safe performance fallback: leave the base untracked
 * so every later launch repeats the clear instead of assuming clean state.
 */
static void prepare_cu_for_launch(uint32_t base_addr)
{
    for (uint32_t i = 0u; i < g_clean_cu_count; i++) {
        if (g_clean_cu_bases[i] == base_addr)
            return;
    }

    (void)rp1_mmio_read32(base_addr + 0x00u);
    rp1_dmb_sy();

    if (g_clean_cu_count < RP1_MAX_INFLIGHT)
        g_clean_cu_bases[g_clean_cu_count++] = base_addr;
}

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
    slot->infinite =
        (rp1_node_get_flags(node) & RP1_FLAG_INFINITE) ? 1 : 0;
    slot->settle_polls = 0;
}

static void remove_inflight(uint32_t idx)
{
    g_inflight_count--;
    if (idx < g_inflight_count)
        g_inflight[idx] = g_inflight[g_inflight_count];
}

static void set_node_status(uint32_t node_index, uint8_t status)
{
    rp1_node_set_status(&g_nodes[node_index], status);
    if (status != RP1_NODE_PENDING)
        rp1_scheduler_remove_node(node_index);
}

/*
 * Finish one successful node execution. Counts are per execution rather than
 * per node, so LOOP/RERUN cycles contribute on every pass.
 */
static void complete_node(uint32_t node_index)
{
    set_node_status(node_index, RP1_NODE_DONE);
    g_completed_operations++;
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

    /*
     * HLS ap_done is sticky and clear-on-read. Pay that NoC read once per CU
     * reset epoch; normal completion polling keeps a reused CU clean.
     */
    prepare_cu_for_launch(kd->kernel_base_addr);

    for (uint16_t i = 0; i < kd->arg_count; i++)
        rp1_mmio_write32(kd->kernel_base_addr + args[i].reg_offset,
                         args[i].value);

    rp1_dmb_st();
    rp1_mmio_write32(kd->kernel_base_addr + 0x00, 0x01); /* ap_start */
}

/* -------------------------------------------------------------------------
 * Immediate-completion opcodes (NOP, SIGNAL, SCALAR_*, DMA_*)
 * ---------------------------------------------------------------------- */

static void execute_immediate(const rp1_node_t *node, uint32_t node_index)
{
    switch (rp1_node_get_opcode(node)) {
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
#pragma GCC unroll 2
        for (uint32_t w = 0; w < RP1_SCALAR_WRITE_MAX; w++) {
            if (!p->writes[w].addr)
                break;
            rp1_mmio_write32(p->writes[w].addr, p->writes[w].value);
        }
        rp1_dsb_st();
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
        rp1_dsb_st();
        break;
    }

    case RP1_OP_DMA_COPY: {
        const rp1_payload_dma_copy_t *p = &node->payload.dma_copy;
        /* Phase 1: DDR-DDR software memcpy (32-bit addresses only). */
        uint32_t *src = (uint32_t *)(uintptr_t)p->src_addr_lo;
        uint32_t *dst = (uint32_t *)(uintptr_t)p->dst_addr_lo;
        uint32_t words = rp1_dma_get_length(p->length_types) / 4u;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = src[w];
        rp1_dsb_st();
        break;
    }

    case RP1_OP_DMA_FILL: {
        const rp1_payload_dma_fill_t *p = &node->payload.dma_fill;
        uint32_t *dst = (uint32_t *)(uintptr_t)p->dst_addr_lo;
        uint32_t words = p->length / 4;
        for (uint32_t w = 0; w < words; w++)
            dst[w] = p->pattern;
        rp1_dsb_st();
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Packet validation
 * ---------------------------------------------------------------------- */

static uint32_t valid_condition(uint8_t op)
{
    return op <= RP1_COP_AND_Z;
}

static uint32_t valid_signal_slot(uint32_t slot)
{
    return slot < RP1_MAX_SIGNALS;
}

/*
 * Validate one endpoint for the phase-1 software DMA path. Only low-word DDR
 * addresses are implemented. Word-aligned zero-byte transfers are valid, and
 * non-empty ranges may reach but must not wrap past the 32-bit address space.
 */
static uint32_t valid_phase1_dma_range(uint32_t lo, uint32_t hi,
                                       uint32_t bytes)
{
    return hi == 0u && ((lo | bytes) & 3u) == 0u &&
           (uint64_t)lo + bytes <= (1ULL << 32);
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
        uint16_t opcode = rp1_node_get_opcode(node);
        uint8_t flags = rp1_node_get_flags(node);
        /*
         * Phase 1: reject reserved control bits, opcode-specific flag misuse,
         * and invalid barrier buckets before interpreting the payload union.
         */
        if ((rp1_node_get_control(node) & RP1_NODE_RESERVED_MASK) != 0u ||
            (flags & (uint8_t)~RP1_FLAG_INFINITE) != 0u ||
            ((flags & RP1_FLAG_INFINITE) != 0u &&
             opcode != RP1_OP_KERNEL_DISPATCH)) {
            *bad_node = i;
            *detail = RP1_NODE_BAD_OPERATION;
            *aux = rp1_node_get_control(node);
            return -1;
        }
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
        switch (opcode) {
        case RP1_OP_NOP:
        case RP1_OP_SCALAR_WRITE:
        case RP1_OP_PDI_LOAD:
        case RP1_OP_HALT:
            break;
        case RP1_OP_DMA_COPY: {
            const rp1_payload_dma_copy_t *dma = &node->payload.dma_copy;
            uint32_t packed = dma->length_types;
            uint32_t length = rp1_dma_get_length(packed);
            if (rp1_dma_get_src_type(packed) == 0u &&
                rp1_dma_get_dst_type(packed) == 0u &&
                valid_phase1_dma_range(
                    dma->src_addr_lo, dma->src_addr_hi, length) &&
                valid_phase1_dma_range(
                    dma->dst_addr_lo, dma->dst_addr_hi, length))
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_ARGUMENTS;
            *aux = packed;
            return -1;
        }
        case RP1_OP_DMA_FILL: {
            const rp1_payload_dma_fill_t *dma = &node->payload.dma_fill;
            if (dma->dst_type == 0u &&
                valid_phase1_dma_range(
                    dma->dst_addr_lo, dma->dst_addr_hi, dma->length))
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_ARGUMENTS;
            *aux = dma->dst_type;
            return -1;
        }
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
                kd->ctrl_flags == 0u &&
                (kd->arg_buffer_offset & 7u) == 0u &&
                kd->arg_buffer_offset <= arg_available &&
                bytes <= arg_available - kd->arg_buffer_offset)
                break;
            *bad_node = i;
            *detail = RP1_NODE_BAD_ARGUMENTS;
            *aux = kd->arg_buffer_offset;
            return -1;
        }
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
                (node->payload.rerun.rerun_flags &
                 (uint8_t)~RP1_RERUN_CLEAR_STATE) == 0u &&
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
            *aux = opcode;
            return -1;
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Inflight kernel polling
 * ---------------------------------------------------------------------- */

/*
 * Cases are complete, expired, or still pending. Every finite timeout is
 * fatal; the timed-out tracker remains present so quiescence can determine
 * whether it subsequently finished or requires recovery.
 *
 * Returns 1 for progress, 0 for none, and -1 for a fatal timeout. PMU elapsed
 * ticks, not scan passes, define every deadline.
 */
static int check_inflight(void)
{
    uint32_t i = 0;
    int made_progress = 0;

    while (i < g_inflight_count) {
        rp1_inflight_t *k = &g_inflight[i];
        uint32_t ctrl = rp1_mmio_read32(k->base_addr + 0x00);

        if (ctrl & 0x2) { /* ap_done */
            if (!k->infinite) {
                complete_node(k->node_index);
                rp1_scheduler_set_barriers(k->set_bucket, k->set_mask);
            }
            rp1_trace_emit(RP1_TRACE_KERNEL_DONE, k->node_index,
                           k->base_addr, k->infinite);
            remove_inflight(i);
            made_progress = 1;
            /* don't increment i — slot was replaced by swap */
        } else {
            uint32_t now = rp1_cycles();
            if (rp1_timeout_elapsed(k->timeout_start,
                                    k->timeout_cycles, now)) {
                set_node_status(k->node_index, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_KERNEL_TIMEOUT, k->node_index,
                                k->base_addr, k->timeout_cycles);
                rp1_trace_emit(RP1_TRACE_KERNEL_TIMEOUT, k->node_index,
                               k->base_addr, k->timeout_cycles);
                return -1;
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
    uint32_t cursor = 0u;

    while (cursor < node_count) {
        uint32_t i = cursor;
        if (rp1_scheduler_enabled()) {
            if (!rp1_scheduler_next_waiting(cursor, &i) || i >= node_count)
                break;
            cursor = i + 1u;
        } else {
            cursor++;
            if (rp1_node_get_status(&g_nodes[i]) != RP1_NODE_WAITING)
                continue;
        }

        const rp1_node_t *node = &g_nodes[i];
        const rp1_payload_wait_t *w = &node->payload.wait;
        uint32_t sig_val = g_signals[w->condition_signal].value;
        if (compare(sig_val, w->condition_op, w->condition_value)) {
            complete_node(i);
            rp1_scheduler_set_barriers(
                node->barrier_set_bucket, node->barrier_set_mask);
            rp1_trace_emit(RP1_TRACE_WAIT_WAKE, i,
                           w->condition_signal, sig_val);
            made_progress = 1;
        }
    }

    return made_progress;
}

/* -------------------------------------------------------------------------
 * Node activation (one full scan pass)
 *
 * Eligible packets split into asynchronous kernel work, synchronous PDI work,
 * scanner control, and immediate operations. Any error returns immediately;
 * the caller then quiesces work already launched by this graph.
 *
 * Returns 1 for progress, 0 for none, -1 for error, and -2 for HALT.
 * ---------------------------------------------------------------------- */

static int activate_nodes(uint32_t node_count)
{
    int made_progress = 0;
    uint32_t scan_index = 0u;

    while (1) {
        uint32_t i;
        if (rp1_scheduler_enabled()) {
            if (!rp1_scheduler_pop_ready(&i))
                break;
        } else {
            if (scan_index >= node_count)
                break;
            i = scan_index++;
        }
        if (rp1_node_get_status(&g_nodes[i]) != RP1_NODE_PENDING)
            continue;

        const rp1_node_t *node = &g_nodes[i];
        uint8_t opcode = rp1_node_get_opcode(node);
        uint8_t flags = rp1_node_get_flags(node);

        if ((g_barriers[node->barrier_await_bucket] & node->barrier_await_mask)
                != node->barrier_await_mask) {
            rp1_scheduler_rearm_node(i);
            continue;
        }

        /*
         * Resource readiness is separate from graph barriers. Skip before
         * publishing activation so a busy CU cannot produce duplicate trace
         * events or receive a second ap_start.
         */
        if (opcode == RP1_OP_KERNEL_DISPATCH &&
            cu_is_inflight(node->payload.kernel_dispatch.kernel_base_addr)) {
            rp1_scheduler_defer_ready(i);
            continue;
        }

        g_ctrl->rp1_current_node = i;
        g_operation_started = 1u;
        rp1_trace_emit(RP1_TRACE_NODE_ACTIVATE, i,
                       opcode, flags);

        /*
         * Phase 1: launch asynchronous fabric work or perform the serialized
         * platform-image transition; both may establish later dispatch state.
         */
        switch (opcode) {

        case RP1_OP_KERNEL_DISPATCH: {
            /* Expected-image guard: a dispatch that names an image (non-zero)
             * must match the image last installed by PDI_LOAD. Fail fast
             * instead of poking an absent kernel and hanging. */
            const rp1_payload_kernel_dispatch_t *kd = &node->payload.kernel_dispatch;
            if (kd->expected_image_id != 0u &&
                (g_active_image_state != RP1_IMAGE_STATE_KNOWN ||
                 kd->expected_image_id != g_active_image_id)) {
                set_node_status(i, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_IMAGE_MISMATCH, i,
                                kd->expected_image_id, g_active_image_id);
                rp1_trace_emit(RP1_TRACE_IMAGE_MISMATCH, i,
                               kd->expected_image_id, g_active_image_id);
                return -1;
            }
            if (g_inflight_count >= RP1_MAX_INFLIGHT) {
                set_node_status(i, RP1_NODE_ERROR);
                rp1_latch_error(RP1_ERR_INFLIGHT_FULL, i,
                                g_inflight_count, RP1_MAX_INFLIGHT);
                return -1;
            }
            launch_kernel(node);
            rp1_trace_emit(RP1_TRACE_KERNEL_LAUNCH, i,
                           kd->kernel_base_addr, kd->arg_count);
            if (flags & RP1_FLAG_INFINITE) {
                complete_node(i);
                rp1_scheduler_set_barriers(
                    node->barrier_set_bucket, node->barrier_set_mask);
            } else {
                set_node_status(i, RP1_NODE_DISPATCHED);
            }
            add_inflight(node, i);
            made_progress = 1;
            break;
        }

        case RP1_OP_PDI_LOAD: {
            const rp1_payload_pdi_load_t *p = &node->payload.pdi_load;
            /*
             * PLM may alter or partially alter the fabric on every attempt.
             * Forget all control-port state before issuing the request.
             */
            rp1_cu_tracking_reset();
            rp1_pdi_result_t result =
                rp1_pdi_load(p->pdi_addr_lo, p->pdi_addr_hi,
                             p->timeout_cycles);
            rp1_trace_emit(RP1_TRACE_PDI_LOAD, i,
                           result.status, result.detail);

            if (result.outcome == RP1_PDI_RESULT_OK) {
                /* A successful named image restores a previously unknown state. */
                g_active_image_id = p->image_id;
                g_active_image_state = p->image_id != 0u ?
                                       RP1_IMAGE_STATE_KNOWN :
                                       RP1_IMAGE_STATE_NONE;
                complete_node(i);
                rp1_scheduler_set_barriers(
                    node->barrier_set_bucket, node->barrier_set_mask);
            } else {
                /*
                 * A timed-out or rejected reconfiguration can leave physical
                 * state partially changed. Forget the previous image before
                 * entering fatal quiescence.
                 */
                g_active_image_id = 0u;
                g_active_image_state = RP1_IMAGE_STATE_UNKNOWN;
                set_node_status(i, RP1_NODE_ERROR);
                if (result.outcome == RP1_PDI_RESULT_TIMEOUT) {
                    uint32_t timeout = p->timeout_cycles ?
                                       p->timeout_cycles :
                                       RP1_DEFAULT_PDI_TIMEOUT_TICKS;
                    /*
                     * The observation bit can remain asserted after timeout,
                     * so a delayed PLM response may still reconfigure the
                     * device. Only reset can establish a safe image state.
                     */
                    rp1_mark_recovery_required();
                    rp1_latch_error(RP1_ERR_PDI_TIMEOUT, i, 0u, timeout);
                } else {
                    rp1_latch_error(RP1_ERR_PDI_FAILED, i,
                                    result.status, result.detail);
                }
                return -1;
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
            rp1_trace_emit(
                RP1_TRACE_LOOP_ITER, i, lp->loop_id,
                (g_loop_iters[lp->loop_id] << 1) |
                    (uint32_t)exit_loop);

            if (exit_loop) {
                complete_node(i);
                rp1_scheduler_set_barriers(
                    node->barrier_set_bucket, node->barrier_set_mask);
            } else {
                for (uint8_t b = lp->bucket_clear_start;
                     b <= lp->bucket_clear_end; b++)
                    rp1_scheduler_clear_barriers(b, UINT32_MAX);
                for (uint32_t n = lp->body_start; n <= lp->body_end; n++) {
                    set_node_status(n, RP1_NODE_PENDING);
                    rp1_scheduler_rearm_node(n);
                }
                complete_node(i);
                /* Do NOT set barrier_set — body + RERUN must fire first. */
            }
            made_progress = 1;
            break;
        }

        case RP1_OP_COND: {
            const rp1_payload_cond_t *cd = &node->payload.cond;
            uint32_t sig_val = g_signals[cd->condition_signal].value;
            uint32_t cond_met = compare(sig_val, cd->condition_op, cd->condition_value);

            rp1_trace_emit(RP1_TRACE_COND_EVAL, i,
                           cd->condition_signal, cond_met);
            if (cond_met) {
                /* Condition met — set done barriers. */
                rp1_scheduler_set_barriers(
                    cd->done_bucket, cd->done_mask);
            } else {
                /* Condition not met — clear body for execution. */
                for (uint8_t b = cd->bucket_clear_start;
                     b <= cd->bucket_clear_end; b++)
                    rp1_scheduler_clear_barriers(b, UINT32_MAX);
                for (uint32_t n = cd->body_start; n <= cd->body_end; n++) {
                    set_node_status(n, RP1_NODE_PENDING);
                    rp1_scheduler_rearm_node(n);
                }
            }
            complete_node(i);
            rp1_scheduler_set_barriers(
                node->barrier_set_bucket, node->barrier_set_mask);
            made_progress = 1;
            break;
        }

        case RP1_OP_RERUN: {
            const rp1_payload_rerun_t *rr = &node->payload.rerun;
            set_node_status(rr->target_node, RP1_NODE_PENDING);
            rp1_scheduler_rearm_node(rr->target_node);
            if (rr->rerun_flags & RP1_RERUN_CLEAR_STATE)
                g_loop_iters[rr->loop_id] = 0;
            complete_node(i);
            rp1_scheduler_set_barriers(
                node->barrier_set_bucket, node->barrier_set_mask);
            made_progress = 1;
            break;
        }

        case RP1_OP_WAIT: {
            const rp1_payload_wait_t *w = &node->payload.wait;
            if (compare(g_signals[w->condition_signal].value,
                        w->condition_op, w->condition_value)) {
                complete_node(i);
                rp1_scheduler_set_barriers(
                    node->barrier_set_bucket, node->barrier_set_mask);
                made_progress = 1;
            } else {
                /* Park the node; check_waits() re-polls the slot each pass. */
                set_node_status(i, RP1_NODE_WAITING);
                rp1_scheduler_park_wait(i);
                rp1_trace_emit(RP1_TRACE_WAIT_PARK, i,
                               w->condition_signal,
                               w->condition_value);
            }
            break;
        }

        case RP1_OP_HALT:
            complete_node(i);
            g_ctrl->terminal_error_node = i;
            g_terminal_opcode = RP1_OP_HALT;
            return -2;

        /*
         * Phase 3: remaining packets complete synchronously, so side effects,
         * status, and barriers all complete in this scan pass.
         */
        default: /* NOP, SIGNAL, SCALAR_*, DMA_* */
            execute_immediate(node, i);
            complete_node(i);
            rp1_scheduler_set_barriers(
                node->barrier_set_bucket, node->barrier_set_mask);
            made_progress = 1;
            break;
        }
    }

    rp1_scheduler_restore_deferred();
    return made_progress;
}

/*
 * Fatal quiescence schedules nothing new, then classifies tracked work:
 * completed finite kernels count as done; pending finite kernels are polled to
 * their deadline; infinite or expired work requires recovery. No completion
 * releases barriers because activation has already stopped.
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

            if (kernel->infinite) {
                g_quiesce_infinite++;
                rp1_mark_recovery_required();
                remove_inflight(i);
                continue;
            }

            uint32_t control =
                rp1_mmio_read32(kernel->base_addr + 0x00u);
            if ((control & 0x2u) != 0u) {
                if (rp1_node_get_status(&g_nodes[kernel->node_index]) !=
                    RP1_NODE_ERROR)
                    complete_node(kernel->node_index);
                g_quiesce_finite_done++;
                rp1_trace_emit(RP1_TRACE_KERNEL_DONE,
                               kernel->node_index,
                               kernel->base_addr, 0u);
                remove_inflight(i);
                continue;
            }

            if (rp1_timeout_elapsed(kernel->timeout_start,
                                    kernel->timeout_cycles,
                                    rp1_cycles())) {
                if (rp1_node_get_status(&g_nodes[kernel->node_index]) !=
                    RP1_NODE_ERROR)
                    set_node_status(kernel->node_index, RP1_NODE_ERROR);
                g_quiesce_finite_timeout++;
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
    uint32_t node_count = g_node_count;
    uint32_t bad_node = RP1_TERMINAL_ERROR_NODE_NONE;
    uint32_t detail = 0u;
    uint32_t aux = 0u;

    /*
     * Validation is an all-or-nothing phase before activation. Invalid packets
     * latch the first error and return before any node can affect hardware.
     */
    if (validate_nodes(node_count, &bad_node, &detail, &aux) != 0) {
        rp1_latch_error(RP1_ERR_INVALID_NODE, bad_node, detail, aux);
        set_node_status(bad_node, RP1_NODE_ERROR);
        return -1;
    }

    /*
     * Sparse graphs use a reverse barrier index and ready bitset. Dense graphs
     * above the local ATCM ceiling retain the scanner without changing ABI or
     * execution semantics.
     */
    (void)rp1_scheduler_build(node_count);

    while (1) {
        /*
         * Each pass activates ready nodes, harvests asynchronous kernels, then
         * wakes parked waits. Any fatal phase quiesces tracked work before
         * returning.
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

        if (!activated && !inflight_progress && !wait_progress) {
            /* No scan progress — keep looping while kernels are in flight or a
             * WAIT is still gated on a signal a peer/host may yet raise. */
            uint32_t has_dispatched = 0;
            uint32_t has_waiting = 0;
            for (uint32_t i = 0; i < node_count; i++) {
                uint8_t st = rp1_node_get_status(&g_nodes[i]);
                if (st == RP1_NODE_DISPATCHED) has_dispatched = 1;
                else if (st == RP1_NODE_WAITING) has_waiting = 1;
            }
            if (!has_dispatched && !has_waiting)
                return 0; /* graph complete */
        }

        g_ctrl->heartbeat++;
    }
}
