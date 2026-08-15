/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * End-to-end graph tests for the RP1 flat scanner, running under Xilinx
 * QEMU with ARM semihosting.  Each test builds a tiny graph in shared
 * DDR, hands it to rp1_run() via the on_scan_pass / on_graph_done hooks
 * defined in rp1_loop.h, and asserts on the resulting node statuses,
 * barriers, signal slots, CQ entries, and dispatch order.
 *
 * The hook completes fake "kernels" by OR-ing 0x2 (ap_done) into the
 * ctrl-reg word of each in-flight kernel after activate_nodes() has
 * dispatched it.  The kernels themselves are just RAM pages at
 * FAKE_KERNEL_BASE; the firmware writes to them through axi_write32()
 * (a plain volatile store), which works fine over QEMU RAM.
 */

#ifdef QEMU_SEMIHOSTING

#include "rp1_test.h"
#include "rp1_hal.h"
#include "rp1_store.h"
#include "rp1_run.h"
#include "rp1_pdi.h"

#include <slash/uapi/rp1_protocol.h>

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * DDR layout for the test graphs.
 *
 * Mirrors the protocol defaults so the test environment matches what the
 * host stack will eventually program into the control block.  The
 * control block sits at RP1_CTRL_PHYS_ADDR; nodes / CQ / args / signals
 * follow at the documented offsets.
 * ---------------------------------------------------------------------- */

#define G_CTRL  ((volatile rp1_ctrl_t *)(uintptr_t)(RP1_CTRL_PHYS_ADDR))
#define G_NODES ((rp1_node_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET))
#define G_CQ    ((volatile rp1_cq_entry_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET))
#define G_ARGS  ((uint32_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET))
#define G_SIGS  ((volatile rp1_signal_slot_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET))
#define G_TRACE ((volatile rp1_trace_entry_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET))

#define TEST_CQ_SIZE  64u
#define TEST_TRACE_SIZE 128u

/* Fake AXI-Lite kernel: 256-byte page per kernel, word 0 is the
 * ap_start/ap_done control reg, word 4 (offset 0x10) is arg 0.  Lives in
 * QEMU RAM well clear of the BAR window. */
#define FAKE_KERNEL_BASE   0x40000000UL
#define FAKE_KERNEL_STRIDE 0x100UL
#define FAKE_KERNEL(i)     (FAKE_KERNEL_BASE + (uintptr_t)(i) * FAKE_KERNEL_STRIDE)

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static void tmemzero(volatile void *dst, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    while (len--) *p++ = 0;
}

/* -------------------------------------------------------------------------
 * Hook state — fake-kernel completion + dispatch tracing.
 *
 * on_scan_pass fires after activate_nodes() and check_inflight() in each
 * iteration of the dispatch loop, which gives us a single observation
 * point per scan.  We use it to (a) note which nodes have left PENDING,
 * (b) record the peak number of in-flight kernels (the proxy for "B and
 * C dispatched in parallel"), and (c) flip ap_done on any in-flight
 * fake kernel so the next iteration's check_inflight() finalises it.
 * ---------------------------------------------------------------------- */

#define TRACE_MAX 64u

static uint32_t s_trace[TRACE_MAX];
static uint32_t s_trace_count;
static uint32_t s_seen[TRACE_MAX / 32u];  /* bitmask of nodes already traced */
static uint32_t s_max_inflight;
static uint32_t s_node_count;
static int      s_graph_done_returns;
static uint32_t s_pass_count;
static uint32_t s_skip_completion_node;
static uint32_t s_drain_cq;
static rp1_cq_entry_t s_cq_capture[128];
static uint32_t s_cq_capture_count;

/* WAIT-arming: after s_wait_after scan passes, write s_wait_value into signal
 * slot s_wait_slot.  s_wait_seen_slot captures the witness slot's value at the
 * moment we arm, proving a downstream node gated on the WAIT had not yet run. */
static int      s_wait_armed;
static uint32_t s_wait_after;
static uint32_t s_wait_slot;
static uint32_t s_wait_value;
static uint32_t s_wait_witness_slot;
static uint32_t s_wait_witness_at_fire;

static void hook_reset(uint32_t node_count)
{
    s_trace_count = 0;
    s_max_inflight = 0;
    s_node_count = node_count;
    s_graph_done_returns = 1;   /* default: exit rp1_run after one graph */
    s_pass_count = 0;
    s_skip_completion_node = RP1_TERMINAL_ERROR_NODE_NONE;
    s_drain_cq = 0;
    s_cq_capture_count = 0;
    s_wait_armed = 0;
    s_wait_after = 0;
    s_wait_slot = 0;
    s_wait_value = 0;
    s_wait_witness_slot = 0;
    s_wait_witness_at_fire = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < TRACE_MAX / 32u; i++) s_seen[i] = 0;
    for (uint32_t i = 0; i < TRACE_MAX; i++) s_trace[i] = 0;
    for (uint32_t i = 0; i < 128u; i++) {
        s_cq_capture[i].node_index = 0;
        s_cq_capture[i].status = 0;
        s_cq_capture[i].error_detail = 0;
        s_cq_capture[i].timestamp = 0;
    }
}

static void hook_on_scan_pass(void)
{
    s_pass_count++;

    if (s_drain_cq) {
        uint32_t cursor = G_CTRL->cq_read_idx;
        uint32_t end = G_CTRL->cq_write_idx;
        while (cursor != end && s_cq_capture_count < 128u) {
            s_cq_capture[s_cq_capture_count++] =
                G_CQ[cursor & (G_CTRL->cq_size - 1u)];
            cursor++;
        }
        G_CTRL->cq_read_idx = end;
    }

    /* Cross-queue producer simulation: raise the awaited signal after a few
     * passes, recording the witness slot first to prove the WAIT held off its
     * dependents until now. */
    if (s_wait_armed && s_pass_count == s_wait_after) {
        s_wait_witness_at_fire = G_SIGS[s_wait_witness_slot].value;
        G_SIGS[s_wait_slot].value = s_wait_value;
    }

    if (g_inflight_count > s_max_inflight)
        s_max_inflight = g_inflight_count;

    for (uint32_t i = 0; i < s_node_count && i < TRACE_MAX; i++) {
        if (s_seen[i >> 5] & (1u << (i & 31u))) continue;
        uint8_t st = g_node_status[i];
        if (st == RP1_NODE_DISPATCHED || st == RP1_NODE_DONE) {
            s_seen[i >> 5] |= (1u << (i & 31u));
            s_trace[s_trace_count++] = i;
        }
    }

    /* Complete each in-flight fake kernel (ap_start -> ap_start | ap_done). */
    for (uint32_t i = 0; i < g_inflight_count; i++) {
        if (g_inflight[i].node_index == s_skip_completion_node)
            continue;
        volatile uint32_t *ctrl =
            (volatile uint32_t *)(uintptr_t)g_inflight[i].base_addr;
        if (*ctrl & 0x1u) *ctrl |= 0x2u;
    }
}

static int hook_on_graph_done(int result)
{
    (void)result;
    return s_graph_done_returns;
}

static const rp1_hooks_t s_hooks = {
    .on_scan_pass  = hook_on_scan_pass,
    .on_graph_done = hook_on_graph_done,
    .on_idle       = 0,
};

static void make_signal(rp1_node_t *n,
                        uint32_t slot, uint32_t value, uint16_t op,
                        uint8_t aw_b, uint32_t aw_m,
                        uint8_t st_b, uint32_t st_m);

static uint32_t s_wrap_graphs;

static int wrap_on_graph_done(int result)
{
    (void)result;
    s_wrap_graphs++;
    if (s_wrap_graphs == 1u) {
        tmemzero((volatile void *)&G_NODES[0], sizeof(rp1_node_t));
        make_signal(&G_NODES[0], 1u, 0x2222u, RP1_SIGOP_SET,
                    0, 0u, 0, 1u);
        G_CTRL->node_count = 1u;
        G_CTRL->cq_read_idx = G_CTRL->cq_write_idx;
        G_CTRL->graph_seq = 0u;
        return 0;
    }
    return 1;
}

static const rp1_hooks_t s_wrap_hooks = {
    .on_scan_pass = hook_on_scan_pass,
    .on_graph_done = wrap_on_graph_done,
    .on_idle = 0,
};

static uint32_t s_terminal_idle_calls;

