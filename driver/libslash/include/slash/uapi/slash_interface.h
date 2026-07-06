/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/**
 * Copyright (C) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * This file is dual-licensed: you may select either the GNU General Public
 * License version 2 (GPL-2.0-only) or the MIT License.  See the LICENSE
 * files in the repository root for the full text of each license.
 */

/**
 * @file slash_interface.h
 *
 * User-kernel ABI for the slash control device and QDMA subsystem.
 *
 * This header defines the ioctl structures and command numbers used by
 * libslash to communicate with the slash kernel module. It covers two
 * areas of functionality:
 *
 *   1. **Control device operations** — querying PCIe BAR information,
 *      obtaining file descriptors for BAR mappings, and retrieving
 *      device identity (BDF, vendor/device IDs).
 *
 *   2. **QDMA operations** — querying QDMA capabilities, adding/
 *      starting/stopping/deleting queue pairs, and obtaining file
 *      descriptors for queue pair I/O.
 *
 * All ioctl structs carry a leading `size` field for versioning: the
 * caller sets `size = sizeof(struct ...)` so the kernel can detect
 * older or newer userspace and handle compatibility.
 *
 * This file is shared between kernel and userspace (UAPI) and must
 * remain compatible with both build environments.
 */


#ifndef SLASH_UAPI_INTERFACE_H
#define SLASH_UAPI_INTERFACE_H

#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif /* __KERNEL__ */

/**
 * ioctl number allocation
 *
 * Numbers are chosen per the kernel's ioctl registry:
 *   https://www.kernel.org/doc/Documentation/userspace-api/ioctl/ioctl-number.rst
 *
 * The following codes are currently free:
 *   Code: 'v'
 *   Seq#: 30-BF
 *
 * We will aim to be good citizens and use a small range like 'v' 30-4F,
 * which is 32 ioctls. However, both these codes and the range used is
 * subject to change for future versions of the driver.
 */

/* ─────────────────────────────────────────────────────────────────────
 * Control device ioctls — BAR and device info
 * ───────────────────────────────────────────────────────────────────── */

/**
 * @brief Query information about a single PCIe BAR.
 */
struct slash_ioctl_bar_info {
    /**
     * Struct size for ABI versioning.  Caller must set to
     * sizeof(struct slash_ioctl_bar_info).
     */
    __u32 size;

    /* Userspace to kernel */
    __u8 bar_number;    /**< [in]  Which BAR to query (0–5). */

    /* Kernel to userspace */
    __u8 usable;        /**< [out] Non-zero if the BAR is present and usable. */
    __u8 in_use;        /**< [out] Non-zero if the BAR is currently mapped / claimed. */
    __u8 pad0;          /**< Padding for natural alignment. */

    __u64 start_address; /**< [out] Physical / bus start address of the BAR. */
    __u64 length;        /**< [out] Size of the BAR region in bytes. */
};

/**
 * @brief Obtain a file descriptor for a BAR.
 *
 * Userspace sends the desired \@bar_number and \@flags; the kernel returns
 * a new fd (via ioctl return convention) and fills in \@length.
 *
 * The actual fd is returned as the return value to the ioctl.
 */
struct slash_ioctl_bar_fd_request {
    __u32 size;          /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u8  bar_number;    /**< [in]  Which BAR to open. */
    __u8  pad0;          /**< Padding. */
    __u16 pad1;          /**< Padding. */

    __u32 flags;         /**< [in]  File descriptor flags.  Only O_CLOEXEC is honoured. */

    /* Kernel to userspace */
    __u64 length;        /**< [out] Size of the BAR region backing the returned fd. */
};

/** Maximum length (including NUL) of a PCI BDF string ("DDDD:BB:DD.F"). */
#define SLASH_PCI_BDF_LEN 32

/**
 * @brief Retrieve PCI identity of the device.
 */
struct slash_ioctl_device_info {
    __u32 size;                       /**< Struct size for ABI versioning. */

    /* Kernel to userspace */
    char bdf[SLASH_PCI_BDF_LEN];      /**< [out] PCI Bus/Device/Function string, NUL-terminated. */
    __u16 vendor_id;                  /**< [out] PCI vendor ID. */
    __u16 device_id;                  /**< [out] PCI device ID. */
    __u16 subsystem_vendor_id;        /**< [out] PCI subsystem vendor ID. */
    __u16 subsystem_device_id;        /**< [out] PCI subsystem device ID. */
};

/** Query BAR properties.  Fills the kernel-to-userspace fields of slash_ioctl_bar_info. */
#define SLASH_CTLDEV_IOCTL_GET_BAR_INFO _IOWR('v', 0x30, struct slash_ioctl_bar_info)

