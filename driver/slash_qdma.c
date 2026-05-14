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
 * The QDMA subsystem binds to PF1 (PCI device ID 0x50B5), while the
 * control device (slash_ctldev) binds to PF2 (device ID 0x50B6).
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

#include <asm/cacheflush.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/anon_inodes.h>

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
 * @misc:               Miscdevice registered under /dev/slash_qdma_ctlN.
 *                      Userspace opens this to issue queue management ioctls.
 * @ref:                Device-level reference count.  The miscdevice open
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
 * @is_misc_registered: True while the miscdevice is live.  Prevents double
 *                      deregistration on error paths.
 * @hw_shutdown:        Set to true during destroy to signal that the HW is
 *                      going away.  Any ioctl arriving after this flag is
 *                      set returns -ENODEV immediately.
 *
 * The three booleans (@have_qdma_handle, @is_misc_registered,
 * @hw_shutdown) track partially-constructed state during probe/remove
 * error paths; outside of create/destroy they should always reflect a
 * fully initialised device.
 */
struct slash_qdma_dev {
    struct pci_dev *pdev;
    unsigned long qdma_handle;

    struct miscdevice misc;
    struct kref ref;
    struct mutex lock;
    struct xarray qpairs;

    /*
     * Initialization booleans.
     * Assume these are always true outside of create/destroy.
     */
    bool have_qdma_handle;
    bool is_misc_registered;
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
 * @qdma_dev: Back-pointer to the owning QDMA device (ref held).
 * @entry:    The queue pair entry this fd operates on (ref held).
 * @qid:      Queue pair ID, cached for debug logging.
 *
 * Allocated in slash_qdma_ioctl_qpair_get_fd_w() and freed in
 * slash_qdma_qpair_release().  Both @qdma_dev and @entry have their
 * reference counts incremented when the ctx is created, and decremented
 * when the fd is closed.
 */
struct slash_qdma_qpair_file_ctx {
    struct slash_qdma_dev *qdma_dev;
    struct slash_qdma_qpair_entry *entry;
    u32 qid;
};

/**
 * struct slash_qdma_io_cb - I/O control block for a single DMA transfer.
 * @buf:      User-space buffer address (source for H2C, destination for C2H).
 * @len:      Transfer length in bytes.
 * @pages_nr: Number of user pages pinned by get_user_pages_fast().
 * @sgl:      Scatter-gather list of qdma_sw_sg entries, one per pinned page.
 *            Allocated as a single contiguous block together with @pages.
 * @pages:    Array of struct page pointers for the pinned user pages.
 *            Points into the same allocation as @sgl (immediately after it).
 * @req:      The libqdma request structure submitted to qdma_request_submit().
 *
 * This is a stack-local structure (allocated in slash_qdma_qpair_read_write)
 * that bundles all per-transfer state.  The SGL and page array are heap-
 * allocated in slash_qdma_map_user_buf_to_sgl() and freed in
 * slash_qdma_iocb_release().
 */
struct slash_qdma_io_cb {
    void __user *buf;
    size_t len;
    unsigned int pages_nr;
    struct qdma_sw_sg *sgl;
    struct page **pages;
    struct qdma_request req;
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
static int slash_qdma_ioctl_info_w(struct miscdevice *misc,
                                   struct slash_qdma_dev *qdma_dev,
                                   void __user *uarg);
static int slash_qdma_ioctl_qpair_add_w(struct miscdevice *misc,
                                         struct slash_qdma_dev *qdma_dev,
                                         void __user *uarg);
static int slash_qdma_ioctl_qpair_add(struct miscdevice *misc,
                                      struct slash_qdma_dev *qdma_dev,
                                      struct slash_qdma_qpair_add *req);
static int slash_qdma_ioctl_qpair_add_q(struct miscdevice *misc,
                                        struct slash_qdma_dev *qdma_dev,
                                        struct slash_qdma_qpair_add *req,
                                        struct slash_qdma_qpair_entry *entry,
                                        enum queue_type_t qtype);
static void slash_qdma_ioctl_qpair_rm_q(struct miscdevice *misc,
                                        struct slash_qdma_dev *qdma_dev,
                                        struct slash_qdma_qpair_entry *entry,
                                        enum queue_type_t qtype);
static int slash_qdma_ioctl_qpair_op_w(struct miscdevice *misc,
                                       struct slash_qdma_dev *qdma_dev,
                                       void __user *uarg);
static int slash_qdma_ioctl_qpair_op(struct miscdevice *misc,
                                     struct slash_qdma_dev *qdma_dev,
                                     struct slash_qdma_qpair_op *req);
static int slash_qdma_ioctl_qpair_op_apply(struct slash_qdma_dev *qdma_dev,
                                           struct slash_qdma_qpair_entry *entry,
                                           struct slash_qdma_qpair_op *req,
                                           slash_qdma_queue_cmd_fn fn,
                                           const char *op_name,
                                           bool stop_on_err);
static int slash_qdma_ioctl_qpair_get_fd_w(struct miscdevice *misc,
                                           struct slash_qdma_dev *qdma_dev,
                                           void __user *uarg);

static ssize_t slash_qdma_qpair_read(struct file *file, char __user *buf,
                                     size_t count, loff_t *ppos);
static ssize_t slash_qdma_qpair_write(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos);
static int slash_qdma_qpair_release(struct inode *inode, struct file *file);
static long slash_qdma_qpair_ioctl(struct file *file,
                                   unsigned int cmd, unsigned long arg);

/**
 * slash_qdma_qpair_fops - File operations for per-qpair anon_inode fds.
 *
 * read()  performs a C2H (card-to-host) DMA transfer.
 * write() performs an H2C (host-to-card) DMA transfer.
 * llseek  uses default_llseek so that pread/pwrite can set the
 *         device-side address via the file position.
 * ioctl   is a stub that returns -ENOTTY (no per-fd ioctls defined yet).
 * release drops the refs on the qpair entry and device.
 */
static const struct file_operations slash_qdma_qpair_fops = {
    .owner          = THIS_MODULE,
    .read           = slash_qdma_qpair_read,
    .write          = slash_qdma_qpair_write,
    .unlocked_ioctl = slash_qdma_qpair_ioctl,
    .release        = slash_qdma_qpair_release,
    .llseek         = default_llseek,
};


static int slash_qdma_fop_open(struct inode *inode, struct file *file);
static int slash_qdma_fop_release(struct inode *inode, struct file *file);
static long slash_qdma_fop_ioctl(struct file *file, unsigned int op, unsigned long arg);
static void slash_qdma_ioctl_info(struct miscdevice *misc, struct slash_qdma_dev *qdma_dev, struct slash_qdma_info *qdma_info);



/**
 * slash_qdma_ids - PCI device ID table for the QDMA PF.
 *
 * Matches only PF1 (device ID 0x50B5) on AMD/Xilinx V80 cards.
 */
static const struct pci_device_id slash_qdma_ids[] = {
    {PCI_DEVICE(SLASH_QDMA_PCI_VENDOR_ID, SLASH_QDMA_PCI_DEVICE_ID)},
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
 * slash_qdma_fops - File operations for the QDMA control miscdevice.
 *
 * The miscdevice (/dev/slash_qdma_ctlN) is the management interface:
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
 * BDF-to-device-number map (stable /dev/slash_qdma_ctlN across hotplug)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * struct slash_qdma_id_entry - Stable BDF-to-number mapping entry.
 * @node:    Intrusive list linkage for @slash_qdma_id_map.
 * @bdf:     Full PCI BDF string including function (e.g. "0000:61:00.1").
 * @number:  The /dev/slash_qdma_ctl<N> suffix permanently assigned to this BDF.
 * @in_use:  True while the device is bound to the driver.  Cleared on remove,
 *           set on probe.  A probe that finds @in_use already true indicates
 *           the kernel handed us a device that was never properly unbound —
 *           this should never happen under normal operation.
 *
 * Entries are allocated in probe and intentionally never freed.  They survive
 * hotplug remove+rescan cycles so that a device always gets back the same N.
 */
struct slash_qdma_id_entry {
    struct list_head node;
    char bdf[32]; /* "DDDD:BB:SS.F\0" fits comfortably in 32 bytes */
    int  number;
    bool in_use;
};

/** Persistent BDF-to-number map; entries live for the module's lifetime. */
static LIST_HEAD(slash_qdma_id_map);
/** Serialises all accesses to @slash_qdma_id_map and @in_use fields. */
static DEFINE_MUTEX(slash_qdma_id_map_lock);
/** Source of new numbers; only incremented when a BDF is seen for the first time. */
static atomic_t slash_qdma_devcount = ATOMIC_INIT(0);

/**
 * slash_qdma_id_get() - Look up or allocate a stable number for a BDF.
 * @bdf: Full PCI BDF string (e.g. "0000:61:00.1") from pci_name().
 *
 * Called from probe.  Returns the number permanently associated with @bdf,
 * allocating a new one if this BDF is seen for the first time.  Also marks
 * the entry as in_use = true.
 *
 * If an existing entry is found with in_use already set, the device was
 * never properly unbound before probe was called again — this indicates a
 * kernel PCI driver bug.  The function logs a loud error and returns
 * -EBUSY so that probe aborts without touching the device.
 *
 * Return: non-negative stable device number on success, negative errno on
 *         failure (-ENOMEM if allocation fails, -EBUSY if already in use).
 */
static int slash_qdma_id_get(const char *bdf)
{
    struct slash_qdma_id_entry *entry;
    int number;

    mutex_lock(&slash_qdma_id_map_lock);

    list_for_each_entry(entry, &slash_qdma_id_map, node) {
        if (strcmp(entry->bdf, bdf) != 0)
            continue;

        if (entry->in_use) {
            pr_err("slash_qdma: BUG: probe called for %s but entry is already in_use "
                   "(number=%d); refusing to bind\n", bdf, entry->number);
            mutex_unlock(&slash_qdma_id_map_lock);
            return -EBUSY;
        }

        entry->in_use = true;
        number = entry->number;
        mutex_unlock(&slash_qdma_id_map_lock);
        pr_info("slash_qdma: reusing number %d for %s\n", number, bdf);
        return number;
    }

    /* First time we've seen this BDF — allocate a fresh entry. */
    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        mutex_unlock(&slash_qdma_id_map_lock);
        return -ENOMEM;
    }

    strscpy(entry->bdf, bdf, sizeof(entry->bdf));
    entry->number = atomic_inc_return(&slash_qdma_devcount) - 1;
    entry->in_use = true;
    list_add_tail(&entry->node, &slash_qdma_id_map);

    number = entry->number;
    mutex_unlock(&slash_qdma_id_map_lock);

    pr_info("slash_qdma: assigned number %d to %s\n", number, bdf);
    return number;
}

/**
 * slash_qdma_id_release() - Mark a BDF's entry as no longer in use.
 * @bdf: Full PCI BDF string passed to the matching slash_qdma_id_get() call.
 *
 * Called when the misc device is deregistered (remove path, or probe error
 * unwind after misc_register succeeds).  Clears in_use so that the next probe
 * for the same BDF can reuse the stored number.  The entry itself is not freed.
 *
 * If no entry exists for @bdf (should never happen after a successful probe),
 * the call is a no-op and a warning is logged.
 */
static void slash_qdma_id_release(const char *bdf)
{
    struct slash_qdma_id_entry *entry;

    mutex_lock(&slash_qdma_id_map_lock);

    list_for_each_entry(entry, &slash_qdma_id_map, node) {
        if (strcmp(entry->bdf, bdf) != 0)
            continue;

        entry->in_use = false;
        mutex_unlock(&slash_qdma_id_map_lock);
        pr_info("slash_qdma: released number %d for %s\n", entry->number, bdf);
        return;
    }

    /* Should be unreachable: release without a prior successful id_get. */
    pr_warn("slash_qdma: WARNING: release called for %s but no entry found\n", bdf);
    mutex_unlock(&slash_qdma_id_map_lock);
}

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
 *   3. Registers the management miscdevice (/dev/slash_qdma_ctlN).
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

