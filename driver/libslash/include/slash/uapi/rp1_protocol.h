/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/**
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * This file is dual-licensed: you may select either the GNU General Public
 * License version 2 (GPL-2.0-only) or the MIT License.  See the LICENSE
 * files in the repository root for the full text of each license.
 */

/**
 * @file rp1_protocol.h
 *
 * Shared protocol definitions for the RP1 HSA command processor.
 *
 * This header is the single source of truth for the on-wire layout used by
 * both the RP1 firmware (Cortex-R5 baremetal, freestanding) and the host
 * stack (libslash, SMI, VRT FpgaDevice).  It describes:
 *
 *   - Opcodes, flags, status codes, and condition operators
 *   - The 32-byte node packet and its 20-byte payload union
 *   - The 4 KB control block at the base of the host-visible BAR window
 *   - The 16-byte signal slot, 64-byte graph result, and trace entry
 *   - The in-flight kernel tracking table
 *   - Recommended default layout offsets within the BAR window
 *
 * All structures are fixed-size and naturally aligned so they can live in
 * DDR and be accessed over AXI without padding surprises.  Sizes and
 * critical offsets are enforced by static assertions at the bottom of the
 * file; both firmware and host pick them up at compile time.
 *
 * Freestanding-friendly: only `<stdint.h>` and `<stddef.h>` are required.
 * No libc, no kernel headers, no Linux-only macros.
 *
 * See linker/slashkit/resources/aved/rp1/ARCHITECTURE.md for the full
 * specification.
 */

#ifndef SLASH_UAPI_RP1_PROTOCOL_H
#define SLASH_UAPI_RP1_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Memory layout (host-visible BAR window)
 *
 * The host-visible BAR window is a 64 MB aperture that begins at
 * RP1_CTRL_PHYS_ADDR in the RP1's physical address space and at
 * RP1_CTRL_BAR_OFFSET in the host's BAR mapping.  The control block sits
 * at offset 0; the recommended sub-region offsets below are what libslash
 * programs into the corresponding *_base_lo/_hi control-block fields.
 * They are conventions, not hard protocol -- firmware reads whatever the
 * host writes -- but kept here so both sides agree on the default.
 * ====================================================================== */

#define RP1_CTRL_PHYS_ADDR              0x30000000UL  /* RP1 absolute */
#define RP1_CTRL_BAR_OFFSET             0x00000000UL  /* host BAR-relative */
#define RP1_CTRL_WINDOW_SIZE            0x04000000UL  /* 64 MB aperture */

#define RP1_DEFAULT_NODE_ARRAY_OFFSET   0x00001000UL  /* 256 KB reserved */
#define RP1_RESERVED_CQ_OFFSET          0x00041000UL  /* legacy v4 CQ gap */
#define RP1_DEFAULT_ARG_BUF_OFFSET      0x00051000UL  /*   1 MB */
#define RP1_DEFAULT_SIG_ARRAY_OFFSET    0x00151000UL  /*   4 KB */
#define RP1_DEFAULT_TRACE_OFFSET        0x00152000UL  /* trace ring */

/* =========================================================================
 * Opcodes
 * ====================================================================== */

typedef enum {
    RP1_OP_NOP             = 0,
    RP1_OP_WAIT            = 1,
    RP1_OP_SIGNAL          = 2,
    RP1_OP_KERNEL_DISPATCH = 3,
    RP1_OP_SCALAR_WRITE    = 4,
    RP1_OP_SCALAR_READ     = 5,
    RP1_OP_SCALAR_COPY     = 6,
    RP1_OP_DMA_COPY        = 7,
    RP1_OP_DMA_FILL        = 8,
    RP1_OP_PDI_LOAD        = 9,
    RP1_OP_LOOP            = 10,
    RP1_OP_COND            = 11,
    RP1_OP_RERUN           = 12,
    RP1_OP_HALT            = 13,
} rp1_opcode_t;

/* =========================================================================
 * Compact node control word
 * ====================================================================== */

#define RP1_NODE_OPCODE_SHIFT    0u
#define RP1_NODE_OPCODE_MASK     0x000Fu
#define RP1_NODE_FLAGS_SHIFT     4u
#define RP1_NODE_FLAGS_MASK      0x00F0u
#define RP1_NODE_STATUS_SHIFT    8u
#define RP1_NODE_STATUS_MASK     0x0F00u
#define RP1_NODE_RESERVED_MASK   0xF000u

/* Bit 0 marks an infinite dispatch; flags bits 1-3 are reserved and zero. */
#define RP1_FLAG_INFINITE        (1u << 0)

/* =========================================================================
 * Node status (written by RP1 into the node control word)
 * ====================================================================== */

typedef enum {
    RP1_NODE_PENDING    = 0,
    RP1_NODE_DISPATCHED = 1,
    RP1_NODE_DONE       = 2,
    RP1_NODE_WAITING    = 3,  /* RP1_OP_WAIT: gated on a signal slot */
    RP1_NODE_ERROR      = 4,
} rp1_node_status_t;

/* =========================================================================
 * Error codes (written to rp1_ctrl_t.rp1_error_code)
 * ====================================================================== */

#define RP1_ERR_INFLIGHT_FULL   1u  /* in-flight kernel table exhausted        */
#define RP1_ERR_KERNEL_TIMEOUT  2u  /* a dispatched kernel did not ap_done      */
#define RP1_ERR_PDI_TIMEOUT     3u  /* PDI_LOAD IPI did not complete in time    */
#define RP1_ERR_IMAGE_MISMATCH  4u  /* KERNEL_DISPATCH expected an image that is
                                     * not the one last loaded by PDI_LOAD       */
