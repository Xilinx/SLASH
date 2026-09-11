/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * End-to-end graph tests for the RP1 flat scanner, running under Xilinx
 * QEMU with ARM semihosting.  Each test builds a tiny graph in shared
 * DDR, hands it to rp1_run() via the on_scan_pass / on_graph_done hooks
 * defined in rp1_loop.h, and asserts on node state, barriers, signal slots,
 * terminal graph results, and dispatch order.
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
 * control block sits at RP1_CTRL_PHYS_ADDR; nodes, args, signals, and trace
 * storage follow at the documented offsets.
 * ---------------------------------------------------------------------- */

#define G_CTRL  ((volatile rp1_ctrl_t *)(uintptr_t)(RP1_CTRL_PHYS_ADDR))
#define G_NODES ((rp1_node_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET))
#define G_ARGS  ((uint32_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET))
#define G_SIGS  ((volatile rp1_signal_slot_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET))
#define G_TRACE ((volatile rp1_trace_entry_t *)(uintptr_t) \
                 (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_TRACE_OFFSET))

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
static uint32_t s_pending_graph_seq;

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
    s_wait_armed = 0;
    s_wait_after = 0;
    s_wait_slot = 0;
    s_wait_value = 0;
    s_wait_witness_slot = 0;
    s_wait_witness_at_fire = 0xFFFFFFFFu;
    s_pending_graph_seq = 1u;
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
        uint8_t st = rp1_node_get_status(&g_nodes[i]);
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

/*
 * Model a host that waits for firmware readiness before ringing the doorbell.
 * setup_graph() records the sequence because startup deliberately clears DDR.
 */
static int submit_on_idle(void)
{
    if (s_pending_graph_seq != 0u) {
        G_CTRL->graph_seq = s_pending_graph_seq;
        s_pending_graph_seq = 0u;
    }
    return 0;
}

static const rp1_hooks_t s_hooks = {
    .on_scan_pass  = hook_on_scan_pass,
    .on_graph_done = hook_on_graph_done,
    .on_idle       = submit_on_idle,
};

/*
 * Model a later submission in one still-running firmware instance. QEMU tests
 * call rp1_run() as a bounded harness, so seed the previously installed image
 * after startup initialization but before ringing this graph's doorbell.
 */
static int submit_known_image_on_idle(void)
{
    if (s_pending_graph_seq != 0u) {
        g_active_image_id = 7u;
        g_active_image_state = RP1_IMAGE_STATE_KNOWN;
        G_CTRL->graph_seq = s_pending_graph_seq;
        s_pending_graph_seq = 0u;
    }
    return 0;
}

static const rp1_hooks_t s_known_image_hooks = {
    .on_scan_pass  = hook_on_scan_pass,
    .on_graph_done = hook_on_graph_done,
    .on_idle       = submit_known_image_on_idle,
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
        G_CTRL->graph_seq = 0u;
        return 0;
    }
    return 1;
}

static const rp1_hooks_t s_wrap_hooks = {
    .on_scan_pass = hook_on_scan_pass,
    .on_graph_done = wrap_on_graph_done,
    .on_idle = submit_on_idle,
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
    if (s_pending_graph_seq != 0u)
        return submit_on_idle();
    s_terminal_idle_calls++;
    return s_terminal_idle_calls >= 2u;
}

static const rp1_hooks_t s_terminal_hooks = {
    .on_scan_pass = hook_on_scan_pass,
    .on_graph_done = terminal_resubmit_on_done,
    .on_idle = terminal_exit_on_idle,
};

static uint32_t s_boot_graphs;

/* Record an unexpected stale graph execution and stop the firmware loop. */
static int boot_graph_done(int result)
{
    (void)result;
    s_boot_graphs++;
    return 1;
}

/* Stop after startup reaches its first idle observation. */
static int boot_exit_on_idle(void)
{
    return 1;
}

static const rp1_hooks_t s_boot_hooks = {
    .on_scan_pass = hook_on_scan_pass,
    .on_graph_done = boot_graph_done,
    .on_idle = boot_exit_on_idle,
};

/* -------------------------------------------------------------------------
 * Graph setup
 * ---------------------------------------------------------------------- */

static void setup_graph(uint32_t node_count, uint32_t fake_kernel_count)
{
    /* Wipe only the regions we touch. rp1_run() replaces the BTCM node
     * snapshot and resets barriers, loop counters, and inflight state through
     * rp1_store_init() for each new graph. */
    tmemzero((volatile void *)G_CTRL,  sizeof(rp1_ctrl_t));
    tmemzero((volatile void *)G_NODES, node_count * sizeof(rp1_node_t));
    tmemzero((volatile void *)G_ARGS,  64u * sizeof(uint32_t));
    tmemzero((volatile void *)G_SIGS,  64u * sizeof(rp1_signal_slot_t));
    tmemzero((volatile void *)G_TRACE, TEST_TRACE_SIZE * sizeof(rp1_trace_entry_t));
    if (fake_kernel_count > 0) {
        tmemzero((volatile void *)(uintptr_t)FAKE_KERNEL_BASE,
                 fake_kernel_count * FAKE_KERNEL_STRIDE);
    }

    G_CTRL->node_count        = node_count;
    G_CTRL->node_base_lo      = (uint32_t)(uintptr_t)G_NODES;
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

/* Initialize one compact header with firmware-owned status set to PENDING. */
static void make_header(rp1_node_t *n, uint8_t opcode, uint8_t flags,
                        uint8_t aw_b, uint32_t aw_m,
                        uint8_t st_b, uint32_t st_m)
{
    rp1_node_set_control(
        n, rp1_node_make_control(opcode, flags, RP1_NODE_PENDING));
    n->barrier_await_mask = aw_m;
    n->barrier_set_mask = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket = st_b;
}

static void make_kernel(rp1_node_t *n, uint32_t kernel_idx,
                        uint8_t aw_b, uint32_t aw_m,
                        uint8_t st_b, uint32_t st_m,
                        uint32_t arg_buf_offset, uint16_t arg_count)
{
    make_header(n, RP1_OP_KERNEL_DISPATCH, 0u,
                aw_b, aw_m, st_b, st_m);

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
    make_header(n, RP1_OP_SIGNAL, 0u, aw_b, aw_m, st_b, st_m);

    n->payload.signal.value       = value;
    n->payload.signal.target_slot = (uint8_t)slot;
    n->payload.signal.operation   = (uint8_t)op;
}

static void make_scalar_write(rp1_node_t *n, uint32_t addr, uint32_t value,
                              uint8_t aw_b, uint32_t aw_m,
                              uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_SCALAR_WRITE, 0u,
                aw_b, aw_m, st_b, st_m);

    n->payload.scalar_write.writes[0].addr  = addr;
    n->payload.scalar_write.writes[0].value = value;
}

static void make_scalar_read(rp1_node_t *n, uint32_t source_addr, uint32_t target_slot,
                             uint8_t aw_b, uint32_t aw_m,
                             uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_SCALAR_READ, 0u,
                aw_b, aw_m, st_b, st_m);

    n->payload.scalar_read.source_addr = source_addr;
    n->payload.scalar_read.target_slot = (uint8_t)target_slot;
}