    /* Register the management miscdevice so userspace can issue ioctls. */
    err = misc_register(&device->misc);
    if (err) {
        dev_err(&pdev->dev, "slash: qdma: could not register misc device: %d", err);
        /*
         * is_misc_registered is still false here, so slash_qdma_destroy_qdma_device
         * will not call misc_deregister or id_release.  Release the id explicitly.
         */
        slash_qdma_id_release(pci_name(pdev));
        goto err_free;
    }
    device->is_misc_registered = true;

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
 * miscdevice, and drops the device reference.
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
 * Allocates the slash_qdma_dev, initialises its mutex, xarray, kref,
 * and miscdevice fields, and stores it in the PCI drvdata.  A static
 * atomic counter provides unique /dev node numbering across devices.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_create_qdma_device(struct pci_dev *pdev, struct slash_qdma_dev **pdevice)
{
    int err;
    struct slash_qdma_dev *device;
    int id;

    device = kzalloc(sizeof(*device), GFP_KERNEL);
    if (!device) {
        return -ENOMEM;
    }
    device->pdev = pdev;
    kref_init(&device->ref);
    mutex_init(&device->lock);
    xa_init_flags(&device->qpairs, XA_FLAGS_ALLOC);
    device->hw_shutdown = false;
    pci_set_drvdata(pdev, device);

    { /* Miscdevice setup */
        device->misc.minor = MISC_DYNAMIC_MINOR;
        device->misc.fops = &slash_qdma_fops;
        device->misc.parent = &pdev->dev;
        device->misc.mode = SLASH_CTLDEV_QDMA_MODE;

        /* Name visible in /sys/class/misc, includes PCI BDF for uniqueness. */
        device->misc.name = kasprintf(GFP_KERNEL, SLASH_QDMA_CTLDEV_NAME_FMT, pci_name(device->pdev));
        if (!device->misc.name) {
            dev_err(&device->pdev->dev, "qdma: kasprintf(name) failed\n");
            err = -ENOMEM;
            goto err_free;
        }

        /* /dev node name: stable numeric index from BDF-to-number map. */
        id = slash_qdma_id_get(pci_name(device->pdev));
        if (id < 0) {
            dev_err(&device->pdev->dev, "qdma: id_get failed: %d\n", id);
            err = id;
            goto err_free_name;
        }

        device->misc.nodename = kasprintf(GFP_KERNEL, SLASH_QDMA_CTLDEV_NODENAME_FMT, id);
        if (!device->misc.nodename) {
            dev_err(&device->pdev->dev, "qdma: kasprintf(nodename) failed\n");
            err = -ENOMEM;
            goto err_release_id;
        }
    }

    *pdevice = device;
    return 0;

err_release_id:
    slash_qdma_id_release(pci_name(device->pdev));

err_free_name:
    kfree(device->misc.name);
    device->misc.name = NULL;

err_free:
    slash_qdma_destroy_qdma_device(device);
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
 *   2. Deregister the miscdevice (prevents new opens).
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

    /* Deregister miscdevice to prevent new file opens. */
    if (device->is_misc_registered) {
        misc_deregister(&device->misc);
        slash_qdma_id_release(pci_name(device->pdev));
        device->is_misc_registered = false;
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

                slash_qdma_ioctl_qpair_rm_q(&device->misc, device, entry, qtype);
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
 * Called when the last reference drops (after both the miscdevice is
 * closed and all anon_inode fds are released).  Frees the dynamically
 * allocated miscdevice name/nodename strings and the device structure.
 */
static void slash_qdma_dev_release(struct kref *ref)
{
    struct slash_qdma_dev *device =
        container_of(ref, struct slash_qdma_dev, ref);

    mutex_destroy(&device->lock);

    if (device->misc.name) {
        kfree(device->misc.name);
    }

    if (device->misc.nodename) {
        kfree(device->misc.nodename);
    }

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
 * Miscdevice file operations (management interface)
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_fop_ioctl() - Dispatch ioctls on the QDMA control miscdevice.
 * @file: Open file for the miscdevice.
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
    struct miscdevice *misc = &qdma_dev->misc;
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
        ret = slash_qdma_ioctl_info_w(misc, qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_QPAIR_ADD:
        ret = slash_qdma_ioctl_qpair_add_w(misc, qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_Q_OP:
        ret = slash_qdma_ioctl_qpair_op_w(misc, qdma_dev, uarg);
        break;

    case SLASH_QDMA_IOCTL_QPAIR_GET_FD:
        ret = slash_qdma_ioctl_qpair_get_fd_w(misc, qdma_dev, uarg);
        break;

    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

/**
 * slash_qdma_fop_open() - Open handler for the QDMA control miscdevice.
 * @inode: Inode of the device node.
 * @file:  File being opened.
 *
 * The misc framework sets file->private_data to the miscdevice before
 * calling open.  We use container_of to recover the slash_qdma_dev,
 * take a device reference, and stash the device pointer in private_data
 * so that subsequent ioctl/release calls can find it directly.
 *
 * Return: 0 on success, -ENODEV if the device is shutting down.
 */
static int slash_qdma_fop_open(struct inode *inode, struct file *file)
{
    struct miscdevice *misc = file->private_data;
    struct slash_qdma_dev *qdma_dev =
        container_of(misc, struct slash_qdma_dev, misc);
    int ret = 0;

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        ret = -ENODEV;
    } else {
        kref_get(&qdma_dev->ref);
        file->private_data = qdma_dev;
    }
    mutex_unlock(&qdma_dev->lock);

    return ret;
}

/**
 * slash_qdma_fop_release() - Release handler for the QDMA control miscdevice.
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

    if (qdma_dev)
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);

    return 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: info
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_info_w() - Wrapper for the QDMA info ioctl.
 * @misc:     Miscdevice handle.
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
static int slash_qdma_ioctl_info_w(struct miscdevice *misc,
                                    struct slash_qdma_dev *qdma_dev,
                                    void __user *uarg)
{
    struct slash_qdma_info info;
    u32 user_size = 0;
    size_t copy_size;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    slash_qdma_ioctl_info(misc, qdma_dev, &info);
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
 * @misc:      Miscdevice handle (unused).
 * @qdma_dev:  QDMA device (unused for now).
 * @qdma_info: [out] Structure to fill with capability data.
 *
 * Currently returns zeroes for all fields.  This is a placeholder for
 * future capability reporting (e.g., querying qdma_device_capabilities).
 */
static void slash_qdma_ioctl_info(struct miscdevice *misc,
                                  struct slash_qdma_dev *qdma_dev,
                                  struct slash_qdma_info *qdma_info)
{
    (void) misc;
    (void) qdma_dev;

    qdma_info->qsets_max = 0;
    qdma_info->msix_qvecs = 0;
    qdma_info->vf_max = 0;
    qdma_info->caps = 0;
}

/* ─────────────────────────────────────────────────────────────────────
 * IOCTL: qpair add
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_ioctl_qpair_add_w() - Wrapper for the qpair-add ioctl.
 * @misc:     Miscdevice handle.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_add struct.
 *
 * Validates userspace inputs:
 *   - @dir_mask must contain only valid direction bits and be non-zero.
 *   - @mode must be MM or ST.
 *   - Ring size indices must be in [0, 15] (CSR table range).
 *
 * On success, the kernel-assigned @qid is written back to userspace.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_add_w(struct miscdevice *misc,
                                         struct slash_qdma_dev *qdma_dev,
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

    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }
    err = slash_qdma_ioctl_qpair_add(misc, qdma_dev, &req);
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
 * @misc:     Miscdevice handle.
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
static int slash_qdma_ioctl_qpair_add(struct miscdevice *misc,
                                      struct slash_qdma_dev *qdma_dev,
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

        ret = slash_qdma_ioctl_qpair_add_q(misc, qdma_dev, req, entry, qtype);
        if (ret)
            goto rollback;

        added[idx] = true;
    }

    return 0;

rollback:
    /* Undo any queues that were successfully added before the failure. */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
        if (added[idx])
            slash_qdma_ioctl_qpair_rm_q(misc, qdma_dev, entry, idx);
    }

    slash_qdma_qpair_remove(qdma_dev, req->qid);

    return ret;
}

/**
 * slash_qdma_ioctl_qpair_add_q() - Add a single HW queue to a queue pair.
 * @misc:     Miscdevice handle (for error logging context).
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
 *   - qconf.aperture_size = 4096: page-granularity (4 KB) for descriptor
 *     addressing.  Each descriptor addresses one page-sized chunk.
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
static int slash_qdma_ioctl_qpair_add_q(struct miscdevice *misc,
                                         struct slash_qdma_dev *qdma_dev,
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

    qconf.aperture_size = 4096;                     /* Page-granularity descriptor addressing */

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
                          "qdma_queue_add start: qid=%u type=%u mode=%u\n",
                          req->qid, qtype, req->mode);
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

    /* Record the handle and mark this direction as active. */
    entry->qhndl[qtype] = qhndl;
    entry->dir_mask |= dir_bit;

    return 0;
}

/**
 * slash_qdma_ioctl_qpair_rm_q() - Remove a single HW queue from a queue pair.
 * @misc:     Miscdevice handle (for logging context).
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
static void slash_qdma_ioctl_qpair_rm_q(struct miscdevice *misc,
                                         struct slash_qdma_dev *qdma_dev,
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
 * @misc:     Miscdevice handle.
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_op struct.
 *
 * Handles the versioned copy-in / copy-out pattern and validates that
 * the requested operation is within the known range.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_op_w(struct miscdevice *misc,
                                       struct slash_qdma_dev *qdma_dev,
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
    ret = slash_qdma_ioctl_qpair_op(misc, qdma_dev, &req);
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
 * @misc:     Miscdevice handle (unused, present for API consistency).
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
static int slash_qdma_ioctl_qpair_op(struct miscdevice *misc,
                                     struct slash_qdma_dev *qdma_dev,
                                     struct slash_qdma_qpair_op *req)
{
    struct slash_qdma_qpair_entry *entry;
    int ret = 0;

    (void) misc;

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
 * DMA I/O: user buffer mapping, SGL construction, and transfer
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_iocb_release() - Free resources in an I/O control block.
 * @iocb: The IOCB to clean up.
 *
 * Frees the combined SGL + page-pointer allocation and clears the
 * pointers.  Does not unpin pages — that must be done separately via
 * slash_qdma_unmap_user_buf() before calling this.
 */
static inline void slash_qdma_iocb_release(struct slash_qdma_io_cb *iocb)
{
    if (iocb->pages)
        iocb->pages = NULL;

    kfree(iocb->sgl);
    iocb->sgl = NULL;
    iocb->buf = NULL;
}

/**
 * slash_qdma_unmap_user_buf() - Unpin user pages after a DMA transfer.
 * @iocb:  I/O control block with pinned pages.
 * @write: Transfer direction from the device's perspective.  If false
 *         (i.e., a C2H/read transfer), the pages were written to by the
 *         device and must be marked dirty so the VM knows the page
 *         contents have changed.
 *
 * Iterates over pinned pages, marks them dirty if this was a read (C2H)
 * transfer (because the device wrote data into those user pages), and
 * releases each page reference acquired by get_user_pages_fast().
 */
static void slash_qdma_unmap_user_buf(struct slash_qdma_io_cb *iocb, bool write)
{
    int i;

    if (!iocb->pages || !iocb->pages_nr)
        return;

    for (i = 0; i < iocb->pages_nr; i++) {
        if (iocb->pages[i]) {
            /*
             * For C2H (read) transfers (!write), the device wrote into
             * these user pages, so mark them dirty to inform the VM.
             */
            if (!write)
                set_page_dirty(iocb->pages[i]);
            put_page(iocb->pages[i]);
        } else {
            break;
        }
    }

    if (i != iocb->pages_nr)
        pr_err("slash: qdma: sgl pages %d/%u.\n", i, iocb->pages_nr);

    iocb->pages_nr = 0;
}

/**
 * slash_qdma_map_user_buf_to_sgl() - Pin user pages and build a scatter-gather list.
 * @iocb:  I/O control block.  @iocb->buf and @iocb->len must be set
 *         before calling.  On success, @iocb->sgl, @iocb->pages, and
 *         @iocb->pages_nr are populated.
 * @write: Transfer direction (true = H2C write, false = C2H read).
 *
 * Steps:
 *   1. Compute the number of pages spanned by the user buffer (accounting
 *      for the offset within the first page).
 *   2. Allocate a single contiguous block for the SGL entries and the
 *      page pointer array (avoids two allocations).
 *   3. Pin user pages via get_user_pages_fast() with write=1 (even for
 *      H2C, because libqdma may write status back).
 *   4. Build the qdma_sw_sg linked list: one entry per page, with the
 *      first entry's offset reflecting the sub-page position of the
 *      user buffer, and the last entry's length truncated to the
 *      remaining byte count.
 *   5. Flush the data cache for each page to ensure coherency between
 *      the CPU cache and the DMA engine's view of memory.
 *
 * Return: 0 on success, negative errno on failure (pages are unpinned
 *         and the SGL is freed on error).
 */
static int slash_qdma_map_user_buf_to_sgl(struct slash_qdma_io_cb *iocb,
                                          bool write)
{
    unsigned long len = iocb->len;
    char *buf = (char *)iocb->buf;
    struct qdma_sw_sg *sg;
    unsigned int pg_off = offset_in_page(buf);
    unsigned int pages_nr = (len + pg_off + PAGE_SIZE - 1) >> PAGE_SHIFT;
    int i;
    int rv;

    if (len == 0)
        pages_nr = 1;
    if (pages_nr == 0)
        return -EINVAL;

    iocb->pages_nr = 0;

    /*
     * Single allocation for both the SGL array and the page pointer
     * array.  The page pointers are placed immediately after the SGL
     * entries in memory.
     */
    sg = kmalloc(pages_nr * (sizeof(struct qdma_sw_sg) +
                             sizeof(struct page *)), GFP_KERNEL);
    if (!sg) {
        pr_err("slash: qdma: sgl allocation failed for %u pages\n",
               pages_nr);
        return -ENOMEM;
    }
    memset(sg, 0, pages_nr * (sizeof(struct qdma_sw_sg) +
                              sizeof(struct page *)));
    iocb->sgl = sg;

    /* Page pointer array lives right after the SGL entries. */
    iocb->pages = (struct page **)(sg + pages_nr);

    /*
     * Pin the user pages into physical memory.  The write=1 flag tells
     * the kernel these pages may be written to (needed for C2H, but we
     * always request write permission for simplicity).
     */
    rv = get_user_pages_fast((unsigned long)buf, pages_nr,
                             1 /* write */, iocb->pages);
    if (rv < 0) {
        pr_err("slash: qdma: unable to pin down %u user pages, %d\n",
               pages_nr, rv);
        goto err_out;
    }
    if (rv != pages_nr) {
        pr_err("slash: qdma: unable to pin down all %u user pages, %d\n",
               pages_nr, rv);
        iocb->pages_nr = rv;
        rv = -EFAULT;
        goto err_out;
    }

    /*
     * Build the scatter-gather list.  Each entry describes one page's
     * worth of data.  The first page may have a non-zero offset, and
     * the last page may have fewer than PAGE_SIZE bytes.
     */
    sg = iocb->sgl;
    for (i = 0; i < pages_nr; i++, sg++) {
        unsigned int offset = offset_in_page(buf);
        unsigned int nbytes = min_t(unsigned int,
                                    PAGE_SIZE - offset, len);
        struct page *pg = iocb->pages[i];

        /* Ensure CPU cache is flushed so the DMA engine sees fresh data. */
        flush_dcache_page(pg);

        sg->next = sg + 1;
        sg->pg = pg;
        sg->offset = offset;
        sg->len = nbytes;
        sg->dma_addr = 0UL;

        buf += nbytes;
        len -= nbytes;
    }

    /* Terminate the linked list. */
    iocb->sgl[pages_nr - 1].next = NULL;
    iocb->pages_nr = pages_nr;
    return 0;

err_out:
    slash_qdma_unmap_user_buf(iocb, write);
    slash_qdma_iocb_release(iocb);

    return rv;
}

/**
 * slash_qdma_qpair_read_write() - Perform a DMA transfer via a qpair fd.
 * @file:  The anon_inode file for this queue pair.
 * @buf:   User-space buffer (source for write/H2C, destination for read/C2H).
 * @count: Number of bytes to transfer.
 * @ppos:  File position — used as the device-side (endpoint) address.
 *         Updated on success to reflect the bytes transferred, enabling
 *         sequential positional I/O.
 * @write: true for H2C (host-to-card write), false for C2H (card-to-host read).
 *
 * Transfer flow:
 *   1. Validate context and check that the required direction (H2C or C2H)
 *      is enabled on this queue pair.
 *   2. Pin user pages and build a scatter-gather list.
 *   3. Populate a qdma_request:
 *      - ep_addr = *ppos: the device-side address (FPGA memory offset).
 *      - h2c_eot = 1: signals end-of-transfer to the FPGA, allowing it to
 *        process the complete data packet.
 *      - timeout_ms = 10000 (10 seconds): if the transfer doesn't complete
 *        in this time, qdma_request_submit returns an error.
 *      - fp_done = NULL: synchronous mode — the call blocks until completion.
 *        If fp_done were set, libqdma would call it asynchronously.
 *      - dma_mapped = 0: libqdma handles the DMA mapping internally.
 *   4. Submit to libqdma via qdma_request_submit().
 *   5. On success, advance *ppos by the number of bytes transferred.
 *   6. Unpin pages and free the SGL.
 *
 * Return: Number of bytes transferred (>= 0) on success, negative errno
 *         on failure.
 */
static ssize_t slash_qdma_qpair_read_write(struct file *file, char __user *buf,
                                           size_t count, loff_t *ppos,
                                           bool write)
{
    struct slash_qdma_qpair_file_ctx *ctx = file->private_data;
    struct slash_qdma_dev *qdma_dev;
    struct slash_qdma_qpair_entry *entry;
    struct slash_qdma_io_cb iocb;
    struct qdma_request *req;
    unsigned long qhndl;
    ssize_t res;
    int rv;

    if (!ctx)
        return -EINVAL;

    qdma_dev = ctx->qdma_dev;
    entry = ctx->entry;

    if (!qdma_dev || !entry)
        return -ENODEV;

    /* Check device liveness and resolve the queue handle for the direction. */
    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }

