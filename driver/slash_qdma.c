/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file slash_qdma.c
 *
 * QDMA (Queue-based DMA) subsystem for the SLASH FPGA driver.
 *
 * This file implements the QDMA data-plane for SLASH, an AMD Alveo V80
 * partial-reconfiguration FPGA design.  It wraps the Xilinx libqdma
 * library (from submodules/qdma_drv/QDMA/linux-kernel/driver/libqdma/)
 * to provide queue-pair-based DMA transfers between host memory and the
 * FPGA fabric.
 *
 * The QDMA subsystem binds to PF1 (PCI device ID 0x50C1, or the legacy
 * 0x50B5 / AVED 0x50BD), while the control device (slash_ctldev) binds to
 * PF2 (device ID 0x50C2, or the legacy 0x50B6).
 *
 * Queue pair lifecycle:
 *   add -> start -> I/O (via anon_inode fd) -> stop -> del
 *
 * Key design decisions:
 *   - **Poll mode** (no interrupts): avoids interrupt overhead for
 *     streaming workloads; the host polls HW-written completion status.
 *   - **Synchronous transfers**: qdma_request_submit() blocks until the
 *     DMA completes or times out (10 s default).
 *   - **XArray for qpair tracking**: provides dynamic ID allocation,
 *     built-in locking, and automatic index management for up to 256
 *     concurrent queue pairs.
 *   - **Reference counting**: kref on both the device and each qpair
 *     entry; the anon_inode fd holds a ref, preventing premature
 *     destruction while userspace still has the fd open.
 */

#include "slash_qdma.h"

#include "libqdma_export.h"

#include "slash.h"
#include "slash_chrdev.h"

#include <asm/cacheflush.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/stddef.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/anon_inodes.h>

#if defined(SLASH_HAVE_URING_CMD)
#include <linux/io_uring.h>
#if __has_include(<linux/io_uring/cmd.h>)
#include <linux/io_uring/cmd.h>
#endif

/**
 * slash_qdma_uring_cmd_payload() - Pointer to a uring_cmd's inline SQE payload.
 * @cmd: The io_uring command.
 *
 * Abstracts the kernel API change that removed struct io_uring_cmd::cmd in
 * favour of ->sqe + io_uring_sqe_cmd().  The accessor is selected at build
 * time by the kcompat probe (SLASH_HAVE_URING_SQE_CMD); both forms return the
 * same inline command payload pointer.
 */
static inline const void *slash_qdma_uring_cmd_payload(struct io_uring_cmd *cmd)
{
#if defined(SLASH_HAVE_URING_SQE_CMD)
    return io_uring_sqe_cmd(cmd->sqe);
#else
    return cmd->cmd;
#endif
}
#endif

/*
 * Direction bitmask constants.
 *
 * These map 1:1 with the libqdma queue_type_t enum values (Q_H2C,
 * Q_C2H, Q_CMPT) but expressed as bit positions so they can be
 * OR'd together in a single dir_mask field.
 *
 * SLASH_QDMA_DIR_H2C  — Host-to-Card (write path)
 * SLASH_QDMA_DIR_C2H  — Card-to-Host (read path)
 * SLASH_QDMA_DIR_CMPT — Completion queue (status/metadata from card)
 */
#define SLASH_QDMA_DIR_H2C  BIT(0)
#define SLASH_QDMA_DIR_C2H  BIT(1)
#define SLASH_QDMA_DIR_CMPT BIT(2)
#define SLASH_QDMA_DIR_MASK (SLASH_QDMA_DIR_H2C | SLASH_QDMA_DIR_C2H | \
                             SLASH_QDMA_DIR_CMPT)

/*
 * CPM5 Host Profile indirect-context programming.
 *
 * The Host Profile context tells the CPM5 QDMA how to route AXI4-MM
 * traffic onto the Versal NoC.  It is programmed via the same indirect
 * context command interface libqdma uses for queue contexts, but with
 * the host-profile selector (0xA).  Register offsets and the command
 * word layout mirror eqdma_cpm5_reg.h:
 *
 *   IND_CTXT_DATA  base 0x804 (8 x u32 context words)
 *   IND_CTXT_MASK  base 0x824 (8 x u32 write masks)
 *   IND_CTXT_CMD   0x844      (busy[0], sel[4:1], op[6:5], qid[18:7])
 *
 * We program two profiles so the per-queue SW-context host_id selects
 * the NoC channel: Host ID 0 -> NoC Channel 0, Host ID 1 -> NoC Channel 1.
 */
#define SLASH_QDMA_HP_DATA_ADDR  0x804u
#define SLASH_QDMA_HP_MASK_ADDR  0x824u
#define SLASH_QDMA_HP_CMD_ADDR   0x844u
#define SLASH_QDMA_HP_CMD_BUSY   BIT(0)
#define SLASH_QDMA_HP_NUM_WORDS  8
#define SLASH_QDMA_HP_SEL        0xAu   /* QDMA_CTXT_SELC_HOST_PROFILE */
#define SLASH_QDMA_HP_OP_WR      0x1u   /* indirect context WR opcode */
#define SLASH_QDMA_HP_OP_RD      0x2u   /* indirect context RD opcode */
#define SLASH_QDMA_HP_SMID_BASE  0x100u /* bit 8 set; base AXI-MM master ID */
#define SLASH_QDMA_HP_POLL_US    1000   /* busy-wait budget in microseconds */

/*
 * The qpair fd data path operates on spans of 4 KiB base pages.  Each
 * scatter-gather entry is exactly one base page, so a whole transfer is
 * submitted to libqdma as a single multi-descriptor request and libqdma
 * refills the descriptor ring as needed -- the transfer size is not bounded
 * by the ring depth.
 */

/**
 * SLASH_QDMA_QTYPE_COUNT - Number of queue types tracked per queue pair.
 *
 * Equals Q_CMPT + 1 (i.e., 3): one slot each for H2C, C2H, and CMPT.
 * Used to size the per-qpair qhndl[] array.
 */
#define SLASH_QDMA_QTYPE_COUNT (Q_CMPT + 1)

/**
 * SLASH_QDMA_MAX_QPAIRS - Maximum number of simultaneous queue pairs.
 *
 * This matches the conf.qsets_max value passed to qdma_device_open()
 * in slash_qdma_conf_options(), keeping the xarray ID space and the
 * HW queue-set limit in sync.
 */
#define SLASH_QDMA_MAX_QPAIRS 256

/**
 * SLASH_QDMA_QPAIR_ID_RANGE - XArray allocation range for qpair IDs.
 *
 * Constrains xa_alloc() to assign IDs in [0, 255].  The xarray handles
 * thread-safe allocation and lookup of queue pair entries within this
 * range.
 */
#define SLASH_QDMA_QPAIR_ID_RANGE XA_LIMIT(0, SLASH_QDMA_MAX_QPAIRS - 1)

/*
 * Debug logging infrastructure.
 *
 * When SLASH_QDMA_OP_DEBUG is non-zero (compile-time flag), every
 * libqdma call and state transition is logged via pr_info / dev_info.
 * In production builds the macros expand to nothing to avoid log spam.
 */
#ifndef SLASH_QDMA_OP_DEBUG
#define SLASH_QDMA_OP_DEBUG 0
#endif

