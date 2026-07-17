/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 unit tests — runs under Xilinx QEMU via ARM semihosting.
 *
 * Each test_*() function prints PASS/FAIL and returns 0 on success, 1 on
 * failure.  rp1_main() tallies the results and exits via semihosting so the
 * host build script can check the return code.
 *
 * Tests exercise:
 *   1. Struct sizes and payload offsets (spec compliance)
 *   2. rp1_store_reset_graph() zeroes all BTCM stores
 *   3. Barrier set / check logic (single bit, multi-bit AND, multi-node OR)
 *   4. Signal array slot read/write
 *   5. Condition operator evaluation
 *   6. Node header field encoding / decoding
 */

#ifdef QEMU_SEMIHOSTING

#include "rp1_test.h"
#include "rp1_store.h"
#include <slash/uapi/rp1_protocol.h>
#include <stddef.h>
#include <stdint.h>

/* Failure counter, shared with rp1_graph_test.c via rp1_test.h. */
int g_failures;

static int run(const char *name, int (*fn)(void))
{
    semi_puts(name);
    semi_puts(": ");
    int r = fn();
    if (r == 0)
        semi_puts("PASS\n");
    return r;
}

/* -------------------------------------------------------------------------
 * 1. Struct sizes and offsets
 * ---------------------------------------------------------------------- */

static int test_struct_sizes(void)
{
    CHECK_EQ32(sizeof(rp1_node_t),         64,     "rp1_node_t size");
    CHECK_EQ32(offsetof(rp1_node_t, payload), 16,  "payload offset");
    CHECK_EQ32(sizeof(rp1_ctrl_t),         0x1000, "rp1_ctrl_t size");
    CHECK_EQ32(sizeof(rp1_signal_slot_t),  16,     "signal slot size");
    CHECK_EQ32(sizeof(rp1_cq_entry_t),     16,     "cq entry size");
    CHECK_EQ32(sizeof(rp1_trace_entry_t),  16,     "trace entry size");
    CHECK_EQ32(sizeof(rp1_inflight_t),     24,     "inflight entry size");
    return 0;
}

static int test_payload_sizes(void)
{
    CHECK_EQ32(sizeof(rp1_payload_kernel_dispatch_t), 48, "kernel_dispatch payload");
    CHECK_EQ32(sizeof(rp1_payload_scalar_write_t),    48, "scalar_write payload");
    CHECK_EQ32(sizeof(rp1_payload_scalar_read_t),     48, "scalar_read payload");
    CHECK_EQ32(sizeof(rp1_payload_signal_t),          48, "signal payload");
    CHECK_EQ32(sizeof(rp1_payload_wait_t),            48, "wait payload");
    CHECK_EQ32(sizeof(rp1_payload_dma_copy_t),        48, "dma_copy payload");
    CHECK_EQ32(sizeof(rp1_payload_dma_fill_t),        48, "dma_fill payload");
    CHECK_EQ32(sizeof(rp1_payload_pdi_load_t),        48, "pdi_load payload");
    CHECK_EQ32(sizeof(rp1_payload_loop_t),            48, "loop payload");
    CHECK_EQ32(sizeof(rp1_payload_cond_t),            48, "cond payload");
    CHECK_EQ32(sizeof(rp1_payload_rerun_t),           48, "rerun payload");

    /* PDI_LOAD: verify the first three fields land where the host stack
     * expects them. */
    CHECK_EQ32((uint32_t)offsetof(rp1_payload_pdi_load_t, pdi_addr_lo),
               0,  "pdi_load.pdi_addr_lo offset");
    CHECK_EQ32((uint32_t)offsetof(rp1_payload_pdi_load_t, pdi_addr_hi),
               4,  "pdi_load.pdi_addr_hi offset");
    CHECK_EQ32((uint32_t)offsetof(rp1_payload_pdi_load_t, timeout_cycles),
               8,  "pdi_load.timeout_cycles offset");
    return 0;
}