static void make_wait(rp1_node_t *n,
                      uint32_t cond_signal, uint16_t cond_op, uint32_t cond_val,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_WAIT, 0u, aw_b, aw_m, st_b, st_m);

    n->payload.wait.condition_value  = cond_val;
    n->payload.wait.condition_signal = (uint8_t)cond_signal;
    n->payload.wait.condition_op     = (uint8_t)cond_op;
}

static void make_loop(rp1_node_t *n,
                      uint32_t body_start, uint32_t body_end,
                      uint32_t cond_signal, uint16_t cond_op, uint32_t cond_val,
                      uint8_t bucket_clear_start, uint8_t bucket_clear_end,
                      uint8_t loop_id, uint32_t max_iter,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_LOOP, 0u, aw_b, aw_m, st_b, st_m);

    n->payload.loop.body_start         = (uint16_t)body_start;
    n->payload.loop.body_end           = (uint16_t)body_end;
    n->payload.loop.max_iterations     = max_iter;
    n->payload.loop.condition_value    = cond_val;
    n->payload.loop.condition_signal   = (uint8_t)cond_signal;
    n->payload.loop.condition_op       = (uint8_t)cond_op;
    n->payload.loop.bucket_clear_start = bucket_clear_start;
    n->payload.loop.bucket_clear_end   = bucket_clear_end;
    n->payload.loop.loop_id            = loop_id;
}

static void make_rerun(rp1_node_t *n, uint32_t target_node,
                       uint8_t aw_b, uint32_t aw_m,
                       uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_RERUN, 0u, aw_b, aw_m, st_b, st_m);

    n->payload.rerun.target_node = (uint16_t)target_node;
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
    make_header(n, RP1_OP_COND, 0u, aw_b, aw_m, st_b, st_m);

    n->payload.cond.condition_value    = cond_val;
    n->payload.cond.condition_signal   = (uint8_t)cond_signal;
    n->payload.cond.condition_op       = (uint8_t)cond_op;
    n->payload.cond.bucket_clear_start = bucket_clear_start;
    n->payload.cond.bucket_clear_end   = bucket_clear_end;
    n->payload.cond.body_start         = (uint16_t)body_start;
    n->payload.cond.body_end           = (uint16_t)body_end;
    n->payload.cond.done_bucket        = done_bucket;
    n->payload.cond.done_mask          = done_mask;
}

static void make_pdi_load(rp1_node_t *n,
                          uint32_t addr_lo, uint32_t addr_hi,
                          uint32_t timeout_cycles, uint8_t flags,
                          uint8_t aw_b, uint32_t aw_m,
                          uint8_t st_b, uint32_t st_m)
{
    make_header(n, RP1_OP_PDI_LOAD, flags, aw_b, aw_m, st_b, st_m);

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
static uint32_t s_publication_watch;
static uint32_t s_publication_started;
static uint32_t s_publication_phase;
static uint32_t s_publication_violation;
/* Optional fake control port whose reads emulate HLS ap_done clear-on-read. */
static uintptr_t s_watched_kernel_ctrl;
/* Number of MMIO reads observed at s_watched_kernel_ctrl. */
static uint32_t s_watched_kernel_ctrl_reads;

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
    if (address == s_watched_kernel_ctrl) {
        volatile uint32_t *control = (volatile uint32_t *)address;
        uint32_t value = *control;

        s_watched_kernel_ctrl_reads++;
        *control = value & ~0x2u;
        return value;
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

static void test_barrier(rp1_barrier_kind_t barrier, void *context)
{
    (void)context;
    record_pdi_access(PDI_ACCESS_BARRIER, 0u, (uint32_t)barrier);

    if (!s_publication_watch)
        return;
    if (G_CTRL->rp1_state == RP1_STATE_RUNNING)
        s_publication_started = 1u;
    if (!s_publication_started)
        return;

    if (G_CTRL->graph_done_seq == G_CTRL->graph_seq) {
        if (G_CTRL->result.magic != RP1_GRAPH_RESULT_MAGIC ||
            G_CTRL->rp1_state == RP1_STATE_RUNNING ||
            s_publication_phase < 2u)
            s_publication_violation = 1u;
        s_publication_phase = 3u;
    } else if (G_CTRL->rp1_state != RP1_STATE_RUNNING) {
        if (G_CTRL->result.magic != RP1_GRAPH_RESULT_MAGIC ||
            s_publication_phase < 1u)
            s_publication_violation = 1u;
        s_publication_phase = 2u;
    } else if (G_CTRL->result.magic == RP1_GRAPH_RESULT_MAGIC) {
        s_publication_phase = 1u;
    }
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
    s_publication_watch = 0u;
    s_publication_started = 0u;
    s_publication_phase = 0u;
    s_publication_violation = 0u;
    s_watched_kernel_ctrl = 0u;
    s_watched_kernel_ctrl_reads = 0u;
    rp1_hal_set_hooks(&s_hal_hooks);
}

/*
 * Count control reads for one fake CU and model the clear-on-read ap_done bit.
 * The normal scan hook still decides when the fake invocation completes.
 */
static void watch_kernel_ctrl(uintptr_t address)
{
    s_watched_kernel_ctrl = address;
    s_watched_kernel_ctrl_reads = 0u;
}

/* Verify each public barrier primitive preserves its distinct HAL identity. */
static int test_barrier_variants(void)
{
    pdi_override_reset();

    rp1_dsb_st();
    rp1_dsb_sy();
    rp1_dmb_st();
    rp1_dmb_sy();
    rp1_hal_reset_hooks();

    CHECK_EQ32(s_pdi_access_count, 4u, "barriers: all variants observed");
    CHECK_EQ32(s_pdi_access[0].value, RP1_BARRIER_DSB_ST,
               "barriers: dsb st");
    CHECK_EQ32(s_pdi_access[1].value, RP1_BARRIER_DSB_SY,
               "barriers: dsb sy");
    CHECK_EQ32(s_pdi_access[2].value, RP1_BARRIER_DMB_ST,
               "barriers: dmb st");
    CHECK_EQ32(s_pdi_access[3].value, RP1_BARRIER_DMB_SY,
               "barriers: dmb sy");
    return 0;
}

/*
 * Observe each firmware barrier and require the release sequence to progress
 * from committed result, to terminal state, to graph_done_seq.
 */
static int test_result_publication_order(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();
    make_signal(&G_NODES[0], 0u, 0xABCDu, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    s_publication_watch = 1u;

    int rc = rp1_run(&s_hooks);
    s_publication_watch = 0u;
    rp1_hal_reset_hooks();

    CHECK_EQ32(rc, 0u, "publish_order: rp1_run rc");
    CHECK_EQ32(s_publication_violation, 0u,
               "publish_order: no release-order violation");
    CHECK_EQ32(s_publication_phase, 3u,
               "publish_order: completion sequence observed");
    return 0;
}

/*
 * A firmware reload must discard both sequence words before publishing magic.
 * Otherwise the stale host doorbell can execute the previous graph immediately.
 */
static int test_boot_sequence_baseline(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0u, 0xABCDu, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->graph_seq = 41u;
    G_CTRL->graph_done_seq = 40u;
    s_boot_graphs = 0u;

    int rc = rp1_run(&s_boot_hooks);

    CHECK_EQ32(rc, 0u, "boot_baseline: idle return");
    CHECK_EQ32(G_CTRL->graph_seq, 0u, "boot_baseline: graph_seq reset");
    CHECK_EQ32(G_CTRL->graph_done_seq, 0u,
               "boot_baseline: graph_done_seq reset");
    CHECK_EQ32(s_boot_graphs, 0u, "boot_baseline: stale graph not executed");
    CHECK_EQ32(G_SIGS[0].value, 0u, "boot_baseline: stale graph had no effect");
    CHECK_EQ32(G_CTRL->rp1_state, RP1_STATE_READY,
               "boot_baseline: firmware ready");
    return 0;
}

/*
 * Protocol-v6 hosts must zero every former CQ control word. Rejecting stale
 * v4 configuration prevents an old host from submitting under the new ABI.
 */
static int test_reserved_cq_config_rejected(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0u, 0xABCDu, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    G_CTRL->_reserved_cq_size = 64u;

    int rc = rp1_run(&s_hooks);

    CHECK(rc == -1, "reserved_cq: graph failed");
    CHECK_EQ32(G_CTRL->result.magic, RP1_GRAPH_RESULT_MAGIC,
               "reserved_cq: result committed");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_FAILED,
               "reserved_cq: failed outcome");
    CHECK_EQ32(G_CTRL->result.error_code, RP1_ERR_INVALID_CONFIG,
               "reserved_cq: invalid config");
    CHECK_EQ32(G_CTRL->result.error_detail, RP1_CONFIG_RESERVED_CQ,
               "reserved_cq: detail");
    CHECK_EQ32(G_CTRL->result.error_aux, 1u,
               "reserved_cq: size bit reported");
    CHECK_EQ32(G_SIGS[0].value, 0u,
               "reserved_cq: graph had no side effect");
    return 0;
}

/*
 * Firmware must execute only the BTCM snapshot. Mutating every meaningful DDR
 * field after rp1_store_init() must not redirect execution or receive status
 * writeback from the scanner.
 */
static int test_btcm_node_snapshot(void)
{
    setup_graph(/* node_count */ 1u, /* fake_kernels */ 0u);
    make_signal(&G_NODES[0], 4u, 0x11112222u, RP1_SIGOP_SET,
                0u, 0u, 0u, 1u);
    for (uint32_t i = 8u; i < sizeof(G_NODES[0].payload.raw); i++)
        G_NODES[0].payload.raw[i] = (uint8_t)(0x80u + i);
    rp1_node_set_status(&G_NODES[0], RP1_NODE_ERROR);
    uint8_t expected_payload[sizeof(G_NODES[0].payload.raw)];
    for (uint32_t i = 0u; i < sizeof(expected_payload); i++)
        expected_payload[i] = G_NODES[0].payload.raw[i];

    uint32_t detail = 0u;
    uint32_t aux = 0u;
    CHECK_EQ32(rp1_store_init(&detail, &aux), 0u,
               "snapshot: store accepted");
    CHECK_EQ32(g_node_count, 1u, "snapshot: local count cached");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_PENDING,
               "snapshot: stale DDR status discarded");
    CHECK_EQ32(rp1_node_get_status(&G_NODES[0]), RP1_NODE_ERROR,
               "snapshot: DDR status not initialized");
    for (uint32_t i = 0u; i < sizeof(expected_payload); i++)
        CHECK_EQ32(g_nodes[0].payload.raw[i], expected_payload[i],
                   "snapshot: every payload byte copied");

    /* Replace the source packet after the snapshot and execute directly. */
    make_signal(&G_NODES[0], 5u, 0x33334444u, RP1_SIGOP_SET,
                0u, 0u, 0u, 2u);
    rp1_node_set_status(&G_NODES[0], RP1_NODE_ERROR);
    uint16_t ddr_control = G_NODES[0].control;
    int rc = rp1_loop(&s_hooks);

    CHECK_EQ32(rc, 0u, "snapshot: scanner result");
    CHECK_EQ32(G_SIGS[4].value, 0x11112222u,
               "snapshot: BTCM payload executed");
    CHECK_EQ32(G_SIGS[5].value, 0u,
               "snapshot: changed DDR payload ignored");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "snapshot: BTCM status mutated");
    CHECK_EQ32(G_NODES[0].control, ddr_control,
               "snapshot: DDR control never written");
    CHECK_EQ32(G_NODES[0].payload.signal.value, 0x33334444u,
               "snapshot: DDR payload never written");
    return 0;
}