static int terminal_resubmit_on_done(int result)
{
    (void)result;
    tmemzero((volatile void *)&G_NODES[0], sizeof(rp1_node_t));
    make_signal(&G_NODES[0], 63u, 0xBAD0BAD0u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->node_count = 1u;
    G_CTRL->graph_seq++;
    return 0;
}

static int terminal_exit_on_idle(void)
{
    s_terminal_idle_calls++;
    return s_terminal_idle_calls >= 2u;
}

static const rp1_hooks_t s_terminal_hooks = {
    .on_scan_pass = hook_on_scan_pass,
    .on_graph_done = terminal_resubmit_on_done,
    .on_idle = terminal_exit_on_idle,
};

/* -------------------------------------------------------------------------
 * Graph setup
 * ---------------------------------------------------------------------- */

static void setup_graph(uint32_t node_count, uint32_t fake_kernel_count)
{
    /* Wipe only the regions we touch.  rp1_run() resets the BTCM state
     * (barriers, node_status, loop_iters, inflight) on each new graph
     * submission via rp1_store_init(). */
    tmemzero((volatile void *)G_CTRL,  sizeof(rp1_ctrl_t));
    tmemzero((volatile void *)G_NODES, node_count * sizeof(rp1_node_t));
    tmemzero((volatile void *)G_CQ,    TEST_CQ_SIZE * sizeof(rp1_cq_entry_t));
    tmemzero((volatile void *)G_ARGS,  64u * sizeof(uint32_t));
    tmemzero((volatile void *)G_SIGS,  64u * sizeof(rp1_signal_slot_t));
    tmemzero((volatile void *)G_TRACE, TEST_TRACE_SIZE * sizeof(rp1_trace_entry_t));
    if (fake_kernel_count > 0) {
        tmemzero((volatile void *)(uintptr_t)FAKE_KERNEL_BASE,
                 fake_kernel_count * FAKE_KERNEL_STRIDE);
    }

    G_CTRL->cq_size           = TEST_CQ_SIZE;
    G_CTRL->node_count        = node_count;
    G_CTRL->node_base_lo      = (uint32_t)(uintptr_t)G_NODES;
    G_CTRL->cq_base_lo        = (uint32_t)(uintptr_t)G_CQ;
    G_CTRL->arg_buf_base_lo   = (uint32_t)(uintptr_t)G_ARGS;
    G_CTRL->sig_array_base_lo = (uint32_t)(uintptr_t)G_SIGS;
    G_CTRL->trace_base_lo     = (uint32_t)(uintptr_t)G_TRACE;
    G_CTRL->trace_size        = TEST_TRACE_SIZE;
    G_CTRL->graph_seq         = 1;

    hook_reset(node_count);
}

/* -------------------------------------------------------------------------
 * Node builders
 *
 * The flat scanner only reads what each opcode's payload defines, so we
 * touch exactly those fields and rely on setup_graph()'s tmemzero for
 * the rest.  Keeping the builders explicit avoids compound literals,
 * which can lower to a memset call under -ffreestanding -nostdlib.
 * ---------------------------------------------------------------------- */

static void make_kernel(rp1_node_t *n, uint32_t kernel_idx,
                        uint8_t aw_b, uint32_t aw_m,
                        uint8_t st_b, uint32_t st_m,
                        uint32_t arg_buf_offset, uint16_t arg_count)
{
    n->opcode               = RP1_OP_KERNEL_DISPATCH;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.kernel_dispatch.kernel_base_addr  = (uint32_t)FAKE_KERNEL(kernel_idx);
    n->payload.kernel_dispatch.arg_buffer_offset = arg_buf_offset;
    n->payload.kernel_dispatch.arg_count         = arg_count;
    n->payload.kernel_dispatch.ctrl_flags        = 0;
    n->payload.kernel_dispatch.timeout_cycles    = 0; /* default */
}

static void make_signal(rp1_node_t *n,
                        uint32_t slot, uint32_t value, uint16_t op,
                        uint8_t aw_b, uint32_t aw_m,
                        uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_SIGNAL;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.signal.target_slot = slot;
    n->payload.signal.value       = value;
    n->payload.signal.operation   = op;
}

static void make_scalar_write(rp1_node_t *n, uint32_t addr, uint32_t value,
                              uint8_t aw_b, uint32_t aw_m,
                              uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_SCALAR_WRITE;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.scalar_write.writes[0].addr  = addr;
    n->payload.scalar_write.writes[0].value = value;
}

static void make_scalar_read(rp1_node_t *n, uint32_t source_addr, uint32_t target_slot,
                             uint8_t aw_b, uint32_t aw_m,
                             uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_SCALAR_READ;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.scalar_read.source_addr = source_addr;
    n->payload.scalar_read.target_slot = target_slot;
}

static void make_wait(rp1_node_t *n,
                      uint32_t cond_signal, uint16_t cond_op, uint32_t cond_val,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_WAIT;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.wait.condition_signal = cond_signal;
    n->payload.wait.condition_value  = cond_val;
    n->payload.wait.condition_op     = cond_op;
}

static void make_loop(rp1_node_t *n,
                      uint32_t body_start, uint32_t body_end,
                      uint32_t cond_signal, uint16_t cond_op, uint32_t cond_val,
                      uint8_t bucket_clear_start, uint8_t bucket_clear_end,
                      uint8_t loop_id, uint32_t max_iter,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_LOOP;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.loop.body_start         = body_start;
    n->payload.loop.body_end           = body_end;
    n->payload.loop.max_iterations     = max_iter;
    n->payload.loop.condition_signal   = cond_signal;
    n->payload.loop.condition_value    = cond_val;
    n->payload.loop.condition_op       = cond_op;
    n->payload.loop.bucket_clear_start = bucket_clear_start;
    n->payload.loop.bucket_clear_end   = bucket_clear_end;
    n->payload.loop.loop_id            = loop_id;
}

static void make_rerun(rp1_node_t *n, uint32_t target_node,
                       uint8_t aw_b, uint32_t aw_m,
                       uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_RERUN;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.rerun.target_node = target_node;
    n->payload.rerun.rerun_flags = 0;
    n->payload.rerun.loop_id     = 0;
}

static void make_cond(rp1_node_t *n,
                      uint32_t cond_signal, uint16_t cond_op, uint32_t cond_val,
                      uint32_t body_start, uint32_t body_end,
                      uint8_t bucket_clear_start, uint8_t bucket_clear_end,
                      uint8_t done_bucket, uint32_t done_mask,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_COND;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.cond.condition_signal   = cond_signal;
    n->payload.cond.condition_value    = cond_val;
    n->payload.cond.condition_op       = cond_op;
    n->payload.cond.bucket_clear_start = bucket_clear_start;
    n->payload.cond.bucket_clear_end   = bucket_clear_end;
    n->payload.cond.body_start         = body_start;
    n->payload.cond.body_end           = body_end;
    n->payload.cond.done_bucket        = done_bucket;
    n->payload.cond.done_mask          = done_mask;
}

static void make_pdi_load(rp1_node_t *n,
                          uint32_t addr_lo, uint32_t addr_hi,
                          uint32_t timeout_cycles, uint16_t flags,
                          uint8_t aw_b, uint32_t aw_m,
                          uint8_t st_b, uint32_t st_m)
{
    n->opcode               = RP1_OP_PDI_LOAD;
    n->flags                = flags;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;

    n->payload.pdi_load.pdi_addr_lo    = addr_lo;
    n->payload.pdi_load.pdi_addr_hi    = addr_hi;
    n->payload.pdi_load.timeout_cycles = timeout_cycles;
}

/* -------------------------------------------------------------------------
 * Injectable HAL model for PDI IPI and fake kernel MMIO.
 * ---------------------------------------------------------------------- */

#define PDI_ACCESS_MAX 32u
#define PDI_ACCESS_READ 1u
#define PDI_ACCESS_WRITE 2u
#define PDI_ACCESS_BARRIER 3u

typedef struct {
    uint32_t kind;
    uint32_t address;
    uint32_t value;
} pdi_access_t;

static uint32_t s_pdi_call_count;
static uint32_t s_pdi_last_addr_lo;
static uint32_t s_pdi_last_addr_hi;
static uint32_t s_pdi_force_timeout;
static uint32_t s_pdi_status;
static uint32_t s_pdi_detail;
static uint32_t s_pdi_obs_reads;
static uint32_t s_fake_cycles;
static uint32_t s_cycle_step;
static pdi_access_t s_pdi_access[PDI_ACCESS_MAX];
static uint32_t s_pdi_access_count;

static void record_pdi_access(uint32_t kind, uintptr_t address, uint32_t value)
{
    if (s_pdi_access_count >= PDI_ACCESS_MAX)
        return;
    s_pdi_access[s_pdi_access_count].kind = kind;
    s_pdi_access[s_pdi_access_count].address = (uint32_t)address;
    s_pdi_access[s_pdi_access_count].value = value;
    s_pdi_access_count++;
}

static uint32_t test_mmio_read32(uintptr_t address, void *context)
{
    (void)context;
    if (address == RP1_PDI_IPI_OBSERVATION_REG) {
        uint32_t value = s_pdi_force_timeout ? RP1_PDI_IPI_TARGET_MASK : 0u;
        s_pdi_obs_reads++;
        record_pdi_access(PDI_ACCESS_READ, address, value);
        return value;
    }
    if (address == RP1_PDI_IPI_RESPONSE_BASE) {
        record_pdi_access(PDI_ACCESS_READ, address, s_pdi_status);
        return s_pdi_status;
    }
    if (address == RP1_PDI_IPI_RESPONSE_BASE + 4u) {
        record_pdi_access(PDI_ACCESS_READ, address, s_pdi_detail);
        return s_pdi_detail;
    }
    return *(volatile uint32_t *)address;
}

static void test_mmio_write32(uintptr_t address, uint32_t value, void *context)
{
    (void)context;
    if (address >= RP1_PDI_IPI_REQUEST_BASE &&
        address < RP1_PDI_IPI_REQUEST_BASE + 16u) {
        record_pdi_access(PDI_ACCESS_WRITE, address, value);
        if (address == RP1_PDI_IPI_REQUEST_BASE + 8u)
            s_pdi_last_addr_hi = value;
        else if (address == RP1_PDI_IPI_REQUEST_BASE + 12u)
            s_pdi_last_addr_lo = value;
        return;
    }
    if (address == RP1_PDI_IPI_TRIGGER_REG) {
        record_pdi_access(PDI_ACCESS_WRITE, address, value);
        s_pdi_call_count++;
        return;
    }
    *(volatile uint32_t *)address = value;
}

static void test_barrier(void *context)
{
    (void)context;
    record_pdi_access(PDI_ACCESS_BARRIER, 0u, 0u);
}

static uint32_t test_cycles(void *context)
{
    (void)context;
    uint32_t now = s_fake_cycles;
    s_fake_cycles += s_cycle_step;
    return now;
}

static const rp1_hal_hooks_t s_hal_hooks = {
    .read32 = test_mmio_read32,
    .write32 = test_mmio_write32,
    .barrier = test_barrier,
    .cycles = test_cycles,
    .context = 0,
};

static void pdi_override_reset(void)
{
    s_pdi_call_count   = 0;
    s_pdi_last_addr_lo = 0;
    s_pdi_last_addr_hi = 0;
    s_pdi_force_timeout = 0;
    s_pdi_status = 0;
    s_pdi_detail = 0;
    s_pdi_obs_reads = 0;
    s_fake_cycles = 0;
    s_cycle_step = 1u;
    s_pdi_access_count = 0;
    rp1_hal_set_hooks(&s_hal_hooks);
}

static void prepare_diamond_graph(void)
{
    setup_graph(/* node_count */ 4, /* fake_kernels */ 4);

    /* Protocol v2: one (reg_offset, value) pair per kernel -- write value i to
     * register 0x10.  Each pair is two words, so kernel i's pair lives at byte
     * offset i*8. */
    for (uint32_t i = 0; i < 4; i++) {
        G_ARGS[2u * i]      = 0x10u;  /* reg_offset */
        G_ARGS[2u * i + 1u] = i;      /* value      */
    }

    make_kernel(&G_NODES[0], 0, 0, 0x00, 0, 0x01, 0u * 8u, 1);
    make_kernel(&G_NODES[1], 1, 0, 0x01, 0, 0x02, 1u * 8u, 1);
    make_kernel(&G_NODES[2], 2, 0, 0x01, 0, 0x04, 2u * 8u, 1);
    make_kernel(&G_NODES[3], 3, 0, 0x06, 0, 0x08, 3u * 8u, 1);
}

/* -------------------------------------------------------------------------
 * test_diamond_dag
 *
 *        A (k0)
 *       / \
 *      B   C  (k1, k2)
 *       \ /
 *        D (k3)
 *
 * Verifies:
 *   - KERNEL_DISPATCH writes args to FAKE_K + 0x10 and ap_start to + 0x00.
 *   - barrier AND ({B,C} done) gates D.
 *   - parallel dispatch: B and C are both in flight at some point.
 *   - the CQ is populated in the natural dispatch order.
 * ---------------------------------------------------------------------- */

static int test_diamond_dag(void)
{
    prepare_diamond_graph();

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "diamond: rp1_run rc");

    CHECK_EQ32(s_trace_count, 4u, "diamond: nodes traced");
    CHECK_EQ32(s_trace[0],    0u, "diamond: A first");
    CHECK_EQ32(s_trace[1],    1u, "diamond: B second");
    CHECK_EQ32(s_trace[2],    2u, "diamond: C third");
    CHECK_EQ32(s_trace[3],    3u, "diamond: D last");
    CHECK(s_max_inflight >= 2u, "diamond: B and C in flight together");

    CHECK_EQ32(G_CTRL->cq_write_idx,   4u,                 "diamond: cq entries");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,                 "diamond: graph_done_seq");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_READY,    "diamond: rp1_state");

    for (uint32_t i = 0; i < 4; i++) {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(i);
        CHECK_EQ32(ctrl[0],        0x3u, "diamond: ctrl reg ap_start|ap_done");
        CHECK_EQ32(ctrl[0x10 / 4], i,    "diamond: kernel arg[0]");
    }
    return 0;
}