#if SLASH_QDMA_OP_DEBUG
#define SLASH_QDMA_OP_LOG(fmt, ...) \
    pr_info("slash: qdma: " fmt, ##__VA_ARGS__)
#define SLASH_QDMA_OP_DEV_LOG(dev, fmt, ...) \
    dev_info((dev), "slash: qdma: " fmt, ##__VA_ARGS__)
#else
#define SLASH_QDMA_OP_LOG(fmt, ...) \
    do {                             \
    } while (0)
#define SLASH_QDMA_OP_DEV_LOG(dev, fmt, ...) \
    do {                                      \
    } while (0)
#endif

/*
 * Per-transfer timing instrumentation (compile-time flag).
 *
 * Retained for parity with the userspace SLASH_QDMA_TIMING knob.  With the
 * kernel-owned buffer model all the expensive setup (page allocation, SGL
 * build, DMA mapping) happens once at SLASH_QDMA_IOCTL_BUF_CREATE time, so the
 * steady-state transfer cost is dominated by the libqdma submit/completion.
 */
#ifndef SLASH_QDMA_TIMING
#define SLASH_QDMA_TIMING 0
#endif

/* Forward declaration; full definition follows. */
struct slash_qdma_dev;

/**
 * struct slash_qdma_qpair_entry - Per-queue-pair state.
 * @ref:        Reference count.  Starts at 1 (held by the xarray slot);
 *              an additional ref is taken when an anon_inode fd is handed
 *              to userspace, so the entry outlives the xarray removal if
 *              the fd is still open.
 * @qhndl:     Array of libqdma queue handles, one per queue type
 *              (Q_H2C, Q_C2H, Q_CMPT).  Entries that are not in use
 *              hold the sentinel QDMA_QUEUE_IDX_INVALID.
 * @dir_mask:   Bitmask of active directions (SLASH_QDMA_DIR_H2C, etc.).
 *              Updated as individual queues are added or removed.
 * @mode:       Queue operating mode (QDMA_Q_MODE_MM or QDMA_Q_MODE_ST).
 * @irq_mode:   Interrupt mode.  Currently always 0 (poll mode).
 * @irq_vector: MSI-X vector assignment.  Currently unused (poll mode).
 */
struct slash_qdma_qpair_entry {
    struct kref ref;
    unsigned long qhndl[SLASH_QDMA_QTYPE_COUNT];
    u32 dir_mask;
    enum qdma_q_mode mode;
    u32 irq_mode;
    u32 irq_vector;
};

/**
 * struct slash_qdma_dev - Per-PCI-device QDMA state.
 * @pdev:               The PCI device (PF1) this instance is bound to.
 * @qdma_handle:        Opaque handle returned by qdma_device_open();
 *                      passed to every subsequent libqdma call.
 * @cdev:               Character device embedded for inode-to-device lookup.
 * @device:             Class device with the BDF-specific sysfs name.
 * @slot:               Stable board number shared with the matching PF2.
 * @ref:                Device-level reference count.  The control-device open
 *                      path and each anon_inode fd hold a ref; the device
 *                      structure is freed when the last ref drops.
 * @lock:               Serialises ioctl paths and protects @qpairs,
 *                      @hw_shutdown, and @have_qdma_handle.
 * @qpairs:             XArray mapping qpair IDs (u32) to
 *                      &struct slash_qdma_qpair_entry pointers.  Using an
 *                      xarray gives us O(1) lookup, thread-safe auto-ID
 *                      allocation, and safe concurrent iteration during
 *                      teardown.
 * @have_qdma_handle:   True once qdma_device_open() succeeds; false after
 *                      qdma_device_close().  Guards against use-after-close.
 * @have_board_slot:    True after the shared board allocator is acquired.
 * @is_cdev_registered: True while the character device is live.
 * @hw_shutdown:        Set to true during destroy to signal that the HW is
 *                      going away.  Any ioctl arriving after this flag is
 *                      set returns -ENODEV immediately.
 *
 * The state booleans (@have_qdma_handle, @have_board_slot,
 * @is_cdev_registered,
 * @hw_shutdown) track partially-constructed state during probe/remove
 * error paths; outside of create/destroy they should always reflect a
 * fully initialised device.
 */
struct slash_qdma_dev {
    struct pci_dev *pdev;
    unsigned long qdma_handle;

    struct cdev cdev;
    struct device *device;
    int slot;
    struct kref ref;
    struct mutex lock;
    struct xarray qpairs;

    /*
     * Initialization booleans.
     * Assume these are always true outside of create/destroy.
     */
    bool have_qdma_handle;
    bool have_board_slot;
    bool is_cdev_registered;
    bool hw_shutdown;
};

/**
 * typedef slash_qdma_queue_cmd_fn - Function pointer for queue lifecycle ops.
 *
 * Matches the signature of qdma_queue_start(), qdma_queue_stop(), and
 * slash_qdma_queue_remove_safe(), allowing slash_qdma_ioctl_qpair_op_apply()
 * to iterate over all directions in a queue pair and apply the same
 * operation generically.
 */
typedef int (*slash_qdma_queue_cmd_fn)(unsigned long qdma_handle,
                                       unsigned long qhndl,
                                       char *errbuf,
                                       int errbuf_sz);

/* Forward declaration — defined below after its helper functions. */
static int slash_qdma_queue_remove_safe(unsigned long qdma_handle,
                                        unsigned long qhndl,
                                        char *errbuf,
                                        int errbuf_sz);

/* ─────────────────────────────────────────────────────────────────────
 * Direction / queue-type conversion helpers
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_dir_to_qtype() - Convert a direction bitmask bit to a queue type.
 * @dir_bit: Exactly one of SLASH_QDMA_DIR_H2C, _C2H, or _CMPT.
 *
 * Return: The corresponding libqdma queue_type_t value.
 *
 * Note: currently unused (hence __attribute__((unused))), but kept as
 * the inverse of slash_qdma_qtype_to_dir() for completeness.
 */
__attribute__((unused))
static enum queue_type_t slash_qdma_dir_to_qtype(u32 dir_bit)
{
    switch (dir_bit) {
    case SLASH_QDMA_DIR_H2C:
        return Q_H2C;
    case SLASH_QDMA_DIR_C2H:
        return Q_C2H;
    case SLASH_QDMA_DIR_CMPT:
        return Q_CMPT;
    default:
        return Q_H2C; /* should never reach */
    }
}

/**
 * slash_qdma_qtype_to_dir() - Convert a queue type to its direction bitmask bit.
 * @qtype: One of Q_H2C, Q_C2H, or Q_CMPT.
 *
 * Return: The corresponding SLASH_QDMA_DIR_* bitmask value, or 0 for
 *         an unrecognised queue type.
 */
static u32 slash_qdma_qtype_to_dir(enum queue_type_t qtype)
{
    switch (qtype) {
    case Q_H2C:
        return SLASH_QDMA_DIR_H2C;
    case Q_C2H:
        return SLASH_QDMA_DIR_C2H;
    case Q_CMPT:
        return SLASH_QDMA_DIR_CMPT;
    default:
        return 0;
    }
}

/**
 * slash_qdma_qhndl_is_valid() - Check if a queue handle is valid.
 * @qhndl: Queue handle from libqdma.
 *
 * Return: true if @qhndl is not the sentinel QDMA_QUEUE_IDX_INVALID,
 *         meaning the queue has been successfully added to the HW.
 */
static inline bool slash_qdma_qhndl_is_valid(unsigned long qhndl)
{
    return qhndl != QDMA_QUEUE_IDX_INVALID;
}

/* ─────────────────────────────────────────────────────────────────────
 * Queue removal with state-machine safety
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_queue_remove_safe() - Stop-then-remove a queue, handling any state.
 * @qdma_handle: Device handle from qdma_device_open().
 * @qhndl:       Queue handle to remove.
 * @errbuf:      Buffer for libqdma error messages.
 * @errbuf_sz:   Size of @errbuf.
 *
 * The QDMA HW queue state machine requires that an ONLINE queue be
 * stopped before it can be removed.  This helper queries the current
 * state and performs the correct transitions:
 *
 *   - Q_STATE_ONLINE   -> stop, then remove
 *   - Q_STATE_ENABLED  -> remove directly (already stopped)
 *   - Q_STATE_DISABLED -> no-op (already removed)
 *   - anything else    -> return -EINVAL
 *
 * This "check-before-stop" pattern prevents errors from trying to stop
 * an already-stopped queue or remove an already-removed one, which is
 * important during teardown where we may not know the current state.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_queue_remove_safe(unsigned long qdma_handle,
                                        unsigned long qhndl,
                                        char *errbuf,
                                        int errbuf_sz)
{
    struct qdma_q_state qstate = {0};
    int err;

    if (!errbuf || errbuf_sz <= 0)
        return -EINVAL;

    errbuf[0] = '\0';

    /* Query the current HW queue state */
    SLASH_QDMA_OP_LOG("qdma_get_queue_state start: handle=%lu qhndl=%lu\n",
                      qdma_handle, qhndl);
    err = qdma_get_queue_state(qdma_handle, qhndl, &qstate, errbuf, errbuf_sz);
    if (err) {
        SLASH_QDMA_OP_LOG("qdma_get_queue_state failed: qhndl=%lu err=%d (%s)\n",
                          qhndl, err, errbuf);
        return err;
    }
    SLASH_QDMA_OP_LOG("qdma_get_queue_state done: qhndl=%lu state=%u\n",
                      qhndl, qstate.qstate);

    switch (qstate.qstate) {
    case Q_STATE_ONLINE:
        /* Queue is active — must stop before removing. */
        SLASH_QDMA_OP_LOG("qdma_queue_stop start: qhndl=%lu\n", qhndl);
        err = qdma_queue_stop(qdma_handle, qhndl, errbuf, errbuf_sz);
        if (err) {
            SLASH_QDMA_OP_LOG("qdma_queue_stop failed: qhndl=%lu err=%d (%s)\n",
                              qhndl, err, errbuf);
            return err;
        }
        SLASH_QDMA_OP_LOG("qdma_queue_stop done: qhndl=%lu\n", qhndl);
        break;
    case Q_STATE_ENABLED:
        /* Queue is added but not started — can remove directly. */
        break;
    case Q_STATE_DISABLED:
        /* Queue is already removed. */
        SLASH_QDMA_OP_LOG("queue already disabled, skip remove: qhndl=%lu\n",
                          qhndl);
        return 0;
    default:
        snprintf(errbuf, errbuf_sz, "queue in unexpected state %u",
                 qstate.qstate);
        SLASH_QDMA_OP_LOG("qdma_get_queue_state unexpected state: qhndl=%lu state=%u\n",
                          qhndl, qstate.qstate);
        return -EINVAL;
    }

    /* State is now ENABLED — safe to remove. */
    SLASH_QDMA_OP_LOG("qdma_queue_remove start: qhndl=%lu\n", qhndl);
    err = qdma_queue_remove(qdma_handle, qhndl, errbuf, errbuf_sz);
    if (err) {
        SLASH_QDMA_OP_LOG("qdma_queue_remove failed: qhndl=%lu err=%d (%s)\n",
                          qhndl, err, errbuf);
        return err;
    }
    SLASH_QDMA_OP_LOG("qdma_queue_remove done: qhndl=%lu\n", qhndl);

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * Queue pair xarray helpers (lookup, refcount, insert, remove)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_qpair_lookup() - Find a qpair entry by ID.
 * @qdma_dev: QDMA device whose xarray to search.
 * @qid:      Queue pair ID.
 *
 * Return: Pointer to the entry, or NULL if @qid is not allocated.
 *
 * Note: the caller must hold @qdma_dev->lock or otherwise guarantee
 * that the entry will not be freed during use (e.g., by holding a ref).
 */
static inline struct slash_qdma_qpair_entry *
slash_qdma_qpair_lookup(struct slash_qdma_dev *qdma_dev, u32 qid)
{
    return xa_load(&qdma_dev->qpairs, qid);
}

/**
 * slash_qdma_qpair_entry_release() - kref release callback for qpair entries.
 * @ref: kref embedded in the slash_qdma_qpair_entry being released.
 *
 * Called when the last reference to a qpair entry is dropped.  Frees
 * the entry structure.  By this point, all associated HW queues must
 * already have been removed.
 */
static void slash_qdma_qpair_entry_release(struct kref *ref)
{
    struct slash_qdma_qpair_entry *entry =
        container_of(ref, struct slash_qdma_qpair_entry, ref);

    kfree(entry);
}

/**
 * slash_qdma_qpair_get() - Acquire a reference on a qpair entry.
 * @entry: The entry to reference.
 *
 * Used when handing out an anon_inode fd so the entry survives until
 * the fd is closed, even if the qpair is deleted from the xarray.
 */
static inline void slash_qdma_qpair_get(struct slash_qdma_qpair_entry *entry)
{
    kref_get(&entry->ref);
}

/**
 * slash_qdma_qpair_put() - Release a reference on a qpair entry.
 * @entry: The entry to dereference.
 *
 * When the last reference drops, the entry is freed via
 * slash_qdma_qpair_entry_release().
 */
static inline void slash_qdma_qpair_put(struct slash_qdma_qpair_entry *entry)
{
    kref_put(&entry->ref, slash_qdma_qpair_entry_release);
}

/**
 * slash_qdma_qpair_insert() - Allocate a new qpair ID and insert the entry.
 * @qdma_dev: QDMA device whose xarray receives the entry.
 * @entry:    The new entry to insert.  Its kref is initialised here.
 * @id:       [out] The auto-assigned queue pair ID.
 *
 * Uses xa_alloc() to atomically pick the lowest available ID in
 * [0, SLASH_QDMA_MAX_QPAIRS-1] and store @entry at that index.
 *
 * Return: 0 on success, -EBUSY if all 256 IDs are in use, or other
 *         negative errno.
 */
static inline int
slash_qdma_qpair_insert(struct slash_qdma_dev *qdma_dev, struct slash_qdma_qpair_entry *entry, u32 *id)
{
    kref_init(&entry->ref);
    return xa_alloc(&qdma_dev->qpairs, id, entry, SLASH_QDMA_QPAIR_ID_RANGE, GFP_KERNEL);
}

/**
 * slash_qdma_qpair_remove() - Erase a qpair from the xarray and drop its ref.
 * @qdma_dev: QDMA device.
 * @qid:      Queue pair ID to remove.
 *
 * After this call, the ID is available for reuse.  The entry itself is
 * only freed when all references (including any held by open fds) are
 * released.
 */
static inline void
slash_qdma_qpair_remove(struct slash_qdma_dev *qdma_dev, u32 qid)
{
    struct slash_qdma_qpair_entry *entry;

    entry = xa_erase(&qdma_dev->qpairs, qid);
    if (entry)
        slash_qdma_qpair_put(entry);
}

/* ─────────────────────────────────────────────────────────────────────
 * Anon-inode file context and I/O control block
 * ───────────────────────────────────────────────────────────────────── */

/**
 * struct slash_qdma_qpair_file_ctx - Private data for an anon_inode qpair fd.
 * @qdma_dev:  Back-pointer to the owning QDMA device (ref held).
 * @entries:   The queue pair entries this fd operates on (one ref each).
 *             A transfer sub-transfer's qpair_index selects an entry here.
 * @qids:      Queue pair IDs, cached for debug logging.
 * @n_qpairs:  Number of valid entries in @entries / @qids
 *             (1..SLASH_QDMA_FD_MAX_QPAIRS).
 *
 * Allocated in slash_qdma_ioctl_qpair_get_fd_w() and freed in
 * slash_qdma_qpair_release().  @qdma_dev and each entry have their reference
 * counts incremented when the ctx is created, and decremented when the fd is
 * closed.
 */
struct slash_qdma_qpair_file_ctx {
    struct slash_qdma_dev *qdma_dev;
    struct slash_qdma_qpair_entry *entries[SLASH_QDMA_FD_MAX_QPAIRS];
    u32 qids[SLASH_QDMA_FD_MAX_QPAIRS];
    u32 n_qpairs;
};

/**
 * struct slash_qdma_buf - A kernel-owned, mmap-able DMA buffer.
 * @ref:        Reference count.  The buffer fd holds one ref, each live VMA
 *              (mmap) holds one ref, and each in-flight transfer holds a
 *              temporary ref so a close cannot tear the buffer down under
 *              active DMA or while userspace still has it mapped.
 * @qdma_dev:   Device whose DMA mappings back this buffer (holds a device
 *              reference for the lifetime of the buffer object).
 * @length:     Buffer length in bytes (a multiple of @granule).
 * @granule:    Bytes per SGL entry / page (PAGE_SIZE).  Uniform across all
 *              entries, so transfer slices are computed by simple division.
 * @pages_nr:   Number of base pages backing the buffer (length / granule).
 * @pages:      Array of @pages_nr kernel pages (alloc_page()), not physically
 *              contiguous.  Used both for the CPU mmap and the DMA SGL.
 * @sgl:        Prebuilt scatter-gather list, one entry per page, each with its
 *              dma_addr filled in once at creation so transfers submit with
 *              req->dma_mapped = 1.
 * @dma_mapped: True once @sgl entries have been DMA-mapped.
 *
 * All expensive setup (page allocation, SGL construction, DMA mapping) happens
 * once at creation; the transfer fast path only slices @sgl, syncs the touched
 * pages, and submits.
 */
struct slash_qdma_buf {
    struct kref ref;
    struct slash_qdma_dev *qdma_dev;
    u64 length;
    u32 granule;
    unsigned int pages_nr;
    struct page **pages;
    struct qdma_sw_sg *sgl;
    bool dma_mapped;
};

/* ─────────────────────────────────────────────────────────────────────
 * Forward declarations
 * ───────────────────────────────────────────────────────────────────── */

static int slash_qdma_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void slash_qdma_remove(struct pci_dev *pdev);
static int slash_qdma_create_qdma_device(struct pci_dev *pdev, struct slash_qdma_dev **pdevice);
static void slash_qdma_destroy_qdma_device(struct slash_qdma_dev *device);
static void slash_qdma_dev_release(struct kref *ref);
static void slash_qdma_conf_options(struct qdma_dev_conf *conf, struct pci_dev *pdev);
static int slash_qdma_ioctl_info_w(struct slash_qdma_dev *qdma_dev,
                                   void __user *uarg);
static int slash_qdma_ioctl_qpair_add_w(struct slash_qdma_dev *qdma_dev,
                                         void __user *uarg);
static int slash_qdma_ioctl_qpair_add(struct slash_qdma_dev *qdma_dev,
                                      struct slash_qdma_qpair_add *req);
static int slash_qdma_ioctl_qpair_add_q(struct slash_qdma_dev *qdma_dev,
                                        struct slash_qdma_qpair_add *req,
                                        struct slash_qdma_qpair_entry *entry,
                                        enum queue_type_t qtype);
static void slash_qdma_ioctl_qpair_rm_q(struct slash_qdma_dev *qdma_dev,
                                        struct slash_qdma_qpair_entry *entry,
                                        enum queue_type_t qtype);
static int slash_qdma_ioctl_qpair_op_w(struct slash_qdma_dev *qdma_dev,
                                       void __user *uarg);
static int slash_qdma_ioctl_qpair_op(struct slash_qdma_dev *qdma_dev,
                                     struct slash_qdma_qpair_op *req);
static int slash_qdma_ioctl_qpair_op_apply(struct slash_qdma_dev *qdma_dev,
                                           struct slash_qdma_qpair_entry *entry,
                                           struct slash_qdma_qpair_op *req,
                                           slash_qdma_queue_cmd_fn fn,
                                           const char *op_name,
                                           bool stop_on_err);
static int slash_qdma_ioctl_qpair_get_fd_w(struct slash_qdma_dev *qdma_dev,
                                           void __user *uarg);
static int slash_qdma_ioctl_buf_create_w(struct slash_qdma_dev *qdma_dev,
                                          void __user *uarg);
static void slash_qdma_buf_release(struct kref *ref);
static void slash_qdma_buf_put(struct slash_qdma_buf *buf);
static long slash_qdma_qpair_transfer(struct file *file, void __user *uarg);

static int slash_qdma_qpair_release(struct inode *inode, struct file *file);
static long slash_qdma_qpair_ioctl(struct file *file,
                                   unsigned int cmd, unsigned long arg);
#if defined(SLASH_HAVE_URING_CMD)
static int slash_qdma_qpair_uring_cmd(struct io_uring_cmd *cmd,
                                      unsigned int issue_flags);
#endif

/**
 * slash_qdma_qpair_fops - File operations for per-qpair anon_inode fds.
 *
 * ioctl    performs buffer DMA transfers and buffer creation for clients that
 *          only hold a queue-pair fd.
 * uring_cmd (optional) is the asynchronous equivalent of the transfer ioctl,
 *          available only on kernels with io_uring uring_cmd support.
 * release  drops the refs on the bound qpair entries and device.
 */
static const struct file_operations slash_qdma_qpair_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = slash_qdma_qpair_ioctl,
#if defined(SLASH_HAVE_URING_CMD)
    .uring_cmd      = slash_qdma_qpair_uring_cmd,
#endif
    .release        = slash_qdma_qpair_release,
};


static int slash_qdma_fop_open(struct inode *inode, struct file *file);
static int slash_qdma_fop_release(struct inode *inode, struct file *file);
static long slash_qdma_fop_ioctl(struct file *file, unsigned int op, unsigned long arg);
static void slash_qdma_ioctl_info(struct slash_qdma_dev *qdma_dev,
                                  struct slash_qdma_info *qdma_info);



/**
 * slash_qdma_ids - PCI device ID table for the QDMA PF.
 *
 * Matches the PF1 QDMA function on AMD/Xilinx V80 cards: the current
 * 0x50C1, the legacy 0x50B5 used by pre-compute-platform designs, and
 * the AVED/V80P 0x50BD.
 */
static const struct pci_device_id slash_qdma_ids[] = {
    {PCI_DEVICE(SLASH_QDMA_PCI_VENDOR_ID, SLASH_QDMA_PCI_DEVICE_ID)},
    {PCI_DEVICE(SLASH_QDMA_PCI_VENDOR_ID, SLASH_QDMA_PCI_DEVICE_ID_LEGACY)},
    {PCI_DEVICE(SLASH_QDMA_PCI_VENDOR_ID, SLASH_AVED_QDMA_PCI_DEVICE_ID)},
    {0,}
};
MODULE_DEVICE_TABLE(pci, slash_qdma_ids);

/**
 * slash_qdma_driver - PCI driver structure for the QDMA subsystem.
 *
 * Registered in slash_qdma_init(); triggers slash_qdma_probe() for each
 * matching PF1 device discovered during PCI enumeration.
 */
static struct pci_driver slash_qdma_driver = {
    .name = SLASH_QDMA_DRV_NAME,
    .id_table = slash_qdma_ids,
    .probe = slash_qdma_probe,
    .remove = slash_qdma_remove,
};

/**
 * slash_qdma_fops - File operations for the QDMA control device.
 *
 * /dev/slash_qdma_ctlN is the management interface:
 * userspace opens it and issues ioctls to add/start/stop/delete queue
 * pairs and to obtain per-qpair I/O fds.
 */
static struct file_operations slash_qdma_fops = {
    .owner          = THIS_MODULE,
    .open           = slash_qdma_fop_open,
    .release        = slash_qdma_fop_release,
    .unlocked_ioctl = slash_qdma_fop_ioctl,
};

/* ─────────────────────────────────────────────────────────────────────
 * Module init / exit
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_init() - Initialise the QDMA subsystem.
 * @num_threads: Worker thread count for libqdma's internal processing.
 * @debugfs:     Optional debugfs mount path, or NULL to disable.
 *
 * Called from the top-level module init.  Initialises the libqdma
 * library first (which sets up internal data structures and worker
 * threads), then registers the PCI driver so that slash_qdma_probe()
 * fires for each PF1 device.
 *
 * Return: 0 on success, negative errno on failure.
 */
int __init slash_qdma_init(unsigned int num_threads, char *debugfs)
{
    int err;

    SLASH_QDMA_OP_LOG("init start: num_threads=%u debugfs=%s\n",
                      num_threads, debugfs ? debugfs : "(null)");

    err = libqdma_init(num_threads, debugfs);
    if (err) {
        SLASH_QDMA_OP_LOG("libqdma_init failed: err=%d\n", err);
        pr_err("slash: libqdma_init failed: %d\n", err);
        return err;
    }
    SLASH_QDMA_OP_LOG("libqdma_init done\n");

    err = pci_register_driver(&slash_qdma_driver);
    if (err) {
        SLASH_QDMA_OP_LOG("pci_register_driver failed: err=%d\n", err);
        pr_err("slash: register qdma driver failed: %d\n", err);
        goto err_exit_libqdma;
    }
    SLASH_QDMA_OP_LOG("pci_register_driver done\n");

    return 0;

err_exit_libqdma:
    SLASH_QDMA_OP_LOG("libqdma_exit start (init rollback)\n");
    libqdma_exit();
    SLASH_QDMA_OP_LOG("libqdma_exit done (init rollback)\n");

    return err;
}

/**
 * slash_qdma_exit() - Tear down the QDMA subsystem.
 *
 * Called from the top-level module exit.  Unregisters the PCI driver
 * (which triggers slash_qdma_remove() for each probed device) and then
 * shuts down the libqdma library.
 */
void slash_qdma_exit(void)
{
    SLASH_QDMA_OP_LOG("exit start\n");

    pci_unregister_driver(&slash_qdma_driver);
    SLASH_QDMA_OP_LOG("pci_unregister_driver done\n");

    libqdma_exit();
    SLASH_QDMA_OP_LOG("libqdma_exit done\n");
}

/* ─────────────────────────────────────────────────────────────────────
 * CPM5 Host Profile context programming
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_hp_set_field() - Set a bit field in the host profile context.
 * @words: Array of SLASH_QDMA_HP_NUM_WORDS u32s holding the 256-bit context
 *         (word i covers bits [32*i+31 : 32*i]).
 * @hi:    Most-significant bit index of the field (inclusive).
 * @lo:    Least-significant bit index of the field (inclusive).
 * @val:   Value to place in [hi:lo]; bits outside the field width are masked.
 *
 * Handles fields that straddle a 32-bit word boundary (e.g. the C2H
 * AXI4-MM steering field at bits [97:94], which spans words 2 and 3).
 */
static void slash_qdma_hp_set_field(u32 *words, unsigned int hi,
                                    unsigned int lo, u32 val)
{
    unsigned int width = hi - lo + 1;
    u32 fmask = (width >= 32) ? ~0u : ((1u << width) - 1u);
    unsigned int word = lo >> 5;
    unsigned int off = lo & 31;
    u64 wmask = (u64)fmask << off;
    u64 wval = (u64)(val & fmask) << off;

    words[word] = (words[word] & ~(u32)(wmask & 0xFFFFFFFFu)) |
                  (u32)(wval & 0xFFFFFFFFu);

    if ((off + width) > 32 && (word + 1) < SLASH_QDMA_HP_NUM_WORDS)
        words[word + 1] = (words[word + 1] & ~(u32)(wmask >> 32)) |
                          (u32)(wval >> 32);
}

/**
 * slash_qdma_hp_wait_ready() - Poll the indirect-context BUSY bit.
 * @device:  QDMA device (provides the libqdma handle for register access).
 * @val_out: If non-NULL, receives the last QDMA_IND_CTXT_CMD value read.
 *
 * Spins (up to SLASH_QDMA_HP_POLL_US microseconds) until the indirect
 * context command BUSY bit clears.  Logging is left to the caller so the
 * write path can treat a timeout as fatal while the readback path can treat
 * it as a warning.
 *
 * Return: 0 once not busy, -ETIMEDOUT on timeout, or a negative errno from
 *         the register read.
 */
static int slash_qdma_hp_wait_ready(struct slash_qdma_dev *device, u32 *val_out)
{
    unsigned int waited_us = 0;
    u32 val = 0;
    int err;

    do {
        err = qdma_device_read_config_register(device->qdma_handle,
                SLASH_QDMA_HP_CMD_ADDR, &val);
        if (err)
            return err;
        if (!(val & SLASH_QDMA_HP_CMD_BUSY)) {
            if (val_out)
                *val_out = val;
            return 0;
        }
        udelay(1);
    } while (++waited_us < SLASH_QDMA_HP_POLL_US);

    if (val_out)
        *val_out = val;
    return -ETIMEDOUT;
}

/**
 * slash_qdma_hp_get_field() - Read a bit field from the host profile context.
 * @words: Array of SLASH_QDMA_HP_NUM_WORDS u32s holding the 256-bit context
 *         (word i covers bits [32*i+31 : 32*i]).
 * @hi:    Most-significant bit index of the field (inclusive).
 * @lo:    Least-significant bit index of the field (inclusive).
 *
 * Inverse of slash_qdma_hp_set_field(); handles fields that straddle a
 * 32-bit word boundary (e.g. the C2H AXI4-MM steering field at bits
 * [97:94], which spans words 2 and 3).
 *
 * Return: the value held in [hi:lo].
 */
static u32 slash_qdma_hp_get_field(const u32 *words, unsigned int hi,
                                   unsigned int lo)
{
    unsigned int width = hi - lo + 1;
    u32 fmask = (width >= 32) ? ~0u : ((1u << width) - 1u);
    unsigned int word = lo >> 5;
    unsigned int off = lo & 31;
    u64 two = (u64)words[word];

    if ((word + 1) < SLASH_QDMA_HP_NUM_WORDS)
        two |= (u64)words[word + 1] << 32;

    return (u32)((two >> off) & fmask);
}

/**
 * slash_qdma_read_host_profile() - Read one CPM5 Host Profile entry back.
 * @device:  QDMA device (provides the libqdma handle for register access).
 * @host_id: Host Profile index to read.
 * @out:     Array of SLASH_QDMA_HP_NUM_WORDS u32s that receives the 256-bit
 *           context.
 *
 * Issues an indirect-context RD command for the host-profile selector,
 * waits for the controller to complete it, and copies the IND_CTXT_DATA
 * words back.  Used to verify a preceding write.
 *
 * Return: 0 on success, negative errno on register-access error or
 *         -ETIMEDOUT if the BUSY bit never clears.
 */
static int slash_qdma_read_host_profile(struct slash_qdma_dev *device,
                                        u32 host_id, u32 *out)
{
    u32 cmd = (host_id << 7) | (SLASH_QDMA_HP_OP_RD << 5) |
              (SLASH_QDMA_HP_SEL << 1);
    int err;
    int i;

    err = qdma_device_write_config_register(device->qdma_handle,
            SLASH_QDMA_HP_CMD_ADDR, cmd);
    if (err)
        return err;

    err = slash_qdma_hp_wait_ready(device, NULL);
    if (err)
        return err;

    for (i = 0; i < SLASH_QDMA_HP_NUM_WORDS; i++) {
        err = qdma_device_read_config_register(device->qdma_handle,
                SLASH_QDMA_HP_DATA_ADDR + (i * sizeof(u32)), &out[i]);
        if (err)
            return err;
    }

    return 0;
}

/**
 * slash_qdma_write_host_profile() - Program and verify one CPM5 Host Profile.
 * @device:  QDMA device (provides the libqdma handle for register access).
 * @host_id: Host Profile index to program (also the AXI4-MM steering value,
 *           i.e. the target NoC channel).
 *
 * Builds the 256-bit host profile context with the SMID and H2C/C2H
 * AXI4-MM steering fields, writes it through the indirect-context
 * registers via the libqdma-exported config register accessors, and
 * polls the command BUSY bit until the controller completes the write.
 *
 * Once the write completes it reads the profile back and verifies the
 * programmed fields (SMID and the two steering fields); a readback error
 * or field mismatch is logged but is non-fatal (the profile is still
 * considered applied).
 *
 * Only the SMID and the two steering fields are non-zero; the AXI
 * prot/cache attributes are left at 0.
 *
 * Return: 0 on success, negative errno on register-access error or
 *         -ETIMEDOUT if the BUSY bit never clears.
 */
static int slash_qdma_write_host_profile(struct slash_qdma_dev *device,
                                         u32 host_id)
{
    u32 data[SLASH_QDMA_HP_NUM_WORDS] = {0};
    u32 smid = SLASH_QDMA_HP_SMID_BASE + host_id;
    u32 cmd;
    u32 val = 0;
    int err;
    int i;

    /* SMID [201:192]; H2C steering [181:178]; C2H steering [97:94]. */
    slash_qdma_hp_set_field(data, 201, 192, smid);
    slash_qdma_hp_set_field(data, 181, 178, host_id);
    slash_qdma_hp_set_field(data, 97, 94, host_id);

    /* Context data words. */
    for (i = 0; i < SLASH_QDMA_HP_NUM_WORDS; i++) {
        err = qdma_device_write_config_register(device->qdma_handle,
                SLASH_QDMA_HP_DATA_ADDR + (i * sizeof(u32)), data[i]);
        if (err)
            goto err_reg;
    }

    /* Context masks: write every bit. */
    for (i = 0; i < SLASH_QDMA_HP_NUM_WORDS; i++) {
        err = qdma_device_write_config_register(device->qdma_handle,
                SLASH_QDMA_HP_MASK_ADDR + (i * sizeof(u32)), 0xFFFFFFFFu);
        if (err)
            goto err_reg;
    }

    /* Command: qid=host_id, op=WR, sel=HOST_PROFILE (0x34 for id 0, 0xB4 for id 1). */
    cmd = (host_id << 7) | (SLASH_QDMA_HP_OP_WR << 5) | (SLASH_QDMA_HP_SEL << 1);
    err = qdma_device_write_config_register(device->qdma_handle,
            SLASH_QDMA_HP_CMD_ADDR, cmd);
    if (err)
        goto err_reg;

    /* Wait for the controller to consume the command. */
    err = slash_qdma_hp_wait_ready(device, &val);
    if (err == -ETIMEDOUT) {
        dev_err(&device->pdev->dev,
                "qdma: host profile %u programming timed out (cmd=0x%x)\n",
                host_id, val);
        return -ETIMEDOUT;
    }
    if (err)
        goto err_reg;

    /*
     * Read the profile back and verify the programmed fields.  A readback
     * error or field mismatch is non-fatal: the write itself completed, so
     * the profile is still considered applied.
     */
    {
        u32 rb[SLASH_QDMA_HP_NUM_WORDS] = {0};
        int rerr = slash_qdma_read_host_profile(device, host_id, rb);

        if (rerr) {
            dev_warn(&device->pdev->dev,
                     "slash: qdma: host profile %u applied (cmd=0x%02x) but readback failed: %d\n",
                     host_id, cmd, rerr);
        } else {
            u32 smid_rb = slash_qdma_hp_get_field(rb, 201, 192);
            u32 h2c_rb = slash_qdma_hp_get_field(rb, 181, 178);
            u32 c2h_rb = slash_qdma_hp_get_field(rb, 97, 94);

            if (smid_rb == smid && h2c_rb == host_id && c2h_rb == host_id) {
                dev_info(&device->pdev->dev,
                         "slash: qdma: host profile %u applied and readback verified: H2C/C2H AXI-MM steering=%u (NoC channel %u), smid=0x%03x (cmd=0x%02x)\n",
                         host_id, host_id, host_id, smid, cmd);
            } else {
                dev_err(&device->pdev->dev,
                        "slash: qdma: host profile %u readback MISMATCH: smid exp=0x%03x got=0x%03x, h2c exp=%u got=%u, c2h exp=%u got=%u\n",
                        host_id, smid, smid_rb, host_id, h2c_rb,
                        host_id, c2h_rb);
            }
        }
    }
    return 0;

err_reg:
    dev_err(&device->pdev->dev,
            "qdma: host profile %u register access failed: %d\n",
            host_id, err);
    return err;
}

/**
 * slash_qdma_program_host_profiles() - Program the CPM5 Host Profiles.
 * @device: QDMA device.
 *
 * Programs Host Profile 0 (steer to NoC Channel 0) and Host Profile 1
 * (steer to NoC Channel 1).  Must run after qdma_device_open() (which
 * clears all contexts) and before any queue context is programmed, per
 * the CPM5 requirement that the host profile exist before AXI4-MM
 * queues are set up.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_program_host_profiles(struct slash_qdma_dev *device)
{
    u32 host_id;
    int err;

    dev_info(&device->pdev->dev,
             "slash: qdma: programming CPM5 host profiles (host_id 0 -> NoC channel 0, host_id 1 -> NoC channel 1)\n");

    for (host_id = 0; host_id <= 1; host_id++) {
        err = slash_qdma_write_host_profile(device, host_id);
        if (err)
            return err;
    }

    dev_info(&device->pdev->dev,
             "slash: qdma: CPM5 host profiles programmed\n");

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * PCI probe / remove
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_probe() - PCI probe callback for QDMA devices.
 * @pdev: The PCI device being probed.
 * @id:   Matching entry from slash_qdma_ids[].
 *
 * Verifies that the device is PF1 (the QDMA IP is only on PF1; PF2 is
 * the control function handled by slash_ctldev).  Then:
 *   1. Allocates and initialises a slash_qdma_dev structure.
 *   2. Configures and opens the libqdma device via qdma_device_open().
 *   3. Registers the management character device (/dev/slash_qdma_ctlN).
 *
 * On any failure, the partially-constructed device is torn down and
 * the probe returns the error.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int err;
    struct qdma_dev_conf conf;
    struct slash_qdma_dev *device = NULL;
    char name[64];

    memset(&conf, 0, sizeof(conf));

    dev_info(&pdev->dev, "slash: qdma: probe start for %s\n", pci_name(pdev));
    SLASH_QDMA_OP_DEV_LOG(&pdev->dev,
                          "probe details: vendor=0x%04x device=0x%04x fn=%u\n",
                          pdev->vendor, pdev->device, PCI_FUNC(pdev->devfn));

    /* Reject anything that is not PF1 — the QDMA IP lives only on PF1. */
    if (PCI_FUNC(pdev->devfn) != SLASH_QDMA_PF) {
        dev_err(&pdev->dev, "slash: expected PF %u, got %u\n", SLASH_QDMA_PF, PCI_FUNC(pdev->devfn));
        return -EINVAL;
    }

    /* Allocate and initialise the per-device structure. */
    err = slash_qdma_create_qdma_device(pdev, &device);
    if (err) {
        goto err_free;
    }

    /* Configure and open the libqdma device. */
    slash_qdma_conf_options(&conf, pdev);
    SLASH_QDMA_OP_DEV_LOG(&pdev->dev,
                          "qdma_device_open start: name=%s qsets_max=%d qsets_base=%d\n",
                          SLASH_NAME, conf.qsets_max, conf.qsets_base);
    err = qdma_device_open(SLASH_NAME, &conf, &device->qdma_handle);
    if (err) {
        SLASH_QDMA_OP_DEV_LOG(&pdev->dev, "qdma_device_open failed: err=%d\n",
                              err);
        dev_err(&pdev->dev, "slash: qdma: could not open qdma device %d", err);
        goto err_free;
    }
    SLASH_QDMA_OP_DEV_LOG(&pdev->dev,
                          "qdma_device_open done: handle=%lu\n",
                          device->qdma_handle);
    device->have_qdma_handle = true;

    /*
     * Program the CPM5 Host Profiles before exposing the character device, so
     * they exist before userspace can add any queue.  Host ID 0 steers
     * AXI4-MM traffic to NoC Channel 0 and Host ID 1 to NoC Channel 1;
     * the per-queue SW-context host_id (mirrored from mm_channel = qid & 1)
     * selects between them.
     */
    err = slash_qdma_program_host_profiles(device);
    if (err) {
        dev_err(&pdev->dev,
                "slash: qdma: could not program host profiles: %d", err);
        goto err_free;
    }

    snprintf(name, sizeof(name), SLASH_QDMA_CTLDEV_NAME_FMT, pci_name(pdev));
    device->device =
        slash_chrdev_add(&device->cdev, &slash_qdma_fops,
                         SLASH_QDMA_MINOR(device->slot), &pdev->dev,
                         device, name);
    if (IS_ERR(device->device)) {
        err = PTR_ERR(device->device);
        device->device = NULL;
        dev_err(&pdev->dev,
                "slash: qdma: could not register character device: %d",
                err);
        goto err_free;
    }
    device->is_cdev_registered = true;

    return 0;

err_free:
    if (device) {
        slash_qdma_destroy_qdma_device(device);
        kref_put(&device->ref, slash_qdma_dev_release);
    }

    return err;
}