/* Accept exactly 1024 packets, execute their BTCM copies, and reject 1025. */
static int test_exact_node_limit(void)
{
    setup_graph(RP1_MAX_NODES, 0u);
    for (uint32_t i = 0u; i < RP1_MAX_NODES; i++)
        make_header(&G_NODES[i], RP1_OP_NOP, 0u, 0u, 0u, 0u, 0u);
    G_NODES[0].payload.raw[0] = 0x11u;
    G_NODES[RP1_MAX_NODES - 1u].payload.raw[19] = 0xEEu;

    uint32_t detail = 0u;
    uint32_t aux = 0u;
    CHECK_EQ32(rp1_store_init(&detail, &aux), 0u,
               "node_limit: exact maximum accepted");
    CHECK_EQ32(g_node_count, RP1_MAX_NODES,
               "node_limit: exact maximum cached");
    CHECK_EQ32(g_nodes[0].payload.raw[0], 0x11u,
               "node_limit: first packet copied");
    CHECK_EQ32(g_nodes[RP1_MAX_NODES - 1u].payload.raw[19], 0xEEu,
               "node_limit: last packet copied");
    CHECK_EQ32(rp1_loop(&s_hooks), 0u,
               "node_limit: exact maximum executed");
    CHECK_EQ32(g_completed_operations, RP1_MAX_NODES,
               "node_limit: every packet completed");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[RP1_MAX_NODES - 1u]),
               RP1_NODE_DONE, "node_limit: last packet done");

    setup_graph(RP1_MAX_NODES + 1u, 0u);
    detail = 0u;
    aux = 0u;
    CHECK(rp1_store_init(&detail, &aux) != 0,
          "node_limit: maximum plus one rejected");
    CHECK_EQ32(detail, RP1_CONFIG_NODE_COUNT,
               "node_limit: count rejection detail");
    CHECK_EQ32(aux, RP1_MAX_NODES + 1u,
               "node_limit: rejected count preserved");
    CHECK_EQ32(g_node_count, 0u,
               "node_limit: rejected graph has no active snapshot");
    return 0;
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
 *   - one sequence-tagged successful graph result is committed.
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

    CHECK_EQ32(G_CTRL->graph_done_seq, 1u,                 "diamond: graph_done_seq");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_READY,    "diamond: rp1_state");
    CHECK_EQ32(G_CTRL->result.magic, RP1_GRAPH_RESULT_MAGIC,
               "diamond: result committed");
    CHECK_EQ32(G_CTRL->result.graph_seq, 1u,
               "diamond: result sequence");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_SUCCESS,
               "diamond: successful outcome");
    CHECK_EQ32(G_CTRL->result.flags, 0u,
               "diamond: no exceptional flags");
    CHECK_EQ32(G_CTRL->result.error_code, 0u,
               "diamond: no terminal error");
    CHECK_EQ32(G_CTRL->result.terminal_node,
               RP1_TERMINAL_ERROR_NODE_NONE,
               "diamond: no terminal node");
    CHECK_EQ32(G_CTRL->result.terminal_opcode,
               RP1_TERMINAL_OPCODE_NONE,
               "diamond: no terminal opcode");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_NONE,
               "diamond: no active image");
    CHECK_EQ32(G_CTRL->result.completed_operations, 4u,
               "diamond: four successful operations");
    CHECK_EQ32(G_CTRL->result.quiescence, 0u,
               "diamond: no quiescence work");
    CHECK(G_CTRL->result.publish_elapsed_ticks >=
          G_CTRL->result.graph_elapsed_ticks,
          "diamond: publication follows graph completion");

    for (uint32_t i = 0; i < 4; i++) {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(i);
        CHECK_EQ32(ctrl[0],        0x3u, "diamond: ctrl reg ap_start|ap_done");
        CHECK_EQ32(ctrl[0x10 / 4], i,    "diamond: kernel arg[0]");
        CHECK_EQ32(rp1_node_get_status(&g_nodes[i]), RP1_NODE_DONE,
                   "diamond: BTCM status is authoritative");
        CHECK_EQ32(rp1_node_get_status(&G_NODES[i]), RP1_NODE_PENDING,
                   "diamond: DDR packet remains unchanged");
    }
    return 0;
}

