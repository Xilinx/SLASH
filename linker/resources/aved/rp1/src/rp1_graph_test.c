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

#define TEST_CQ_SIZE  64u

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
    s_wait_armed = 0;
    s_wait_after = 0;
    s_wait_slot = 0;
    s_wait_value = 0;
    s_wait_witness_slot = 0;
    s_wait_witness_at_fire = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < TRACE_MAX / 32u; i++) s_seen[i] = 0;
    for (uint32_t i = 0; i < TRACE_MAX; i++) s_trace[i] = 0;
}

static void hook_on_scan_pass(void)
{
    s_pass_count++;

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
 * rp1_pdi_load() override — preempts the weak default at link time.
 *
 * Records the arguments handed to it on each call so the test bodies can
 * assert what the scanner forwarded.  s_pdi_force_rc lets the caller pick
 * the return value (0 = success / 1 = timeout) to exercise both paths.
 * ---------------------------------------------------------------------- */

static uint32_t s_pdi_call_count;
static uint32_t s_pdi_last_addr_lo;
static uint32_t s_pdi_last_addr_hi;
static uint32_t s_pdi_last_timeout;
static int      s_pdi_force_rc;

static void pdi_override_reset(void)
{
    s_pdi_call_count   = 0;
    s_pdi_last_addr_lo = 0;
    s_pdi_last_addr_hi = 0;
    s_pdi_last_timeout = 0;
    s_pdi_force_rc     = 0;
}

int rp1_pdi_load(uint32_t addr_lo, uint32_t addr_hi, uint32_t timeout_cycles)
{
    s_pdi_call_count++;
    s_pdi_last_addr_lo = addr_lo;
    s_pdi_last_addr_hi = addr_hi;
    s_pdi_last_timeout = timeout_cycles;
    return s_pdi_force_rc;
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
    CHECK_EQ32(s_pdi_last_timeout,     12345u,      "pdi_basic: timeout forwarded");

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
 *   returns 1, which the scanner must surface as ERR_PDI_TIMEOUT (3).
 * ---------------------------------------------------------------------- */

static int test_pdi_load_timeout(void)
{
    /* ---- Run 1: non-fatal timeout (no HALT_ON_ERROR) ---- */
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_force_rc = 1;  /* force timeout */

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 0u, /* flags */ 0,
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
    s_pdi_force_rc = 1;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 0u,
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
    run("kernel_unblocks_signal", test_kernel_unblocks_signal);
    run("signal_chain",        test_signal_chain);
    run("loop_decrement",      test_loop_decrement);
    run("loop_fixed_count",    test_loop_fixed_count);
    run("cond_boolean",        test_cond_boolean);
    run("scalar_read",         test_scalar_read);
    run("wait_blocks",         test_wait_blocks);
    run("pdi_load_basic",   test_pdi_load_basic);
    run("pdi_load_timeout", test_pdi_load_timeout);
    run("pdi_load_chained", test_pdi_load_chained);
    run("image_guard",      test_image_guard);
}

#endif /* QEMU_SEMIHOSTING */