/**
 * slash_qdma_remove() - PCI remove callback for QDMA devices.
 * @pdev: The PCI device being removed.
 *
 * Tears down all HW queues, closes the libqdma device, deregisters the
 * character device, and drops the device reference.
 */
static void slash_qdma_remove(struct pci_dev *pdev)
{
    struct slash_qdma_dev *device = pci_get_drvdata(pdev);

    if (!device)
        return;

    slash_qdma_destroy_qdma_device(device);
    kref_put(&device->ref, slash_qdma_dev_release);
}

/* ─────────────────────────────────────────────────────────────────────
 * Device allocation and teardown
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_create_qdma_device() - Allocate and initialise a QDMA device.
 * @pdev:    PCI device to bind to.
 * @pdevice: [out] Receives a pointer to the new device on success.
 *
 * Allocates the slash_qdma_dev, initialises its mutex, xarray, kref, stable
 * board slot, and stores it in the PCI drvdata.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_create_qdma_device(struct pci_dev *pdev, struct slash_qdma_dev **pdevice)
{
    struct slash_qdma_dev *device;
    int err;

    pci_set_drvdata(pdev, NULL);
    device = kzalloc(sizeof(*device), GFP_KERNEL);
    if (!device)
        return -ENOMEM;

    device->pdev = pci_dev_get(pdev);
    device->slot = -1;
    kref_init(&device->ref);
    mutex_init(&device->lock);
    xa_init_flags(&device->qpairs, XA_FLAGS_ALLOC);
    device->hw_shutdown = false;
    pci_set_drvdata(pdev, device);

    err = slash_chrdev_board_get(pdev);
    if (err < 0) {
        dev_err(&pdev->dev, "qdma: board-slot allocation failed: %d\n",
                err);
        goto err_free;
    }
    device->slot = err;
    device->have_board_slot = true;

    *pdevice = device;
    return 0;

err_free:
    slash_qdma_destroy_qdma_device(device);
    kref_put(&device->ref, slash_qdma_dev_release);
    *pdevice = NULL;

    return err;
}

/**
 * slash_qdma_destroy_qdma_device() - Tear down a QDMA device.
 * @device: The device to destroy.
 *
 * Idempotent: uses @hw_shutdown to ensure the teardown sequence runs
 * only once even if called from multiple paths (e.g., probe error +
 * remove).
 *
 * Teardown order:
 *   1. Set @hw_shutdown = true (prevents new ioctls).
 *   2. Deregister the character device (prevents new opens).
 *   3. Iterate all queue pairs: stop, remove each HW queue, erase from
 *      xarray, and drop the xarray's ref.
 *   4. Destroy the xarray.
 *   5. Close the libqdma device handle.
 *
 * Note: the device structure itself is freed later by the kref callback
 * (slash_qdma_dev_release) when the last reference drops.
 */