#define RP1_ERR_PDI_FAILED      5u  /* PLM rejected a partial-PDI load command   */
#define RP1_ERR_INVALID_CONFIG  6u  /* invalid shared control-block configuration */
#define RP1_ERR_INVALID_NODE    7u  /* malformed node packet                      */
/* terminal_error_detail values for RP1_ERR_INVALID_CONFIG. */
#define RP1_CONFIG_NODE_COUNT       1u
#define RP1_CONFIG_NODE_BASE        2u
#define RP1_CONFIG_RESERVED_CQ      3u
#define RP1_CONFIG_ARG_BASE         6u
#define RP1_CONFIG_SIGNAL_BASE      7u
#define RP1_CONFIG_TRACE            8u

/* terminal_error_detail values for RP1_ERR_INVALID_NODE. */
#define RP1_NODE_BAD_BARRIER        1u
#define RP1_NODE_BAD_SIGNAL_SLOT    2u
#define RP1_NODE_BAD_LOOP_CONFIG    3u
#define RP1_NODE_BAD_TARGET         4u
#define RP1_NODE_BAD_OPERATION      5u
#define RP1_NODE_BAD_ARGUMENTS      6u

/* =========================================================================
 * Condition operators (used by LOOP and COND)
 * ====================================================================== */

typedef enum {
    RP1_COP_EQ     = 0,  /* signal == value  */
    RP1_COP_NE     = 1,  /* signal != value  */
    RP1_COP_LT     = 2,  /* signal <  value  */
    RP1_COP_GE     = 3,  /* signal >= value  */
    RP1_COP_AND_NZ = 4,  /* (signal & value) != 0 */
    RP1_COP_AND_Z  = 5,  /* (signal & value) == 0 */
} rp1_condop_t;

/* =========================================================================
 * SIGNAL operation (rp1_payload_signal_t.operation)
 * ====================================================================== */

typedef enum {
    RP1_SIGOP_SET = 0,
    RP1_SIGOP_ADD = 1,
    RP1_SIGOP_OR  = 2,
    RP1_SIGOP_AND = 3,
} rp1_sigop_t;

/* =========================================================================
 * RERUN flags
 * ====================================================================== */

#define RP1_RERUN_CLEAR_STATE  (1u << 0)    /* reset loop_iterations[loop_id] */

/* =========================================================================
 * RP1 state (control block rp1_state field)
 * ====================================================================== */

typedef enum {
    RP1_STATE_INIT    = 0,
    RP1_STATE_READY   = 1,
    RP1_STATE_RUNNING = 2,
    RP1_STATE_ERROR   = 3,
    RP1_STATE_HALTED  = 4,
} rp1_state_t;

/* =========================================================================
 * Terminal graph result (protocol v6)
 * ====================================================================== */

#define RP1_GRAPH_RESULT_MAGIC  0x52534C54UL  /* "RSLT" */
#define RP1_TERMINAL_OPCODE_NONE 0xFFFFFFFFu

typedef enum {
    RP1_GRAPH_RESULT_NONE    = 0,
    RP1_GRAPH_RESULT_SUCCESS = 1,
    RP1_GRAPH_RESULT_FAILED  = 2,
    RP1_GRAPH_RESULT_HALTED  = 3,
} rp1_graph_outcome_t;

typedef enum {
    RP1_IMAGE_STATE_NONE    = 0,
    RP1_IMAGE_STATE_KNOWN   = 1,
    RP1_IMAGE_STATE_UNKNOWN = 2,
} rp1_image_state_t;

#define RP1_RESULT_RECOVERY_REQUIRED    (1u << 0)
#define RP1_RESULT_EFFECTS_MAY_BE_PARTIAL (1u << 1)
#define RP1_RESULT_INFINITE_WORK_REMAINS  (1u << 2)
#define RP1_RESULT_TRACE_ENABLED          (1u << 3)
#define RP1_RESULT_TRACE_OVERFLOW         (1u << 4)
#define RP1_RESULT_UNREACHED_NODES        (1u << 5)

#define RP1_QUIESCE_FINITE_DONE_SHIFT      0u
#define RP1_QUIESCE_FINITE_TIMEOUT_SHIFT   8u
#define RP1_QUIESCE_INFINITE_SHIFT        16u
#define RP1_QUIESCE_COUNT_MASK             0xFFu
#define RP1_QUIESCE_PACK(done, timeout, infinite)                       \
    ((((uint32_t)(done) & RP1_QUIESCE_COUNT_MASK)                       \
      << RP1_QUIESCE_FINITE_DONE_SHIFT) |                               \
     (((uint32_t)(timeout) & RP1_QUIESCE_COUNT_MASK)                    \
      << RP1_QUIESCE_FINITE_TIMEOUT_SHIFT) |                            \
     (((uint32_t)(infinite) & RP1_QUIESCE_COUNT_MASK)                   \
      << RP1_QUIESCE_INFINITE_SHIFT))