/** Obtain a mappable fd for a BAR region. */
#define SLASH_CTLDEV_IOCTL_GET_BAR_FD   _IOWR('v', 0x31, struct slash_ioctl_bar_fd_request)

/** Retrieve PCI identity strings and IDs for the device. */
#define SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO _IOWR('v', 0x32, struct slash_ioctl_device_info)


/* ─────────────────────────────────────────────────────────────────────
 * QDMA ioctls — DMA queue management
 * ───────────────────────────────────────────────────────────────────── */

/**
 * @brief Query QDMA subsystem capabilities.
 *
 * \@caps is reserved for future use; the kernel currently sets it to 0.
 *
 * \@bdf returns the PCI BDF string of the QDMA physical function (PF 1),
 * NUL-terminated, e.g. "0000:61:00.1".  It lets userspace discover the device's
 * PCI identity without reading /sys/ (needed by the emulation daemon, which
 * exposes no sysfs, and convenient for real-driver device pairing).
 */
struct slash_qdma_info {
    __u32 size;          /**< Struct size for ABI versioning. */

    /* Kernel to userspace */
    __u32 qsets_max;     /**< [out] Maximum number of queue sets the hardware supports. */
    __u32 msix_qvecs;    /**< [out] Number of MSI-X vectors available for queues. */
    __u32 vf_max;        /**< [out] Maximum number of virtual functions. */
    __u32 caps;          /**< [out] Capability bitmask. */
    char  bdf[SLASH_PCI_BDF_LEN]; /**< [out] PCI BDF string of the QDMA PF (PF 1),
                                   *         NUL-terminated, e.g. "0000:61:00.1". */
};

/**
 * @brief AXI-MM / NoC channel selection for a queue pair.
 *
 * Selects which CPM5 AXI-MM channel a queue pair uses.  libqdma mirrors the
 * channel into the SW-context host_id, which selects the programmed Host
 * Profile and hence the NoC channel.
 */
enum slash_qdma_mm_channel {
    SLASH_QDMA_MM_CHANNEL_AUTO = 0, /**< Stripe across channels by (qid & 1). */
    SLASH_QDMA_MM_CHANNEL_0    = 1, /**< Pin to AXI-MM/NoC channel 0. */
    SLASH_QDMA_MM_CHANNEL_1    = 2, /**< Pin to AXI-MM/NoC channel 1. */
};

/**
 * @brief Add (allocate) a new QDMA queue pair.
 *
 * \@mode must be one of:
 *   - QDMA_Q_MODE_MM (0) — AXI Memory Mapped mode.
 *   - QDMA_Q_MODE_ST (1) — AXI Streaming mode (not yet supported; returns -EOPNOTSUPP).
 *
 * \@dir_mask selects which directions to enable:
 *   - bit 0 (0x1) — H2C  (Host-to-Card).
 *   - bit 1 (0x2) — C2H  (Card-to-Host).
 *   - bit 2 (0x4) — CMPT (Completion queue; not yet supported, returns -EOPNOTSUPP).
 *
 * The ring size fields are hardware CSR table indices (valid range
 * 0–15), not byte or descriptor counts.  Each index selects a
 * pre-configured descriptor-ring depth from the global CSR ring-size
 * table (e.g. index 0 → 2049 descriptors, index 15 → 16385).
 */
struct slash_qdma_qpair_add {
    __u32 size;          /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u32 mode;          /**< [in]  Queue operating mode. */
    __u32 dir_mask;      /**< [in]  Direction bitmask — which directions to enable. */
    __u32 mm_channel;    /**< [in]  AXI-MM/NoC channel selection (enum slash_qdma_mm_channel). */

    __u32 h2c_ring_sz;   /**< [in]  Host-to-card descriptor ring size. */
    __u32 c2h_ring_sz;   /**< [in]  Card-to-host descriptor ring size. */
    __u32 cmpt_ring_sz;  /**< [in]  Completion ring size. */

    /* Kernel to userspace */
    __u32 qid;           /**< [out] Kernel-assigned queue pair ID. */
};

/**
 * Queue pair lifecycle operations, used in slash_qdma_qpair_op::op.
 *
 * The expected lifecycle of a queue pair is:
 *   ADD → START → (I/O) → STOP → DEL
 */
enum {
    SLASH_QDMA_QUEUE_OP_START,  /**< Start (activate) the queue pair. */
    SLASH_QDMA_QUEUE_OP_STOP,   /**< Stop (quiesce) the queue pair. */
    SLASH_QDMA_QUEUE_OP_DEL,    /**< Delete (free) the queue pair. */
};

/**
 * @brief Perform a lifecycle operation on a queue pair.
 */