static void slash_qdma_destroy_qdma_device(struct slash_qdma_dev *device)
{
    int err;

    if (!device) {
        return;
    }

    mutex_lock(&device->lock);
    if (device->hw_shutdown) {
        mutex_unlock(&device->lock);
        return;
    }
    device->hw_shutdown = true;
    mutex_unlock(&device->lock);

    /* Detach from PCI drvdata so no new lookups can find us. */
    pci_set_drvdata(device->pdev, NULL);

    /* Remove the character device before tearing down hardware state. */
    if (device->is_cdev_registered) {
        slash_chrdev_del(&device->cdev, device->device);
        device->device = NULL;
        device->is_cdev_registered = false;
    }
    if (device->have_board_slot) {
        slash_chrdev_board_put(device->pdev);
        device->slot = -1;
        device->have_board_slot = false;
    }

    mutex_lock(&device->lock);

    {
        /*
         * Tear down all remaining queue pairs.  This handles the case
         * where userspace did not cleanly delete its queues before the
         * device is removed (e.g., surprise removal or unclean exit).
         */
        struct slash_qdma_qpair_entry *entry;
        unsigned long index;
        unsigned int idx;

        xa_for_each(&device->qpairs, index, entry) {
            for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
                enum queue_type_t qtype = idx;
                u32 dir_bit = slash_qdma_qtype_to_dir(qtype);

                if (!(entry->dir_mask & dir_bit))
                    continue;

                slash_qdma_ioctl_qpair_rm_q(device, entry, qtype);
            }
            xa_erase(&device->qpairs, index);
            slash_qdma_qpair_put(entry);
        }
        xa_destroy(&device->qpairs);
    }

    /* Close the libqdma device handle, releasing HW resources. */
    if (device->have_qdma_handle) {
        SLASH_QDMA_OP_DEV_LOG(&device->pdev->dev,
                              "qdma_device_close start: handle=%lu\n",
                              device->qdma_handle);
        err = qdma_device_close(device->pdev, device->qdma_handle);
        if (err) {
            SLASH_QDMA_OP_DEV_LOG(&device->pdev->dev,
                                  "qdma_device_close failed: err=%d\n", err);
            dev_err(&device->pdev->dev, "Error in qdma_device_close: %d\n", err);
        } else {
            SLASH_QDMA_OP_DEV_LOG(&device->pdev->dev,
                                  "qdma_device_close done\n");
        }
        device->have_qdma_handle = false;
    }

    mutex_unlock(&device->lock);
}

/**
 * slash_qdma_dev_release() - kref release callback for the QDMA device.
 * @ref: kref embedded in the slash_qdma_dev being released.
 *
 * Called when the last reference drops (after both the control device is
 * closed and all anon_inode fds are released).  Drops the retained PCI
 * device reference and frees the device structure.
 */
static void slash_qdma_dev_release(struct kref *ref)
{
    struct slash_qdma_dev *device =
        container_of(ref, struct slash_qdma_dev, ref);

    mutex_destroy(&device->lock);
    pci_dev_put(device->pdev);
    kfree(device);
}

/* ─────────────────────────────────────────────────────────────────────
 * libqdma device configuration
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_conf_options() - Populate the qdma_dev_conf for device open.
 * @conf: Configuration structure to fill in.
 * @pdev: PCI device being opened.
 *
 * Sets up the libqdma device configuration with parameters tuned for
 * the V80 SLASH design:
 *
 *   - qsets_max = 256: maximum number of queue pairs (matches
 *     SLASH_QDMA_MAX_QPAIRS).
 *   - zerolen_dma = 0: zero-length transfers are disallowed.
 *   - master_pf = 1: this is the master physical function.
 *   - qdma_drv_mode = POLL_MODE: avoids interrupt overhead for
 *     streaming workloads.  The host polls HW-written completion
 *     status in memory instead of waiting for MSI-X interrupts.
 *   - msix_qvec_max = 32: Versal-specific MSI-X vector limit for
 *     queues.  Even though we use poll mode, libqdma still needs
 *     a non-zero value here for internal setup.
 *   - intr_rngsz = INTR_RING_SZ_4KB: interrupt ring size from the
 *     reference driver defaults.
 *   - bar_num_config = 0: BAR 0 is the configuration BAR.
 *   - bar_num_user / bar_num_bypass = -1: not used in this design.
 *   - qsets_base = -1: let libqdma auto-assign the queue set base.
 *   - All optional callbacks (ISR handlers, FLR resource free) are
 *     set to NULL since we operate in poll mode.
 */
static void slash_qdma_conf_options(struct qdma_dev_conf *conf, struct pci_dev *pdev)
{
    conf->pdev               = pdev;
    conf->qsets_max          = 256; /* Maximum number of queue paris. Might be lowered. TODO: tune */
    conf->zerolen_dma        = 0; /* Disallow 0-length transfers */
    conf->master_pf          = 1; /* This is the master PF */
    conf->intr_moderation    = 1;
    conf->vf_max             = 8;
    conf->intr_rngsz         = INTR_RING_SZ_4KB; // TODO: tune

    // Ask for as many queue MSI-X vectors as you'd like to dedicate to queues
    conf->msix_qvec_max      = 32; // For Versal
    conf->user_msix_qvec_max = 1;
    conf->data_msix_qvec_max = 5;

    conf->qdma_drv_mode      = POLL_MODE; // TODO: experiment with this
    conf->uld                = 0;

    conf->bar_num_config     = 0;
    conf->bar_num_user       = -1;
    conf->bar_num_bypass     = -1;
    conf->qsets_base         = -1;

    // Optional callbacks
    conf->fp_user_isr_handler = NULL;
    conf->fp_q_isr_top_dev    = NULL;
    conf->fp_flr_free_resource= NULL;
    conf->debugfs_dev_root    = NULL;
}

/* ─────────────────────────────────────────────────────────────────────
 * Character-device file operations (management interface)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_fop_ioctl() - Dispatch ioctls on the QDMA control device.
 * @file: Open file for the character device.
 * @op:   Ioctl command number.
 * @arg:  User-space argument pointer.
 *
 * Routes incoming ioctls to the appropriate handler:
 *   - SLASH_QDMA_IOCTL_INFO:       query QDMA capabilities
 *   - SLASH_QDMA_IOCTL_QPAIR_ADD:  allocate a new queue pair
 *   - SLASH_QDMA_IOCTL_Q_OP:       start/stop/delete a queue pair
 *   - SLASH_QDMA_IOCTL_QPAIR_GET_FD: obtain an I/O fd for a queue pair
 *
 * All paths check @hw_shutdown before proceeding to reject ioctls
 * that arrive during or after device teardown.
 *
 * Return: 0 or positive fd on success, negative errno on failure.
 */
static long slash_qdma_fop_ioctl(struct file *file, unsigned int op, unsigned long arg)
{
    struct slash_qdma_dev *qdma_dev = file->private_data;
    void __user *uarg = (void __user *)arg;
    long ret = 0;

    if (!qdma_dev)
        return -ENODEV;

    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev, "ioctl op=0x%x\n", op);

    /* Early rejection if the device is shutting down. */
    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    mutex_unlock(&qdma_dev->lock);

    switch (op) {
    case SLASH_QDMA_IOCTL_INFO:
        ret = slash_qdma_ioctl_info_w(qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_QPAIR_ADD:
        ret = slash_qdma_ioctl_qpair_add_w(qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_Q_OP:
        ret = slash_qdma_ioctl_qpair_op_w(qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_QPAIR_GET_FD:
        ret = slash_qdma_ioctl_qpair_get_fd_w(qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_BUF_CREATE:
        ret = slash_qdma_ioctl_buf_create_w(qdma_dev, uarg);
        break;

    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

/**
 * slash_qdma_fop_open() - Open handler for the QDMA control device.
 * @inode: Inode of the device node.
 * @file:  File being opened.
 *
 * Uses inode->i_cdev to recover the slash_qdma_dev, takes a device reference,
 * and stores it in file->private_data for ioctl/release.
 *
 * Return: 0 on success, -ENODEV if the device is shutting down.
 */
static int slash_qdma_fop_open(struct inode *inode, struct file *file)
{
    struct slash_qdma_dev *qdma_dev =
        container_of(inode->i_cdev, struct slash_qdma_dev, cdev);

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    kref_get(&qdma_dev->ref);
    mutex_unlock(&qdma_dev->lock);

    file->private_data = qdma_dev;

    return 0;
}

/**
 * slash_qdma_fop_release() - Release handler for the QDMA control device.
 * @inode: Inode of the device node.
 * @file:  File being closed.
 *
 * Drops the device reference acquired in open.  If this is the last
 * reference, the device structure is freed.
 *
 * Return: Always 0.
 */
static int slash_qdma_fop_release(struct inode *inode, struct file *file)
{
    struct slash_qdma_dev *qdma_dev = file->private_data;

    if (!qdma_dev)
        return 0;

    kref_put(&qdma_dev->ref, slash_qdma_dev_release);

    file->private_data = NULL;

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: info
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_info_w() - Wrapper for the QDMA info ioctl.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_info struct.
 *
 * Implements the versioned copy-in / copy-out pattern:
 *   1. Read the leading @size field to learn how large the caller's
 *      struct is (ABI forward/backward compatibility).
 *   2. Fill the kernel-side struct via slash_qdma_ioctl_info().
 *   3. Copy back only min(user_size, kernel_size) bytes.
 *
 * Return: 0 on success, -EFAULT on copy failure, -ENODEV if shutting down.
 */
static int slash_qdma_ioctl_info_w(struct slash_qdma_dev *qdma_dev,
                                    void __user *uarg)
{
    struct slash_qdma_info info;
    u32 user_size = 0;
    size_t copy_size;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_INFO_MIN_SIZE) {
        dev_warn(&qdma_dev->pdev->dev,
                 "qdma: INFO size too small (%u)\n", user_size);
        return -EINVAL;
    }

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    slash_qdma_ioctl_info(qdma_dev, &info);
    mutex_unlock(&qdma_dev->lock);

    copy_size = min_t(size_t, user_size, sizeof(info));
    if (copy_to_user(uarg, &info, copy_size))
        return -EFAULT;
    if (user_size > sizeof(info)) {
        if (clear_user((void __user *)((unsigned long)uarg + sizeof(info)),
                       user_size - sizeof(info)))
            return -EFAULT;
    }

    return 0;
}

/**
 * slash_qdma_ioctl_info() - Populate QDMA capability information.
 * @qdma_dev:  QDMA device whose PF1 identity is reported.
 * @qdma_info: [out] Structure to fill with capability data.
 *
 * The capability fields are placeholders (zeroed) pending future reporting
 * (e.g., querying qdma_device_capabilities).  @bdf is filled with the QDMA
 * PF's PCI BDF string so userspace can discover the device's PCI identity
 * without reading sysfs.
 */
static void slash_qdma_ioctl_info(struct slash_qdma_dev *qdma_dev,
                                  struct slash_qdma_info *qdma_info)
{
    qdma_info->qsets_max = 0;
    qdma_info->msix_qvecs = 0;
    qdma_info->vf_max = 0;
    qdma_info->caps = 0;
    strscpy(qdma_info->bdf, pci_name(qdma_dev->pdev), sizeof(qdma_info->bdf));
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: qpair add
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_qpair_add_w() - Wrapper for the qpair-add ioctl.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_add struct.
 *
 * Validates userspace inputs:
 *   - @dir_mask must contain only valid direction bits and be non-zero.
 *   - @mode must be MM or ST.
 *   - Ring size indices must be in [0, 15] (CSR table range).
 *   - @aperture_size must be zero (linear addressing) or a power-of-two
 *     libqdma keyhole aperture.
 *
 * On success, the kernel-assigned @qid is written back to userspace.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_add_w(struct slash_qdma_dev *qdma_dev,
                                         void __user *uarg)
{
    struct slash_qdma_qpair_add req;
    __u32 user_size = 0;
    size_t copy_size;
    u32 dir_mask;
    int err;

    /*
     * First, fetch the size field from userspace so we can
     * safely handle callers built against older or newer
     * versions of the struct.
     */
    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_QPAIR_ADD_MIN_SIZE) {
        dev_warn(&qdma_dev->pdev->dev,
                 "qdma: QPAIR_ADD size too small (%u)\n", user_size);
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));

    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    /* Validate direction mask: must be non-zero and contain only known bits. */
    dir_mask = req.dir_mask & SLASH_QDMA_DIR_MASK;
    if (!dir_mask || dir_mask != req.dir_mask)
        return -EINVAL;

    /* Only memory-mapped and streaming modes are supported. */
    if (req.mode != QDMA_Q_MODE_MM && req.mode != QDMA_Q_MODE_ST)
        return -EINVAL;

    /*
     * Ring size fields are CSR table indices (0-15), not raw descriptor
     * counts.  Each index selects a pre-programmed ring depth from the
     * global CSR ring-size table.
     */
    if (req.h2c_ring_sz >= 16 || req.c2h_ring_sz >= 16 || req.cmpt_ring_sz >= 16)
        return -EINVAL;

    /* Validate the per-queue AXI-MM channel selection. */
    if (req.mm_channel != SLASH_QDMA_MM_CHANNEL_AUTO &&
        req.mm_channel != SLASH_QDMA_MM_CHANNEL_0 &&
        req.mm_channel != SLASH_QDMA_MM_CHANNEL_1)
        return -EINVAL;

    if (req.aperture_size != 0 &&
        (req.aperture_size & (req.aperture_size - 1)) != 0)
        return -EINVAL;

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    err = slash_qdma_ioctl_qpair_add(qdma_dev, &req);
    mutex_unlock(&qdma_dev->lock);

    if (err)
        return err;

    /*
     * On success, update the size field to reflect the
     * kernel's view of the struct and copy back only as
     * many bytes as the caller originally provided.
     */
    req.size = sizeof(req);
    copy_size = min_t(size_t, user_size, sizeof(req));
    if (copy_to_user(uarg, &req, copy_size))
        return -EFAULT;
    if (user_size > sizeof(req)) {
        if (clear_user((void __user *)((unsigned long)uarg + sizeof(req)),
                       user_size - sizeof(req)))
            return -EFAULT;
    }

    return err;
}

/**
 * slash_qdma_ioctl_qpair_add() - Allocate a qpair and add its constituent queues.
 * @qdma_dev: QDMA device.
 * @req:      Add request (dir_mask, mode, ring sizes); @qid is set on success.
 *
 * Allocates a slash_qdma_qpair_entry, inserts it into the xarray (which
 * auto-assigns the qpair ID), and then iterates over the requested
 * directions to add each individual HW queue.  If any queue addition
 * fails, all previously-added queues are rolled back and the xarray
 * entry is removed.
 *
 * The xarray-assigned ID is used as the QDMA queue index for all queues
 * in the pair, so H2C queue N and C2H queue N share the same index.
 * Any qid value provided by userspace in the request is ignored.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_add(struct slash_qdma_dev *qdma_dev,
                                      struct slash_qdma_qpair_add *req)
{
    struct slash_qdma_qpair_entry *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    unsigned int idx;
    bool added[SLASH_QDMA_QTYPE_COUNT] = {0};
    int ret = 0;

    if (!entry)
        return -ENOMEM;

    entry->mode = req->mode;
    entry->irq_mode = 0;
    entry->irq_vector = 0;

    /* Initialise all queue handles to invalid (not yet added). */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++)
        entry->qhndl[idx] = QDMA_QUEUE_IDX_INVALID;

    /*
     * Allocate a new qpair ID in the xarray and use it as the
     * QDMA queue index for all queues in this pair. Any qid
     * value provided by userspace is ignored.
     */
    ret = slash_qdma_qpair_insert(qdma_dev, entry, &req->qid);
    if (ret) {
        dev_err(&qdma_dev->pdev->dev,
                "qdma: qpair insert failed: %d\n", ret);
        kfree(entry);
        return ret;
    }

    /* Add each requested direction's HW queue. */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
        enum queue_type_t qtype = idx;
        u32 dir_bit = slash_qdma_qtype_to_dir(qtype);

        if (!(req->dir_mask & dir_bit))
            continue;

        ret = slash_qdma_ioctl_qpair_add_q(qdma_dev, req, entry, qtype);
        if (ret)
            goto rollback;

        added[idx] = true;
    }

    return 0;

rollback:
    /* Undo any queues that were successfully added before the failure. */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
        if (added[idx])
            slash_qdma_ioctl_qpair_rm_q(qdma_dev, entry, idx);
    }

    slash_qdma_qpair_remove(qdma_dev, req->qid);

    return ret;
}