static int test_cq_timestamps(void)
{
    prepare_diamond_graph();

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cq_ts: first rp1_run rc");
    CHECK_EQ32(G_CTRL->cq_write_idx, 4u, "cq_ts: first cq entries");

    for (uint32_t i = 1; i < G_CTRL->cq_write_idx; i++)
        CHECK(G_CQ[i].timestamp >= G_CQ[i - 1u].timestamp,
              "cq_ts: timestamps non-decreasing");
    CHECK(G_CQ[3].timestamp > G_CQ[0].timestamp,
          "cq_ts: timestamps advanced");

    uint32_t first_last = G_CQ[3].timestamp;

    prepare_diamond_graph();
    rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cq_ts: second rp1_run rc");
    CHECK(G_CQ[0].timestamp < first_last,
          "cq_ts: second graph timestamp baseline reset");
    return 0;
}

static int test_trace_disabled_by_default(void)
{
    prepare_diamond_graph();

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "trace_default: rp1_run rc");
    CHECK_EQ32(G_CTRL->trace_write_idx, 0u, "trace_default: trace disabled");
    return 0;
}

static int test_trace_queue(void)
{
    prepare_diamond_graph();
    G_CTRL->trace_enable = 1u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "trace_queue: rp1_run rc");

    uint32_t writes = G_CTRL->trace_write_idx;
    CHECK(writes > 0u, "trace_queue: wrote entries");
    CHECK_EQ32(G_TRACE[0].event, RP1_TRACE_GRAPH_START,
               "trace_queue: first event graph start");
    CHECK_EQ32(G_TRACE[0].node_index, 0xFFFFu,
               "trace_queue: graph start node");
    CHECK_EQ32(G_TRACE[writes - 1u].event, RP1_TRACE_GRAPH_DONE,
               "trace_queue: last event graph done");

    uint32_t launch_count = 0;
    uint32_t done_count = 0;
    for (uint32_t i = 1; i < writes; i++) {
        CHECK(G_TRACE[i].timestamp >= G_TRACE[i - 1u].timestamp,
              "trace_queue: timestamps non-decreasing");
        if (G_TRACE[i].event == RP1_TRACE_KERNEL_LAUNCH) {
            CHECK_EQ32(G_TRACE[i].node_index, launch_count,
                       "trace_queue: launch order");
            launch_count++;
        } else if (G_TRACE[i].event == RP1_TRACE_KERNEL_DONE) {
            done_count++;
        }
    }

    CHECK_EQ32(launch_count, 4u, "trace_queue: launch entries");
    CHECK_EQ32(done_count, 4u, "trace_queue: done entries");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_kernel_unblocks_signal
 *
 *   SIGNAL -> KERNEL_DISPATCH -> SIGNAL
 *
 * The fake kernel is marked ap_done by the scan-pass hook after the scanner
 * has already attempted node activation for that pass.  The completion must
 * still count as progress so rp1_loop() performs another activation pass for
 * the downstream SIGNAL instead of declaring the graph complete.
 * ---------------------------------------------------------------------- */

static int test_kernel_unblocks_signal(void)
{
    setup_graph(/* node_count */ 3, /* fake_kernels */ 1);

    /* Protocol v2: a single (reg_offset, value) pair writing 0x12345678 to 0x10. */
    G_ARGS[0] = 0x10u;
    G_ARGS[1] = 0x12345678u;

    make_signal(&G_NODES[0], 0, 0xBEEFBEEFu, RP1_SIGOP_SET,
                0, 0x00, 0, 0x1);
    make_kernel(&G_NODES[1], 0, 0, 0x1, 0, 0x2, 0u, 1);
    make_signal(&G_NODES[2], 1, 0xCAFEBABEu, RP1_SIGOP_SET,
                0, 0x2, 0, 0x4);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "kernel_chain: rp1_run rc");

    CHECK_EQ32(G_SIGS[0].value, 0xBEEFBEEFu, "kernel_chain: pre signal");
    CHECK_EQ32(G_SIGS[1].value, 0xCAFEBABEu, "kernel_chain: post signal");
    CHECK_EQ32(g_node_status[0], RP1_NODE_DONE, "kernel_chain: node 0 DONE");
    CHECK_EQ32(g_node_status[1], RP1_NODE_DONE, "kernel_chain: node 1 DONE");
    CHECK_EQ32(g_node_status[2], RP1_NODE_DONE, "kernel_chain: node 2 DONE");
    CHECK_EQ32(g_barriers[0] & 0x7u, 0x7u, "kernel_chain: barriers raised");

    CHECK_EQ32(s_trace_count, 3u, "kernel_chain: nodes traced");
    CHECK_EQ32(s_trace[0],    0u, "kernel_chain: pre first");
    CHECK_EQ32(s_trace[1],    1u, "kernel_chain: kernel second");
    CHECK_EQ32(s_trace[2],    2u, "kernel_chain: post third");

    CHECK_EQ32(G_CTRL->cq_write_idx, 3u, "kernel_chain: cq entries");
    CHECK_EQ32(G_CQ[0].node_index,   0u, "kernel_chain: CQ[0] pre");
    CHECK_EQ32(G_CQ[1].node_index,   1u, "kernel_chain: CQ[1] kernel");
    CHECK_EQ32(G_CQ[2].node_index,   2u, "kernel_chain: CQ[2] post");

    volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
    CHECK_EQ32(ctrl[0],        0x3u,        "kernel_chain: ctrl ap_start|ap_done");
    CHECK_EQ32(ctrl[0x10 / 4], 0x12345678u, "kernel_chain: arg[0]");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_signal_chain
 *
 *   n0 -> n1 -> n2 -> n3   (each writes a different signal slot)
 *
 * Pure-scanner sanity check: no kernels, only immediate-completion
 * SIGNAL ops chained via single-bit barrier dependencies in bucket 0.
 * Exercises the DDR-resolved pointers (g_nodes, g_signals, g_cq).
 * ---------------------------------------------------------------------- */