typedef struct {
    volatile uint32_t magic;               /* Written last: RP1_GRAPH_RESULT_MAGIC */
    volatile uint32_t graph_seq;           /* Accepted graph sequence               */
    volatile uint32_t outcome;             /* rp1_graph_outcome_t                    */
    volatile uint32_t flags;               /* RP1_RESULT_*                           */
    volatile uint32_t error_code;          /* First terminal RP1_ERR_* code          */
    volatile uint32_t terminal_node;        /* Failing or HALT node                   */
    volatile uint32_t terminal_opcode;      /* Opcode at terminal_node                */
    volatile uint32_t error_detail;         /* Error-specific primary detail          */
    volatile uint32_t error_aux;            /* Error-specific auxiliary detail        */
    volatile uint32_t active_image_id;      /* Final known image id, or 0             */
    volatile uint32_t image_state;          /* rp1_image_state_t                      */
    volatile uint32_t completed_operations; /* Successful node executions             */
    volatile uint32_t graph_elapsed_ticks;  /* Graph work through GRAPH_DONE          */
    volatile uint32_t publish_elapsed_ticks;/* Includes final trace/result preparation */
    volatile uint32_t trace_write_idx;      /* Final monotonic trace producer cursor  */
    volatile uint32_t quiescence;           /* RP1_QUIESCE_PACK counts                */
} rp1_graph_result_t;

/* =========================================================================
 * Trace events (optional trace ring)
 * ====================================================================== */

typedef enum {
    RP1_TRACE_GRAPH_START    = 0,
    RP1_TRACE_NODE_ACTIVATE  = 1,
    RP1_TRACE_KERNEL_LAUNCH  = 2,
    RP1_TRACE_KERNEL_DONE    = 3,
    RP1_TRACE_KERNEL_TIMEOUT = 4,
    RP1_TRACE_LOOP_ITER      = 5,
    RP1_TRACE_COND_EVAL      = 6,
    RP1_TRACE_WAIT_PARK      = 7,
    RP1_TRACE_WAIT_WAKE      = 8,
    RP1_TRACE_PDI_LOAD       = 9,
    RP1_TRACE_IMAGE_MISMATCH = 10,
    RP1_TRACE_GRAPH_DONE     = 11,
    RP1_TRACE_FLUSH_START    = 12,
    RP1_TRACE_FLUSH_END      = 13,
} rp1_trace_event_t;

/* =========================================================================
 * Payload structures (embedded in the 20-byte rp1_node_t payload)
 * ====================================================================== */

/* Single kernel argument register write -- protocol v2.
 *
 * The KERNEL_DISPATCH argument buffer is an array of these pairs.  Each pair
 * names an AXI-Lite register by its byte offset from kernel_base_addr and the
 * 32-bit value to write there.  This replaces the v1 dense-word layout (which
 * assumed contiguous registers starting at +0x10) and supports the reserved
 * gaps real HLS s_axilite maps leave between arguments.  A 64-bit argument is
 * emitted as two consecutive pairs: (off, lo) then (off + 4, hi). */
typedef struct {
    uint32_t reg_offset;   /* Byte offset from kernel_base_addr (AXI-Lite)    */
    uint32_t value;        /* 32-bit value to write                           */
} rp1_kernel_arg_t;

/* KERNEL_DISPATCH
 *
 * arg_buffer_offset is the byte offset into the shared argument buffer where
 * this kernel's rp1_kernel_arg_t[] begins; arg_count is the number of
 * (reg_offset, value) pairs there (each 64-bit argument counts as two pairs).
 * launch_kernel writes kernel_base_addr + reg_offset = value for each pair,
 * then pulses ap_start at +0x00. */
typedef struct {
    uint32_t kernel_base_addr;   /* AXI-Lite base in R5 address space        */
    uint32_t arg_buffer_offset;  /* Byte offset into argument buffer          */
    uint16_t arg_count;          /* Number of (reg_offset, value) arg pairs   */
    uint8_t  ctrl_flags;         /* Reserved in phase 1; must be zero          */
    uint8_t  _reserved;
    uint32_t timeout_cycles;     /* PMU ticks; 0 = firmware default           */
    uint32_t expected_image_id;  /* Image this kernel needs; 0 = no guard.
                                  * If non-zero and != the image last loaded
                                  * by PDI_LOAD, RP1 fails the node fast       */
} rp1_payload_kernel_dispatch_t;

/* SCALAR_WRITE -- up to two register writes, stop at first addr == 0. */
typedef struct {
    uint32_t addr;
    uint32_t value;
} rp1_write_pair_t;

#define RP1_SCALAR_WRITE_MAX  2

typedef struct {
    rp1_write_pair_t writes[RP1_SCALAR_WRITE_MAX];
    uint8_t _reserved[4];
} rp1_payload_scalar_write_t;

/* SCALAR_READ */
typedef struct {
    uint32_t source_addr;    /* AXI-Lite address to read            */
    uint8_t  target_slot;    /* Signal array slot index (0-255)     */
    uint8_t  _reserved[3];
} rp1_payload_scalar_read_t;

/* SCALAR_COPY -- copy a signal slot's value into an AXI-Lite register.
 *
 * The slot<->register inverse of SCALAR_READ: writes g_signals[source_slot] to
 * dest_addr.  Used to feed a loop-carried scalar held in a host-visible signal
 * slot into a body kernel's s_axilite input register each iteration, so the
 * carried value can flow through a kernel argument rather than a DDR buffer. */
typedef struct {
    uint32_t dest_addr;      /* AXI-Lite address to write           */
    uint8_t  source_slot;    /* Signal array slot index to read     */
    uint8_t  _reserved[3];
} rp1_payload_scalar_copy_t;