/**
 * slash_qdma_ioctl_qpair_add_q() - Add a single HW queue to a queue pair.
 * @qdma_dev: QDMA device.
 * @req:      The add request (provides queue index, mode, and ring sizes).
 * @entry:    The qpair entry to attach the new queue to.
 * @qtype:    Which queue type to add (Q_H2C, Q_C2H, or Q_CMPT).
 *
 * Fills a qdma_queue_conf structure and calls qdma_queue_add().  The
 * configuration fields deserve detailed explanation:
 *
 *   - qconf.qidx: set to the xarray-assigned qpair ID so all queues
 *     in a pair share the same HW queue index.
 *   - qconf.st: 1 for streaming mode (QDMA_Q_MODE_ST), 0 for memory-
 *     mapped (QDMA_Q_MODE_MM).  Streaming uses AXI-Stream for data
 *     transfer; MM uses AXI Memory Mapped.
 *   - qconf.irq_en = 0: interrupts disabled — we use poll mode.
 *   - qconf.cmpl_en_intr = 0: no completion interrupts — poll mode.
 *   - qconf.cmpl_trig_mode = TRIG_MODE_DISABLE: no automatic completion
 *     trigger; the host explicitly polls for completion status.
 *   - qconf.wb_status_en = 1: enables HW write-back of completion status
 *     to host memory, which is how the poll-mode driver detects transfer
 *     completion.
 *   - qconf.cmpl_status_acc_en = 1: accumulate completion status entries
 *     (required for poll-mode operation per the reference driver).
 *   - qconf.cmpl_status_pend_chk = 1: check for pending completions
 *     (required for poll-mode operation per the reference driver).
 *   - qconf.cmpl_stat_en = 1: enable completion status generation
 *     (required for poll-mode operation per the reference driver).
 *   - qconf.aperture_size: zero disables libqdma keyhole mode so MM
 *     transfers advance linearly through endpoint memory.  Non-zero values
 *     enable keyhole mode and wrap addresses within that byte aperture.
 *   - qconf.desc_rng_sz_idx: CSR table index (0-15) selecting the
 *     descriptor ring depth.  Not a raw descriptor count — the actual
 *     count is looked up from the global CSR ring-size table.
 *   - qconf.cmpl_rng_sz_idx: same as desc_rng_sz_idx but for the
 *     completion ring (C2H and CMPT queues only).
 *   - qconf.cmpl_desc_sz = CMPT_DESC_SZ_16B: 16-byte completion
 *     descriptors (C2H and CMPT queues only).
 *
 * For CMPT-type queues, streaming mode is forced off (qconf.st = 0)
 * because the completion queue is always memory-mapped regardless of
 * the data queue mode.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_add_q(struct slash_qdma_dev *qdma_dev,
                                         struct slash_qdma_qpair_add *req,
                                         struct slash_qdma_qpair_entry *entry,
                                         enum queue_type_t qtype)
{
    u32 dir_bit = slash_qdma_qtype_to_dir(qtype);
    struct qdma_queue_conf qconf = {0};
    char errbuf[128] = {0};
    u32 dir_mask = req->dir_mask;
    int err;
    unsigned long qhndl = QDMA_QUEUE_IDX_INVALID;

    if (!(dir_mask & dir_bit))
        return -EINVAL;

    /* --- Common queue configuration (all directions) --- */
    qconf.qidx = req->qid;                          /* Use xarray-assigned ID as HW queue index */
    qconf.q_type = qtype;
    qconf.st = (req->mode == QDMA_Q_MODE_ST);       /* Streaming vs memory-mapped */
    qconf.irq_en = 0;                               /* Poll mode: no interrupts */
    qconf.cmpl_en_intr = 0;                         /* Poll mode: no completion interrupts */
    qconf.cmpl_trig_mode = TRIG_MODE_DISABLE;       /* No auto-trigger; we poll explicitly */

    qconf.wb_status_en = 1;                         /* HW writes completion status to host memory */
    qconf.cmpl_status_acc_en = 1;                   /* Accumulate completion status (poll-mode req) */
    qconf.cmpl_status_pend_chk = 1;                 /* Check pending completions (poll-mode req) */
    qconf.cmpl_stat_en = 1;                         /* Enable completion status generation */

    qconf.aperture_size = req->aperture_size;       /* 0 = linear MM; non-zero = keyhole aperture */
    /*
     * CPM5 exposes two MM channels.  The per-queue mm_channel selection
     * (validated in slash_qdma_ioctl_qpair_add_w) chooses the channel: AUTO
     * stripes across channels by (qid & 1); CHANNEL_0/CHANNEL_1 pin to a single
     * channel.  libqdma mirrors mm_channel into the SW-context host_id, so this
     * also selects the programmed Host Profile: channel 0 -> Host Profile 0
     * (NoC Channel 0), channel 1 -> Host Profile 1 (NoC Channel 1).  See
     * slash_qdma_program_host_profiles().
     */
    switch (req->mm_channel) {
    case SLASH_QDMA_MM_CHANNEL_0:
        qconf.mm_channel = 0;
        break;
    case SLASH_QDMA_MM_CHANNEL_1:
        qconf.mm_channel = 1;
        break;
    case SLASH_QDMA_MM_CHANNEL_AUTO:
    default:
        qconf.mm_channel = req->qid & 1;
        break;
    }

    /* --- Per-direction ring configuration --- */
    switch (qtype) {
    case Q_H2C:
        qconf.desc_rng_sz_idx = req->h2c_ring_sz;   /* CSR table index for H2C descriptor ring */
        break;
    case Q_C2H:
        qconf.desc_rng_sz_idx = req->c2h_ring_sz;   /* CSR table index for C2H descriptor ring */
        qconf.cmpl_rng_sz_idx = req->cmpt_ring_sz;  /* CSR table index for C2H completion ring */
        qconf.cmpl_desc_sz = CMPT_DESC_SZ_16B;      /* 16-byte completion descriptors */
        break;
    case Q_CMPT:
        qconf.st = 0;                               /* CMPT queue is always memory-mapped */
        qconf.desc_rng_sz_idx = req->cmpt_ring_sz;
        qconf.cmpl_rng_sz_idx = req->cmpt_ring_sz;
        qconf.cmpl_desc_sz = CMPT_DESC_SZ_16B;      /* 16-byte completion descriptors */
        qconf.cmpl_en_intr = 0;                      /* Redundant but explicit: no CMPT interrupts */
        break;
    default:
        break;
    }

    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "queue add qid=%u type=%u mode=%u mm_channel=%u (req=%u) aperture_size=%u\n",
                          req->qid, qtype, req->mode, qconf.mm_channel,
                          req->mm_channel, qconf.aperture_size);
    err = qdma_queue_add(qdma_dev->qdma_handle, &qconf, &qhndl,
                            errbuf, sizeof(errbuf));
    if (err) {
        SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                              "qdma_queue_add failed: qid=%u type=%u err=%d (%s)\n",
                              req->qid, qtype, err, errbuf);
        dev_err(&qdma_dev->pdev->dev,
                "qdma: queue add failed (qid=%u, type=%u): %d (%s)\n",
                req->qid, qtype, err, errbuf);
        return err;
    }
    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "qdma_queue_add done: qid=%u type=%u qhndl=%lu\n",
                          req->qid, qtype, qhndl);

    /*
     * Reconfigure the queue immediately after adding it.
     *
     * qdma_queue_add() runs qdma_descq_config(..., reconfig=0), which on
     * Versal hard IP does NOT mirror qconf.mm_channel into descq->channel --
     * only the reconfig=1 branch does.  descq->channel feeds the SW-context
     * mm_chn/host_id programmed when the queue is started; without this step
     * it would stay 0 and collapse both queues onto NoC channel 0, defeating
     * mm-channel selection.  Calling qdma_queue_config() here (the queue is in
     * Q_STATE_ENABLED, before start) replays the same qconf through the
     * reconfig=1 path, setting descq->channel.  This replaces the former
     * 0002-libqdma-versal-channel.patch without modifying libqdma.
     */
    err = qdma_queue_config(qdma_dev->qdma_handle, qhndl, &qconf,
                            errbuf, sizeof(errbuf));
    if (err) {
        SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                              "qdma_queue_config failed: qid=%u type=%u err=%d (%s)\n",
                              req->qid, qtype, err, errbuf);
        dev_err(&qdma_dev->pdev->dev,
                "qdma: queue config failed (qid=%u, type=%u): %d (%s)\n",
                req->qid, qtype, err, errbuf);
        /*
         * The queue was added but is not yet tracked in @entry, so the
         * caller's rollback (keyed on its local added[] array) will not
         * reach it.  Remove it here to avoid leaking the libqdma queue.
         */
        slash_qdma_queue_remove_safe(qdma_dev->qdma_handle, qhndl,
                                     errbuf, sizeof(errbuf));
        return err;
    }

    /* Record the handle and mark this direction as active. */
    entry->qhndl[qtype] = qhndl;
    entry->dir_mask |= dir_bit;

    return 0;
}

/**
 * slash_qdma_ioctl_qpair_rm_q() - Remove a single HW queue from a queue pair.
 * @qdma_dev: QDMA device.
 * @entry:    The qpair entry to remove the queue from.
 * @qtype:    Which queue type to remove (Q_H2C, Q_C2H, or Q_CMPT).
 *
 * Uses slash_qdma_queue_remove_safe() to handle all possible HW queue
 * states (online, enabled, or already disabled).  On completion, the
 * queue handle is set to QDMA_QUEUE_IDX_INVALID and the direction bit
 * is cleared from the entry's dir_mask.
 *
 * Errors are logged but not propagated — this is best-effort cleanup
 * used during teardown.
 */
static void slash_qdma_ioctl_qpair_rm_q(struct slash_qdma_dev *qdma_dev,
                                         struct slash_qdma_qpair_entry *entry,
                                         enum queue_type_t qtype)
{
    unsigned long qhndl = entry->qhndl[qtype];
    char errbuf[128] = {0};
    int err;

    /* If the handle is already invalid, just clear state and return. */
    if (!slash_qdma_qhndl_is_valid(qhndl)) {
        entry->qhndl[qtype] = QDMA_QUEUE_IDX_INVALID;
        entry->dir_mask &= ~slash_qdma_qtype_to_dir(qtype);
        return;
    }

    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "queue_remove_safe start: type=%u qhndl=%lu\n",
                          qtype, qhndl);
    err = slash_qdma_queue_remove_safe(qdma_dev->qdma_handle, qhndl,
                                       errbuf, sizeof(errbuf));

    if (err) {
        SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                              "queue_remove_safe failed: type=%u qhndl=%lu err=%d (%s)\n",
                              qtype, qhndl, err, errbuf);
        dev_err(&qdma_dev->pdev->dev,
                "qdma: queue remove failed (type=%u): %d (%s)\n",
                qtype, err, errbuf);
        return;
    }
    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "queue_remove_safe done: type=%u qhndl=%lu\n",
                          qtype, qhndl);

    entry->qhndl[qtype] = QDMA_QUEUE_IDX_INVALID;
    entry->dir_mask &= ~slash_qdma_qtype_to_dir(qtype);
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: qpair op (start / stop / delete)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_qpair_op_w() - Wrapper for the qpair operation ioctl.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_op struct.
 *
 * Handles the versioned copy-in / copy-out pattern and validates that
 * the requested operation is within the known range.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_op_w(struct slash_qdma_dev *qdma_dev,
                                       void __user *uarg)
{
    struct slash_qdma_qpair_op req;
    __u32 user_size = 0;
    size_t copy_size;
    int ret;

    /*
     * First, fetch the size field from userspace so we can
     * safely handle callers built against older or newer
     * versions of the struct.
     */
    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_QPAIR_OP_MIN_SIZE) {
        dev_warn(&qdma_dev->pdev->dev,
                 "qdma: Q_OP size too small (%u)\n", user_size);
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));

    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    if (req.op > SLASH_QDMA_QUEUE_OP_DEL)
        return -EINVAL;

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    ret = slash_qdma_ioctl_qpair_op(qdma_dev, &req);
    mutex_unlock(&qdma_dev->lock);

    if (ret)
        return ret;

    /*
     * On success, update the size field to reflect the
     * kernel's view of the struct and copy back only as
     * many bytes as the caller originally provided.
     */
    req.size = sizeof(req);
    copy_size = min_t(size_t, user_size, sizeof(req));
    if (copy_to_user(uarg, &req, copy_size))
        return -EFAULT;
    if (user_size > sizeof(req)) {
        if (clear_user((void __user *)((unsigned long)uarg + sizeof(req)),
                       user_size - sizeof(req)))
            return -EFAULT;
    }

    return ret;
}

/**
 * slash_qdma_ioctl_qpair_op() - Dispatch a lifecycle operation on a queue pair.
 * @qdma_dev: QDMA device.
 * @req:      Operation request (@qid identifies the target, @op selects
 *            the action).
 *
 * Looks up the qpair entry and dispatches to slash_qdma_ioctl_qpair_op_apply()
 * with the appropriate libqdma function pointer:
 *
 *   - START: qdma_queue_start (stop_on_err=true — fail fast)
 *   - STOP:  qdma_queue_stop  (stop_on_err=true — fail fast)
 *   - DEL:   slash_qdma_queue_remove_safe (stop_on_err=false — best effort,
 *            try to remove as many queues as possible even if one fails);
 *            on success, also removes the entry from the xarray.
 *
 * Return: 0 on success, -ENOENT if qpair not found, other negative errno
 *         from the underlying libqdma call.
 */
static int slash_qdma_ioctl_qpair_op(struct slash_qdma_dev *qdma_dev,
                                     struct slash_qdma_qpair_op *req)
{
    struct slash_qdma_qpair_entry *entry;
    int ret = 0;

    if (!qdma_dev->have_qdma_handle)
        return -ENODEV;

    entry = slash_qdma_qpair_lookup(qdma_dev, req->qid);
    if (!entry)
        return -ENOENT;