static int test_signal_chain(void)
{
    setup_graph(/* node_count */ 4, /* fake_kernels */ 0);

    make_signal(&G_NODES[0], 0, 0xA000u, RP1_SIGOP_SET, 0, 0x00, 0, 0x1);
    make_signal(&G_NODES[1], 1, 0xB001u, RP1_SIGOP_SET, 0, 0x01, 0, 0x2);
    make_signal(&G_NODES[2], 2, 0xC002u, RP1_SIGOP_SET, 0, 0x02, 0, 0x4);
    make_signal(&G_NODES[3], 3, 0xD003u, RP1_SIGOP_SET, 0, 0x04, 0, 0x8);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "chain: rp1_run rc");

    CHECK_EQ32(G_SIGS[0].value, 0xA000u, "chain: slot 0");
    CHECK_EQ32(G_SIGS[1].value, 0xB001u, "chain: slot 1");
    CHECK_EQ32(G_SIGS[2].value, 0xC002u, "chain: slot 2");
    CHECK_EQ32(G_SIGS[3].value, 0xD003u, "chain: slot 3");

    CHECK_EQ32(s_trace_count,        4u, "chain: nodes traced");
    CHECK_EQ32(G_CTRL->cq_write_idx, 4u, "chain: cq entries");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u, "chain: graph_done_seq");
    return 0;
}

static int test_cq_flow_control(void)
{
    setup_graph(/* node_count */ 12, /* fake_kernels */ 0);
    G_CTRL->cq_size = 4u;
    G_CQ[4].node_index = 0xA1A2A3A4u;
    G_CQ[4].status = 0xB1B2B3B4u;
    G_CQ[4].error_detail = 0xC1C2C3C4u;
    G_CQ[4].timestamp = 0xD1D2D3D4u;
    for (uint32_t i = 0; i < 12u; i++) {
        make_signal(&G_NODES[i], i, 0x100u + i, RP1_SIGOP_SET,
                    0, 0u, 0, 1u << (i & 31u));
    }
    s_drain_cq = 1u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cq_flow: rp1_run rc");
    CHECK_EQ32(s_cq_capture_count, 12u,
               "cq_flow: all entries incrementally drained");
    for (uint32_t i = 0; i < 12u; i++)
        CHECK_EQ32(s_cq_capture[i].node_index, i,
                   "cq_flow: lossless node order");
    CHECK_EQ32(G_CTRL->cq_write_idx, 12u, "cq_flow: producer cursor");
    CHECK_EQ32(G_CTRL->cq_read_idx, 12u, "cq_flow: consumer cursor");
    CHECK_EQ32(G_CQ[4].node_index, 0xA1A2A3A4u,
               "cq_flow: canary node");
    CHECK_EQ32(G_CQ[4].status, 0xB1B2B3B4u,
               "cq_flow: canary status");
    CHECK_EQ32(G_CQ[4].error_detail, 0xC1C2C3C4u,
               "cq_flow: canary detail");
    CHECK_EQ32(G_CQ[4].timestamp, 0xD1D2D3D4u,
               "cq_flow: canary timestamp");
    return 0;
}

static int test_cq_config_validation(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0, 1u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->cq_size = 3u;
    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "cq_config: non-power-of-two rejected");
    CHECK_EQ32(G_CTRL->terminal_error_detail, RP1_CONFIG_CQ_SIZE,
               "cq_config: size detail");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 3u,
               "cq_config: invalid size value");

    setup_graph(1, 0);
    make_signal(&G_NODES[0], 0, 1u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->cq_size = RP1_MAX_CQ_ENTRIES * 2u;
    rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "cq_config: oversize rejected");
    CHECK_EQ32(G_CTRL->terminal_error_detail, RP1_CONFIG_CQ_SIZE,
               "cq_config: oversize detail");

    setup_graph(1, 0);
    make_signal(&G_NODES[0], 0, 1u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->cq_size = RP1_MAX_CQ_ENTRIES;
    rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cq_config: maximum accepted");
    CHECK_EQ32(G_CTRL->cq_write_idx, 1u,
               "cq_config: maximum ring wrote entry");
    return 0;
}

static int test_cq_cursor_wrap(void)
{
    setup_graph(/* node_count */ 3, /* fake_kernels */ 0);
    G_CTRL->cq_size = 4u;
    G_CTRL->cq_write_idx = 0xFFFFFFFEu;
    G_CTRL->cq_read_idx = 0xFFFFFFFEu;
    for (uint32_t i = 0; i < 3u; i++) {
        make_signal(&G_NODES[i], i, i + 1u, RP1_SIGOP_SET,
                    0, 0u, 0, 1u << i);
    }

    uint32_t detail = 0u;
    uint32_t aux = 0u;
    CHECK_EQ32(rp1_store_init(&detail, &aux), 0u,
               "cq_wrap: store init");
    g_graph_start_cycles = rp1_cycles();
    int rc = rp1_loop(&s_hooks);
    CHECK_EQ32(rc, 0u, "cq_wrap: scanner result");
    CHECK_EQ32(G_CTRL->cq_write_idx, 1u,
               "cq_wrap: producer wrapped");
    CHECK_EQ32(G_CQ[2].node_index, 0u, "cq_wrap: slot 2");
    CHECK_EQ32(G_CQ[3].node_index, 1u, "cq_wrap: slot 3");
    CHECK_EQ32(G_CQ[0].node_index, 2u, "cq_wrap: slot 0");
    return 0;
}

static int test_graph_sequence_wrap(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0u, 0x1111u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->graph_seq = 0xFFFFFFFFu;
    s_wrap_graphs = 0u;

    int rc = rp1_run(&s_wrap_hooks);
    CHECK_EQ32(rc, 0u, "seq_wrap: second graph result");
    CHECK_EQ32(s_wrap_graphs, 2u, "seq_wrap: both graphs ran");
    CHECK_EQ32(G_CTRL->graph_done_seq, 0u,
               "seq_wrap: equality completion wrapped");
    CHECK_EQ32(G_SIGS[0].value, 0x1111u,
               "seq_wrap: pre-wrap graph ran");
    CHECK_EQ32(G_SIGS[1].value, 0x2222u,
               "seq_wrap: wrapped graph ran");
    return 0;
}