/* SIGNAL */
typedef struct {
    uint32_t value;
    uint8_t  target_slot;    /* Signal array slot index (0-255)     */
    uint8_t  operation;      /* rp1_sigop_t                         */
    uint8_t  _reserved[2];
} rp1_payload_signal_t;

/* WAIT -- block the node until a signal slot satisfies a condition.
 *
 * The cross-queue rendezvous primitive: another command queue (a peer device's
 * RP1 graph, or the host writing over the BAR) raises @c condition_signal via
 * SIGNAL/SCALAR_READ; this node stays in RP1_NODE_WAITING until
 * `compare(signal[condition_signal], condition_op, condition_value)` holds,
 * then completes and raises its barrier_set_mask.  Unlike a barrier (BTCM,
 * private to one graph execution), the signal array is host-visible DDR, so a
 * WAIT can gate on producers outside this graph.  While any WAIT is
 * outstanding the scanner keeps polling (it does not wfi), so host-written
 * signal updates are observed promptly. */
typedef struct {
    uint32_t condition_value;   /* Comparison value                    */
    uint8_t  condition_signal;  /* Signal array slot to poll           */
    uint8_t  condition_op;      /* rp1_condop_t                        */
    uint8_t  _reserved[2];
} rp1_payload_wait_t;

/* DMA_COPY */
typedef struct {
    uint32_t src_addr_lo;
    uint32_t src_addr_hi;
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t length_types;
} rp1_payload_dma_copy_t;

#define RP1_DMA_LENGTH_MASK     0x0FFFFFFFu
#define RP1_DMA_SRC_TYPE_SHIFT  28u
#define RP1_DMA_DST_TYPE_SHIFT  30u
#define RP1_DMA_TYPE_MASK       0x3u

/**
 * Return a packed DMA copy length and endpoint-type word.
 *
 * Values outside the 28-bit length and 2-bit type ranges are truncated.
 */
static inline uint32_t rp1_dma_pack(uint32_t length, uint8_t src_type,
                                    uint8_t dst_type)
{
    return (length & RP1_DMA_LENGTH_MASK) |
           (((uint32_t)src_type & RP1_DMA_TYPE_MASK)
            << RP1_DMA_SRC_TYPE_SHIFT) |
           (((uint32_t)dst_type & RP1_DMA_TYPE_MASK)
            << RP1_DMA_DST_TYPE_SHIFT);
}

/** Return the byte length encoded in a packed DMA copy word. */
static inline uint32_t rp1_dma_get_length(uint32_t packed)
{
    return packed & RP1_DMA_LENGTH_MASK;
}

/** Return the source memory type encoded in a packed DMA copy word. */
static inline uint8_t rp1_dma_get_src_type(uint32_t packed)
{
    return (uint8_t)((packed >> RP1_DMA_SRC_TYPE_SHIFT) &
                     RP1_DMA_TYPE_MASK);
}

/** Return the destination memory type encoded in a packed DMA copy word. */
static inline uint8_t rp1_dma_get_dst_type(uint32_t packed)
{
    return (uint8_t)((packed >> RP1_DMA_DST_TYPE_SHIFT) &
                     RP1_DMA_TYPE_MASK);
}

/** Replace the byte length in a packed DMA copy word. */
static inline uint32_t rp1_dma_set_length(uint32_t packed, uint32_t length)
{
    return (packed & ~RP1_DMA_LENGTH_MASK) |
           (length & RP1_DMA_LENGTH_MASK);
}

/** Replace the source memory type in a packed DMA copy word. */
static inline uint32_t rp1_dma_set_src_type(uint32_t packed, uint8_t src_type)
{
    return (packed & ~(RP1_DMA_TYPE_MASK << RP1_DMA_SRC_TYPE_SHIFT)) |
           (((uint32_t)src_type & RP1_DMA_TYPE_MASK)
            << RP1_DMA_SRC_TYPE_SHIFT);
}

/** Replace the destination memory type in a packed DMA copy word. */
static inline uint32_t rp1_dma_set_dst_type(uint32_t packed, uint8_t dst_type)
{
    return (packed & ~(RP1_DMA_TYPE_MASK << RP1_DMA_DST_TYPE_SHIFT)) |
           (((uint32_t)dst_type & RP1_DMA_TYPE_MASK)
            << RP1_DMA_DST_TYPE_SHIFT);
}

/* DMA_FILL */
typedef struct {
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t length;
    uint32_t pattern;
    uint8_t  dst_type;
    uint8_t  _reserved[3];
} rp1_payload_dma_fill_t;

/* PDI_LOAD -- partial PDI reconfiguration via PMC IPI.
 *
 * The host pre-stages a partial PDI in DDR at (pdi_addr_hi << 32 | pdi_addr_lo)
 * and submits this node.  When the node fires, RP1 issues the canonical
 * Versal "load PDI from DDR" command through the R5_1-owned source IPI
 * selected from generated platform metadata, then blocks on that source
 * agent's observation register until the PMC completes the request.
 * The host is responsible for ensuring no kernels are in-flight against
 * the region being reconfigured (use barrier dependencies). */
typedef struct {
    uint32_t pdi_addr_lo;     /* DDR physical address (low 32 bits)   */
    uint32_t pdi_addr_hi;     /* DDR physical address (high 32 bits)  */
    uint32_t timeout_cycles;  /* PMU ticks; 0 = firmware default      */
    uint32_t image_id;        /* Image id this PDI installs; recorded as
                               * the active image so KERNEL_DISPATCH can
                               * guard against stale dispatches. 0 = none */
    uint8_t  _reserved[4];
} rp1_payload_pdi_load_t;