/*
 * Independent dispatches to one CU must serialize. The second invocation
 * reuses the completion read as its ap_done clear, while a subsequent PDI
 * transition invalidates that knowledge and restores the first-use clear.
 */
static int test_cu_clean_tracking(void)
{
    setup_graph(/* node_count */ 4u, /* fake_kernels */ 1u);
    pdi_override_reset();
    watch_kernel_ctrl(FAKE_KERNEL(0));

    make_kernel(&G_NODES[0], 0u, 0u, 0u, 0u, 1u, 0u, 0u);
    make_kernel(&G_NODES[1], 0u, 0u, 0u, 0u, 2u, 0u, 0u);
    make_pdi_load(&G_NODES[2], 0x10000000u, 0u, 20u,
                  0u, 0u, 3u, 0u, 4u);
    make_kernel(&G_NODES[3], 0u, 0u, 4u, 0u, 8u, 0u, 0u);

    int rc = rp1_run(&s_hooks);
    uint32_t control_reads = s_watched_kernel_ctrl_reads;
    rp1_hal_reset_hooks();

    CHECK_EQ32(rc, 0u, "cu_clean: rp1_run rc");
    CHECK_EQ32(s_max_inflight, 1u,
               "cu_clean: same-CU dispatches serialized");
    CHECK_EQ32(s_pdi_call_count, 1u, "cu_clean: PDI issued once");
    CHECK_EQ32(control_reads, 8u,
               "cu_clean: reused clear skipped and PDI invalidated cache");
    for (uint32_t i = 0u; i < 4u; i++)
        CHECK_EQ32(rp1_node_get_status(&g_nodes[i]), RP1_NODE_DONE,
                   "cu_clean: every node completed");
    return 0;
}

static int test_trace_disabled_by_default(void)
{
    prepare_diamond_graph();

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "trace_default: rp1_run rc");
    CHECK_EQ32(G_CTRL->trace_write_idx, 0u, "trace_default: trace disabled");
    CHECK_EQ32(G_CTRL->result.trace_write_idx, 0u,
               "trace_default: result cursor zero");
    CHECK((G_CTRL->result.flags & RP1_RESULT_TRACE_ENABLED) == 0u,
          "trace_default: result trace flag clear");
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
    CHECK_EQ32(G_CTRL->result.trace_write_idx, writes,
               "trace_queue: result cursor is final");
    CHECK((G_CTRL->result.flags & RP1_RESULT_TRACE_ENABLED) != 0u,
          "trace_queue: result reports tracing");

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

/*
 * Fill one BTCM trace page and prove the synchronous DDR copy is bracketed by
 * adjacent FLUSH_START/END events.
 */
static int test_trace_btcm_flush(void)
{
    const uint32_t node_count = 260u;
    const uint32_t trace_size = 512u;
    setup_graph(node_count, 0);
    tmemzero((volatile void *)G_TRACE,
             trace_size * sizeof(rp1_trace_entry_t));
    G_CTRL->trace_enable = 1u;
    G_CTRL->trace_size = trace_size;

    for (uint32_t i = 0; i < node_count; i++) {
        make_header(&G_NODES[i], RP1_OP_NOP, 0u, 0, 0u, 0, 0u);
    }

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "trace_flush: rp1_run rc");
    CHECK_EQ32(G_CTRL->trace_write_idx, 264u,
               "trace_flush: events plus flush markers");
    CHECK_EQ32(G_TRACE[0].event, RP1_TRACE_GRAPH_START,
               "trace_flush: graph start first");
    CHECK_EQ32(G_TRACE[255].event, RP1_TRACE_FLUSH_START,
               "trace_flush: full page ends with flush start");
    CHECK_EQ32(G_TRACE[256].event, RP1_TRACE_FLUSH_END,
               "trace_flush: fresh page begins with flush end");
    CHECK_EQ32(G_TRACE[263].event, RP1_TRACE_GRAPH_DONE,
               "trace_flush: final partial page ends with graph done");
    CHECK_EQ32(G_CTRL->result.trace_write_idx, 264u,
               "trace_flush: result captures final cursor");
    CHECK_EQ32(G_CTRL->result.completed_operations, node_count,
               "trace_flush: all NOP operations counted");
    CHECK_EQ32(G_TRACE[255].aux0, RP1_TRACE_STAGING_ENTRIES,
               "trace_flush: start reports page entries");
    CHECK_EQ32(G_TRACE[255].aux1, 0u,
               "trace_flush: start reports old DDR cursor");
    CHECK_EQ32(G_TRACE[256].aux0, RP1_TRACE_STAGING_ENTRIES,
               "trace_flush: end reports page entries");
    CHECK_EQ32(G_TRACE[256].aux1, RP1_TRACE_STAGING_ENTRIES,
               "trace_flush: end reports new DDR cursor");
    CHECK(G_TRACE[256].timestamp >= G_TRACE[255].timestamp,
          "trace_flush: end follows start");
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
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "kernel_chain: node 0 DONE");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_DONE,
               "kernel_chain: node 1 DONE");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[2]), RP1_NODE_DONE,
               "kernel_chain: node 2 DONE");
    CHECK_EQ32(g_barriers[0] & 0x7u, 0x7u, "kernel_chain: barriers raised");

    CHECK_EQ32(s_trace_count, 3u, "kernel_chain: nodes traced");
    CHECK_EQ32(s_trace[0],    0u, "kernel_chain: pre first");
    CHECK_EQ32(s_trace[1],    1u, "kernel_chain: kernel second");
    CHECK_EQ32(s_trace[2],    2u, "kernel_chain: post third");

    CHECK_EQ32(G_CTRL->result.completed_operations, 3u,
               "kernel_chain: operations counted");

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
 * Exercises the DDR-resolved node and signal pointers.
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
    CHECK_EQ32(G_CTRL->result.completed_operations, 4u,
               "chain: operations counted");
    CHECK_EQ32(G_CTRL->graph_done_seq, 1u, "chain: graph_done_seq");
    return 0;
}