    switch (req->op) {
    case SLASH_QDMA_QUEUE_OP_START:
        ret = slash_qdma_ioctl_qpair_op_apply(qdma_dev, entry, req,
                                          qdma_queue_start,
                                          "start", true);
        break;
    case SLASH_QDMA_QUEUE_OP_STOP:
        ret = slash_qdma_ioctl_qpair_op_apply(qdma_dev, entry, req,
                                          qdma_queue_stop,
                                          "stop", true);
        break;
    case SLASH_QDMA_QUEUE_OP_DEL:
        /*
         * For delete, use stop_on_err=false to attempt removal of all
         * directions even if one fails, then remove from the xarray.
         */
        ret = slash_qdma_ioctl_qpair_op_apply(qdma_dev, entry, req,
                                          slash_qdma_queue_remove_safe,
                                          "remove", false);
        if (!ret)
            slash_qdma_qpair_remove(qdma_dev, req->qid);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

/**
 * slash_qdma_ioctl_qpair_op_apply() - Apply a lifecycle operation to all queues in a pair.
 * @qdma_dev:    QDMA device.
 * @entry:       Queue pair entry.
 * @req:         Operation request (used for @qid in log messages).
 * @fn:          The libqdma function to call per queue (e.g., qdma_queue_start).
 * @op_name:     Human-readable operation name for log messages.
 * @stop_on_err: If true, return immediately on the first error.
 *               If false, continue through all directions and return
 *               the first error encountered.
 *
 * Iterates over all queue types (H2C, C2H, CMPT).  For each direction
 * that is active in the entry's dir_mask, calls @fn with the corresponding
 * queue handle.
 *
 * Return: 0 if all calls succeed, otherwise the first negative errno.
 */
static int slash_qdma_ioctl_qpair_op_apply(struct slash_qdma_dev *qdma_dev,
                                           struct slash_qdma_qpair_entry *entry,
                                           struct slash_qdma_qpair_op *req,
                                           slash_qdma_queue_cmd_fn fn,
                                           const char *op_name,
                                           bool stop_on_err)
{
    int idx;
    int first_err = 0;

    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
        enum queue_type_t qtype = idx;
        u32 dir_bit = slash_qdma_qtype_to_dir(qtype);
        char errbuf[128] = {0};
        int err;

        /* Skip directions not present in this queue pair. */
        if (!(entry->dir_mask & dir_bit) ||
            !slash_qdma_qhndl_is_valid(entry->qhndl[qtype]))
            continue;

        SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                              "qdma_queue_%s start: qid=%u type=%u qhndl=%lu\n",
                              op_name, req->qid, qtype, entry->qhndl[qtype]);
        err = fn(qdma_dev->qdma_handle, entry->qhndl[qtype],
                 errbuf, (int)sizeof(errbuf));
        if (err) {
            SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                                  "qdma_queue_%s failed: qid=%u type=%u qhndl=%lu err=%d (%s)\n",
                                  op_name, req->qid, qtype, entry->qhndl[qtype], err, errbuf);
            dev_err(&qdma_dev->pdev->dev,
                    "qdma: queue %s failed (qid=%u, type=%u): %d (%s)\n",
                    op_name, req->qid, qtype, err, errbuf);

            if (stop_on_err)
                return err;

            if (!first_err)
                first_err = err;
        } else {
            SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                                  "qdma_queue_%s done: qid=%u type=%u qhndl=%lu\n",
                                  op_name, req->qid, qtype, entry->qhndl[qtype]);
        }
    }

    return first_err;
}

/* ─────────────────────────────────────────────────────────────────────
 * Kernel DMA buffers: page allocation, SGL, DMA mapping, mmap
 *
 * A buffer owns a set of individually-allocated 4 KiB base pages (not
 * physically contiguous).  At creation time the pages are allocated, a
 * one-descriptor-per-page SGL is built, and every page is DMA-mapped once;
 * the steady-state transfer path then only slices the SGL, syncs the touched
 * pages for the relevant DMA direction, and submits.  The same pages are also
 * exposed to userspace through the buffer fd's mmap, so the CPU and the DMA
 * engine share one allocation, coherent only at the transfer boundaries.
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_buf_dma_unmap() - Tear down the cached DMA mapping of a buffer.
 * @buf: Buffer whose SGL entries were DMA-mapped.
 *
 * Unmaps every SGL entry that carries a non-zero dma_addr and clears it.
 * Safe to call on a partially-mapped buffer (used on the create error path).
 */
static void slash_qdma_buf_dma_unmap(struct slash_qdma_buf *buf)
{
    struct device *dev = &buf->qdma_dev->pdev->dev;
    unsigned int i;

    if (!buf->sgl || !buf->dma_mapped)
        return;

    for (i = 0; i < buf->pages_nr; i++) {
        struct qdma_sw_sg *sg = &buf->sgl[i];

        if (sg->dma_addr) {
            dma_unmap_page(dev, sg->dma_addr, sg->len, DMA_BIDIRECTIONAL);
            sg->dma_addr = 0UL;
        }
    }

    buf->dma_mapped = false;
}

/**
 * slash_qdma_buf_dma_map() - DMA-map every SGL entry of a buffer.
 * @buf: Buffer with a freshly built SGL.
 *
 * Maps each page with DMA_BIDIRECTIONAL so the same cached mapping serves both
 * H2C and C2H transfers.  On any failure all previously mapped entries are
 * unmapped before returning.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_buf_dma_map(struct slash_qdma_buf *buf)
{
    struct device *dev = &buf->qdma_dev->pdev->dev;
    unsigned int i;

    for (i = 0; i < buf->pages_nr; i++) {
        struct qdma_sw_sg *sg = &buf->sgl[i];

        sg->dma_addr = dma_map_page(dev, sg->pg, sg->offset, sg->len,
                                    DMA_BIDIRECTIONAL);
        if (dma_mapping_error(dev, sg->dma_addr)) {
            sg->dma_addr = 0UL;
            pr_err("slash: qdma: buffer DMA map failed at entry %u/%u\n",
                   i, buf->pages_nr);
            buf->dma_mapped = true; /* allow unmap of the entries done so far */
            slash_qdma_buf_dma_unmap(buf);
            return -ENOMEM;
        }
    }

    buf->dma_mapped = true;
    return 0;
}

/**
 * slash_qdma_buf_free_pages() - Free a buffer's pages and SGL.
 * @buf: Buffer to tear down.
 *
 * Releases each allocated page (put_page() so pages still mapped into a VMA
 * stay alive until the last mapping is torn down) and frees the SGL/page
 * arrays.  The DMA mapping must already have been removed.
 */
static void slash_qdma_buf_free_pages(struct slash_qdma_buf *buf)
{
    unsigned int i;

    if (buf->pages) {
        for (i = 0; i < buf->pages_nr; i++) {
            if (buf->pages[i])
                put_page(buf->pages[i]);
        }
    }

    kvfree(buf->pages);
    buf->pages = NULL;
    kvfree(buf->sgl);
    buf->sgl = NULL;
    buf->pages_nr = 0;
}

/**
 * slash_qdma_buf_alloc() - Allocate pages, build the SGL, and DMA-map.
 * @buf: Buffer with @length and @qdma_dev set; @granule defaults to PAGE_SIZE.
 *
 * Allocates @length / PAGE_SIZE individual base pages (not contiguous), builds
 * a one-page-per-entry SGL, and DMA-maps every page.  All of this is the
 * amortised, do-it-once setup cost paid by SLASH_QDMA_IOCTL_BUF_CREATE.
 *
 * Return: 0 on success, negative errno on failure (partial state cleaned up).
 */
static int slash_qdma_buf_alloc(struct slash_qdma_buf *buf)
{
    size_t entries = buf->length / PAGE_SIZE;
    unsigned int i;
    int rv;

    if (buf->length == 0 || (buf->length % PAGE_SIZE) != 0 ||
        entries == 0 || entries > UINT_MAX)
        return -EINVAL;

    buf->granule = PAGE_SIZE;
    buf->pages_nr = (unsigned int)entries;

    buf->pages = kvcalloc(entries, sizeof(*buf->pages), GFP_KERNEL);
    if (!buf->pages)
        return -ENOMEM;

    buf->sgl = kvcalloc(entries, sizeof(*buf->sgl), GFP_KERNEL);
    if (!buf->sgl) {
        kvfree(buf->pages);
        buf->pages = NULL;
        return -ENOMEM;
    }

    for (i = 0; i < entries; i++) {
        struct page *pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
        struct qdma_sw_sg *sg = &buf->sgl[i];

        if (!pg) {
            rv = -ENOMEM;
            goto err_free;
        }

        buf->pages[i] = pg;
        sg->next = (i + 1 < entries) ? &buf->sgl[i + 1] : NULL;
        sg->pg = pg;
        sg->offset = 0;
        sg->len = PAGE_SIZE;
        sg->dma_addr = 0UL;
    }

    rv = slash_qdma_buf_dma_map(buf);
    if (rv < 0)
        goto err_free;

    return 0;

err_free:
    slash_qdma_buf_free_pages(buf);
    return rv;
}

/**
 * slash_qdma_buf_sync_sgl_for_device() - Hand a transfer slice to the device.
 * @buf:       Buffer being transferred.
 * @sgl:       Per-transfer SGL slice.
 * @n_entries: Number of SGL entries in @sgl.
 * @dir:       DMA direction (DMA_TO_DEVICE for H2C, DMA_FROM_DEVICE for C2H).
 *
 * Synchronises CPU-written data out to the device (and/or invalidates CPU
 * caches) for exactly the SGL spans a sub-transfer touches.  On
 * cache-coherent hosts these are no-ops; on others they bound coherency to
 * the transfer.
 */
static void slash_qdma_buf_sync_sgl_for_device(struct slash_qdma_buf *buf,
                                               const struct qdma_sw_sg *sgl,
                                               u64 n_entries,
                                               enum dma_data_direction dir)
{
    struct device *dev = &buf->qdma_dev->pdev->dev;
    u64 i;

    for (i = 0; i < n_entries; i++) {
        const struct qdma_sw_sg *sg = &sgl[i];

        dma_sync_single_for_device(dev, sg->dma_addr, sg->len, dir);
    }
}

/**
 * slash_qdma_buf_sync_sgl_for_cpu() - Reclaim a transfer slice for the CPU.
 * @buf:       Buffer being transferred.
 * @sgl:       Per-transfer SGL slice.
 * @n_entries: Number of SGL entries in @sgl.
 * @dir:       DMA direction (DMA_FROM_DEVICE for a completed C2H read).
 *
 * Makes device-written data visible to the CPU for exactly the pages a C2H
 * sub-transfer touched.  Called after the transfer completes.
 */
static void slash_qdma_buf_sync_sgl_for_cpu(struct slash_qdma_buf *buf,
                                            const struct qdma_sw_sg *sgl,
                                            u64 n_entries,
                                            enum dma_data_direction dir)
{
    struct device *dev = &buf->qdma_dev->pdev->dev;
    u64 i;

    for (i = 0; i < n_entries; i++) {
        const struct qdma_sw_sg *sg = &sgl[i];

        dma_sync_single_for_cpu(dev, sg->dma_addr, sg->len, dir);
    }
}

/**
 * slash_qdma_buf_release() - kref release callback for a buffer.
 * @ref: kref embedded in the slash_qdma_buf being freed.
 *
 * Runs when the last reference drops (fd ref, every live VMA ref, and any
 * in-flight transfer ref).  Tears down the DMA mapping, frees the pages and
 * SGL, drops the device reference, and frees the struct.
 */
static void slash_qdma_buf_release(struct kref *ref)
{
    struct slash_qdma_buf *buf =
        container_of(ref, struct slash_qdma_buf, ref);
    struct slash_qdma_dev *qdma_dev = buf->qdma_dev;

    slash_qdma_buf_dma_unmap(buf);
    slash_qdma_buf_free_pages(buf);
    if (qdma_dev)
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
    kfree(buf);
}

static inline void slash_qdma_buf_get(struct slash_qdma_buf *buf)
{
    kref_get(&buf->ref);
}

static void slash_qdma_buf_put(struct slash_qdma_buf *buf)
{
    kref_put(&buf->ref, slash_qdma_buf_release);
}

/* ─────────────────────────────────────────────────────────────────────
 * Buffer fd: mmap support and lifetime
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_buf_vm_open() - VMA open callback (fork / VMA split).
 * @vma: The VMA gaining an independent reference.
 *
 * Each live VMA holds one buffer reference so the pages (and DMA mapping)
 * outlive the buffer fd if userspace keeps the mapping after close().
 */
static void slash_qdma_buf_vm_open(struct vm_area_struct *vma)
{
    struct slash_qdma_buf *buf = vma->vm_private_data;

    if (buf)
        slash_qdma_buf_get(buf);
}

/**
 * slash_qdma_buf_vm_close() - VMA close callback (munmap / exit).
 * @vma: The VMA being torn down.
 */
static void slash_qdma_buf_vm_close(struct vm_area_struct *vma)
{
    struct slash_qdma_buf *buf = vma->vm_private_data;

    if (buf)
        slash_qdma_buf_put(buf);
}

static const struct vm_operations_struct slash_qdma_buf_vm_ops = {
    .open  = slash_qdma_buf_vm_open,
    .close = slash_qdma_buf_vm_close,
};

/**
 * slash_qdma_buf_mmap() - mmap a kernel buffer's pages into userspace.
 * @file: The buffer fd.
 * @vma:  The mapping request.
 *
 * Maps the whole buffer (offset 0, full length) into the calling process.
 * The pages are ordinary kernel pages, so vm_map_pages_zero() inserts them
 * directly; each VMA takes a buffer reference (initial one here, duplicated by
 * the .open callback) so the pages stay valid for the life of the mapping.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_buf_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct slash_qdma_buf *buf = file->private_data;
    unsigned long span = vma->vm_end - vma->vm_start;
    int rv;

    if (!buf)
        return -ENODEV;

    /* Only a full, offset-0 mapping of the buffer is supported. */
    if (vma->vm_pgoff != 0)
        return -EINVAL;
    if (span != (unsigned long)buf->length)
        return -EINVAL;

    /*
     * Normal page mapping (no VM_PFNMAP): keep it from being expanded beyond
     * the buffer and excluded from core dumps.
     */
    slash_vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);

    rv = vm_map_pages_zero(vma, buf->pages, buf->pages_nr);
    if (rv)
        return rv;

    vma->vm_ops = &slash_qdma_buf_vm_ops;
    vma->vm_private_data = buf;
    slash_qdma_buf_get(buf); /* dropped by vm_close when this VMA goes away */

    return 0;
}

/**
 * slash_qdma_buf_fop_release() - Release callback for a buffer fd.
 * @inode: Unused (anon inode).
 * @file:  The buffer fd being closed.
 *
 * Drops the fd's buffer reference.  Pages survive until any remaining VMA
 * references are dropped too.
 *
 * Return: Always 0.
 */
static int slash_qdma_buf_fop_release(struct inode *inode, struct file *file)
{
    struct slash_qdma_buf *buf = file->private_data;

    (void)inode;

    if (buf) {
        slash_qdma_buf_put(buf);
        file->private_data = NULL;
    }

    return 0;
}

/**
 * slash_qdma_buf_fops - File operations for buffer fds.
 *
 * mmap     maps the buffer's pages for CPU access.
 * release  drops the fd's reference on the buffer.
 */
static const struct file_operations slash_qdma_buf_fops = {
    .owner   = THIS_MODULE,
    .mmap    = slash_qdma_buf_mmap,
    .release = slash_qdma_buf_fop_release,
};

/**
 * slash_qdma_buf_from_file() - Resolve a buffer fd to its buffer object.
 * @file: A file obtained from fget() on a candidate buffer fd.
 *
 * Return: The buffer if @file is a SLASH buffer fd, else NULL.
 */
