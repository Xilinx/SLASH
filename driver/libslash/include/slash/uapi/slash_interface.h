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
 */
struct slash_qdma_info {
    __u32 size;          /**< Struct size for ABI versioning. */

    /* Kernel to userspace */
    __u32 qsets_max;     /**< [out] Maximum number of queue sets the hardware supports. */
    __u32 msix_qvecs;    /**< [out] Number of MSI-X vectors available for queues. */
    __u32 vf_max;        /**< [out] Maximum number of virtual functions. */
    __u32 caps;          /**< [out] Capability bitmask. */
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
 * @brief Obtain a file descriptor for queue I/O.
 *
 * The returned fd can be used for read/write (or mmap) to transfer data
 * through the queue pair.
 *
 * The fd is returned as the ioctl return value (same convention as
 * the BAR fd ioctl).  A single fd is returned per queue pair;
 * read() on the fd performs C2H transfers and write() performs H2C
 * transfers, using whichever directions were enabled in \@dir_mask
 * when the queue pair was added.
 */
struct slash_qdma_qpair_fd_request {
    __u32 size;  /**< Struct size for ABI versioning. */

    /* Userspace to kernel */
    __u32 qid;   /**< [in] Queue pair ID. */
    __u32 flags; /**< [in] File descriptor flags.  Only O_CLOEXEC is honoured. */
};

/** Query QDMA subsystem capabilities. */
#define SLASH_QDMA_IOCTL_INFO          _IOWR('v', 0x50, struct slash_qdma_info)

/** Allocate a new queue pair; returns assigned qid. */
#define SLASH_QDMA_IOCTL_QPAIR_ADD     _IOWR('v', 0x51, struct slash_qdma_qpair_add)

/** Start, stop, or delete an existing queue pair. */
#define SLASH_QDMA_IOCTL_Q_OP          _IOWR('v', 0x52, struct slash_qdma_qpair_op)

/** Obtain an I/O file descriptor for a queue pair. */
#define SLASH_QDMA_IOCTL_QPAIR_GET_FD  _IOWR('v', 0x53, struct slash_qdma_qpair_fd_request)

#endif