static int test_signal_slot_validation(void)
{
    static const uint16_t opcodes[] = {
        RP1_OP_SIGNAL,
        RP1_OP_WAIT,
        RP1_OP_SCALAR_READ,
        RP1_OP_SCALAR_COPY,
        RP1_OP_LOOP,
        RP1_OP_COND,
    };

    for (uint32_t test = 0; test < sizeof(opcodes) / sizeof(opcodes[0]);
         test++) {
        setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
        rp1_node_t *node = &G_NODES[0];
        node->opcode = opcodes[test];
        node->status = RP1_NODE_PENDING;
        switch (node->opcode) {
        case RP1_OP_SIGNAL:
            node->payload.signal.target_slot = RP1_MAX_SIGNALS;
            node->payload.signal.operation = RP1_SIGOP_SET;
            break;
        case RP1_OP_WAIT:
            node->payload.wait.condition_signal = RP1_MAX_SIGNALS;
            node->payload.wait.condition_op = RP1_COP_EQ;
            break;
        case RP1_OP_SCALAR_READ:
            node->payload.scalar_read.target_slot = RP1_MAX_SIGNALS;
            break;
        case RP1_OP_SCALAR_COPY:
            node->payload.scalar_copy.source_slot = RP1_MAX_SIGNALS;
            break;
        case RP1_OP_LOOP:
            node->payload.loop.condition_signal = RP1_MAX_SIGNALS;
            node->payload.loop.condition_op = RP1_COP_EQ;
            node->payload.loop.body_start = 0u;
            node->payload.loop.body_end = 0u;
            break;
        case RP1_OP_COND:
            node->payload.cond.condition_signal = RP1_MAX_SIGNALS;
            node->payload.cond.condition_op = RP1_COP_EQ;
            node->payload.cond.body_start = 1u;
            node->payload.cond.body_end = 0u;
            node->payload.cond.bucket_clear_start = 1u;
            node->payload.cond.bucket_clear_end = 0u;
            break;
        default:
            break;
        }

        int rc = rp1_run(&s_hooks);
        CHECK_EQ32((uint32_t)(rc + 1), 0u,
                   "slot_validation: graph rejected");
        CHECK_EQ32(G_CTRL->terminal_error_node, 0u,
                   "slot_validation: node latched");
        CHECK_EQ32(G_CTRL->terminal_error_detail,
                   RP1_NODE_BAD_SIGNAL_SLOT,
                   "slot_validation: detail");
        CHECK_EQ32(G_CTRL->terminal_error_aux, RP1_MAX_SIGNALS,
                   "slot_validation: bad slot preserved");
        CHECK_EQ32(G_CQ[0].status, RP1_CQ_ERROR,
                   "slot_validation: CQ evidence");
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * test_loop_decrement
 *
 *  init -> LOOP --(body)--> RERUN
 *           ^                 |
 *           +-----------------+
 *           |
 *           +--exit-> finalize
 *
 *  Node 0: SIGNAL  slot=0 SET 3
 *  Node 1: LOOP    body=[2,3], cond: slot[0] EQ 0, bucket_clear=[1,1]
 *  Node 2: SIGNAL  slot=0 ADD 0xFFFFFFFF       (decrement by 1)
 *  Node 3: RERUN   target=1
 *  Node 4: SIGNAL  slot=10 SET 0xCAFEBABE
 *
 * Expected:
 *   - body runs 3 times (slot 3 -> 2 -> 1 -> 0); the 4th LOOP pass hits
 *     the exit condition (loop_iters[0] is incremented before the check,
 *     so it lands at 4 on exit).
 *   - finalize fires after the LOOP node sets its own barrier on exit.
 *   - CQ entries: init + 3*(body SIGNAL + body RERUN) + LOOP_exit + final = 9.
 *     (LOOP does NOT write CQ on the continue path, only on exit.)
 * ---------------------------------------------------------------------- */

static int test_loop_decrement(void)
{
    setup_graph(/* node_count */ 5, /* fake_kernels */ 0);

    make_signal(&G_NODES[0], 0, 3u, RP1_SIGOP_SET, 0, 0x00, 0, 0x1);
    make_loop(  &G_NODES[1],
                /* body */ 2, 3,
                /* cond */ 0, RP1_COP_EQ, 0u,
                /* clear */ 1, 1,
                /* loop_id */ 0, /* max_iter */ 10u,
                /* await */ 0, 0x1, /* set on exit */ 0, 0x2);
    make_signal(&G_NODES[2], 0, 0xFFFFFFFFu, RP1_SIGOP_ADD, 1, 0x00, 1, 0x1);
    make_rerun( &G_NODES[3], /* target */ 1,
                /* await */ 1, 0x1, /* set */ 1, 0x2);
    make_signal(&G_NODES[4], 10, 0xCAFEBABEu, RP1_SIGOP_SET, 0, 0x02, 0, 0x4);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "loop: rp1_run rc");

    CHECK_EQ32(G_SIGS[0].value,        0u,          "loop: slot[0] reached 0");
    CHECK_EQ32(G_SIGS[10].value,       0xCAFEBABEu, "loop: finalize ran");
    CHECK_EQ32(g_loop_iters[0],        4u,          "loop: iteration counter");
    CHECK_EQ32(G_CTRL->cq_write_idx,   9u,          "loop: cq entries");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,          "loop: graph_done_seq");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_cond_boolean
 *
 *  Node 0: SIGNAL  slot=5 SET <test_value>
 *  Node 1: COND    cond: slot[5] EQ 42
 *                  body=[empty], bucket_clear=[empty]
 *                  set=0/0x10  (always)   done=0/0x20  (only on met)
 *  Node 2: SIGNAL  slot=20 SET 0xAAAA  await=0/0x10  (always)
 *  Node 3: SIGNAL  slot=21 SET 0xBBBB  await=0/0x20  (only on met)
 *
 * Avoids the if/else-via-body pattern from ARCHITECTURE.md § E (which
 * relies on body_clear + node_status reset to gate body execution and
 * isn't airtight when body-await masks are zero) and instead exercises
 * COND as a pure boolean: condition evaluation, the always-set
 * barrier_set_mask, and the conditional done_mask in done_bucket.
 *
 * Run twice — once with the condition met, once without — to confirm
 * both branches of the conditional are reachable from the same graph
 * template.
 * ---------------------------------------------------------------------- */

static int test_cond_boolean(void)
{
    /* ---- Run 1: condition met (slot[5] == 42) ---- */
    setup_graph(/* node_count */ 4, /* fake_kernels */ 0);

    make_signal(&G_NODES[0], 5, 42u, RP1_SIGOP_SET, 0, 0x00, 0, 0x1);
    make_cond(  &G_NODES[1],
                /* cond */ 5, RP1_COP_EQ, 42u,
                /* body */ 255, 0,   /* empty range */
                /* clear */ 255, 0,  /* empty range */
                /* done */ 0, 0x20,
                /* await */ 0, 0x1, /* set */ 0, 0x10);
    make_signal(&G_NODES[2], 20, 0xAAAAu, RP1_SIGOP_SET, 0, 0x10, 0, 0x40);
    make_signal(&G_NODES[3], 21, 0xBBBBu, RP1_SIGOP_SET, 0, 0x20, 0, 0x80);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cond[met]: rp1_run rc");
    CHECK_EQ32(G_SIGS[20].value,     0xAAAAu, "cond[met]: 'always' branch ran");
    CHECK_EQ32(G_SIGS[21].value,     0xBBBBu, "cond[met]: 'met-only' branch ran");
    CHECK_EQ32(G_CTRL->cq_write_idx, 4u,      "cond[met]: cq entries");

    /* ---- Run 2: condition NOT met (slot[5] == 99) ---- */
    setup_graph(4, 0);

    make_signal(&G_NODES[0], 5, 99u, RP1_SIGOP_SET, 0, 0x00, 0, 0x1);
    make_cond(  &G_NODES[1],
                5, RP1_COP_EQ, 42u,
                255, 0,
                255, 0,
                0, 0x20,
                0, 0x1, 0, 0x10);
    make_signal(&G_NODES[2], 20, 0xAAAAu, RP1_SIGOP_SET, 0, 0x10, 0, 0x40);
    make_signal(&G_NODES[3], 21, 0xBBBBu, RP1_SIGOP_SET, 0, 0x20, 0, 0x80);

    rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "cond[nomet]: rp1_run rc");
    CHECK_EQ32(G_SIGS[20].value,     0xAAAAu, "cond[nomet]: 'always' branch ran");
    CHECK_EQ32(G_SIGS[21].value,     0u,      "cond[nomet]: 'met-only' silent");
    CHECK_EQ32(G_CTRL->cq_write_idx, 3u,      "cond[nomet]: cq entries");
    CHECK_EQ32(g_node_status[3],     RP1_NODE_PENDING,
               "cond[nomet]: node 3 stayed PENDING");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_loop_fixed_count
 *
 *  Node 0: LOOP    body=[1,2], cond NEVER (AND_NZ 0), max_iter=3,
 *                  bucket_clear=[1,1], set-on-exit=0/0x2
 *  Node 1: KERNEL  (re-dispatched in place each iteration)  set=1/0x1
 *  Node 2: RERUN   target=0                                 await=1/0x1 set=1/0x2
 *  Node 3: SIGNAL  slot=10 SET 0xD0NE                       await=0/0x2
 *
 * This is the shape the host loop-lowering emits for a fixed-count FPGA loop:
 * termination governed purely by max_iterations (the data-dependent predicate
 * is wired to never fire), with a real KERNEL_DISPATCH body that must be
 * re-dispatched (inflight cleared) on every iteration.  loop_decrement covers
 * the condition-exit + signal-body path; this covers max_iterations + a kernel.
 *
 * Expected: body runs 3 times (loop_iters lands at 4 on exit), finalize fires.
 * CQ = 3*(kernel + rerun) + loop_exit + finalize = 8.
 * ---------------------------------------------------------------------- */

static int test_loop_fixed_count(void)
{
    setup_graph(/* node_count */ 4, /* fake_kernels */ 1);

    G_ARGS[0] = 0x10u;  /* (reg_offset, value) pair for the body kernel */
    G_ARGS[1] = 0x55u;

    make_loop(  &G_NODES[0],
                /* body */ 1, 2,
                /* cond */ 0, RP1_COP_AND_NZ, 0u,   /* (sig & 0) != 0 -> never */
                /* clear */ 1, 1,
                /* loop_id */ 0, /* max_iter */ 3u,
                /* await */ 0, 0x0, /* set on exit */ 0, 0x2);
    make_kernel(&G_NODES[1], /* kidx */ 0, 1, 0x0, 1, 0x1, /* args */ 0u, 1);
    make_rerun( &G_NODES[2], /* target */ 0, 1, 0x1, 1, 0x2);
    make_signal(&G_NODES[3], 10, 0xD05Eu, RP1_SIGOP_SET, 0, 0x2, 0, 0x4);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "loop_fixed: rp1_run rc");

    CHECK_EQ32(g_loop_iters[0],        4u,       "loop_fixed: 3 body runs (iters=4)");
    CHECK_EQ32(G_SIGS[10].value,       0xD05Eu,  "loop_fixed: finalize ran on exit");
    CHECK_EQ32(G_CTRL->cq_write_idx,   8u,       "loop_fixed: cq entries");
    CHECK_EQ32(g_node_status[3],       RP1_NODE_DONE, "loop_fixed: finalize DONE");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,       "loop_fixed: graph_done_seq");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_scalar_read
 *
 *  Node 0: SCALAR_WRITE  fake_reg = 0x1234ABCD   (stands in for a kernel's
 *                                                 s_axilite output register)
 *  Node 1: SCALAR_READ   slot[6] = *fake_reg
 *
 * Validates the firmware primitive the host output-scalar lowering relies on:
 * capturing an AXI-Lite register value into a host-visible signal slot, which
 * a downstream LOOP/COND can then evaluate (Phase B/F).
 * ---------------------------------------------------------------------- */

static int test_scalar_read(void)
{
    setup_graph(/* node_count */ 2, /* fake_kernels */ 1);

    const uint32_t reg = (uint32_t)FAKE_KERNEL(0) + 0x40u;

    make_scalar_write(&G_NODES[0], reg, 0x1234ABCDu, 0, 0x00, 0, 0x1);
    make_scalar_read( &G_NODES[1], reg, /* slot */ 6u, 0, 0x1, 0, 0x2);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "scalar_read: rp1_run rc");

    CHECK_EQ32(G_SIGS[6].value,        0x1234ABCDu, "scalar_read: slot captured reg");
    CHECK_EQ32(g_node_status[1],       RP1_NODE_DONE, "scalar_read: node DONE");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,          "scalar_read: graph_done_seq");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_wait_blocks
 *
 *  Node 0: WAIT    slot[7] EQ 0xABCD     set=0/0x1
 *  Node 1: SIGNAL  slot[20] SET 0xF00D   await=0/0x1
 *
 * The cross-queue rendezvous primitive: node 1 must not run until an external
 * writer (simulated by the scan-pass hook after 3 passes) raises slot[7].  The
 * hook records slot[20] at the moment it fires the signal; it must still be 0,
 * proving the WAIT parked node 0 (RP1_NODE_WAITING) and gated node 1 until the
 * condition held — not the immediate-NOP behaviour an unknown opcode would get.
 * ---------------------------------------------------------------------- */