static int test_graph_sequence_wrap(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0u, 0x1111u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    s_pending_graph_seq = 0xFFFFFFFFu;
    s_wrap_graphs = 0u;

    int rc = rp1_run(&s_wrap_hooks);
    CHECK_EQ32(rc, 0u, "seq_wrap: second graph result");
    CHECK_EQ32(s_wrap_graphs, 2u, "seq_wrap: both graphs ran");
    CHECK_EQ32(G_CTRL->graph_done_seq, 0u,
               "seq_wrap: equality completion wrapped");
    CHECK_EQ32(G_CTRL->result.magic, RP1_GRAPH_RESULT_MAGIC,
               "seq_wrap: wrapped result committed");
    CHECK_EQ32(G_CTRL->result.graph_seq, 0u,
               "seq_wrap: result sequence wrapped");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_SUCCESS,
               "seq_wrap: wrapped graph succeeded");
    CHECK_EQ32(G_SIGS[0].value, 0x1111u,
               "seq_wrap: pre-wrap graph ran");
    CHECK_EQ32(G_SIGS[1].value, 0x2222u,
               "seq_wrap: wrapped graph ran");
    return 0;
}

/*
 * Every uint8_t signal value names one of the 256 slots, so protocol-v6 slot
 * encoding has no invalid value. Retain all-or-nothing validation coverage
 * with the remaining compact discriminants.
 */
static int test_compact_operation_validation(void)
{
    static const uint8_t opcodes[] = {
        RP1_OP_SIGNAL,
        RP1_OP_WAIT,
        14u,
        RP1_OP_NOP,
        RP1_OP_NOP,
        RP1_OP_NOP,
    };
    static const uint8_t flags[] = {
        0u,
        0u,
        0u,
        0x2u,
        0u,
        RP1_FLAG_INFINITE,
    };
    static const uint16_t reserved[] = {
        0u,
        0u,
        0u,
        0u,
        0x1000u,
        0u,
    };
    static const uint16_t bad_values[] = {
        RP1_SIGOP_AND + 1u,
        RP1_COP_AND_Z + 1u,
        14u,
        0x20u,
        0x1000u,
        0x10u,
    };

    for (uint32_t test = 0; test < sizeof(opcodes) / sizeof(opcodes[0]);
         test++) {
        setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
        rp1_node_t *node = &G_NODES[0];
        make_header(node, opcodes[test], flags[test],
                    0u, 0u, 0u, 0u);
        rp1_node_set_control(
            node, (uint16_t)(rp1_node_get_control(node) | reserved[test]));
        switch (opcodes[test]) {
        case RP1_OP_SIGNAL:
            node->payload.signal.operation = (uint8_t)bad_values[test];
            break;
        case RP1_OP_WAIT:
            node->payload.wait.condition_op = (uint8_t)bad_values[test];
            break;
        default:
            break;
        }

        int rc = rp1_run(&s_hooks);
        CHECK_EQ32((uint32_t)(rc + 1), 0u,
                   "operation_validation: graph rejected");
        CHECK_EQ32(G_CTRL->terminal_error_node, 0u,
                   "operation_validation: node latched");
        CHECK_EQ32(G_CTRL->terminal_error_detail,
                   RP1_NODE_BAD_OPERATION,
                   "operation_validation: detail");
        CHECK_EQ32(G_CTRL->terminal_error_aux, bad_values[test],
                   "operation_validation: bad value preserved");
        CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_FAILED,
                   "operation_validation: failed result");
        CHECK_EQ32(G_CTRL->result.terminal_opcode, opcodes[test],
                   "operation_validation: opcode preserved");
        CHECK_EQ32(G_CTRL->result.completed_operations, 0u,
                   "operation_validation: no operation started");
        CHECK((G_CTRL->result.flags &
               RP1_RESULT_EFFECTS_MAY_BE_PARTIAL) == 0u,
              "operation_validation: no partial effects");
    }
    return 0;
}

/*
 * Phase-1 DMA and compact control validation must reject every field the
 * executor would otherwise ignore. A trailing SIGNAL proves validation
 * completes before any graph side effect.
 */
static int test_phase1_payload_validation(void)
{
    enum {
        BAD_DMA_COPY_HIGH,
        BAD_DMA_COPY_TYPE,
        BAD_DMA_COPY_RANGE,
        BAD_DMA_FILL_HIGH,
        BAD_DMA_FILL_TYPE,
        BAD_DMA_FILL_RANGE,
        BAD_DISPATCH_FLAGS,
        BAD_RERUN_FLAGS,
        BAD_CASE_COUNT,
    };
    static const uint32_t expected_detail[BAD_CASE_COUNT] = {
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_ARGUMENTS,
        RP1_NODE_BAD_TARGET,
    };

    for (uint32_t test = 0u; test < BAD_CASE_COUNT; test++) {
        setup_graph(/* node_count */ 2u, /* fake_kernels */ 1u);
        rp1_node_t *node = &G_NODES[0];

        if (test <= BAD_DMA_COPY_RANGE) {
            make_header(node, RP1_OP_DMA_COPY, 0u, 0u, 0u, 0u, 0u);
            node->payload.dma_copy.src_addr_lo = 0x1000u;
            node->payload.dma_copy.dst_addr_lo = 0x2000u;
            node->payload.dma_copy.length_types =
                rp1_dma_pack(8u, 0u, 0u);
            if (test == BAD_DMA_COPY_HIGH)
                node->payload.dma_copy.src_addr_hi = 1u;
            else if (test == BAD_DMA_COPY_TYPE)
                node->payload.dma_copy.length_types =
                    rp1_dma_pack(8u, 1u, 0u);
            else
                node->payload.dma_copy.src_addr_lo = 0xFFFFFFFCu;
        } else if (test <= BAD_DMA_FILL_RANGE) {
            make_header(node, RP1_OP_DMA_FILL, 0u, 0u, 0u, 0u, 0u);
            node->payload.dma_fill.dst_addr_lo = 0x2000u;
            node->payload.dma_fill.length = 8u;
            if (test == BAD_DMA_FILL_HIGH)
                node->payload.dma_fill.dst_addr_hi = 1u;
            else if (test == BAD_DMA_FILL_TYPE)
                node->payload.dma_fill.dst_type = 1u;
            else
                node->payload.dma_fill.dst_addr_lo = 0xFFFFFFFCu;
        } else if (test == BAD_DISPATCH_FLAGS) {
            make_kernel(node, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
            node->payload.kernel_dispatch.ctrl_flags = 1u;
        } else {
            make_rerun(node, 0u, 0u, 0u, 0u, 0u);
            node->payload.rerun.rerun_flags = 0x2u;
        }
        make_signal(&G_NODES[1], 63u, 0xBAD0u, RP1_SIGOP_SET,
                    0u, 0u, 0u, 1u);

        int rc = rp1_run(&s_hooks);
        CHECK_EQ32((uint32_t)(rc + 1), 0u,
                   "phase1_validation: graph rejected");
        CHECK_EQ32(G_CTRL->terminal_error_node, 0u,
                   "phase1_validation: bad node latched");
        CHECK_EQ32(G_CTRL->terminal_error_detail, expected_detail[test],
                   "phase1_validation: detail");
        CHECK_EQ32(G_CTRL->result.completed_operations, 0u,
                   "phase1_validation: no operation started");
        CHECK_EQ32(G_SIGS[63].value, 0u,
                   "phase1_validation: sentinel did not run");
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
 *   - successful operation count includes all four LOOP evaluations: 12.
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
    CHECK_EQ32(G_CTRL->result.completed_operations, 12u,
               "loop: successful operation count");
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
 * relies on body_clear + packed BTCM status reset to gate body execution and
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
    CHECK_EQ32(G_CTRL->result.completed_operations, 4u,
               "cond[met]: successful operation count");

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
    CHECK_EQ32(G_CTRL->result.completed_operations, 3u,
               "cond[nomet]: successful operation count");
    CHECK((G_CTRL->result.flags & RP1_RESULT_UNREACHED_NODES) != 0u,
          "cond[nomet]: skipped node reported");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[3]), RP1_NODE_PENDING,
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
 * Successful operations include all four LOOP evaluations: 11.
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
    CHECK_EQ32(G_CTRL->result.completed_operations, 11u,
               "loop_fixed: successful operation count");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[3]), RP1_NODE_DONE,
               "loop_fixed: finalize DONE");
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
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_DONE,
               "scalar_read: node DONE");
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
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "wait: WAIT node DONE");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_DONE,
               "wait: downstream DONE");
    CHECK_EQ32(g_barriers[0] & 0x3u, 0x3u,           "wait: both barriers raised");
    CHECK_EQ32(G_CTRL->result.completed_operations, 2u,
               "wait: operations counted after wake");
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
    CHECK_EQ32(s_pdi_access[4].value, RP1_BARRIER_DMB_ST,
               "pdi_mmio: request stores ordered before trigger");
    CHECK_EQ32(s_pdi_access[5].address, RP1_PDI_IPI_TRIGGER_REG,
               "pdi_mmio: generated trigger address");
    CHECK_EQ32(s_pdi_access[5].value, RP1_PDI_IPI_TARGET_MASK,
               "pdi_mmio: generated target mask");
    CHECK_EQ32(s_pdi_access[6].value, RP1_BARRIER_DSB_ST,
               "pdi_mmio: trigger completed before deadline starts");
    CHECK_EQ32(s_pdi_access[7].address, RP1_PDI_IPI_OBSERVATION_REG,
               "pdi_mmio: generated observation address");
    CHECK_EQ32(s_pdi_access[8].value, RP1_BARRIER_DMB_SY,
               "pdi_mmio: acknowledgement ordered before response");
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
    G_NODES[0].payload.kernel_dispatch.timeout_cycles = 12u;
    s_skip_completion_node = 0u;
    int slow = rp1_run(&s_hooks);
    uint32_t slow_passes = s_pass_count;

    setup_graph(1, 1);
    pdi_override_reset();
    s_cycle_step = 4u;
    make_kernel(&G_NODES[0], 0, 0, 0u, 0, 1u, 0u, 0u);
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
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_FAILED,
               "kernel_deadline: failed result");
    CHECK_EQ32(G_CTRL->result.terminal_opcode, RP1_OP_KERNEL_DISPATCH,
               "kernel_deadline: terminal opcode");
    CHECK_EQ32(G_CTRL->result.quiescence,
               RP1_QUIESCE_PACK(0u, 1u, 0u),
               "kernel_deadline: finite timeout classified");
    CHECK((G_CTRL->result.flags & RP1_RESULT_RECOVERY_REQUIRED) != 0u,
          "kernel_deadline: timeout requires recovery");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_load_basic
 *
 *   Single PDI_LOAD node, override returns success.
 *   Verifies the scanner forwards the payload to rp1_pdi_load() verbatim
 *   with a zero flags nibble.
 * ---------------------------------------------------------------------- */