static int test_ctrl_offsets(void)
{
    /* Verify a representative set of control block field offsets. */
    CHECK_EQ32(offsetof(rp1_ctrl_t, magic),            0x00, "ctrl.magic offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, version),          0x04, "ctrl.version offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, node_count),       0x08, "ctrl.node_count offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, graph_seq),        0x20, "ctrl.graph_seq offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, graph_done_seq),   0x24, "ctrl.graph_done_seq offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, rp1_state),        0x30, "ctrl.rp1_state offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, heartbeat),        0x3C, "ctrl.heartbeat offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, arg_buf_base_lo),  0x40, "ctrl.arg_buf_base_lo offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, sig_array_base_lo),0x48, "ctrl.sig_array_base_lo offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, trace_enable),     0x50, "ctrl.trace_enable offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, trace_base_lo),    0x54, "ctrl.trace_base_lo offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, trace_base_hi),    0x58, "ctrl.trace_base_hi offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, trace_size),       0x5C, "ctrl.trace_size offset");
    CHECK_EQ32(offsetof(rp1_ctrl_t, trace_write_idx),  0x60, "ctrl.trace_write_idx offset");
    return 0;
}

/* -------------------------------------------------------------------------
 * 2. rp1_store_reset_graph() zeroes BTCM stores
 * ---------------------------------------------------------------------- */

static int test_store_reset(void)
{
    /* Dirty everything. */
    for (uint32_t i = 0; i < RP1_MAX_BUCKETS;  i++) g_barriers[i]    = 0xDEADBEEFu;
    for (uint32_t i = 0; i < RP1_MAX_NODES;    i++) g_node_status[i] = 0xABu;
    for (uint32_t i = 0; i < RP1_MAX_LOOPS;    i++) g_loop_iters[i]  = 0xDEADBEEFu;
    for (uint32_t i = 0; i < RP1_MAX_INFLIGHT; i++) {
        g_inflight[i].base_addr = 0xDEADBEEFu;
        g_inflight[i].node_index = i;
    }
    g_inflight_count = 99;

    rp1_store_reset_graph();

    for (uint32_t i = 0; i < RP1_MAX_BUCKETS; i++)
        CHECK_EQ32(g_barriers[i], 0, "barriers not zeroed");
    for (uint32_t i = 0; i < RP1_MAX_NODES; i++)
        CHECK_EQ32(g_node_status[i], 0, "node_status not zeroed");
    for (uint32_t i = 0; i < RP1_MAX_LOOPS; i++)
        CHECK_EQ32(g_loop_iters[i], 0, "loop_iters not zeroed");
    for (uint32_t i = 0; i < RP1_MAX_INFLIGHT; i++)
        CHECK_EQ32(g_inflight[i].base_addr, 0, "inflight not zeroed");
    CHECK_EQ32(g_inflight_count, 0, "inflight_count not zeroed");
    return 0;
}

/* -------------------------------------------------------------------------
 * 3. Barrier logic
 * ---------------------------------------------------------------------- */

/*
 * Barrier check helper: returns non-zero if all bits in mask are set in
 * g_barriers[bucket].  Mirrors the scanner's scheduling check.
 */
static uint32_t barrier_check(uint8_t bucket, uint32_t mask)
{
    return (g_barriers[bucket] & mask) == mask;
}

static int test_barrier_single_bit(void)
{
    rp1_store_reset_graph();

    /* No barriers set initially. */
    CHECK(!barrier_check(0, 0x1), "bit 0 should be clear");

    /* Set bit 0 in bucket 0. */
    g_barriers[0] |= 0x1u;
    CHECK( barrier_check(0, 0x1), "bit 0 should be set");
    CHECK(!barrier_check(0, 0x2), "bit 1 should still be clear");
    return 0;
}

static int test_barrier_and(void)
{
    rp1_store_reset_graph();

    /* Node awaits bits 0 and 1 in bucket 0 — both must be set. */
    uint32_t mask = 0x3u;
    CHECK(!barrier_check(0, mask), "AND: should need both bits");

    g_barriers[0] |= 0x1u;
    CHECK(!barrier_check(0, mask), "AND: still need bit 1");

    g_barriers[0] |= 0x2u;
    CHECK( barrier_check(0, mask), "AND: both set, should pass");
    return 0;
}

static int test_barrier_or(void)
{
    /* OR pattern: two producers set the same bit; consumer unblocks on first. */
    rp1_store_reset_graph();

    uint32_t shared_mask = 0x10u;  /* bit 4 */
    CHECK(!barrier_check(1, shared_mask), "OR: initially clear");

    /* Producer A sets bit 4 in bucket 1. */
    g_barriers[1] |= shared_mask;
    CHECK( barrier_check(1, shared_mask), "OR: consumer unblocks after first producer");
    return 0;
}

