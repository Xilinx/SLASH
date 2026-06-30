/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 dispatch loop and test hooks.
 */

#ifndef RP1_LOOP_H
#define RP1_LOOP_H

#ifdef QEMU_SEMIHOSTING

/*
 * Hook table for QEMU test harness.  Any individual callback may be
 * NULL (skipped).
 */
typedef struct {
    /*
     * Inner loop: called each iteration after activate_nodes +
     * check_inflight.  Use to simulate kernel completions, host
     * writes, or inspect state mid-graph.
     */
    void (*on_scan_pass)(void);

    /*
     * Outer loop: called when a graph finishes (complete, error, or halt).
     *   result: 0 = complete, -1 = error, -2 = halt.
     * Return 0 to continue processing graphs, non-zero to exit.
     */
    int (*on_graph_done)(int result);

    /*
     * Outer loop: called when idle (graph_seq == graph_done_seq).
     * Use to submit a new graph or signal exit.
     * Return 0 to keep waiting, non-zero to exit.
     */
    int (*on_idle)(void);
} rp1_hooks_t;

int rp1_loop(const rp1_hooks_t *hooks);

#else

int rp1_loop(void);

#endif /* QEMU_SEMIHOSTING */

#endif /* RP1_LOOP_H */