static int test_pdi_load_basic(void)
{
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();

    make_pdi_load(&G_NODES[0],
                  /* addr_lo */ 0x10000000u,
                  /* addr_hi */ 0x00000001u,
                  /* timeout */ 12345u,
                  /* flags   */ 0u,
                  /* await   */ 0, 0x00,
                  /* set     */ 0, 0x01);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "pdi_basic: rp1_run rc");

    CHECK_EQ32(s_pdi_call_count,       1u,          "pdi_basic: invoked once");
    CHECK_EQ32(s_pdi_last_addr_lo,     0x10000000u, "pdi_basic: addr_lo forwarded");
    CHECK_EQ32(s_pdi_last_addr_hi,     0x00000001u, "pdi_basic: addr_hi forwarded");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "pdi_basic: node DONE");
    CHECK_EQ32(g_barriers[0] & 0x1u,   0x1u,          "pdi_basic: barrier set");
    CHECK_EQ32(G_CTRL->rp1_state,      RP1_STATE_READY, "pdi_basic: rp1_state");
    CHECK_EQ32(G_CTRL->rp1_error_code, 0u,            "pdi_basic: no error");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_SUCCESS,
               "pdi_basic: success result");
    CHECK_EQ32(G_CTRL->result.completed_operations, 1u,
               "pdi_basic: operation counted");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_pdi_load_timeout
 *
 *   Every PDI transport timeout or PLM rejection is fatal. The active image
 *   becomes UNKNOWN, no dependent is activated, and the structured response
 *   remains intact in both the legacy latch and committed result.
 * ---------------------------------------------------------------------- */

static int test_pdi_load_timeout(void)
{
    setup_graph(/* node_count */ 2, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_force_timeout = 1u;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 3u, /* flags */ 0,
                  0, 0x00, 0, 0x01);
    make_signal(&G_NODES[1], 30, 0xFEEDBEEFu, RP1_SIGOP_SET,
                0, 0x01, 0, 0x02);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "pdi_timeout: rp1_run returned -1");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_ERROR,
               "pdi_timeout: node ERROR");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_PENDING,
               "pdi_timeout: downstream blocked");
    CHECK_EQ32(G_SIGS[30].value, 0u,
               "pdi_timeout: downstream had no effect");
    CHECK_EQ32(G_CTRL->rp1_state, RP1_STATE_ERROR,
               "pdi_timeout: terminal state");
    CHECK_EQ32(G_CTRL->result.error_code, RP1_ERR_PDI_TIMEOUT,
               "pdi_timeout: result error");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_UNKNOWN,
               "pdi_timeout: image state unknown");
    CHECK_EQ32(G_CTRL->result.active_image_id, 0u,
               "pdi_timeout: unknown image has no id");
    CHECK((G_CTRL->result.flags &
           RP1_RESULT_EFFECTS_MAY_BE_PARTIAL) != 0u,
          "pdi_timeout: started operation may have partial effects");
    CHECK((G_CTRL->result.flags & RP1_RESULT_RECOVERY_REQUIRED) != 0u,
          "pdi_timeout: delayed response requires recovery");
    CHECK((G_CTRL->result.flags & RP1_RESULT_UNREACHED_NODES) != 0u,
          "pdi_timeout: dependent is unreached");

    /* PLM completed the command with an error response. */
    setup_graph(/* node_count */ 1, /* fake_kernels */ 0);
    pdi_override_reset();
    s_pdi_status = 0x80002001u;
    s_pdi_detail = 0xDEADCAFEu;

    make_pdi_load(&G_NODES[0],
                  0xDEAD0000u, 0u, 0u,
                  /* flags */ 0u,
                  0, 0x00, 0, 0x01);

    rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u, "pdi_error: rp1_run returned -1");
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_PDI_FAILED,
               "pdi_error: PLM failure code");
    CHECK_EQ32(G_CTRL->terminal_error_detail, 0x80002001u,
               "pdi_error: terminal record preserves PLM status");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 0xDEADCAFEu,
               "pdi_error: terminal record preserves PLM detail");
    CHECK_EQ32(G_CTRL->result.error_detail, 0x80002001u,
               "pdi_error: result preserves PLM status");
    CHECK_EQ32(G_CTRL->result.error_aux, 0xDEADCAFEu,
               "pdi_error: result preserves PLM detail");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_UNKNOWN,
               "pdi_error: rejected load leaves unknown image");
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
    G_NODES[0].payload.pdi_load.image_id = 1u;
    make_pdi_load(&G_NODES[1],
                  /* addr_lo */ 0x22220000u,
                  /* addr_hi */ 0x00000002u,
                  /* timeout */ 0u,
                  /* flags   */ 0,
                  /* await   */ 0, 0x01,
                  /* set     */ 0, 0x02);
    G_NODES[1].payload.pdi_load.image_id = 2u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32(rc, 0u, "pdi_chain: rp1_run rc");

    /* Both nodes fired, in order — the recorder captures the latest call. */
    CHECK_EQ32(s_pdi_call_count,       2u,            "pdi_chain: invoked twice");
    CHECK_EQ32(s_pdi_last_addr_lo,     0x22220000u,   "pdi_chain: last addr_lo (node 1)");
    CHECK_EQ32(s_pdi_last_addr_hi,     0x00000002u,   "pdi_chain: last addr_hi (node 1)");

    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "pdi_chain: node 0 DONE");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_DONE,
               "pdi_chain: node 1 DONE");
    CHECK_EQ32(g_barriers[0] & 0x3u, 0x3u,          "pdi_chain: both barriers raised");
    CHECK_EQ32(G_CTRL->result.completed_operations, 2u,
               "pdi_chain: both operations counted");
    CHECK_EQ32(G_CTRL->result.active_image_id, 2u,
               "pdi_chain: final image id");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_KNOWN,
               "pdi_chain: final image is known");
    return 0;
}