static int test_wait_blocks(void)
{
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);

    make_wait(  &G_NODES[0], /* cond */ 7, RP1_COP_EQ, 0xABCDu,
                /* await */ 0, 0x00, /* set */ 0, 0x1);
    make_signal(&G_NODES[1], 20, 0xF00Du, RP1_SIGOP_SET, 0, 0x1, 0, 0x2);

    s_wait_armed        = 1;
    s_wait_after        = 3u;       /* raise the awaited signal on pass 3 */
    s_wait_slot         = 7u;
    s_wait_value        = 0xABCDu;
    s_wait_witness_slot = 20u;      /* node 1's output slot */

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "wait: rp1_run rc");

    CHECK_EQ32(s_wait_witness_at_fire, 0u,
               "wait: downstream stayed blocked until the signal arrived");
    CHECK_EQ32(G_SIGS[20].value,     0xF00Du,        "wait: downstream ran after release");
    CHECK_EQ32(g_node_status[0],     RP1_NODE_DONE,  "wait: WAIT node DONE");
    CHECK_EQ32(g_node_status[1],     RP1_NODE_DONE,  "wait: downstream DONE");
    CHECK_EQ32(g_barriers[0] & 0x3u, 0x3u,           "wait: both barriers raised");
    CHECK_EQ32(G_CTRL->cq_write_idx, 2u,             "wait: cq entries");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,           "wait: graph_done_seq");
    CHECK(s_pass_count > s_wait_after, "wait: scanner kept polling while parked");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_mmio_contract
 * ---------------------------------------------------------------------- */

static int test_pdi_mmio_contract(void)
{
    pdi_override_reset();
    s_pdi_status = 0x80000001u;
    s_pdi_detail = 0xA5A55A5Au;

    rp1_pdi_result_t result =
        rp1_pdi_load(0x11223344u, 0x55667788u, 12345u);

    CHECK_EQ32(result.outcome, RP1_PDI_RESULT_PLM_ERROR,
               "pdi_mmio: structured error outcome");
    CHECK_EQ32(result.status, 0x80000001u,
               "pdi_mmio: high-bit status preserved");
    CHECK_EQ32(result.detail, 0xA5A55A5Au,
               "pdi_mmio: detail preserved");
    CHECK_EQ32(s_pdi_access_count, 11u,
               "pdi_mmio: exact access count");
    CHECK_EQ32(s_pdi_access[0].address, RP1_PDI_IPI_REQUEST_BASE,
               "pdi_mmio: request command address");
    CHECK_EQ32(s_pdi_access[4].kind, PDI_ACCESS_BARRIER,
               "pdi_mmio: request barrier");
    CHECK_EQ32(s_pdi_access[5].address, RP1_PDI_IPI_TRIGGER_REG,
               "pdi_mmio: generated trigger address");
    CHECK_EQ32(s_pdi_access[5].value, RP1_PDI_IPI_TARGET_MASK,
               "pdi_mmio: generated target mask");
    CHECK_EQ32(s_pdi_access[7].address, RP1_PDI_IPI_OBSERVATION_REG,
               "pdi_mmio: generated observation address");
    CHECK_EQ32(s_pdi_access[9].address, RP1_PDI_IPI_RESPONSE_BASE,
               "pdi_mmio: response status address");
    CHECK_EQ32(s_pdi_access[10].address, RP1_PDI_IPI_RESPONSE_BASE + 4u,
               "pdi_mmio: response detail address");
    return 0;
}

static int test_pdi_timeout_invariant(void)
{
    pdi_override_reset();
    s_pdi_force_timeout = 1u;
    s_cycle_step = 1u;
    rp1_pdi_result_t slow =
        rp1_pdi_load(0x1000u, 0u, 12u);
    uint32_t slow_reads = s_pdi_obs_reads;

    pdi_override_reset();
    s_pdi_force_timeout = 1u;
    s_cycle_step = 4u;
    rp1_pdi_result_t fast =
        rp1_pdi_load(0x1000u, 0u, 12u);
    uint32_t fast_reads = s_pdi_obs_reads;

    CHECK_EQ32(slow.outcome, RP1_PDI_RESULT_TIMEOUT,
               "pdi_deadline: unit-step timeout");
    CHECK_EQ32(fast.outcome, RP1_PDI_RESULT_TIMEOUT,
               "pdi_deadline: coarse-step timeout");
    CHECK_EQ32(slow_reads, 12u,
               "pdi_deadline: unit-step poll count");
    CHECK_EQ32(fast_reads, 3u,
               "pdi_deadline: coarse-step poll count");
    return 0;
}

static int test_kernel_timeout_invariant(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 1);
    pdi_override_reset();
    s_cycle_step = 1u;
    make_kernel(&G_NODES[0], 0, 0, 0u, 0, 1u, 0u, 0u);
    G_NODES[0].flags = RP1_FLAG_HALT_ON_ERROR;
    G_NODES[0].payload.kernel_dispatch.timeout_cycles = 12u;
    s_skip_completion_node = 0u;
    int slow = rp1_run(&s_hooks);
    uint32_t slow_passes = s_pass_count;

    setup_graph(1, 1);
    pdi_override_reset();
    s_cycle_step = 4u;
    make_kernel(&G_NODES[0], 0, 0, 0u, 0, 1u, 0u, 0u);
    G_NODES[0].flags = RP1_FLAG_HALT_ON_ERROR;
    G_NODES[0].payload.kernel_dispatch.timeout_cycles = 12u;
    s_skip_completion_node = 0u;
    int fast = rp1_run(&s_hooks);
    uint32_t fast_passes = s_pass_count;

    CHECK_EQ32((uint32_t)(slow + 1), 0u,
               "kernel_deadline: unit-step timeout");
    CHECK_EQ32((uint32_t)(fast + 1), 0u,
               "kernel_deadline: coarse-step timeout");
    CHECK(slow_passes > fast_passes,
          "kernel_deadline: scanner poll count does not define timeout");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 12u,
               "kernel_deadline: requested PMU deadline retained");
    return 0;
}

static int test_silent_pdi_rejected(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();
    make_pdi_load(&G_NODES[0], 0x10000000u, 0u, 10u,
                  RP1_FLAG_SILENT, 0, 0u, 0, 1u);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "pdi_silent: graph rejected");
    CHECK_EQ32(s_pdi_call_count, 0u,
               "pdi_silent: IPI was not triggered");
    CHECK_EQ32(G_CTRL->terminal_error_detail,
               RP1_NODE_PDI_WITHOUT_CQ,
               "pdi_silent: validation detail");
    CHECK_EQ32(G_CQ[0].status, RP1_CQ_ERROR,
               "pdi_silent: forced CQ evidence");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_load_basic
 *
 *   Single PDI_LOAD node, override returns success.
 *   Verifies the scanner forwards the payload to rp1_pdi_load() verbatim
 *   and marks the node DONE / writes an OK CQ entry on return.
 * ---------------------------------------------------------------------- */