    if (write) {
        /* H2C: writing data from host to card */
        if (!(entry->dir_mask & SLASH_QDMA_DIR_H2C) ||
            !slash_qdma_qhndl_is_valid(entry->qhndl[Q_H2C])) {
            mutex_unlock(&qdma_dev->lock);
            return -ENODEV;
        }
        qhndl = entry->qhndl[Q_H2C];
    } else {
        /* C2H: reading data from card to host */
        if (!(entry->dir_mask & SLASH_QDMA_DIR_C2H) ||
            !slash_qdma_qhndl_is_valid(entry->qhndl[Q_C2H])) {
            mutex_unlock(&qdma_dev->lock);
            return -ENODEV;
        }
        qhndl = entry->qhndl[Q_C2H];
    }
    mutex_unlock(&qdma_dev->lock);

    /* Pin user pages and build the scatter-gather list. */
    memset(&iocb, 0, sizeof(iocb));
    iocb.buf = buf;
    iocb.len = count;
    rv = slash_qdma_map_user_buf_to_sgl(&iocb, write);
    if (rv < 0)
        return rv;

    /* Populate the libqdma request structure. */
    req = &iocb.req;
    req->sgcnt = iocb.pages_nr;         /* Number of SGL entries */
    req->sgl = iocb.sgl;                /* Scatter-gather list */
    req->write = write ? 1 : 0;         /* Direction flag for libqdma */
    req->dma_mapped = 0;                /* Let libqdma handle DMA mapping */
    req->udd_len = 0;                   /* No user-defined data */
    req->ep_addr = (u64)*ppos;           /* Device-side (endpoint) address */
    req->count = count;                  /* Total byte count */
    req->timeout_ms = 10 * 1000;         /* 10-second timeout */
    req->fp_done = NULL;                 /* Synchronous: block until complete */
    req->h2c_eot = 1;                   /* End-of-transfer marker for FPGA */

    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "qdma_request_submit start: qid=%u qhndl=%lu write=%d count=%zu ep_addr=0x%llx\n",
                          ctx->qid, qhndl, req->write, req->count,
                          (unsigned long long)req->ep_addr);
    res = qdma_request_submit(qdma_dev->qdma_handle, qhndl, req);
    SLASH_QDMA_OP_DEV_LOG(&qdma_dev->pdev->dev,
                          "qdma_request_submit done: qid=%u qhndl=%lu res=%zd\n",
                          ctx->qid, qhndl, res);