/*
 * A successful PDI is durable evidence even if a later kernel fails. The
 * failed graph remains partial, but its result still names the installed
 * image so the host can reconcile physical state before surfacing the error.
 */
static int test_image_survives_later_error(void)
{
    setup_graph(/* node_count */ 2, /* fake_kernels */ 1);
    pdi_override_reset();
    make_pdi_load(&G_NODES[0], 0x12340000u, 0u, 20u, 0u,
                  0, 0u, 0, 1u);
    G_NODES[0].payload.pdi_load.image_id = 42u;
    make_kernel(&G_NODES[1], 0, 0, 1u, 0, 2u, 0u, 0u);
    G_NODES[1].payload.kernel_dispatch.expected_image_id = 42u;
    G_NODES[1].payload.kernel_dispatch.timeout_cycles = 3u;
    s_skip_completion_node = 1u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "image_later_error: fatal kernel result");
    CHECK_EQ32(G_CTRL->result.error_code, RP1_ERR_KERNEL_TIMEOUT,
               "image_later_error: kernel error retained");
    CHECK_EQ32(G_CTRL->result.active_image_id, 42u,
               "image_later_error: installed image retained");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_KNOWN,
               "image_later_error: installed image remains known");
    CHECK_EQ32(G_CTRL->result.completed_operations, 1u,
               "image_later_error: successful PDI counted");
    CHECK((G_CTRL->result.flags &
           RP1_RESULT_EFFECTS_MAY_BE_PARTIAL) != 0u,
          "image_later_error: failed graph reports partial effects");
    return 0;
}

/* -------------------------------------------------------------------------
 * test_image_guard
 *
 * Exercises the expected-image guard. g_active_image_id persists across graph
 * submissions in one firmware instance. The bounded QEMU harness re-enters
 * rp1_run() for each sub-run, so Run 2 seeds the prior known image from its
 * idle hook after startup; Run 3 deliberately verifies reboot state:
 *
 *   Run 1 (match):      PDI_LOAD{image_id=7} -> DISPATCH{expected=7} launches.
 *   Run 2 (mismatch):   DISPATCH{expected=9} with active image still 7 fails
 *                       fast and reports the still-known image; the kernel is
 *                       never launched.
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
    CHECK_EQ32(g_active_image_state, RP1_IMAGE_STATE_KNOWN,
               "image_guard[match]: image state known");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[1]), RP1_NODE_DONE,
               "image_guard[match]: dispatch DONE");
    {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
        CHECK_EQ32(ctrl[0], 0x3u, "image_guard[match]: kernel launched (ap_start|ap_done)");
    }
    CHECK_EQ32(G_CTRL->rp1_error_code, 0u, "image_guard[match]: no error");
    CHECK_EQ32(G_CTRL->result.active_image_id, 7u,
               "image_guard[match]: result image id");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_KNOWN,
               "image_guard[match]: result image known");

    /* ---- Run 2: separate submission, stale expected image -> fail fast. ---- */
    setup_graph(/* node_count */ 1, /* fake_kernels */ 1);

    make_kernel(&G_NODES[0], /* kernel_idx */ 0,
                /* await */ 0, 0x00, /* set */ 0, 0x01,
                /* arg_buf_offset */ 0u, /* arg_count */ 0);
    G_NODES[0].payload.kernel_dispatch.expected_image_id = 9u;  /* active is still 7 */

    rc = rp1_run(&s_known_image_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "image_guard[mismatch]: fatal result");
    CHECK_EQ32(g_active_image_id, 7u, "image_guard[mismatch]: active image unchanged");
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_ERROR,
               "image_guard[mismatch]: node ERROR");
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_IMAGE_MISMATCH,
               "image_guard[mismatch]: err code");
    CHECK_EQ32(G_CTRL->result.error_detail, 9u,
               "image_guard[mismatch]: expected image detail");
    CHECK_EQ32(G_CTRL->result.error_aux, 7u,
               "image_guard[mismatch]: active image detail");
    CHECK_EQ32(G_CTRL->result.active_image_id, 7u,
               "image_guard[mismatch]: known image survives later failure");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_KNOWN,
               "image_guard[mismatch]: known state survives later failure");
    CHECK_EQ32(G_CTRL->result.completed_operations, 0u,
               "image_guard[mismatch]: no operation completed");
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
    CHECK_EQ32(rp1_node_get_status(&g_nodes[0]), RP1_NODE_DONE,
               "image_guard[unguarded]: dispatch DONE");
    CHECK_EQ32(G_CTRL->result.active_image_id, 0u,
               "image_guard[unguarded]: reboot forgets prior image id");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_NONE,
               "image_guard[unguarded]: reboot image state is none");
    {
        volatile uint32_t *ctrl = (volatile uint32_t *)(uintptr_t)FAKE_KERNEL(0);
        CHECK_EQ32(ctrl[0], 0x3u, "image_guard[unguarded]: kernel launched");
    }
    return 0;
}

/*
 * Explicit HALT is a committed HALTED outcome, not an error. Activation stops
 * at the HALT node even when a later command is already barrier-ready.
 */
