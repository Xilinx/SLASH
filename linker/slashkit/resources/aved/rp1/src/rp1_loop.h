/*
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 dispatch loop and test hooks.
 */

#ifndef RP1_LOOP_H
#define RP1_LOOP_H

/*
 * Forget which compute units have had sticky ap_done cleared. Firmware startup
 * and every partial-reconfiguration attempt call this before later dispatches.
 */
void rp1_cu_tracking_reset(void);

/*
 * A scanner pass activates barrier-ready nodes, polls inflight kernels, then
 * revisits parked WAITs. It repeats while asynchronous or externally signalled
 * work can progress; fatal and HALT exits first classify all tracked work.
 */
#ifdef QEMU_SEMIHOSTING

/*
 * Hook table for QEMU test harness.  Any individual callback may be
 * NULL (skipped).
 */
typedef struct {
    /*
     * Inner loop: called after activation, inflight polling, and WAIT polling;
     * quiescence also invokes it while waiting for finite kernels. Use it to
     * simulate completions or host writes and to inspect state between passes.
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