    /* Advance the file position by the number of bytes transferred. */
    if (res > 0)
        *ppos += res;

    /* Unpin pages (marking dirty for C2H reads) and free the SGL. */
    slash_qdma_unmap_user_buf(&iocb, write);
    slash_qdma_iocb_release(&iocb);

    return res;
}

/**
 * slash_qdma_qpair_read() - Read (C2H) file operation for a qpair fd.
 * @file:  Anon_inode file for the queue pair.
 * @buf:   User-space destination buffer.
 * @count: Number of bytes to read.
 * @ppos:  Device-side address to read from.
 *
 * Thin wrapper that delegates to slash_qdma_qpair_read_write() with
 * write=false (C2H direction).
 *
 * Return: Bytes transferred or negative errno.
 */
static ssize_t slash_qdma_qpair_read(struct file *file, char __user *buf,
                                     size_t count, loff_t *ppos)
{
    return slash_qdma_qpair_read_write(file, buf, count, ppos, false);
}

/**
 * slash_qdma_qpair_write() - Write (H2C) file operation for a qpair fd.
 * @file:  Anon_inode file for the queue pair.
 * @buf:   User-space source buffer.
 * @count: Number of bytes to write.
 * @ppos:  Device-side address to write to.
 *
 * Thin wrapper that delegates to slash_qdma_qpair_read_write() with
 * write=true (H2C direction).
 *
 * Return: Bytes transferred or negative errno.
 */