static int test_explicit_halt(void)
{
    setup_graph(/* node_count */ 3, /* fake_kernels */ 0);
    make_signal(&G_NODES[0], 0u, 0x1111u, RP1_SIGOP_SET,
                0, 0u, 0, 1u);
    make_header(&G_NODES[1], RP1_OP_HALT, 0u, 0u, 1u, 0u, 0u);
    make_signal(&G_NODES[2], 1u, 0x2222u, RP1_SIGOP_SET,
                0, 1u, 0, 2u);

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 2), 0u, "halt: rp1_run returned -2");
    CHECK_EQ32(G_CTRL->rp1_state, RP1_STATE_HALTED,
               "halt: terminal state");
    CHECK_EQ32(G_CTRL->rp1_error_code, 0u,
               "halt: no error code");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_HALTED,
               "halt: distinct outcome");
    CHECK_EQ32(G_CTRL->result.terminal_node, 1u,
               "halt: terminal node");
    CHECK_EQ32(G_CTRL->result.terminal_opcode, RP1_OP_HALT,
               "halt: terminal opcode");
    CHECK_EQ32(G_CTRL->result.completed_operations, 2u,
               "halt: signal and HALT counted");
    CHECK_EQ32(G_CTRL->result.quiescence, 0u,
               "halt: no inflight work");
    CHECK((G_CTRL->result.flags &
           RP1_RESULT_EFFECTS_MAY_BE_PARTIAL) != 0u,
          "halt: prior effects may be partial");
    CHECK((G_CTRL->result.flags & RP1_RESULT_UNREACHED_NODES) != 0u,
          "halt: later node reported unreached");
    CHECK_EQ32(G_SIGS[1].value, 0u,
               "halt: later ready node was not activated");
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
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_KERNEL_TIMEOUT,
               "fatal_kernel: first error code");
    CHECK((G_CTRL->result.flags & RP1_RESULT_RECOVERY_REQUIRED) != 0u,
          "fatal_kernel: unresponsive work requires recovery");
    CHECK_EQ32(G_CTRL->terminal_error_node, 0u,
               "fatal_kernel: failing node latched");
    CHECK_EQ32(G_CTRL->terminal_error_detail, (uint32_t)FAKE_KERNEL(0),
               "fatal_kernel: failing kernel base");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 3u,
               "fatal_kernel: timeout ticks");
    CHECK_EQ32(G_SIGS[40].value, 0u,
               "fatal_kernel: sentinel did not run");
    CHECK_EQ32(G_CTRL->result.outcome, RP1_GRAPH_RESULT_FAILED,
               "fatal_kernel: failed graph result");
    CHECK_EQ32(G_CTRL->result.terminal_opcode, RP1_OP_KERNEL_DISPATCH,
               "fatal_kernel: terminal opcode");
    CHECK_EQ32(G_CTRL->result.completed_operations, 1u,
               "fatal_kernel: finite peer completed during quiescence");
    CHECK_EQ32(G_CTRL->result.quiescence,
               RP1_QUIESCE_PACK(1u, 1u, 0u),
               "fatal_kernel: quiescence counts");
    CHECK((G_CTRL->result.flags &
           RP1_RESULT_EFFECTS_MAY_BE_PARTIAL) != 0u,
          "fatal_kernel: started graph may have partial effects");
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
    rp1_node_set_flags(&G_NODES[1], RP1_FLAG_INFINITE);
    G_NODES[1].payload.kernel_dispatch.timeout_cycles = 100u;
    make_pdi_load(&G_NODES[2], 0x10000000u, 0u, 20u,
                  0u, 0, 0u, 0, 4u);
    make_signal(&G_NODES[3], 41u, 0x51514E54u, RP1_SIGOP_SET,
                0, 7u, 0, 8u);
    s_skip_completion_node = 1u;

    int rc = rp1_run(&s_hooks);
    CHECK_EQ32((uint32_t)(rc + 1), 0u,
               "fatal_pdi: terminal error result");
    CHECK_EQ32(G_CTRL->rp1_error_code, RP1_ERR_PDI_FAILED,
               "fatal_pdi: first error remains PDI");
    CHECK((G_CTRL->result.flags & RP1_RESULT_RECOVERY_REQUIRED) != 0u,
          "fatal_pdi: infinite kernel requires recovery");
    CHECK_EQ32(G_CTRL->terminal_error_node, 2u,
               "fatal_pdi: failing node latched");
    CHECK_EQ32(G_CTRL->terminal_error_detail, 0x8000F00Du,
               "fatal_pdi: high-bit PLM status");
    CHECK_EQ32(G_CTRL->terminal_error_aux, 0x1234ABCDu,
               "fatal_pdi: full PLM detail");
    CHECK_EQ32(G_SIGS[41].value, 0u,
               "fatal_pdi: sentinel did not run");
    CHECK_EQ32(G_CTRL->result.terminal_opcode, RP1_OP_PDI_LOAD,
               "fatal_pdi: terminal opcode");
    CHECK_EQ32(G_CTRL->result.completed_operations, 2u,
               "fatal_pdi: launched kernels counted");
    CHECK_EQ32(G_CTRL->result.quiescence,
               RP1_QUIESCE_PACK(1u, 0u, 1u),
               "fatal_pdi: quiescence counts");
    CHECK((G_CTRL->result.flags &
           RP1_RESULT_INFINITE_WORK_REMAINS) != 0u,
          "fatal_pdi: infinite work reported");
    CHECK_EQ32(G_CTRL->result.image_state, RP1_IMAGE_STATE_UNKNOWN,
               "fatal_pdi: failed load makes image unknown");
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
    run("cu_clean_tracking",   test_cu_clean_tracking);
    run("trace_disabled_by_default", test_trace_disabled_by_default);
    run("trace_queue",         test_trace_queue);
    run("trace_btcm_flush",    test_trace_btcm_flush);
    run("kernel_unblocks_signal", test_kernel_unblocks_signal);
    run("signal_chain",        test_signal_chain);
    run("graph_sequence_wrap", test_graph_sequence_wrap);
    run("compact_operation_validation",
        test_compact_operation_validation);
    run("phase1_payload_validation",
        test_phase1_payload_validation);
    run("btcm_node_snapshot",  test_btcm_node_snapshot);
    run("exact_node_limit",    test_exact_node_limit);
    run("loop_decrement",      test_loop_decrement);
    run("loop_fixed_count",    test_loop_fixed_count);
    run("cond_boolean",        test_cond_boolean);
    run("scalar_read",         test_scalar_read);
    run("wait_blocks",         test_wait_blocks);
    run("barrier_variants",    test_barrier_variants);
    run("result_publication_order", test_result_publication_order);
    run("boot_sequence_baseline", test_boot_sequence_baseline);
    run("reserved_cq_config_rejected", test_reserved_cq_config_rejected);
    run("pdi_mmio_contract", test_pdi_mmio_contract);
    run("pdi_timeout_invariant", test_pdi_timeout_invariant);
    run("kernel_timeout_invariant", test_kernel_timeout_invariant);
    run("pdi_load_basic",   test_pdi_load_basic);
    run("pdi_load_timeout", test_pdi_load_timeout);
    run("pdi_load_chained", test_pdi_load_chained);
    run("image_survives_later_error",
        test_image_survives_later_error);
    run("image_guard",      test_image_guard);
    run("explicit_halt",     test_explicit_halt);
    run("fatal_kernel_quiesce_and_reject",
        test_fatal_kernel_quiesce_and_reject);
    run("fatal_pdi_quiesce_and_recovery",
        test_fatal_pdi_quiesce_and_recovery);
}

#endif /* QEMU_SEMIHOSTING */