/* LOOP */
typedef struct {
    uint16_t body_start;          /* First node index of loop body     */
    uint16_t body_end;            /* Last node index (inclusive)        */
    uint32_t max_iterations;      /* Hard cap (0 = condition-only)     */
    uint32_t condition_value;     /* Exit when signal matches          */
    uint8_t  condition_signal;    /* Signal array slot to check        */
    uint8_t  condition_op;        /* rp1_condop_t                      */
    uint8_t  bucket_clear_start;  /* First bucket to clear per iter    */
    uint8_t  bucket_clear_end;    /* Last bucket to clear (inclusive)  */
    uint8_t  loop_id;             /* Index into loop_iterations[]      */
    uint8_t  _reserved[3];
} rp1_payload_loop_t;

/* COND */
typedef struct {
    uint32_t condition_value;
    uint32_t done_mask;
    uint16_t body_start;          /* First node index (inclusive)      */
    uint16_t body_end;            /* Last node index (inclusive)       */
    uint8_t  condition_signal;    /* Signal slot to evaluate           */
    uint8_t  condition_op;        /* rp1_condop_t                      */
    uint8_t  bucket_clear_start;  /* First bucket to clear (inclusive) */
    uint8_t  bucket_clear_end;    /* Last bucket to clear (inclusive)  */
    uint8_t  done_bucket;
    uint8_t  _reserved[3];
} rp1_payload_cond_t;

/* RERUN */
typedef struct {
    uint16_t target_node;    /* Node index to reset DONE -> PENDING  */
    uint8_t  rerun_flags;    /* RP1_RERUN_CLEAR_STATE                */
    uint8_t  loop_id;        /* Loop ID to clear (if CLEAR_STATE)    */
    uint8_t  _reserved[16];
} rp1_payload_rerun_t;

/* =========================================================================
 * Node packet -- 32 bytes, 12-byte header + 20-byte payload
 * ====================================================================== */

typedef struct {
    uint16_t control;              /* Packed opcode, flags, and status      */
    uint8_t  barrier_await_bucket; /* Which of 32 buckets to check (0-31)   */
    uint8_t  barrier_set_bucket;   /* Which of 32 buckets to write (0-31)   */
    uint32_t barrier_await_mask;   /* Which bits in await_bucket must be set */
    uint32_t barrier_set_mask;     /* Which bits in set_bucket to raise      */

    /* Payload (20 bytes) */
    union {
        rp1_payload_kernel_dispatch_t kernel_dispatch;
        rp1_payload_scalar_write_t    scalar_write;
        rp1_payload_scalar_read_t     scalar_read;
        rp1_payload_scalar_copy_t     scalar_copy;
        rp1_payload_signal_t          signal;
        rp1_payload_wait_t            wait;
        rp1_payload_dma_copy_t        dma_copy;
        rp1_payload_dma_fill_t        dma_fill;
        rp1_payload_pdi_load_t        pdi_load;
        rp1_payload_loop_t            loop;
        rp1_payload_cond_t            cond;
        rp1_payload_rerun_t           rerun;
        uint8_t raw[20];
    } payload;
} rp1_node_t;

/**
 * Build a control word from unshifted opcode, flag, and status values.
 *
 * Values are truncated to their four-bit fields and reserved bits are zero.
 */
static inline uint16_t rp1_node_make_control(uint16_t opcode, uint16_t flags,
                                             uint16_t status)
{
    return (uint16_t)(((opcode << RP1_NODE_OPCODE_SHIFT) &
                       RP1_NODE_OPCODE_MASK) |
                      ((flags << RP1_NODE_FLAGS_SHIFT) &
                       RP1_NODE_FLAGS_MASK) |
                      ((status << RP1_NODE_STATUS_SHIFT) &
                       RP1_NODE_STATUS_MASK));
}

/** Return the complete packed node control word. */
static inline uint16_t rp1_node_get_control(const rp1_node_t *node)
{
    return node->control;
}

/** Replace the complete packed node control word. */
static inline void rp1_node_set_control(rp1_node_t *node, uint16_t control)
{
    node->control = control;
}

/** Return the unshifted opcode nibble from a node. */
static inline uint8_t rp1_node_get_opcode(const rp1_node_t *node)
{
    return (uint8_t)((node->control & RP1_NODE_OPCODE_MASK) >>
                     RP1_NODE_OPCODE_SHIFT);
}

/** Replace the opcode nibble without changing flags, status, or reserved bits. */
static inline void rp1_node_set_opcode(rp1_node_t *node, uint16_t opcode)
{
    node->control =
        (uint16_t)((node->control & (uint16_t)~RP1_NODE_OPCODE_MASK) |
                   ((opcode << RP1_NODE_OPCODE_SHIFT) &
                    RP1_NODE_OPCODE_MASK));
}

/** Return the unshifted flags nibble from a node. */
static inline uint8_t rp1_node_get_flags(const rp1_node_t *node)
{
    return (uint8_t)((node->control & RP1_NODE_FLAGS_MASK) >>
                     RP1_NODE_FLAGS_SHIFT);
}

/** Replace the flags nibble without changing opcode, status, or reserved bits. */
static inline void rp1_node_set_flags(rp1_node_t *node, uint16_t flags)
{
    node->control =
        (uint16_t)((node->control & (uint16_t)~RP1_NODE_FLAGS_MASK) |
                   ((flags << RP1_NODE_FLAGS_SHIFT) &
                    RP1_NODE_FLAGS_MASK));
}