static ssize_t slash_qdma_qpair_write(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    return slash_qdma_qpair_read_write(file, (char __user *)buf,
                                       count, ppos, true);
}

/**
 * slash_qdma_qpair_ioctl() - Ioctl handler for per-qpair anon_inode fds.
 * @file: Anon_inode file.
 * @cmd:  Ioctl command number.
 * @arg:  User-space argument.
 *
 * Currently a stub — no per-fd ioctls are defined.  Returns -ENOTTY
 * for all commands.
 *
 * Return: -ENOTTY (no valid ioctl).
 */
static long slash_qdma_qpair_ioctl(struct file *file,
                                   unsigned int cmd, unsigned long arg)
{
    (void)file;
    (void)cmd;
    (void)arg;

    return -ENOTTY;
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

    (void)inode;

    if (ctx) {
        if (ctx->entry)
            slash_qdma_qpair_put(ctx->entry);
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
 * slash_qdma_ioctl_qpair_get_fd_w() - Create an anon_inode fd for a queue pair.
 * @misc:     Miscdevice handle (unused).
 * @qdma_dev: QDMA device.
 * @uarg:     User-space pointer to a slash_qdma_qpair_fd_request struct.
 *
 * Creates an anonymous inode file descriptor that userspace can use
 * for read() (C2H) and write() (H2C) DMA transfers on the specified
 * queue pair.  The fd holds references to both the qpair entry and the
 * device, preventing either from being freed while the fd is open.
 *
 * The only supported flag is O_CLOEXEC (close-on-exec).
 *
 * The file is created with FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE
 * enabled, allowing pread/pwrite and lseek to set the device-side
 * address for DMA transfers.
 *
 * Error handling: on any failure after resources are acquired, all
 * refs and allocations are cleaned up before returning.
 *
 * Return: The new fd (>= 0) on success, negative errno on failure.
 */
static int slash_qdma_ioctl_qpair_get_fd_w(struct miscdevice *misc,
                                           struct slash_qdma_dev *qdma_dev,
                                           void __user *uarg)
{
    struct slash_qdma_qpair_fd_request req;
    __u32 user_size = 0;
    size_t copy_size;
    struct slash_qdma_qpair_entry *entry;
    struct slash_qdma_qpair_file_ctx *ctx;
    struct file *file;
    int fd;
    int err;

    (void)misc;

    if (copy_from_user(&user_size, uarg, sizeof(user_size)))
        return -EFAULT;

    memset(&req, 0, sizeof(req));

    if (copy_from_user(&req, uarg, min_t(size_t, user_size, sizeof(req))))
        return -EFAULT;

    /* Only O_CLOEXEC is a valid flag. */
    if (req.flags & ~O_CLOEXEC)
        return -EINVAL;

    /* Look up the qpair entry and take refs while holding the lock. */
    mutex_lock(&qdma_dev->lock);
    if (qdma_dev->hw_shutdown || !qdma_dev->have_qdma_handle) {
        mutex_unlock(&qdma_dev->lock);
        return -ENODEV;
    }

    entry = slash_qdma_qpair_lookup(qdma_dev, req.qid);
    if (!entry || !entry->dir_mask) {
        mutex_unlock(&qdma_dev->lock);
        return -ENOENT;
    }

    /*
     * Take a ref on the entry and the device.  These refs are held by
     * the file context and released when the fd is closed, ensuring
     * neither the entry nor the device can be freed prematurely.
     */
    slash_qdma_qpair_get(entry);
    kref_get(&qdma_dev->ref);
    mutex_unlock(&qdma_dev->lock);

    /* Allocate the per-fd context. */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx) {
        slash_qdma_qpair_put(entry);
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
        return -ENOMEM;
    }

    ctx->qdma_dev = qdma_dev;
    ctx->entry = entry;
    ctx->qid = req.qid;

    /* Create the anonymous inode file with read/write access. */
    file = anon_inode_getfile("slash_qdma_qpair", &slash_qdma_qpair_fops,
                              ctx, O_RDWR | (req.flags & O_CLOEXEC));
    if (IS_ERR(file)) {
        err = PTR_ERR(file);
        slash_qdma_qpair_put(entry);
        kref_put(&qdma_dev->ref, slash_qdma_dev_release);
        kfree(ctx);
        return err;
    }

    /* Enable seek and positional read/write for device-address control. */
    file->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;


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

/* ─────────────────────────────────────────────────────────────────────
 * Queue pair teardown helper
 * ───────────────────────────────────────────────────────────────────── */

/**
 * slash_qdma_qpair_teardown() - Fully remove a queue pair and its HW queues.
 * @qdma_dev: QDMA device.
 * @qid:      Queue pair ID.
 * @entry:    Queue pair entry to tear down.
 *
 * Must be called with @qdma_dev->lock held.
 *
 * Stops and removes all HW queues in the pair, invalidates all handles,
 * erases the entry from the xarray, and drops the xarray's ref on the
 * entry.  The entry itself is freed only when all references (including
 * any held by open anon_inode fds) have been released.
 */
/* Must be called with qdma_dev->lock held */
static void slash_qdma_qpair_teardown(struct slash_qdma_dev *qdma_dev, u32 qid,
                                      struct slash_qdma_qpair_entry *entry)
{
    unsigned int idx;

    if (!entry)
        return;

    /* Remove any queues that still exist */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++) {
        enum queue_type_t qtype = idx;

        if (entry->dir_mask & slash_qdma_qtype_to_dir(qtype))
            slash_qdma_ioctl_qpair_rm_q(&qdma_dev->misc, qdma_dev, entry, qtype);
    }

    /* Mark entry dead for any stale FDs */
    for (idx = 0; idx < SLASH_QDMA_QTYPE_COUNT; idx++)
        entry->qhndl[idx] = QDMA_QUEUE_IDX_INVALID;
    entry->dir_mask = 0;

    /* Drop from xarray and release ref */
    xa_erase(&qdma_dev->qpairs, qid);
    slash_qdma_qpair_put(entry);
}