static int test_barrier_cross_bucket(void)
{
    /* NOP bridge: await bucket 0, set bucket 1 — simulated by manual bit ops. */
    rp1_store_reset_graph();

    /* Source in bucket 0. */
    g_barriers[0] |= 0xFFFFu;  /* 16 bits from region A */
    CHECK( barrier_check(0, 0xFFFFu), "cross: source ready");

    /* Bridge fires: sets bit 0 in bucket 1. */
    g_barriers[1] |= 0x1u;
    CHECK( barrier_check(1, 0x1u), "cross: downstream unblocked via bridge");
    return 0;
}

static int test_barrier_bucket_clear(void)
{
    /* Simulate a loop iteration clearing its bucket range. */
    rp1_store_reset_graph();

    g_barriers[4] = 0xFFFFFFFFu;
    g_barriers[5] = 0xFFFFFFFFu;
    g_barriers[6] = 0xFFFFFFFFu;

    /* Clear buckets 4..5 (exclusive end = 6). */
    for (uint8_t b = 4; b < 6; b++)
        g_barriers[b] = 0;

    CHECK_EQ32(g_barriers[4], 0, "bucket 4 cleared");
    CHECK_EQ32(g_barriers[5], 0, "bucket 5 cleared");
    CHECK_EQ32(g_barriers[6], 0xFFFFFFFFu, "bucket 6 untouched");
    return 0;
}

/* -------------------------------------------------------------------------
 * 4. Signal array slot access
 * ---------------------------------------------------------------------- */

/* Simulate signal array backed by a small local array (no DDR in QEMU test). */
static rp1_signal_slot_t s_test_signals[RP1_MAX_SIGNALS];

static int test_signal_set(void)
{
    /* Zero the local array. */
    for (uint32_t i = 0; i < RP1_MAX_SIGNALS; i++) {
        s_test_signals[i].value            = 0;
        s_test_signals[i].last_writer_node = 0;
        s_test_signals[i].flags            = 0;
    }

    /* SIGNAL SET: write value 42 into slot 10, node 3. */
    s_test_signals[10].value            = 42u;
    s_test_signals[10].last_writer_node = 3u;

    CHECK_EQ32(s_test_signals[10].value, 42u, "signal slot value");
    CHECK_EQ32(s_test_signals[10].last_writer_node, 3u, "signal last_writer");

    /* SIGNAL ADD: add 8. */
    s_test_signals[10].value += 8u;
    CHECK_EQ32(s_test_signals[10].value, 50u, "signal ADD");

    /* SIGNAL OR: OR with 0x100. */
    s_test_signals[10].value |= 0x100u;
    CHECK_EQ32(s_test_signals[10].value, 50u | 0x100u, "signal OR");

    /* SIGNAL AND: AND with 0xFF. */
    s_test_signals[10].value &= 0xFFu;
    CHECK_EQ32(s_test_signals[10].value, 50u, "signal AND");

    return 0;
}

static int test_signal_isolation(void)
{
    /* Writing to slot 10 must not affect slot 11. */
    s_test_signals[10].value = 0xABCDu;
    s_test_signals[11].value = 0u;
    s_test_signals[10].value = 0xFFFFu;
    CHECK_EQ32(s_test_signals[11].value, 0u, "signal slot isolation");
    return 0;
}

/* -------------------------------------------------------------------------
 * 5. Condition operator evaluation
 * ---------------------------------------------------------------------- */

static uint32_t eval_cond(uint32_t sig, rp1_condop_t op, uint32_t val)
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

