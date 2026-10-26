/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 barrier-event execution index.
 */

#include "rp1_scheduler.h"
#include "rp1_store.h"

#include <slash/uapi/rp1_protocol.h>
#include <stdint.h>

#define BTCM_SECTION __attribute__((section(".btcm")))
#define ATCM_DATA_SECTION __attribute__((section(".atcm_data")))
#define RP1_EVENT_COUNT (RP1_MAX_BUCKETS * 32u)
#define RP1_NODE_WORDS  ((RP1_MAX_NODES + 31u) / 32u)

/*
 * Sparse event-to-node references. ATCM is writable and otherwise mostly
 * unused after code placement, while these sequential reads avoid DDR stalls.
 *
 * ponytail: dense graphs above this ceiling use the flat scanner. Increase the
 * split ATCM/BTCM capacity or stage host-built CSR data to raise the ceiling.
 */
static uint16_t
    g_event_subscribers[RP1_SCHEDULER_MAX_SUBSCRIPTIONS] ATCM_DATA_SECTION;

/* CSR offsets indexed by flattened bucket/bit event id. */
static uint16_t g_event_offsets[RP1_EVENT_COUNT + 1u] BTCM_SECTION;

/* Number of currently unset awaited bits for each node. */
static uint8_t g_remaining[RP1_MAX_NODES] BTCM_SECTION;

/* Dependency-ready PENDING nodes, with one summary bit per 32-node word. */
static uint32_t g_ready[RP1_NODE_WORDS] BTCM_SECTION;
static uint32_t g_ready_summary BTCM_SECTION;

/* Resource-blocked ready nodes restored after the current activation pass. */
static uint32_t g_deferred[RP1_NODE_WORDS] BTCM_SECTION;
static uint32_t g_deferred_summary BTCM_SECTION;

/* WAIT nodes whose barrier dependencies resolved but signal did not. */
static uint32_t g_waiting[RP1_NODE_WORDS] BTCM_SECTION;
static uint32_t g_waiting_summary BTCM_SECTION;

/* Per-graph index state; zero selects the scanner fallback. */
static uint32_t g_enabled BTCM_SECTION;
static uint32_t g_subscription_count BTCM_SECTION;

_Static_assert(RP1_SCHEDULER_MAX_SUBSCRIPTIONS <= UINT16_MAX,
               "scheduler offsets must represent every subscription");
_Static_assert(RP1_NODE_WORDS == 32u,
               "scheduler bitsets must cover exactly 1024 nodes");

/* Return the index of the least-significant set bit in a non-zero word. */
static uint32_t first_set(uint32_t word)
{
    return (uint32_t)__builtin_ctz(word);
}

/* Clear every word and its summary for one node bitset. */
static void clear_bitset(uint32_t *words, uint32_t *summary)
{
    for (uint32_t i = 0u; i < RP1_NODE_WORDS; i++)
        words[i] = 0u;
    *summary = 0u;
}

/* Add one node to a summarized bitset. */
static void add_bit(uint32_t *words, uint32_t *summary, uint32_t node_index)
{
    uint32_t word = node_index >> 5;
    words[word] |= 1u << (node_index & 31u);
    *summary |= 1u << word;
}

/* Remove one node from a summarized bitset. */
static void remove_bit(uint32_t *words, uint32_t *summary,
                       uint32_t node_index)
{
    uint32_t word = node_index >> 5;
    words[word] &= ~(1u << (node_index & 31u));
    if (words[word] == 0u)
        *summary &= ~(1u << word);
}

/* Add one pending node whose complete await mask is already satisfied. */
static void add_ready(uint32_t node_index)
{
    if (rp1_node_get_status(&g_nodes[node_index]) == RP1_NODE_PENDING &&
        g_remaining[node_index] == 0u)
        add_bit(g_ready, &g_ready_summary, node_index);
}

/* Flatten one barrier bucket and bit into its CSR event id. */
static uint32_t event_id(uint8_t bucket, uint32_t bit)
{
    return (uint32_t)bucket * 32u + bit;
}

/* Return the unresolved await-bit count against current barrier state. */
static uint8_t unresolved_count(const rp1_node_t *node)
{
    uint32_t unresolved =
        node->barrier_await_mask &
        ~g_barriers[node->barrier_await_bucket];
    return (uint8_t)__builtin_popcount(unresolved);
}