static struct slash_qdma_buf *slash_qdma_buf_from_file(struct file *file)
{
    if (!file || file->f_op != &slash_qdma_buf_fops)
        return NULL;
    return file->private_data;
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: buffer create
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_buf_create_w() - Allocate a kernel buffer and return its fd.
 * @qdma_dev: QDMA device the buffer is bound to (for DMA mapping).
 * @uarg:     User pointer to a struct slash_qdma_buf_create.
 *
 * Allocates the buffer's pages, builds the SGL, and DMA-maps everything once,
 * then wraps it in an anon_inode fd whose mmap exposes the pages for CPU
 * access.  The fd is returned as the ioctl return value (same convention as
 * the BAR/queue-pair fd ioctls).  Closing the fd (and unmapping any VMA)
 * releases the buffer.
 *
 * Return: The new buffer fd (>= 0) on success, negative errno on failure.
 */
static int slash_qdma_ioctl_buf_create_w(struct slash_qdma_dev *qdma_dev,
                                         void __user *uarg)
{
    struct slash_qdma_buf_create req;
    struct slash_qdma_buf *buf;
    struct file *file;
    __u32 user_size = 0;
    size_t copy_size;
    int fd;
    int rv;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_BUF_CREATE_MIN_SIZE) {
        dev_warn(&qdma_dev->pdev->dev,
                 "qdma: BUF_CREATE size too small (%u)\n", user_size);
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));
    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    if (req.flags & ~O_CLOEXEC)
        return -EINVAL;

    if (req.length == 0 || (req.length % PAGE_SIZE) != 0)
        return -EINVAL;

    buf = kzalloc(sizeof(*buf), GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    kref_init(&buf->ref);
    /* The buffer holds a device reference for its whole lifetime. */
    kref_get(&qdma_dev->ref);
    buf->qdma_dev = qdma_dev;
    buf->length = req.length;

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
        kfree(buf);
        return -ENODEV;
    }
    rv = slash_qdma_buf_alloc(buf);
    mutex_unlock(&qdma_dev->lock);
    if (rv < 0) {
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
        kfree(buf);
        return rv;
    }

    file = anon_inode_getfile("slash_qdma_buf", &slash_qdma_buf_fops, buf,
                              O_RDWR | (req.flags & O_CLOEXEC));
    if (IS_ERR(file)) {
        rv = PTR_ERR(file);
        slash_qdma_buf_put(buf); /* drops the only ref: frees buf + dev ref */
        return rv;
    }

    fd = get_unused_fd_flags(req.flags & O_CLOEXEC);
    if (fd < 0) {
        fput(file); /* triggers buf release */
        return fd;
    }

    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "buf create: fd=%d len=%llu granule=%u pages=%u\n",
                          fd, (unsigned long long)req.length,
                          buf->granule, buf->pages_nr);

    /* Fill the output fields before installing the fd. */
    req.size = sizeof(req);
    req.granule = buf->granule;
    req.transfer_hint = SLASH_QDMA_TRANSFER_HINT_V80;
    copy_size = min_t(size_t, user_size, sizeof(req));
    if (copy_to_user(uarg, &req, copy_size)) {
        put_unused_fd(fd);
        fput(file);
        return -EFAULT;
    }
    if (user_size > sizeof(req)) {
        if (clear_user((void __user *)((unsigned long)uarg + sizeof(req)),
                       user_size - sizeof(req))) {
            put_unused_fd(fd);
            fput(file);
            return -EFAULT;
        }
    }

    fd_install(fd, file);

    return fd;
}

/**
 * struct slash_qdma_xfer_req - Runtime state for one sub-transfer submission.
 * @qreq:           libqdma request (built by slash_qdma_xfer_prep()).
 * @done:           Completion signalled by @qreq.fp_done for async submissions.
 * @buf:            Kernel buffer the transfer references (one ref held).
 * @qhndl:          Resolved libqdma queue handle for the direction/qpair.
 * @start_entry:    First page index of the buffer slice being transferred.
 * @n_entries:      Number of pages in the slice (for the DMA sync).
 * @xfer_sgl:       Optional per-transfer SGL copy with a clipped final entry.
 * @dma_dir:        DMA direction for the streaming sync calls.
 * @is_c2h:         True for a C2H (device-to-host) sub-transfer, so the slice
 *                  is synced back for the CPU after completion.
 * @bytes_done:     Bytes transferred, filled on completion.
 * @err:            Negative errno if the sub-transfer failed, else 0.
 * @async_inflight: True once queued asynchronously and awaiting fp_done.
 *
 * Allocated as an array (one per sub-transfer) for the duration of a transfer
 * batch.  @qreq must outlive the in-flight async request, so the array stays
 * alive until every async completion has fired.
 */
struct slash_qdma_xfer_req {
    struct qdma_request qreq;
    struct completion done;
    struct slash_qdma_buf *buf;
    unsigned long qhndl;
    u64 start_entry;
    u64 n_entries;
    struct qdma_sw_sg *xfer_sgl;
    enum dma_data_direction dma_dir;
    bool is_c2h;
    unsigned int bytes_done;
    int err;
    bool async_inflight;
};

/**
 * slash_qdma_xfer_done() - libqdma fp_done callback for async sub-transfers.
 * @qreq:       The completed request (embedded in a slash_qdma_xfer_req).
 * @bytes_done: Bytes transferred.
 * @err:        Negative errno on failure, else 0.
 *
 * Records the result and wakes the submitter waiting on @done.  Runs in
 * libqdma worker-thread context.
 *
 * Return: Always 0 (libqdma may free/re-task the request).
 */
static int slash_qdma_xfer_done(struct qdma_request *qreq,
                                unsigned int bytes_done, int err)
{
    struct slash_qdma_xfer_req *xr =
        container_of(qreq, struct slash_qdma_xfer_req, qreq);

    xr->bytes_done = bytes_done;
    xr->err = err;
    complete(&xr->done);
    return 0;
}

/**
 * slash_qdma_xfer_prep() - Validate one sub-transfer and build its request.
 * @qdma_dev: QDMA device.
 * @entry:    Queue pair entry selected by the sub-transfer's qpair_index.
 * @desc:     User-supplied sub-transfer descriptor.
 * @xr:       [out] Receives a built (but not yet submitted) request, and a
 *            reference on the kernel buffer it targets.
 *
 * Shared submit core used by both the synchronous transfer ioctl and the
 * optional io_uring uring_cmd path.  Resolves the buffer fd named by the
 * descriptor and refs the buffer, validates the slice against the buffer's
 * page granule and length, resolves the queue handle for the requested
 * direction, builds the per-transfer SGL slice, syncs the bytes touched by
 * that slice for the device, and fills @xr->qreq (dma_mapped = 1,
 * fp_done = NULL).
 * No pages are allocated or DMA-mapped here; that was amortised at creation.
 *
 * On success the caller owns the buffer ref in @xr->buf and must release it
 * with slash_qdma_buf_put() once the request is no longer in flight.
 *
 * Return: 0 on success, negative errno on failure (no ref held on failure).
 */
static int slash_qdma_xfer_prep(struct slash_qdma_dev *qdma_dev,
                                struct slash_qdma_qpair_entry *entry,
                                const struct slash_qdma_subxfer *desc,
                                struct slash_qdma_xfer_req *xr)
{
    struct slash_qdma_buf *buf;
    struct qdma_sw_sg *sgl;
    struct file *file;
    unsigned long qhndl;
    bool write;
    u32 dir_bit;
    enum queue_type_t qtype;
    enum dma_data_direction dma_dir;
    u64 start_entry, n_entries;

    switch (desc->direction) {
    case SLASH_QDMA_XFER_H2C:
        write = true;
        dir_bit = SLASH_QDMA_DIR_H2C;
        qtype = Q_H2C;
        dma_dir = DMA_TO_DEVICE;
        break;
    case SLASH_QDMA_XFER_C2H:
        write = false;
        dir_bit = SLASH_QDMA_DIR_C2H;
        qtype = Q_C2H;
        dma_dir = DMA_FROM_DEVICE;
        break;
    default:
        return -EINVAL;
    }

    /* libqdma's request count is a 32-bit byte count. */
    if (desc->length == 0 || desc->length > UINT_MAX)
        return -EINVAL;

    /* Resolve the buffer fd and take a ref that outlives the fd. */
    file = fget(desc->buf_fd);
    if (!file)
        return -EBADF;
    buf = slash_qdma_buf_from_file(file);
    if (!buf) {
        fput(file);
        return -EINVAL;
    }
    /* DMA mappings are device-specific: the buffer must belong to this device. */
    if (buf->qdma_dev != qdma_dev) {
        fput(file);
        return -EINVAL;
    }
    slash_qdma_buf_get(buf);
    fput(file);

    /* The buffer offset must be page-aligned because SGL entries are page
     * based.  The requested byte count may end within the final page; libqdma
     * uses qreq.count for the exact byte count while sgcnt covers the touched
     * pages. */
    if (buf->granule == 0 ||
        (desc->buf_offset % buf->granule) != 0) {
        slash_qdma_buf_put(buf);
        return -EINVAL;
    }
    if (desc->buf_offset > buf->length ||
        desc->length > buf->length - desc->buf_offset) {
        slash_qdma_buf_put(buf);
        return -EINVAL;
    }

    start_entry = desc->buf_offset / buf->granule;
    n_entries = (desc->length + buf->granule - 1) / buf->granule;
    if (start_entry + n_entries > buf->pages_nr) {
        slash_qdma_buf_put(buf);
        return -EINVAL;
    }

    /* Check device liveness and resolve the queue handle for the direction. */
    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        slash_qdma_buf_put(buf);
        return -ENODEV;
    }
    if (!(entry->dir_mask & dir_bit) ||
        !slash_qdma_qhndl_is_valid(entry->qhndl[qtype])) {
        mutex_unlock(&qdma_dev->lock);
        slash_qdma_buf_put(buf);
        return -ENODEV;
    }
    qhndl = entry->qhndl[qtype];
    mutex_unlock(&qdma_dev->lock);

    /*
     * Full-page transfers can point directly into the cached buffer SGL.
     * Exact-length transfers with a partial final page need a per-transfer
     * copy so libqdma sees the shortened tail descriptor without mutating the
     * reusable buffer SGL.
     */
    if ((desc->length % buf->granule) == 0) {
        sgl = &buf->sgl[start_entry];
    } else {
        u64 i;
        u64 tail = desc->length % buf->granule;

        xr->xfer_sgl = kvmalloc_array(n_entries, sizeof(*xr->xfer_sgl),
                                      GFP_KERNEL);
        if (!xr->xfer_sgl) {
            slash_qdma_buf_put(buf);
            return -ENOMEM;
        }

        for (i = 0; i < n_entries; i++) {
            xr->xfer_sgl[i] = buf->sgl[start_entry + i];
            xr->xfer_sgl[i].next =
                (i + 1 < n_entries) ? &xr->xfer_sgl[i + 1] : NULL;
        }
        xr->xfer_sgl[n_entries - 1].len = tail;
        sgl = xr->xfer_sgl;
    }

    /*
     * Build the request from a cached or clipped SGL slice.  dma_mapped = 1
     * tells libqdma the SGL is already DMA-mapped (dma_addr filled at buffer
     * creation), so it skips the per-request map/unmap entirely.
     */
    memset(&xr->qreq, 0, sizeof(xr->qreq));
    xr->qreq.sgcnt = (unsigned int)n_entries;
    xr->qreq.sgl = sgl;
    xr->qreq.write = write ? 1 : 0;
    xr->qreq.dma_mapped = 1;
    xr->qreq.udd_len = 0;
    xr->qreq.ep_addr = (u64)desc->dev_addr;
    xr->qreq.count = (unsigned int)desc->length;
    xr->qreq.timeout_ms = 10 * 1000;
    xr->qreq.fp_done = NULL;
    xr->qreq.h2c_eot = 1;

    xr->buf = buf;
    xr->qhndl = qhndl;
    xr->start_entry = start_entry;
    xr->n_entries = n_entries;
    xr->dma_dir = dma_dir;
    xr->is_c2h = !write;
    xr->bytes_done = 0;
    xr->err = 0;
    xr->async_inflight = false;

    /*
     * Hand the touched bytes to the device.  The mapping is persistent
     * (dma_mapped = 1); only this transfer's SGL spans are synced, so
     * coherency cost scales with the transfer, not the whole buffer.
     */
    slash_qdma_buf_sync_sgl_for_device(buf, xr->qreq.sgl, n_entries, dma_dir);
    return 0;
}

/**
 * slash_qdma_xfer_cleanup() - Drop resources held by a prepared sub-transfer.
 * @xr: Prepared transfer request.
 *
 * Releases the optional clipped SGL and the buffer reference taken in prep.
 * It is safe for prepared-but-unsubmitted error paths.
 */
static void slash_qdma_xfer_cleanup(struct slash_qdma_xfer_req *xr)
{
    kvfree(xr->xfer_sgl);
    xr->xfer_sgl = NULL;
    if (xr->buf)
        slash_qdma_buf_put(xr->buf);
    xr->buf = NULL;
}

/**
 * slash_qdma_xfer_finish() - Post-completion DMA sync + buffer ref drop.
 * @xr: A prepared (and now completed) sub-transfer request.
 *
 * For a C2H sub-transfer that moved data, makes the device-written pages
 * visible to the CPU before releasing the buffer reference taken in prep.
 */
static void slash_qdma_xfer_finish(struct slash_qdma_xfer_req *xr)
{
    if (xr->is_c2h && xr->bytes_done)
        slash_qdma_buf_sync_sgl_for_cpu(xr->buf, xr->qreq.sgl,
                                        xr->qreq.sgcnt, xr->dma_dir);
    slash_qdma_xfer_cleanup(xr);
}

/**
 * slash_qdma_qpair_transfer() - Buffer DMA transfer batch on a queue-pair fd.
 * @file: Anon_inode file for the queue-pair collection.
 * @uarg: User pointer to a struct slash_qdma_transfer (1..N sub-transfers).
 *
 * Validates and prepares every sub-transfer, then submits them so those that
 * target distinct queue pairs run concurrently: all but the last are submitted
 * asynchronously (fp_done set), the last is submitted synchronously (blocking),
 * and the async ones are then waited on.  A single sub-transfer therefore takes
 * the plain blocking path with no async overhead.
 *
 * Return: total number of bytes transferred (>= 0) on success; the first
 *         sub-transfer error (negative errno) on failure.
 */
static long slash_qdma_qpair_transfer(struct file *file, void __user *uarg)
{
    struct slash_qdma_qpair_file_ctx *ctx = file->private_data;
    struct slash_qdma_dev *qdma_dev;
    struct slash_qdma_transfer req;
    struct slash_qdma_xfer_req *xrs;
    __u32 user_size = 0;
    u32 count, i, last;
    u64 total = 0;
    int first_err = 0;
    ssize_t res;

    if (!ctx)
        return -EINVAL;

    qdma_dev = ctx->qdma_dev;

    if (!qdma_dev || ctx->n_qpairs == 0)
        return -ENODEV;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_TRANSFER_MIN_SIZE)
        return -EINVAL;

    memset(&req, 0, sizeof(req));
    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    count = req.count;
    if (count == 0 || count > SLASH_QDMA_FD_MAX_QPAIRS)
        return -EINVAL;

    xrs = kcalloc(count, sizeof(*xrs), GFP_KERNEL);
    if (!xrs)
        return -ENOMEM;

    /* Validate and prepare every sub-transfer (each takes a buffer ref). */
    for (i = 0; i < count; i++) {
        const struct slash_qdma_subxfer *d = &req.xfers[i];
        int rv;

        if (d->qpair_index >= ctx->n_qpairs)
            rv = -EINVAL;
        else
            rv = slash_qdma_xfer_prep(qdma_dev,
                                      ctx->entries[d->qpair_index], d,
                                      &xrs[i]);
        if (rv) {
            while (i-- > 0)
                slash_qdma_xfer_cleanup(&xrs[i]);
            kfree(xrs);
            return rv;
        }

        SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                              "transfer[%u]: qid=%u buf_fd=%d off=%llu dev=0x%llx len=%llu dir=%s\n",
                              i, ctx->qids[d->qpair_index], d->buf_fd,
                              (unsigned long long)d->buf_offset,
                              (unsigned long long)d->dev_addr,
                              (unsigned long long)d->length,
                              d->direction == SLASH_QDMA_XFER_H2C ? "H2C" : "C2H");
    }

    last = count - 1;

    /*
     * Submit all but the last asynchronously so the sub-transfers run on their
     * (distinct) queue pairs in parallel; libqdma calls fp_done on completion.
     */
    for (i = 0; i < last; i++) {
        init_completion(&xrs[i].done);
        xrs[i].qreq.fp_done = slash_qdma_xfer_done;
        res = qdma_request_submit(qdma_dev->qdma_handle, xrs[i].qhndl,
                                  &xrs[i].qreq);
        if (res < 0)
            xrs[i].err = (int)res; /* not queued: fp_done will not fire */
        else
            xrs[i].async_inflight = true;
    }

    /* Submit the last sub-transfer synchronously (blocks until complete). */
    res = qdma_request_submit(qdma_dev->qdma_handle, xrs[last].qhndl,
                              &xrs[last].qreq);
    if (res < 0)
        xrs[last].err = (int)res;
    else
        xrs[last].bytes_done = (unsigned int)res;

    /* Wait for the async sub-transfers, then aggregate (first error wins). */
    for (i = 0; i < last; i++) {
        if (xrs[i].async_inflight)
            wait_for_completion(&xrs[i].done);
    }

    for (i = 0; i < count; i++) {
        if (xrs[i].err && !first_err)
            first_err = xrs[i].err;
        total += xrs[i].bytes_done;
        slash_qdma_xfer_finish(&xrs[i]);
    }

    kfree(xrs);

    if (first_err)
        return (long)first_err;

    return (long)total;
}