static int test_pdi_load_basic(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();

    make_pdi_load(&G_NODES[0],
                  /* addr_lo */ 0x10000000u,
                  /* addr_hi */ 0x00000001u,
                  /* timeout */ 12345u,
                  /* flags   */ 0,
                  /* await   */ 0, 0x00,
                  /* set     */ 0, 0x01);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "pdi_basic: rp1_run rc");

    CHECK_EQ32(s_pdi_call_count,       1u,          "pdi_basic: invoked once");
    CHECK_EQ32(s_pdi_last_addr_lo,     0x10000000u, "pdi_basic: addr_lo forwarded");
    CHECK_EQ32(s_pdi_last_addr_hi,     0x00000001u, "pdi_basic: addr_hi forwarded");
    CHECK_EQ32(g_node_status[0],       RP1_NODE_DONE, "pdi_basic: node DONE");
    CHECK_EQ32(g_barriers[0] & 0x1u,   0x1u,          "pdi_basic: barrier set");
    CHECK_EQ32(G_CTRL->cq_write_idx,   1u,            "pdi_basic: one CQ entry");
    CHECK_EQ32(G_CQ[0].status,         RP1_CQ_OK,     "pdi_basic: CQ status OK");
    CHECK_EQ32(G_CQ[0].node_index,     0u,            "pdi_basic: CQ node_index");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_READY, "pdi_basic: rp1_state");
    CHECK_EQ32(G_CTRL->rp1_error_code, 0u,            "pdi_basic: no error");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_load_timeout
 *
 *   Two passes: first with HALT_ON_ERROR cleared (graph continues, barrier
 *   still raised so downstream can run), second with HALT_ON_ERROR set
 *   (scanner aborts via rp1_state = ERROR).  In both cases rp1_pdi_load()
 *   returns -1, which the scanner must surface as ERR_PDI_TIMEOUT (3).
 * ---------------------------------------------------------------------- */

static int test_pdi_load_timeout(void)
{
    /* ---- Run 1: non-fatal timeout (no HALT_ON_ERROR) ---- */
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_force_timeout = 1u;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 3u, /* flags */ 0,
                  0, 0x00, 0, 0x01);
    /* Downstream SIGNAL gated on the PDI's set bit — verifies the
     * scanner still raises barriers on non-fatal timeout. */
    make_signal(&G_NODES[1], 30, 0xFEEDBEEFu, RP1_SIGOP_SET,
                0, 0x01, 0, 0x02);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "pdi_timeout[non-fatal]: rp1_run rc");

    CHECK_EQ32(g_node_status[0],       RP1_NODE_ERROR,  "pdi_timeout[non-fatal]: node ERROR");
    CHECK_EQ32(G_CTRL->rp1_error_code, 3u,              "pdi_timeout[non-fatal]: err code 3");
    CHECK_EQ32(G_CQ[0].status,         RP1_CQ_TIMEOUT,  "pdi_timeout[non-fatal]: CQ TIMEOUT");
    CHECK_EQ32(g_barriers[0] & 0x1u,   0x1u,            "pdi_timeout[non-fatal]: barrier still set");
    CHECK_EQ32(G_SIGS[30].value,       0xFEEDBEEFu,     "pdi_timeout[non-fatal]: downstream ran");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_READY, "pdi_timeout[non-fatal]: rp1_state");

    /* ---- Run 2: fatal timeout (HALT_ON_ERROR set) ---- */
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_force_timeout = 1u;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 3u,
                  /* flags */ RP1_FLAG_HALT_ON_ERROR,
                  0, 0x00, 0, 0x01);
    make_signal(&G_NODES[1], 30, 0xFEEDBEEFu, RP1_SIGOP_SET,
                0, 0x01, 0, 0x02);

    rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u, "pdi_timeout[fatal]: rp1_run returned -1");

    CHECK_EQ32(g_node_status[0],       RP1_NODE_ERROR,  "pdi_timeout[fatal]: node ERROR");
    CHECK_EQ32(g_node_status[1],       RP1_NODE_PENDING,"pdi_timeout[fatal]: downstream blocked");
    CHECK_EQ32(G_SIGS[30].value,       0u,              "pdi_timeout[fatal]: downstream silent");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_ERROR, "pdi_timeout[fatal]: rp1_state ERROR");
    CHECK_EQ32(G_CTRL->rp1_error_code, 3u,              "pdi_timeout[fatal]: err code 3");

    /* ---- Run 3: PLM completed the command with an error response ---- */
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_status = 0x80002001u;
    s_pdi_detail = 0xDEADCAFEu;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 0u,
                  /* flags */ RP1_FLAG_HALT_ON_ERROR,
                  0, 0x00, 0, 0x01);

    rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u, "pdi_error: rp1_run returned -1");
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_PDI_FAILED,
               "pdi_error: PLM failure code");
    CHECK_EQ32(G_CQ[0].status, RP1_CQ_ERROR,
               "pdi_error: CQ status ERROR");
    CHECK_EQ32(G_CQ[0].error_detail, 0x80002001u,
               "pdi_error: CQ preserves high-bit PLM status");
    CHECK_EQ32(G_CTRL->terminal_error_detail, 0x80002001u,
               "pdi_error: terminal record preserves PLM status");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 0xDEADCAFEu,
               "pdi_error: terminal record preserves PLM detail");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_load_chained
 *
 *   Two PDI_LOAD nodes chained via a bucket-0 barrier (node 1 awaits the
 *   set bit raised by node 0).  Each call to the override returns 0 and
 *   updates the recorder; if the scanner honours the barrier dependency,
 *   the override sees two distinct invocations in node-order, so the
 *   last_addr_lo field ends up holding the second node's payload.
 *
 *   We avoid an upstream KERNEL_DISPATCH on purpose: the fake-kernel
 *   ap_done hook is unrelated to PDI_LOAD and is exercised by the
 *   diamond/loop tests already.
 * ---------------------------------------------------------------------- */

static int test_pdi_load_chained(void)
{
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);
    pdi_override_reset();

    make_pdi_load(&G_NODES[0],
                  /* addr_lo */ 0x11110000u,
                  /* addr_hi */ 0x00000001u,
                  /* timeout */ 0u,
                  /* flags   */ 0,
                  /* await   */ 0, 0x00,
                  /* set     */ 0, 0x01);
    make_pdi_load(&G_NODES[1],
                  /* addr_lo */ 0x22220000u,
                  /* addr_hi */ 0x00000002u,
                  /* timeout */ 0u,
                  /* flags   */ 0,
                  /* await   */ 0, 0x01,
                  /* set     */ 0, 0x02);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "pdi_chain: rp1_run rc");

    /* Both nodes fired, in order — the recorder captures the latest call. */
    CHECK_EQ32(s_pdi_call_count,       2u,            "pdi_chain: invoked twice");
    CHECK_EQ32(s_pdi_last_addr_lo,     0x22220000u,   "pdi_chain: last addr_lo (node 1)");
    CHECK_EQ32(s_pdi_last_addr_hi,     0x00000002u,   "pdi_chain: last addr_hi (node 1)");

    CHECK_EQ32(g_node_status[0],     RP1_NODE_DONE, "pdi_chain: node 0 DONE");
    CHECK_EQ32(g_node_status[1],     RP1_NODE_DONE, "pdi_chain: node 1 DONE");
    CHECK_EQ32(g_barriers[0] & 0x3u, 0x3u,          "pdi_chain: both barriers raised");
    CHECK_EQ32(G_CTRL->cq_write_idx, 2u,            "pdi_chain: two CQ entries");
    CHECK_EQ32(G_CQ[0].node_index,   0u,            "pdi_chain: CQ[0] is node 0");
    CHECK_EQ32(G_CQ[1].node_index,   1u,            "pdi_chain: CQ[1] is node 1");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_image_guard
 *
 * Exercises the expected-image guard. g_active_image_id persists across graph
 * submissions (it mirrors physical reconfig state and is not cleared by
 * rp1_store_reset_graph), so the three sub-runs below share it:
 *
 *   Run 1 (match):      PDI_LOAD{image_id=7} -> DISPATCH{expected=7} launches.
 *   Run 2 (mismatch):   DISPATCH{expected=9} with active image still 7 fails
 *                       fast -- NODE_ERROR, ERR_IMAGE_MISMATCH, CQ ERROR, and
 *                       the kernel is never launched (ctrl reg untouched).
 *   Run 3 (unguarded):  DISPATCH{expected=0} launches regardless of image.
 * ---------------------------------------------------------------------- */

