/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * Shared test framework for the QEMU-semihosting RP1 test binary.
 *
 * - rp1_test.c       hosts the unit tests (struct sizes, store reset,
 *                    barrier logic, signal slots, condops, node header)
 *                    and the rp1_main() entry point.
 * - rp1_graph_test.c hosts the end-to-end graph tests (diamond DAG,
 *                    signal chain, LOOP/RERUN, COND-as-boolean).
 *
 * Both consume the same CHECK macros and semihosting helpers so the test
 * output is uniform and a single g_failures counter drives the final
 * PASS/FAIL verdict from rp1_main().
 */

#ifndef RP1_TEST_H
#define RP1_TEST_H

#ifdef QEMU_SEMIHOSTING

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Semihosting primitives.
 *
 * Marked static inline because the test binary is freestanding and we don't
 * want to introduce another translation unit just for these three helpers.
 * The duplicated rodata across TUs is a handful of bytes.
 * ---------------------------------------------------------------------- */

#define SEMI_SYS_WRITE0  0x04
#define SEMI_SYS_EXIT    0x18

static inline int semi_call(int op, void *arg)
{
    register int    r0 __asm__("r0") = op;
    register void  *r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x00123456" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static inline void semi_puts(const char *s)
{
    semi_call(SEMI_SYS_WRITE0, (void *)s);
}

static inline void semi_exit(int code)
{
    uint32_t args[2] = { 0x20026, (uint32_t)code };
    semi_call(SEMI_SYS_EXIT, args);
}

static inline void semi_print_u32(uint32_t v)
{
    char buf[11] = "0x00000000";
    const char hex[] = "0123456789abcdef";
    for (int i = 9; i >= 2; i--) { buf[i] = hex[v & 0xf]; v >>= 4; }
    semi_puts(buf);
}

/* -------------------------------------------------------------------------
 * Failure counter (defined in rp1_test.c).
 * ---------------------------------------------------------------------- */

extern int g_failures;

/* -------------------------------------------------------------------------
 * Assertion macros.
 *
 * Each test function returns 0 on success, 1 on failure.  CHECK bumps
 * g_failures and returns 1 so the framework can print "PASS" only on
 * functions that ran to completion without a failed assertion.
 * ---------------------------------------------------------------------- */

#define CHECK(cond, msg) do {                       \
    if (!(cond)) {                                  \
        semi_puts("  FAIL: " msg "\n");             \
        g_failures++;                               \
        return 1;                                   \
    }                                               \
} while (0)

#define CHECK_EQ32(a, b, msg) do {                  \
    uint32_t _a = (uint32_t)(a);                    \
    uint32_t _b = (uint32_t)(b);                    \
    if (_a != _b) {                                 \
        semi_puts("  FAIL: " msg " got ");          \
        semi_print_u32(_a);                         \
        semi_puts(" expected ");                    \
        semi_print_u32(_b);                         \
        semi_puts("\n");                            \
        g_failures++;                               \
        return 1;                                   \
    }                                               \
} while (0)

/* -------------------------------------------------------------------------
 * Entry points.
 * ---------------------------------------------------------------------- */

/* Runs the graph-level integration tests.  Updates g_failures in place. */
void rp1_graph_test_run(void);

#endif /* QEMU_SEMIHOSTING */

#endif /* RP1_TEST_H */