struct slash_qdma_qpair_op {
    __u32 size; /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u32 qid;  /**< [in] Queue pair ID (as returned by slash_qdma_qpair_add). */
    __u32 op;   /**< [in] One of the SLASH_QDMA_QUEUE_OP_* constants. */
};

/**
 * @brief Maximum number of queue pairs a single transfer fd may own.
 *
 * A transfer fd is a collection of up to this many queue pairs (the intended
 * use is one per AXI-MM/NoC channel).  A single transfer ioctl issued on the
 * fd may fan a buffer transfer across all of them, running up to this many
 * hardware DMAs in parallel.  Each bound qpair keeps whatever settings it was
 * given at SLASH_QDMA_IOCTL_QPAIR_ADD time (mm_channel, ring sizes, etc.), so
 * the two channels can be configured independently.
 */
#define SLASH_QDMA_FD_MAX_QPAIRS 2u

/**
 * @brief Obtain a file descriptor for queue I/O.
 *
 * The returned fd is a collection of one or two queue pairs.  It can be used
 * for registered-buffer ioctls to transfer data through those queue pairs.
 *
 * The fd is returned as the ioctl return value (same convention as the BAR fd
 * ioctl).  Data movement is issued via SLASH_QDMA_QPAIR_IOCTL_TRANSFER, whose
 * sub-transfers select a bound queue pair by index and a direction (which must
 * have been enabled in \@dir_mask when that queue pair was added).
 *
 * Set \@qpair_count to the number of queue pairs to bind and list their IDs in
 * \@qpair_ids; the array index becomes the qpair_index used by
 * struct slash_qdma_subxfer.  For backward compatibility \@qpair_count == 0
 * binds the single queue pair named by \@qid.
 */
struct slash_qdma_qpair_fd_request {
    __u32 size;  /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u32 qid;   /**< [in] Legacy single queue pair ID; used only when
                  *        @qpair_count == 0. */
    __u32 flags; /**< [in] File descriptor flags.  Only O_CLOEXEC is honoured. */
    __u32 qpair_count; /**< [in] Number of valid entries in @qpair_ids
                        *        (1..SLASH_QDMA_FD_MAX_QPAIRS); 0 = use @qid. */
    __u32 qpair_ids[SLASH_QDMA_FD_MAX_QPAIRS]; /**< [in] Queue pair IDs bound to
                        *  this fd; the array index is the qpair_index. */
};

/**
 * @brief Transfer direction for a registered-buffer DMA transfer.
 */
enum slash_qdma_transfer_dir {
    SLASH_QDMA_XFER_H2C = 1, /**< Host-to-Card (write to device). */
    SLASH_QDMA_XFER_C2H = 2, /**< Card-to-Host (read from device). */
};

/**
 * @brief Advisory transfer topology for a registered QDMA buffer.
 *
 * The kernel returns this hint when a buffer is registered so userspace can
 * choose a suitable transfer strategy without hard-coding hardware-specific
 * scheduling policy.  The hint is advisory: transfers are still valid with any
 * queue pair whose direction and ownership checks pass.
 */
enum slash_qdma_transfer_hint {
    SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR = 1, /**< Prefer a single qpair (all traffic on one channel). */
    SLASH_QDMA_TRANSFER_HINT_V80          = 2, /**< Apply the V80 placement-aware channel policy. */
};

/**
 * @brief Create a kernel-owned DMA buffer and return a mappable fd.
 *
 * The kernel allocates @length bytes of host memory as a set of 4 KiB base
 * pages (not physically contiguous), builds the transfer scatter-gather list,
 * and DMA-maps every page once.  All of this expensive setup happens here, at
 * creation time, so the steady-state transfer path only slices the prebuilt
 * SGL, syncs the relevant pages, and submits.
 *
 * The new buffer is returned as an fd (via the ioctl return value, same
 * convention as the BAR/queue-pair fd ioctls).  Userspace maps it with
 * mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0) to obtain
 * a CPU pointer, and passes @buf_fd in struct slash_qdma_subxfer to move data.
 * The pages stay alive as long as either the fd or any mapping exists, and the
 * DMA mapping is torn down once both are gone and no transfer is in flight.
 *
 * The buffer is bound to the QDMA device of the fd it is created on (control
 * fd or queue-pair fd); transfers must use a queue-pair fd of the same device.
 *
 * \@length must be a non-zero multiple of the host page size.  The kernel
 * returns the page @granule (bytes per descriptor) and a @transfer_hint;
 * current SLASH hardware returns SLASH_QDMA_TRANSFER_HINT_V80.
 */