static int test_image_guard(void)
{
    /* ---- Run 1: PDI sets image 7, matching dispatch launches. ---- */
    setup_graph(/* node_count */ 2, /* fake_kernels */ 1);
    pdi_override_reset();

    make_pdi_load(&G_NODES[0],
                  /* addr_lo */ 0x10000000u, /* addr_hi */ 0x00000001u,
                  /* timeout */ 0u, /* flags */ 0,
                  /* await   */ 0, 0x00, /* set */ 0, 0x01);
    G_NODES[0].payload.pdi_load.image_id = 7u;

    make_kernel(&G_NODES[1], /* kernel_idx */ 0,
                /* await */ 0, 0x01, /* set */ 0, 0x02,
                /* arg_buf_offset */ 0u, /* arg_count */ 0);
    G_NODES[1].payload.kernel_dispatch.expected_image_id = 7u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "image_guard[match]: rp1_run rc");
    CHECK_EQ32(g_active_image_id, 7u, "image_guard[match]: active image recorded");
    CHECK_EQ32(g_node_status[1], RP1_NODE_DONE, "image_guard[match]: dispatch DONE");
    {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
        CHECK_EQ32(ctrl[0], 0x3u, "image_guard[match]: kernel launched (ap_start|ap_done)");
    }
    CHECK_EQ32(G_CTRL->rp1_error_code, 0u, "image_guard[match]: no error");

    /* ---- Run 2: separate submission, stale expected image -> fail fast. ---- */
    setup_graph(/* node_count */ 1, /* fake_kernels */ 1);

    make_kernel(&G_NODES[0], /* kernel_idx */ 0,
                /* await */ 0, 0x00, /* set */ 0, 0x01,
                /* arg_buf_offset */ 0u, /* arg_count */ 0);
    G_NODES[0].payload.kernel_dispatch.expected_image_id = 9u;  /* active is still 7 */

    rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "image_guard[mismatch]: rp1_run rc (non-fatal)");
    CHECK_EQ32(g_active_image_id, 7u, "image_guard[mismatch]: active image unchanged");
    CHECK_EQ32(g_node_status[0], RP1_NODE_ERROR, "image_guard[mismatch]: node ERROR");
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_IMAGE_MISMATCH,
               "image_guard[mismatch]: err code");
    CHECK_EQ32(G_CQ[0].status, RP1_CQ_ERROR, "image_guard[mismatch]: CQ ERROR");
    CHECK_EQ32(G_CQ[0].error_detail, 7u, "image_guard[mismatch]: CQ carries active image");
    {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
        CHECK_EQ32(ctrl[0], 0u, "image_guard[mismatch]: kernel NOT launched");
    }

    /* ---- Run 3: expected_image_id 0 disables the guard. ---- */
    setup_graph(/* node_count */ 1, /* fake_kernels */ 1);

    make_kernel(&G_NODES[0], /* kernel_idx */ 0,
                /* await */ 0, 0x00, /* set */ 0, 0x01,
                /* arg_buf_offset */ 0u, /* arg_count */ 0);
    G_NODES[0].payload.kernel_dispatch.expected_image_id = 0u;

    rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "image_guard[unguarded]: rp1_run rc");
    CHECK_EQ32(g_node_status[0], RP1_NODE_DONE, "image_guard[unguarded]: dispatch DONE");
    {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
        CHECK_EQ32(ctrl[0], 0x3u, "image_guard[unguarded]: kernel launched");
    }
    return 0;
}

/*
 * Fatal-path graphs end in a nonzero SIGNAL sentinel gated on all upstream
 * barriers. It must remain zero: any write proves scheduling escaped terminal
 * quiescence. The kernel case also resubmits to test the reset-only latch.
 */
static int test_fatal_kernel_quiesce_and_reject(void)
{
    setup_graph(/* node_count */ 3, /* fake_kernels */ 2);
    pdi_override_reset();
    make_kernel(&G_NODES[0], 0, 0, 0u, 0, 1u, 0u, 0u);
    G_NODES[0].flags = RP1_FLAG_HALT_ON_ERROR;
    G_NODES[0].payload.kernel_dispatch.timeout_cycles = 3u;
    make_kernel(&G_NODES[1], 1, 0, 0u, 0, 2u, 0u, 0u);
    G_NODES[1].payload.kernel_dispatch.timeout_cycles = 100u;
    make_signal(&G_NODES[2], 40u, 0x51514E54u, RP1_SIGOP_SET,
                0, 3u, 0, 4u);
    s_skip_completion_node = 0u;
    s_terminal_idle_calls = 0u;

    int rc = rp1_run(&s_terminal_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "fatal_kernel: terminal error result");
    CHECK_EQ32(G_CTRL->rp1_state, RP1_STATE_ERROR,
               "fatal_kernel: terminal state");
    CHECK_EQ32(G_CTRL->rp1_error_code & RP1_ERR_CODE_MASK,
               RP1_ERR_KERNEL_TIMEOUT,
               "fatal_kernel: first error code");
    CHECK((G_CTRL->rp1_error_code & RP1_ERR_RECOVERY_REQUIRED) != 0u,
          "fatal_kernel: unresponsive work requires recovery");
    CHECK_EQ32(G_CTRL->terminal_error_node, 0u,
               "fatal_kernel: failing node latched");
    CHECK_EQ32(G_CTRL->terminal_error_detail, (uint32_t)FAKE_KERNEL(0),
               "fatal_kernel: failing kernel base");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 3u,
               "fatal_kernel: timeout ticks");
    CHECK_EQ32(G_SIGS[40].value, 0u,
               "fatal_kernel: sentinel did not run");
    CHECK_EQ32(G_CTRL->cq_write_idx, 2u,
               "fatal_kernel: error and quiesced CQ evidence");
    CHECK_EQ32(G_CQ[0].status, RP1_CQ_TIMEOUT,
               "fatal_kernel: timeout evidence first");
    CHECK_EQ32(G_CQ[1].node_index, 1u,
               "fatal_kernel: finite peer quiesced");
    CHECK_EQ32(G_CQ[1].status, RP1_CQ_OK,
               "fatal_kernel: finite peer completion retained");
    CHECK_EQ32(G_CTRL->graph_seq, 2u,
               "fatal_kernel: later graph was submitted");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,
               "fatal_kernel: terminal firmware rejected later graph");
    CHECK_EQ32(G_SIGS[63].value, 0u,
               "fatal_kernel: rejected graph had no side effect");
    return 0;
}

static int test_fatal_pdi_quiesce_and_recovery(void)
{
    setup_graph(/* node_count */ 4, /* fake_kernels */ 2);
    pdi_override_reset();
    s_pdi_status = 0x8000F00Du;
    s_pdi_detail = 0x1234ABCDu;

    make_kernel(&G_NODES[0], 0, 0, 0u, 0, 1u, 0u, 0u);
    G_NODES[0].payload.kernel_dispatch.timeout_cycles = 100u;
    make_kernel(&G_NODES[1], 1, 0, 0u, 0, 2u, 0u, 0u);
    G_NODES[1].flags = RP1_FLAG_INFINITE;
    G_NODES[1].payload.kernel_dispatch.timeout_cycles = 100u;
    make_pdi_load(&G_NODES[2], 0x10000000u, 0u, 20u,
                  RP1_FLAG_HALT_ON_ERROR, 0, 0u, 0, 4u);
    make_signal(&G_NODES[3], 41u, 0x51514E54u, RP1_SIGOP_SET,
                0, 7u, 0, 8u);
    s_skip_completion_node = 1u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "fatal_pdi: terminal error result");
    CHECK_EQ32(G_CTRL->rp1_error_code & RP1_ERR_CODE_MASK,
               RP1_ERR_PDI_FAILED,
               "fatal_pdi: first error remains PDI");
    CHECK((G_CTRL->rp1_error_code & RP1_ERR_RECOVERY_REQUIRED) != 0u,
          "fatal_pdi: infinite kernel requires recovery");
    CHECK_EQ32(G_CTRL->terminal_error_node, 2u,
               "fatal_pdi: failing node latched");
    CHECK_EQ32(G_CTRL->terminal_error_detail, 0x8000F00Du,
               "fatal_pdi: high-bit PLM status");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 0x1234ABCDu,
               "fatal_pdi: full PLM detail");
    CHECK_EQ32(G_SIGS[41].value, 0u,
               "fatal_pdi: sentinel did not run");
    CHECK_EQ32(G_CTRL->cq_write_idx, 3u,
               "fatal_pdi: infinite, PDI, finite evidence");
    CHECK_EQ32(G_CQ[1].node_index, 2u,
               "fatal_pdi: PDI CQ evidence");
    CHECK_EQ32(G_CQ[1].error_detail, 0x8000F00Du,
               "fatal_pdi: CQ preserves PLM status");
    CHECK_EQ32(G_CQ[2].node_index, 0u,
               "fatal_pdi: finite kernel quiesced");
    return 0;
}

/* -------------------------------------------------------------------------
 * Runner
 * ---------------------------------------------------------------------- */

static int run(const char *name, int (*fn)(void))
{
    semi_puts(name);
    semi_puts(": ");
    int r = fn();
    if (r == 0) semi_puts("PASS\n");
    return r;
}

void rp1_graph_test_run(void)
{
    run("diamond_dag",         test_diamond_dag);
    run("cq_timestamps",       test_cq_timestamps);
    run("trace_disabled_by_default", test_trace_disabled_by_default);
    run("trace_queue",         test_trace_queue);
    run("kernel_unblocks_signal", test_kernel_unblocks_signal);
    run("signal_chain",        test_signal_chain);
    run("cq_flow_control",     test_cq_flow_control);
    run("cq_config_validation", test_cq_config_validation);
    run("cq_cursor_wrap",      test_cq_cursor_wrap);
    run("graph_sequence_wrap", test_graph_sequence_wrap);
    run("signal_slot_validation", test_signal_slot_validation);
    run("loop_decrement",      test_loop_decrement);
    run("loop_fixed_count",    test_loop_fixed_count);
    run("cond_boolean",        test_cond_boolean);
    run("scalar_read",         test_scalar_read);
    run("wait_blocks",         test_wait_blocks);
    run("pdi_mmio_contract", test_pdi_mmio_contract);
    run("pdi_timeout_invariant", test_pdi_timeout_invariant);
    run("kernel_timeout_invariant", test_kernel_timeout_invariant);
    run("silent_pdi_rejected", test_silent_pdi_rejected);
    run("pdi_load_basic",   test_pdi_load_basic);
    run("pdi_load_timeout", test_pdi_load_timeout);
    run("pdi_load_chained", test_pdi_load_chained);
    run("image_guard",      test_image_guard);
    run("fatal_kernel_quiesce_and_reject",
        test_fatal_kernel_quiesce_and_reject);
    run("fatal_pdi_quiesce_and_recovery",
        test_fatal_pdi_quiesce_and_recovery);
}

#endif /* QEMU_SEMIHOSTING */