/** Return the unshifted status nibble from a node. */
static inline uint8_t rp1_node_get_status(const rp1_node_t *node)
{
    return (uint8_t)((node->control & RP1_NODE_STATUS_MASK) >>
                     RP1_NODE_STATUS_SHIFT);
}

/** Replace the status nibble without changing opcode, flags, or reserved bits. */
static inline void rp1_node_set_status(rp1_node_t *node, uint16_t status)
{
    node->control =
        (uint16_t)((node->control & (uint16_t)~RP1_NODE_STATUS_MASK) |
                   ((status << RP1_NODE_STATUS_SHIFT) &
                    RP1_NODE_STATUS_MASK));
}

/* =========================================================================
 * Control block -- 4 KB DDR region at RP1_CTRL_PHYS_ADDR
 * ====================================================================== */

#define RP1_CTRL_MAGIC  0x53515231UL  /* "SQR1" */

/* Protocol-v6 firmware capabilities.  These bits describe support for the
 * reserved v6 contracts; host code must reject firmware missing any bit in
 * RP1_REQUIRED_CAPABILITIES. */
#define RP1_CAP_PLATFORM_PDI_IPI_CONFIG   (1u << 0)
#define RP1_CAP_PMU_CYCLE_TIMEOUTS        (1u << 1)
#define RP1_CAP_STRUCTURED_PDI_RESPONSE   (1u << 3)
#define RP1_CAP_LATCHED_TERMINAL_ERRORS   (1u << 4)
#define RP1_CAP_BTCM_TRACE_STAGING        (1u << 5)
#define RP1_CAP_GRAPH_RESULT              (1u << 6)

#define RP1_REQUIRED_CAPABILITIES                                      \
    (RP1_CAP_PLATFORM_PDI_IPI_CONFIG | RP1_CAP_PMU_CYCLE_TIMEOUTS |   \
     RP1_CAP_STRUCTURED_PDI_RESPONSE |                                \
     RP1_CAP_LATCHED_TERMINAL_ERRORS | RP1_CAP_BTCM_TRACE_STAGING |   \
     RP1_CAP_GRAPH_RESULT)

#define RP1_PDI_IPI_PLATFORM_UNKNOWN  0u
#define RP1_TERMINAL_ERROR_NODE_NONE  0xFFFFFFFFu

typedef struct {
    /* RP1 writes */
    volatile uint32_t magic;            /* 0x00: RP1_CTRL_MAGIC when ready       */
    volatile uint32_t version;          /* 0x04: protocol version                */
    /* Host writes, RP1 reads */
    volatile uint32_t node_count;       /* 0x08: nodes in this graph             */
    volatile uint32_t _reserved_cq_size;/* 0x0C: must be zero in protocol v6     */
    volatile uint32_t node_base_lo;     /* 0x10: node array base (low 32 bits)   */
    volatile uint32_t node_base_hi;     /* 0x14: node array base (high 32 bits)  */
    volatile uint32_t _reserved_cq_base_lo; /* 0x18: must be zero in v6           */
    volatile uint32_t _reserved_cq_base_hi; /* 0x1C: must be zero in v6           */
    volatile uint32_t graph_seq;        /* 0x20: host increments per graph        */
    /* RP1 writes */
    volatile uint32_t graph_done_seq;   /* 0x24: last completed graph             */
    volatile uint32_t _reserved_cq_write_idx; /* 0x28: zero in protocol v6         */
    volatile uint32_t _reserved_cq_read_idx;  /* 0x2C: zero in protocol v6         */
    /* RP1 writes */
    volatile uint32_t rp1_state;        /* 0x30: rp1_state_t                     */
    volatile uint32_t rp1_error_code;   /* 0x34: last error code                 */
    volatile uint32_t rp1_current_node; /* 0x38: current node (debug)            */
    volatile uint32_t heartbeat;        /* 0x3C: liveness counter                */
    /* Host writes, RP1 reads */
    volatile uint32_t arg_buf_base_lo;  /* 0x40: argument buffer base            */
    volatile uint32_t arg_buf_base_hi;  /* 0x44                                  */
    volatile uint32_t sig_array_base_lo;/* 0x48: signal array base               */
    volatile uint32_t sig_array_base_hi;/* 0x4C                                  */
    volatile uint32_t trace_enable;     /* 0x50: non-zero enables trace writes   */
    volatile uint32_t trace_base_lo;    /* 0x54: trace ring base (low 32 bits)   */
    volatile uint32_t trace_base_hi;    /* 0x58: trace ring base (high 32 bits)  */
    volatile uint32_t trace_size;       /* 0x5C: trace entries (power of 2)      */
    volatile uint32_t trace_write_idx;  /* 0x60: next trace write position       */
    /* RP1 writes: protocol-v6 contract publication and diagnostics */
    volatile uint32_t capabilities;          /* 0x64: RP1_CAP_*                    */
    volatile uint32_t pdi_ipi_platform_id;   /* 0x68: selected IPI/platform config */
    volatile uint32_t terminal_error_node;   /* 0x6C: latched failing node         */
    volatile uint32_t terminal_error_detail; /* 0x70: structured error detail      */
    volatile uint32_t terminal_error_aux;    /* 0x74: structured auxiliary detail  */
    uint32_t _reserved_result_align[2];      /* 0x78-0x7F                       */
    rp1_graph_result_t result;               /* 0x80-0xBF: committed graph result */
    uint8_t _reserved[0x1000 - 0xC0];
} rp1_ctrl_t;