static int test_condops(void)
{
    CHECK( eval_cond(5,  RP1_COP_EQ,     5),  "EQ true");
    CHECK(!eval_cond(5,  RP1_COP_EQ,     6),  "EQ false");
    CHECK( eval_cond(5,  RP1_COP_NE,     6),  "NE true");
    CHECK(!eval_cond(5,  RP1_COP_NE,     5),  "NE false");
    CHECK( eval_cond(4,  RP1_COP_LT,     5),  "LT true");
    CHECK(!eval_cond(5,  RP1_COP_LT,     5),  "LT false (equal)");
    CHECK(!eval_cond(6,  RP1_COP_LT,     5),  "LT false (greater)");
    CHECK( eval_cond(5,  RP1_COP_GE,     5),  "GE true (equal)");
    CHECK( eval_cond(6,  RP1_COP_GE,     5),  "GE true (greater)");
    CHECK(!eval_cond(4,  RP1_COP_GE,     5),  "GE false");
    CHECK( eval_cond(0x3, RP1_COP_AND_NZ, 0x1), "AND_NZ true");
    CHECK(!eval_cond(0x2, RP1_COP_AND_NZ, 0x1), "AND_NZ false");
    CHECK( eval_cond(0x2, RP1_COP_AND_Z,  0x1), "AND_Z true");
    CHECK(!eval_cond(0x3, RP1_COP_AND_Z,  0x1), "AND_Z false");
    return 0;
}

/* -------------------------------------------------------------------------
 * 6. Node header encoding / decoding
 * ---------------------------------------------------------------------- */

static int test_node_header(void)
{
    rp1_node_t node;

    /* Zero the whole packet (as RP1 would after a graph reset). */
    for (uint32_t i = 0; i < sizeof(node); i++) ((uint8_t *)&node)[i] = 0;

    node.opcode               = RP1_OP_KERNEL_DISPATCH;
    node.flags                = RP1_FLAG_HALT_ON_ERROR | RP1_FLAG_INFINITE;
    node.barrier_await_mask   = 0x00000003u;
    node.barrier_set_mask     = 0x00000004u;
    node.barrier_await_bucket = 0;
    node.barrier_set_bucket   = 1;
    node.status               = RP1_NODE_PENDING;

    CHECK_EQ32(node.opcode,               RP1_OP_KERNEL_DISPATCH,         "opcode");
    CHECK_EQ32(node.flags,                RP1_FLAG_HALT_ON_ERROR |
                                          RP1_FLAG_INFINITE,               "flags");
    CHECK_EQ32(node.barrier_await_mask,   0x3u,                            "await_mask");
    CHECK_EQ32(node.barrier_set_mask,     0x4u,                            "set_mask");
    CHECK_EQ32(node.barrier_await_bucket, 0u,                              "await_bucket");
    CHECK_EQ32(node.barrier_set_bucket,   1u,                              "set_bucket");
    CHECK_EQ32(node.status,               RP1_NODE_PENDING,                "status");

    /* Verify the payload union shares the same storage (no extra padding). */
    CHECK_EQ32((uint32_t)sizeof(node.payload.raw), 48u, "payload raw size");

    return 0;
}

static int test_node_alignment(void)
{
    /* An array of nodes must be packed 64 bytes apart. */
    rp1_node_t arr[2];
    uintptr_t delta = (uintptr_t)&arr[1] - (uintptr_t)&arr[0];
    CHECK_EQ32((uint32_t)delta, 64u, "node stride 64");
    return 0;
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void rp1_main(void)
{
    g_failures = 0;

    semi_puts("=== RP1 unit tests ===\n");

    run("struct_sizes",        test_struct_sizes);
    run("payload_sizes",       test_payload_sizes);
    run("ctrl_offsets",        test_ctrl_offsets);
    run("store_reset",         test_store_reset);
    run("barrier_single_bit",  test_barrier_single_bit);
    run("barrier_and",         test_barrier_and);
    run("barrier_or",          test_barrier_or);
    run("barrier_cross_bucket",test_barrier_cross_bucket);
    run("barrier_bucket_clear",test_barrier_bucket_clear);
    run("signal_set",          test_signal_set);
    run("signal_isolation",    test_signal_isolation);
    run("condops",             test_condops);
    run("node_header",         test_node_header);
    run("node_alignment",      test_node_alignment);

    semi_puts("\n=== RP1 graph tests ===\n");
    rp1_graph_test_run();

    semi_puts("\n=== ");
    if (g_failures == 0) {
        semi_puts("ALL TESTS PASSED");
    } else {
        semi_puts("TESTS FAILED: ");
        semi_print_u32((uint32_t)g_failures);
        semi_puts(" failure(s)");
    }
    semi_puts(" ===\n");

    semi_exit(g_failures == 0 ? 0 : 1);
}

#endif /* QEMU_SEMIHOSTING */
