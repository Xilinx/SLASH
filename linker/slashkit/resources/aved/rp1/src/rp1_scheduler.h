/*
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: MIT
 *
 * RP1 barrier-event execution index.
 */

#ifndef RP1_SCHEDULER_H
#define RP1_SCHEDULER_H

#include <stdint.h>

/*
 * ATCM capacity for barrier-event subscriber references. Graphs above this
 * sparse-edge ceiling retain the flat scanner as a correctness fallback.
 */
#ifdef QEMU_SEMIHOSTING
#define RP1_SCHEDULER_MAX_SUBSCRIPTIONS 4096u
#else
#define RP1_SCHEDULER_MAX_SUBSCRIPTIONS 16384u
#endif

/*
 * Build the event-to-subscriber CSR and initial ready set from g_nodes.
 *
 * @return 1 when the event index fits local TCM, or 0 when execution must use
 *         the flat scanner fallback.
 */
uint32_t rp1_scheduler_build(uint32_t node_count);

/* Return non-zero when the current graph uses the event index. */
uint32_t rp1_scheduler_enabled(void);

/* Return the number of event-to-node references in the current graph. */
uint32_t rp1_scheduler_subscription_count(void);

/* Pop the lowest-index ready node, returning zero when no node is ready. */
uint32_t rp1_scheduler_pop_ready(uint32_t *node_index);

/* Defer one resource-blocked ready node until the next activation pass. */
void rp1_scheduler_defer_ready(uint32_t node_index);

/* Restore all deferred nodes that remain pending and dependency-ready. */
void rp1_scheduler_restore_deferred(void);

/*
 * Publish barrier bits and wake subscribers on each zero-to-one transition.
 * The barrier array remains authoritative in indexed and fallback modes.
 */
void rp1_scheduler_set_barriers(uint8_t bucket, uint32_t mask);

/*
 * Clear selected barrier bits and make pending subscribers unresolved again.
 * Rearmed DONE nodes are handled separately by rp1_scheduler_rearm_node().
 */
void rp1_scheduler_clear_barriers(uint8_t bucket, uint32_t mask);

/* Recompute one PENDING node's unresolved count and ready membership. */
void rp1_scheduler_rearm_node(uint32_t node_index);

/* Remove a node from ready, deferred, and parked-WAIT sets. */
void rp1_scheduler_remove_node(uint32_t node_index);

/* Move one dependency-ready WAIT node into the parked signal-poll set. */
void rp1_scheduler_park_wait(uint32_t node_index);

/*
 * Find the first parked WAIT at or after start_index.
 *
 * @return 1 and populate node_index on success, or 0 when none remain.
 */
uint32_t rp1_scheduler_next_waiting(uint32_t start_index,
                                    uint32_t *node_index);

#endif /* RP1_SCHEDULER_H */