struct slash_qdma_buf_create {
    __u32 size;          /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u32 flags;         /**< [in]  File descriptor flags.  Only O_CLOEXEC is honoured. */
    __u64 length;        /**< [in]  Buffer length in bytes (page multiple). */

    /* Kernel to userspace */
    __u32 granule;       /**< [out] Bytes per SGL descriptor (host page size). */
    __u32 transfer_hint; /**< [out] enum slash_qdma_transfer_hint. */
};

/**
 * @brief One per-queue-pair sub-transfer within a transfer batch.
 *
 * Moves \@length bytes between the kernel buffer named by \@buf_fd at
 * \@buf_offset and the device endpoint address \@dev_addr, on the queue pair
 * selected by \@qpair_index (an index into the fd's bound qpairs).
 * \@buf_offset and \@length must be aligned to the buffer's 4 KiB page granule,
 * and \@buf_offset + \@length must not exceed the buffer length.  \@direction
 * must be one of enum slash_qdma_transfer_dir and must be enabled on the
 * selected queue pair.
 */
struct slash_qdma_subxfer {
    __u32 qpair_index; /**< [in] Index into the fd's bound qpairs. */
    __u32 direction;   /**< [in] enum slash_qdma_transfer_dir (H2C or C2H). */
    __s32 buf_fd;      /**< [in] Kernel buffer fd from SLASH_QDMA_IOCTL_BUF_CREATE. */
    __u32 pad0;        /**< Padding for natural alignment. */
    __u64 buf_offset;  /**< [in] Byte offset within the buffer. */
    __u64 dev_addr;    /**< [in] Device-side (endpoint) address. */
    __u64 length;      /**< [in] Number of bytes to transfer. */
};

/**
 * @brief Perform one or more buffer DMA sub-transfers in one call.
 *
 * Issued on a queue-pair I/O fd (from SLASH_QDMA_IOCTL_QPAIR_GET_FD).  The
 * kernel submits all \@count sub-transfers and waits for completion, running
 * those that target distinct queue pairs concurrently (so a single syscall can
 * drive both NoC channels in parallel).  The total number of bytes transferred
 * across all sub-transfers is returned as the ioctl return value.
 */
struct slash_qdma_transfer {
    __u32 size;        /**< Struct size for ABI versioning. */
    __u32 count;       /**< [in] Number of sub-transfers (1..SLASH_QDMA_FD_MAX_QPAIRS). */
    struct slash_qdma_subxfer xfers[SLASH_QDMA_FD_MAX_QPAIRS]; /**< [in] Sub-transfers. */
};

/** Query QDMA subsystem capabilities. */
#define SLASH_QDMA_IOCTL_INFO          _IOWR('v', 0x50, struct slash_qdma_info)

/** Allocate a new queue pair; returns assigned qid. */
#define SLASH_QDMA_IOCTL_QPAIR_ADD     _IOWR('v', 0x51, struct slash_qdma_qpair_add)

/** Start, stop, or delete an existing queue pair. */
#define SLASH_QDMA_IOCTL_Q_OP          _IOWR('v', 0x52, struct slash_qdma_qpair_op)

/** Obtain an I/O file descriptor for a queue pair. */
#define SLASH_QDMA_IOCTL_QPAIR_GET_FD  _IOWR('v', 0x53, struct slash_qdma_qpair_fd_request)

/**
 * Create a kernel-owned DMA buffer (allocate pages + build SGL + DMA-map once);
 * returns a mappable buffer fd as the ioctl return value.  May be issued on the
 * control device or a queue-pair I/O fd.
 */
#define SLASH_QDMA_IOCTL_BUF_CREATE    _IOWR('v', 0x54, struct slash_qdma_buf_create)

/* 'v' 0x55 is reserved (previously SLASH_QDMA_IOCTL_BUF_UNREGISTER, removed:
 * kernel buffers are released by closing their fd). */

/**
 * Perform a buffer DMA transfer.  Issued on a queue-pair I/O fd (not the
 * control device); returns the number of bytes transferred.
 */
#define SLASH_QDMA_QPAIR_IOCTL_TRANSFER _IOWR('v', 0x56, struct slash_qdma_transfer)

/**
 * io_uring command opcode (SQE cmd_op) for an asynchronous buffer transfer
 * batch, issued on a queue-pair I/O fd via IORING_OP_URING_CMD.  The SQE inline
 * command carries a single __u64: the userspace pointer to a struct
 * slash_qdma_transfer.  The completion CQE res holds the total bytes
 * transferred (>= 0) or a negative errno.  This path is optional and only
 * available on kernels with io_uring uring_cmd support.
 */
#define SLASH_QDMA_URING_CMD_TRANSFER 0x56u

#endif