#if defined(SLASH_HAVE_URING_CMD)
/**
 * struct slash_qdma_uring_cmd_ctx - Async state for one uring_cmd transfer.
 * @cmd:         The io_uring command being served.
 * @xrs:         Per-sub-transfer requests (buffer refs held until completion).
 * @count:       Number of sub-transfers.
 * @outstanding: Sub-transfers not yet completed; the one that drops it to 0
 *               schedules the completion task-work.
 * @total_bytes: Aggregate bytes transferred.
 * @first_err:   First negative errno seen, or 0.
 *
 * Heap-allocated for the lifetime of an asynchronous transfer; a pointer to it
 * is stashed in cmd->pdu so the completion task-work can recover it.
 */
struct slash_qdma_uring_cmd_ctx {
    struct io_uring_cmd *cmd;
    struct slash_qdma_xfer_req xrs[SLASH_QDMA_FD_MAX_QPAIRS];
    u32 count;
    atomic_t outstanding;
    atomic_long_t total_bytes;
    atomic_t first_err;
};

/**
 * slash_qdma_uring_cmd_complete() - Task-work that finishes a uring_cmd.
 * @cmd:         The io_uring command.
 * @issue_flags: io_uring issue flags for io_uring_cmd_done().
 *
 * Runs in task context once all sub-transfers have completed: drops the
 * buffer refs, completes the CQE with the total bytes (or first error), and
 * frees the command context.
 */
static void slash_qdma_uring_cmd_complete(struct io_uring_cmd *cmd,
                                          unsigned int issue_flags)
{
    struct slash_qdma_uring_cmd_ctx *uc;
    int err;
    long ret;
    u32 i;

    memcpy(&uc, cmd->pdu, sizeof(uc));
    err = atomic_read(&uc->first_err);
    ret = err ? err : atomic_long_read(&uc->total_bytes);

    for (i = 0; i < uc->count; i++)
        slash_qdma_xfer_finish(&uc->xrs[i]);

    io_uring_cmd_done(cmd, ret, 0, issue_flags);
    kfree(uc);
}

/**
 * slash_qdma_uring_xfer_done() - fp_done for async uring_cmd sub-transfers.
 * @qreq:       Completed request (embedded in a slash_qdma_xfer_req).
 * @bytes_done: Bytes transferred.
 * @err:        Negative errno on failure, else 0.
 *
 * Accumulates the result and, when the last sub-transfer of the command
 * completes, schedules the completion task-work.  Runs in libqdma worker
 * context.
 *
 * Return: Always 0.
 */
static int slash_qdma_uring_xfer_done(struct qdma_request *qreq,
                                      unsigned int bytes_done, int err)
{
    struct slash_qdma_xfer_req *xr =
        container_of(qreq, struct slash_qdma_xfer_req, qreq);
    struct slash_qdma_uring_cmd_ctx *uc =
        (struct slash_qdma_uring_cmd_ctx *)qreq->uld_data;

    xr->bytes_done = bytes_done;
    xr->err = err;
    if (bytes_done)
        atomic_long_add(bytes_done, &uc->total_bytes);
    if (err)
        atomic_cmpxchg(&uc->first_err, 0, err);

    if (atomic_dec_and_test(&uc->outstanding))
        io_uring_cmd_complete_in_task(uc->cmd,
                                      slash_qdma_uring_cmd_complete);
    return 0;
}

/**
 * slash_qdma_qpair_uring_cmd() - Asynchronous transfer batch via io_uring.
 * @cmd:         The io_uring command; its inline SQE data is a single __u64
 *               userspace pointer to a struct slash_qdma_transfer.
 * @issue_flags: io_uring issue flags.
 *
 * The optional async sibling of SLASH_QDMA_QPAIR_IOCTL_TRANSFER: it prepares
 * every sub-transfer, submits them all asynchronously (so they run on their
 * distinct queue pairs concurrently), and completes the CQE from task-work
 * once they all finish.  Many such commands can be in flight at once, which is
 * the intended multi-buffer optimization.
 *
 * Return: -EIOCBQUEUED once submission is under way (completion arrives via
 *         the CQE); a negative errno if the command is rejected before any
 *         sub-transfer is queued; -EAGAIN to defer a non-blocking issue.
 */
static int slash_qdma_qpair_uring_cmd(struct io_uring_cmd *cmd,
                                      unsigned int issue_flags)
{
    struct file *file = cmd->file;
    struct slash_qdma_qpair_file_ctx *ctx = file->private_data;
    struct slash_qdma_dev *qdma_dev;
    struct slash_qdma_uring_cmd_ctx *uc;
    struct slash_qdma_transfer req;
    u64 uptr = 0;
    u32 count, i;
    ssize_t res;

    if (cmd->cmd_op != SLASH_QDMA_URING_CMD_TRANSFER)
        return -EOPNOTSUPP;

    if (!ctx)
        return -EINVAL;

    qdma_dev = ctx->qdma_dev;
    if (!qdma_dev || ctx->n_qpairs == 0)
        return -ENODEV;

    /*
     * Copying the descriptor from userspace may fault and sleep, so defer a
     * non-blocking issue to a blocking io_uring context.
     */
    if (issue_flags & IO_URING_F_NONBLOCK)
        return -EAGAIN;

    /* The SQE inline command carries the user pointer to the descriptor. */
    memcpy(&uptr, slash_qdma_uring_cmd_payload(cmd), sizeof(uptr));

    memset(&req, 0, sizeof(req));
    if (copy_from_user(&req, u64_to_user_ptr(uptr), sizeof(req)))
        return -EFAULT;

    count = req.count;
    if (count == 0 || count > SLASH_QDMA_FD_MAX_QPAIRS)
        return -EINVAL;

    uc = kzalloc(sizeof(*uc), GFP_KERNEL);
    if (!uc)
        return -ENOMEM;

    uc->cmd = cmd;
    uc->count = count;
    atomic_set(&uc->outstanding, count);
    atomic_long_set(&uc->total_bytes, 0);
    atomic_set(&uc->first_err, 0);

    /* Validate and prepare every sub-transfer before queueing any of them. */
    for (i = 0; i < count; i++) {
        const struct slash_qdma_subxfer *d = &req.xfers[i];
        int rv;

        if (d->qpair_index >= ctx->n_qpairs)
            rv = -EINVAL;
        else
            rv = slash_qdma_xfer_prep(qdma_dev,
                                      ctx->entries[d->qpair_index], d,
                                      &uc->xrs[i]);
        if (rv) {
            while (i-- > 0)
                slash_qdma_xfer_cleanup(&uc->xrs[i]);
            kfree(uc);
            return rv;
        }
        uc->xrs[i].qreq.uld_data = (unsigned long)uc;
        uc->xrs[i].qreq.fp_done = slash_qdma_uring_xfer_done;
    }

    /* Stash the context for the completion task-work. */
    memcpy(cmd->pdu, &uc, sizeof(uc));

    /*
     * Submit all sub-transfers asynchronously.  Completion (success or the
     * inline submit-failure path below) is funnelled through the outstanding
     * counter so the CQE is posted exactly once from task-work.
     */
    for (i = 0; i < count; i++) {
        res = qdma_request_submit(qdma_dev->qdma_handle, uc->xrs[i].qhndl,
                                  &uc->xrs[i].qreq);
        if (res < 0) {
            /* Not queued: fp_done will not fire, account for it here. */
            uc->xrs[i].err = (int)res;
            atomic_cmpxchg(&uc->first_err, 0, (int)res);
            if (atomic_dec_and_test(&uc->outstanding))
                io_uring_cmd_complete_in_task(uc->cmd,
                                              slash_qdma_uring_cmd_complete);
        }
    }

    return -EIOCBQUEUED;
}
#endif /* SLASH_HAVE_URING_CMD */

/**
 * slash_qdma_qpair_ioctl() - Ioctl handler for per-qpair anon_inode fds.
 * @file: Anon_inode file.
 * @cmd:  Ioctl command number.
 * @arg:  User-space argument.
 *
 * Supports SLASH_QDMA_IOCTL_BUF_CREATE (allocate a kernel buffer for clients
 * that hold only a queue-pair fd) and SLASH_QDMA_QPAIR_IOCTL_TRANSFER (buffer
 * DMA transfer).
 *
 * Return: bytes transferred (>= 0) for TRANSFER, a new fd for BUF_CREATE, or
 *         -ENOTTY for any other command.
 */
static long slash_qdma_qpair_ioctl(struct file *file,
                                   unsigned int cmd, unsigned long arg)
{
    struct slash_qdma_qpair_file_ctx *ctx = file->private_data;

    if (!ctx || !ctx->qdma_dev)
        return -ENODEV;

    switch (cmd) {
    case SLASH_QDMA_IOCTL_BUF_CREATE:
        return slash_qdma_ioctl_buf_create_w(ctx->qdma_dev,
                                             (void __user *)arg);
    case SLASH_QDMA_QPAIR_IOCTL_TRANSFER:
        return slash_qdma_qpair_transfer(file, (void __user *)arg);
    default:
        return -ENOTTY;
    }
}

/**
 * slash_qdma_qpair_release() - Release handler for per-qpair anon_inode fds.
 * @inode: Inode (unused for anon_inodes).
 * @file:  The file being closed.
 *
 * Drops the references acquired in slash_qdma_ioctl_qpair_get_fd_w():
 *   - One ref on the qpair entry (may free the entry if the qpair has
 *     already been deleted from the xarray).
 *   - One ref on the QDMA device (may free the device if it has already
 *     been removed from PCI).
 *
 * Also frees the file context structure.
 *
 * Return: Always 0.
 */
static int slash_qdma_qpair_release(struct inode *inode, struct file *file)
{
    struct slash_qdma_qpair_file_ctx *ctx = file->private_data;
    u32 i;

    (void)inode;

    if (ctx) {
        for (i = 0; i < ctx->n_qpairs; i++) {
            if (ctx->entries[i])
                slash_qdma_qpair_put(ctx->entries[i]);
        }
        if (ctx->qdma_dev)
            kref_put(&ctx->qdma_dev->ref, slash_qdma_dev_release);
        kfree(ctx);
        file->private_data = NULL;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: qpair get fd
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_qpair_get_fd_w() - Create an anon_inode fd for queue I/O.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_fd_request struct.
 *
 * Creates an anonymous inode file descriptor that userspace can use for
 * buffer transfer ioctls.  The fd is a collection of one or two queue pairs
 * (see slash_qdma_qpair_fd_request): @qpair_count == 0 binds the single qpair
 * named by @qid (back-compat), otherwise @qpair_count IDs from @qpair_ids are
 * bound, their array index becoming the transfer qpair_index.
 *
 * The fd holds references to each bound qpair entry and the device, preventing
 * either from being freed while the fd is open.  Each bound qpair keeps the
 * per-qpair configuration (mm_channel, ring sizes, directions) it was given at
 * add time, so the channels can differ.
 *
 * The only supported flag is O_CLOEXEC (close-on-exec).
 *
 * Error handling: on any failure after resources are acquired, all refs and
 * allocations are cleaned up before returning.
 *
 * Return: The new fd (>= 0) on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_get_fd_w(struct slash_qdma_dev *qdma_dev,
                                           void __user *uarg)
{
    struct slash_qdma_qpair_fd_request req;
    __u32 user_size = 0;
    __u32 ids[SLASH_QDMA_FD_MAX_QPAIRS];
    u32 n_qpairs;
    u32 i;
    size_t copy_size;
    struct slash_qdma_qpair_file_ctx *ctx;
    struct file *file;
    int fd;
    int err;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    if (user_size < SLASH_QDMA_QPAIR_FD_REQUEST_MIN_SIZE) {
        dev_warn(&qdma_dev->pdev->dev,
                 "qdma: QPAIR_GET_FD size too small (%u)\n", user_size);
        return -EINVAL;
    }

    memset(&req, 0, sizeof(req));

    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    /* Only O_CLOEXEC is a valid flag. */
    if (req.flags & ~O_CLOEXEC)
        return -EINVAL;

    /*
     * Resolve the requested qpair-id set.  qpair_count == 0 is the legacy
     * single-qpair form using @qid; otherwise bind @qpair_count ids.
     */
    if (req.qpair_count == 0) {
        n_qpairs = 1;
        ids[0] = req.qid;
    } else {
        if (req.qpair_count > SLASH_QDMA_FD_MAX_QPAIRS)
            return -EINVAL;
        n_qpairs = req.qpair_count;
        for (i = 0; i < n_qpairs; i++)
            ids[i] = req.qpair_ids[i];
    }

    /* Allocate the per-fd context. */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    /* Look up each qpair entry and take refs while holding the lock. */
    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        kfree(ctx);
        return -ENODEV;
    }

    for (i = 0; i < n_qpairs; i++) {
        struct slash_qdma_qpair_entry *entry =
            slash_qdma_qpair_lookup(qdma_dev, ids[i]);

        if (!entry || !entry->dir_mask) {
            /* Drop refs taken so far for the earlier entries. */
            while (i-- > 0)
                slash_qdma_qpair_put(ctx->entries[i]);
            mutex_unlock(&qdma_dev->lock);
            kfree(ctx);
            return -ENOENT;
        }

        /*
         * Take a ref on the entry.  These refs are held by the file context
         * and released when the fd is closed, ensuring the entries cannot be
         * freed prematurely.
         */
        slash_qdma_qpair_get(entry);
        ctx->entries[i] = entry;
        ctx->qids[i] = ids[i];
    }
    ctx->n_qpairs = n_qpairs;

    kref_get(&qdma_dev->ref);
    mutex_unlock(&qdma_dev->lock);

    ctx->qdma_dev = qdma_dev;

    /* Create the anonymous inode file with read/write access. */
    file = anon_inode_getfile("slash_qdma_qpair", &slash_qdma_qpair_fops,
                              ctx, O_RDWR | (req.flags & O_CLOEXEC));
    if (IS_ERR(file)) {
        err = PTR_ERR(file);
        for (i = 0; i < ctx->n_qpairs; i++)
            slash_qdma_qpair_put(ctx->entries[i]);
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
        kfree(ctx);
        return err;
    }

    /* Allocate a file descriptor number. */
    fd = get_unused_fd_flags(req.flags & O_CLOEXEC);
    if (fd < 0) {
        fput(file); /* triggers slash_qdma_qpair_release -> drops entry/dev refs, frees ctx */
        return fd;
    }

    /* Copy the response back to userspace before installing the fd. */
    req.size = sizeof(req);
    copy_size = min_t(size_t, user_size, sizeof(req));
    if (copy_to_user(uarg, &req, copy_size)) {
        put_unused_fd(fd);
        fput(file); /* triggers slash_qdma_qpair_release -> drops entry/dev refs, frees ctx */
        return -EFAULT;
    }
    if (user_size > sizeof(req)) {
        if (clear_user((void __user *)((unsigned long)uarg + sizeof(req)),
                       user_size - sizeof(req))) {
            put_unused_fd(fd);
            fput(file);
            return -EFAULT;
        }
    }

    /*
     * Install the fd.  After this point the fd is visible to userspace
     * and the file's release callback will handle cleanup.
     */
    fd_install(fd, file);

    return fd;
}