/* =========================================================================
 * Signal array slot -- 16 bytes, RP1_MAX_SIGNALS slots in DDR
 * ====================================================================== */

#define RP1_SIG_FLAG_HOST_VISIBLE  (1u << 0)

typedef struct {
    volatile uint32_t value;            /* Current 32-bit value               */
    volatile uint32_t _reserved;
    volatile uint32_t last_writer_node; /* Node that last wrote               */
    volatile uint32_t flags;            /* RP1_SIG_FLAG_*                     */
} rp1_signal_slot_t;

/* =========================================================================
 * Trace queue entry -- 16 bytes
 * ====================================================================== */

typedef struct {
    volatile uint32_t timestamp;  /* PMU ticks since graph start            */
    volatile uint16_t event;      /* rp1_trace_event_t                      */
    volatile uint16_t node_index; /* Node index, or 0xFFFF for graph events */
    volatile uint32_t aux0;       /* Event-specific detail                  */
    volatile uint32_t aux1;       /* Event-specific detail                  */
} rp1_trace_entry_t;

/* =========================================================================
 * In-flight kernel table entry (BTCM, max RP1_MAX_INFLIGHT entries)
 * ====================================================================== */

typedef struct {
    uint32_t base_addr;        /* AXI-Lite base address in R5 space  */
    uint32_t node_index;
    uint32_t set_mask;
    uint32_t timeout_start;      /* PMU tick captured when launched   */
    uint32_t timeout_cycles;     /* elapsed PMU ticks before timeout  */
    uint8_t  set_bucket;
    uint8_t  infinite;         /* Non-zero: INFINITE flag set        */
    uint8_t  settle_polls;     /* Ignore stale ap_done for N polls   */
    uint8_t  _reserved;
} rp1_inflight_t;

/* =========================================================================
 * Compile-time size constants
 * ====================================================================== */

#define RP1_MAX_NODES          1024
#define RP1_MAX_TRACE_ENTRIES  4096
#define RP1_MAX_LOOPS            64
#define RP1_MAX_INFLIGHT         32
#define RP1_MAX_SIGNALS         256
#define RP1_MAX_BUCKETS          32

#define RP1_PROTOCOL_VERSION  6u

/* Cortex-R5 PMCCNTR is configured with PMCR.D, so one protocol PMU tick is
 * exactly 64 R5 core cycles. timeout_cycles and trace/result timestamps use this
 * unit. Hardware defaults are derived from the generated R5 clock frequency. */
#define RP1_PMU_CYCLE_DIVISOR             64u
#define RP1_DEFAULT_KERNEL_TIMEOUT_MS    1000u
#define RP1_DEFAULT_PDI_TIMEOUT_MS       5000u

/* =========================================================================
 * Static assertions -- enforced on every translation unit that includes
 * this header (firmware and host both).  C11 _Static_assert is used in C;
 * C++11+ static_assert is used in C++.
 * ====================================================================== */

#if defined(__cplusplus)
#  define RP1_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#  define RP1_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* Node packet must be exactly 32 bytes with payload at offset 12. */
RP1_STATIC_ASSERT(sizeof(rp1_node_t) == 32,
                  "rp1_node_t must be exactly 32 bytes");
#if defined(__cplusplus)
RP1_STATIC_ASSERT(alignof(rp1_node_t) == 4,
                  "rp1_node_t must be naturally four-byte aligned");
#else
RP1_STATIC_ASSERT(_Alignof(rp1_node_t) == 4,
                  "rp1_node_t must be naturally four-byte aligned");
#endif
RP1_STATIC_ASSERT(offsetof(rp1_node_t, control) == 0,
                  "rp1_node_t control must start at byte 0");
RP1_STATIC_ASSERT(offsetof(rp1_node_t, barrier_await_bucket) == 2,
                  "rp1_node_t await bucket must start at byte 2");
RP1_STATIC_ASSERT(offsetof(rp1_node_t, barrier_set_bucket) == 3,
                  "rp1_node_t set bucket must start at byte 3");
RP1_STATIC_ASSERT(offsetof(rp1_node_t, barrier_await_mask) == 4,
                  "rp1_node_t await mask must start at byte 4");
RP1_STATIC_ASSERT(offsetof(rp1_node_t, barrier_set_mask) == 8,
                  "rp1_node_t set mask must start at byte 8");
RP1_STATIC_ASSERT(offsetof(rp1_node_t, payload) == 12,
                  "rp1_node_t payload must start at byte 12");

/* Kernel argument pair (protocol v2) must be exactly 8 bytes. */
RP1_STATIC_ASSERT(sizeof(rp1_kernel_arg_t) == 8,
                  "rp1_kernel_arg_t must be exactly 8 bytes");

/* Every payload has its exact protocol-v6 natural size. */
RP1_STATIC_ASSERT(sizeof(rp1_payload_kernel_dispatch_t) == 20,
                  "kernel_dispatch payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_scalar_write_t)    == 20,
                  "scalar_write payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_scalar_read_t)     == 8,
                  "scalar_read payload must be 8 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_scalar_copy_t)     == 8,
                  "scalar_copy payload must be 8 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_signal_t)          == 8,
                  "signal payload must be 8 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_wait_t)            == 8,
                  "wait payload must be 8 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_dma_copy_t)        == 20,
                  "dma_copy payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_dma_fill_t)        == 20,
                  "dma_fill payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_pdi_load_t)        == 20,
                  "pdi_load payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_loop_t)            == 20,
                  "loop payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_cond_t)            == 20,
                  "cond payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_payload_rerun_t)           == 20,
                  "rerun payload must be 20 bytes");