uint32_t rp1_scheduler_build(uint32_t node_count)
{
    g_enabled = 0u;
    g_subscription_count = 0u;
    for (uint32_t i = 0u; i <= RP1_EVENT_COUNT; i++)
        g_event_offsets[i] = 0u;
    for (uint32_t i = 0u; i < RP1_MAX_NODES; i++)
        g_remaining[i] = 0u;
    clear_bitset(g_ready, &g_ready_summary);
    clear_bitset(g_deferred, &g_deferred_summary);
    clear_bitset(g_waiting, &g_waiting_summary);

    /*
     * Count subscriptions into offset[event + 1]. Abort before prefixing when
     * the sparse graph cannot fit; scanner fallback needs no partial index.
     */
    uint32_t total = 0u;
    for (uint32_t node_index = 0u; node_index < node_count; node_index++) {
        const rp1_node_t *node = &g_nodes[node_index];
        uint32_t bits = node->barrier_await_mask;
        while (bits != 0u) {
            uint32_t bit = first_set(bits);
            uint32_t event = event_id(node->barrier_await_bucket, bit);
            total++;
            if (total > RP1_SCHEDULER_MAX_SUBSCRIPTIONS)
                return 0u;
            g_event_offsets[event + 1u]++;
            bits &= bits - 1u;
        }
    }

    for (uint32_t event = 0u; event < RP1_EVENT_COUNT; event++)
        g_event_offsets[event + 1u] += g_event_offsets[event];

    /*
     * Fill each event range backwards by decrementing its end offset. After
     * filling, offset[event + 1] holds that event's start; shift starts left
     * once to restore conventional CSR [start, end) ranges without cursors.
     */
    for (uint32_t node_index = 0u; node_index < node_count; node_index++) {
        const rp1_node_t *node = &g_nodes[node_index];
        uint32_t bits = node->barrier_await_mask;
        while (bits != 0u) {
            uint32_t bit = first_set(bits);
            uint32_t event = event_id(node->barrier_await_bucket, bit);
            uint16_t position = --g_event_offsets[event + 1u];
            g_event_subscribers[position] = (uint16_t)node_index;
            bits &= bits - 1u;
        }
    }
    for (uint32_t event = 0u; event < RP1_EVENT_COUNT; event++)
        g_event_offsets[event] = g_event_offsets[event + 1u];
    g_event_offsets[RP1_EVENT_COUNT] = (uint16_t)total;

    g_subscription_count = total;
    g_enabled = 1u;
    for (uint32_t node_index = 0u; node_index < node_count; node_index++) {
        g_remaining[node_index] = unresolved_count(&g_nodes[node_index]);
        add_ready(node_index);
    }
    return 1u;
}

uint32_t rp1_scheduler_enabled(void)
{
    return g_enabled;
}

uint32_t rp1_scheduler_subscription_count(void)
{
    return g_subscription_count;
}

uint32_t rp1_scheduler_pop_ready(uint32_t *node_index)
{
    if (!g_enabled || g_ready_summary == 0u)
        return 0u;

    uint32_t word = first_set(g_ready_summary);
    uint32_t bit = first_set(g_ready[word]);
    *node_index = word * 32u + bit;
    remove_bit(g_ready, &g_ready_summary, *node_index);
    return 1u;
}

void rp1_scheduler_defer_ready(uint32_t node_index)
{
    if (g_enabled)
        add_bit(g_deferred, &g_deferred_summary, node_index);
}

void rp1_scheduler_restore_deferred(void)
{
    while (g_enabled && g_deferred_summary != 0u) {
        uint32_t word = first_set(g_deferred_summary);
        uint32_t bit = first_set(g_deferred[word]);
        uint32_t node_index = word * 32u + bit;
        remove_bit(g_deferred, &g_deferred_summary, node_index);
        add_ready(node_index);
    }
}

void rp1_scheduler_set_barriers(uint8_t bucket, uint32_t mask)
{
    uint32_t new_bits = mask & ~g_barriers[bucket];
    g_barriers[bucket] |= mask;
    if (!g_enabled)
        return;

    while (new_bits != 0u) {
        uint32_t bit = first_set(new_bits);
        uint32_t event = event_id(bucket, bit);
        for (uint16_t i = g_event_offsets[event];
             i < g_event_offsets[event + 1u]; i++) {
            uint32_t node_index = g_event_subscribers[i];
            if (rp1_node_get_status(&g_nodes[node_index]) ==
                    RP1_NODE_PENDING &&
                g_remaining[node_index] != 0u) {
                g_remaining[node_index]--;
                add_ready(node_index);
            }
        }
        new_bits &= new_bits - 1u;
    }
}

void rp1_scheduler_clear_barriers(uint8_t bucket, uint32_t mask)
{
    uint32_t cleared = mask & g_barriers[bucket];
    g_barriers[bucket] &= ~mask;
    if (!g_enabled)
        return;

    while (cleared != 0u) {
        uint32_t bit = first_set(cleared);
        uint32_t event = event_id(bucket, bit);
        for (uint16_t i = g_event_offsets[event];
             i < g_event_offsets[event + 1u]; i++) {
            uint32_t node_index = g_event_subscribers[i];
            if (rp1_node_get_status(&g_nodes[node_index]) ==
                RP1_NODE_PENDING) {
                remove_bit(g_ready, &g_ready_summary, node_index);
                remove_bit(g_deferred, &g_deferred_summary, node_index);
                g_remaining[node_index]++;
            }
        }
        cleared &= cleared - 1u;
    }
}

void rp1_scheduler_rearm_node(uint32_t node_index)
{
    if (!g_enabled)
        return;

    rp1_scheduler_remove_node(node_index);
    g_remaining[node_index] = unresolved_count(&g_nodes[node_index]);
    add_ready(node_index);
}

void rp1_scheduler_remove_node(uint32_t node_index)
{
    if (!g_enabled)
        return;
    remove_bit(g_ready, &g_ready_summary, node_index);
    remove_bit(g_deferred, &g_deferred_summary, node_index);
    remove_bit(g_waiting, &g_waiting_summary, node_index);
}

void rp1_scheduler_park_wait(uint32_t node_index)
{
    if (!g_enabled)
        return;
    remove_bit(g_ready, &g_ready_summary, node_index);
    remove_bit(g_deferred, &g_deferred_summary, node_index);
    add_bit(g_waiting, &g_waiting_summary, node_index);
}

uint32_t rp1_scheduler_next_waiting(uint32_t start_index,
                                    uint32_t *node_index)
{
    if (!g_enabled || start_index >= RP1_MAX_NODES)
        return 0u;

    uint32_t word = start_index >> 5;
    uint32_t bits =
        g_waiting[word] & (~0u << (start_index & 31u));
    while (bits == 0u) {
        word++;
        if (word >= RP1_NODE_WORDS)
            return 0u;
        bits = g_waiting[word];
    }
    *node_index = word * 32u + first_set(bits);
    return 1u;
}