RP1_STATIC_ASSERT(sizeof(((rp1_node_t *)0)->payload.raw) == 20,
                  "raw payload must be 20 bytes");

/* Compact payload field offsets are part of the wire ABI. */
RP1_STATIC_ASSERT(offsetof(rp1_payload_kernel_dispatch_t, arg_count) == 8,
                  "dispatch arg_count offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_kernel_dispatch_t, ctrl_flags) == 10,
                  "dispatch ctrl_flags offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_kernel_dispatch_t, timeout_cycles) == 12,
                  "dispatch timeout offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_kernel_dispatch_t,
                           expected_image_id) == 16,
                  "dispatch image offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_scalar_read_t, target_slot) == 4,
                  "scalar_read target offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_scalar_copy_t, source_slot) == 4,
                  "scalar_copy source offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_signal_t, target_slot) == 4,
                  "signal target offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_signal_t, operation) == 5,
                  "signal operation offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_wait_t, condition_signal) == 4,
                  "wait signal offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_wait_t, condition_op) == 5,
                  "wait operation offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_dma_copy_t, length_types) == 16,
                  "dma_copy packed offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_dma_fill_t, dst_type) == 16,
                  "dma_fill type offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_pdi_load_t, image_id) == 12,
                  "pdi_load image offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_loop_t, max_iterations) == 4,
                  "loop maximum offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_loop_t, condition_value) == 8,
                  "loop value offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_loop_t, condition_signal) == 12,
                  "loop signal offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_loop_t, loop_id) == 16,
                  "loop id offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_cond_t, done_mask) == 4,
                  "cond done mask offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_cond_t, body_start) == 8,
                  "cond body offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_cond_t, condition_signal) == 12,
                  "cond signal offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_cond_t, done_bucket) == 16,
                  "cond done bucket offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_rerun_t, rerun_flags) == 2,
                  "rerun flags offset");
RP1_STATIC_ASSERT(offsetof(rp1_payload_rerun_t, loop_id) == 3,
                  "rerun loop offset");

/* Control block must be exactly 4 KB. */
RP1_STATIC_ASSERT(sizeof(rp1_ctrl_t) == 0x1000,
                  "rp1_ctrl_t must be exactly 4 KB");

/* Critical control-block field offsets (hardware/host ABI). */
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, magic)              == 0x00, "ctrl.magic offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, version)            == 0x04, "ctrl.version offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, node_count)         == 0x08, "ctrl.node_count offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, _reserved_cq_size)  == 0x0C, "ctrl.reserved_cq_size offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, node_base_lo)       == 0x10, "ctrl.node_base_lo offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, _reserved_cq_base_lo) == 0x18, "ctrl.reserved_cq_base offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, graph_seq)          == 0x20, "ctrl.graph_seq offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, graph_done_seq)     == 0x24, "ctrl.graph_done_seq offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, _reserved_cq_write_idx) == 0x28, "ctrl.reserved_cq_write offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, _reserved_cq_read_idx) == 0x2C, "ctrl.reserved_cq_read offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, rp1_state)          == 0x30, "ctrl.rp1_state offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, heartbeat)          == 0x3C, "ctrl.heartbeat offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, arg_buf_base_lo)    == 0x40, "ctrl.arg_buf_base_lo offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, sig_array_base_lo)  == 0x48, "ctrl.sig_array_base_lo offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, trace_enable)       == 0x50, "ctrl.trace_enable offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, trace_base_lo)      == 0x54, "ctrl.trace_base_lo offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, trace_base_hi)      == 0x58, "ctrl.trace_base_hi offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, trace_size)         == 0x5C, "ctrl.trace_size offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, trace_write_idx)    == 0x60, "ctrl.trace_write_idx offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, capabilities)       == 0x64, "ctrl.capabilities offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, pdi_ipi_platform_id)== 0x68, "ctrl.pdi_ipi_platform_id offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, terminal_error_node)== 0x6C, "ctrl.terminal_error_node offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, terminal_error_detail) == 0x70, "ctrl.terminal_error_detail offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, terminal_error_aux) == 0x74, "ctrl.terminal_error_aux offset");
RP1_STATIC_ASSERT(offsetof(rp1_ctrl_t, result)             == 0x80, "ctrl.result offset");

/* Result, signal slot, trace entry, and inflight tracker sizes. */
RP1_STATIC_ASSERT(sizeof(rp1_graph_result_t) == 64,
                  "rp1_graph_result_t must be 64 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_signal_slot_t) == 16,
                  "rp1_signal_slot_t must be 16 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_trace_entry_t) == 16,
                  "rp1_trace_entry_t must be 16 bytes");
RP1_STATIC_ASSERT(sizeof(rp1_inflight_t) == 24,
                  "rp1_inflight_t must be 24 bytes");
RP1_STATIC_ASSERT(offsetof(rp1_inflight_t, timeout_start) == 0x0C,
                  "inflight timeout_start offset");
RP1_STATIC_ASSERT(offsetof(rp1_inflight_t, timeout_cycles) == 0x10,
                  "inflight timeout_cycles offset");

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SLASH_UAPI_RP1_PROTOCOL_H */
