..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##########
Kernel ABI
##########

The SLASH kernel module (``slash.ko``) exposes AMD Alveo V80 FPGA cards to userspace through a set
of character devices. It drives two PCI physical functions per card and registers three categories
of device nodes: a per-card control device for BAR enumeration and MMIO access, a per-card QDMA
device for DMA queue management, and a single global hotplug device for PCIe lifecycle operations.

This reference specifies the ioctl ABI for all three device categories. It begins with data
conventions that apply uniformly across all devices, followed by a per-device chapter containing a
usage guide and a formal reference for each ioctl operation. Every ioctl entry follows the same
structure: a top-level description, the C interface definition, the ioctl direction, preconditions
on inputs, postconditions on outputs, and return values.

The module allocates one dynamic character-device major and creates a dedicated
``/sys/class/slash`` class. Device numbers within that major have a fixed layout:

- minor 0 is the global ``slash_hotplug`` device;
- board ``N`` uses minor ``2*N+1`` for ``slash_ctl<N>`` and minor ``2*N+2`` for
  ``slash_qdma_ctl<N>``;
- ``N`` is in the range 0–15, so one loaded module supports at most 16 cards.

PF1 and PF2 are matched by their board BDF (the ``DDDD:BB:SS`` portion) and share the same ``N``.
The board-to-``N`` assignment is retained while the module is loaded, including across PCI
remove/rescan cycles. Consequently the class entry name, ``/dev`` path, and device number
(``dev_t``) return unchanged after a remove/rescan. The major itself is dynamically allocated and
may change when the module is unloaded and loaded again.

``/dev/slash_ctl<N>`` / ``/sys/class/slash/slash_ctl_<BDF>/device``
    Provides BAR enumeration, MMIO access, and PCI device identity. Associated with PF2 (device ID
    ``10EE:50B6``). Examples: ``/dev/slash_ctl0``, ``/dev/slash_ctl1``,
    ``/sys/class/slash/slash_ctl_0000:61:00.2/device``.

``/dev/slash_qdma_ctl<N>`` / ``/sys/class/slash/slash_qdma_ctl_<BDF>/device``
    Manages DMA queue pairs for bulk data movement between host and card memory, as well as
    reconfiguration. Associated with PF1 (device ID ``10EE:50B5``). Examples: ``/dev/slash_qdma_ctl0``,
    ``/dev/slash_qdma_ctl1``, ``/sys/class/slash/slash_qdma_ctl_0000:61:00.1/device``.

``/dev/slash_hotplug`` / ``/sys/class/slash/slash_hotplug``
    A single global instance created at module load. Provides privileged control over the PCIe
    lifecycle of SLASH cards (remove, rescan, secondary bus reset).

Class entry names use the full function-level BDF, while ``DEVNAME`` in each entry's ``uevent``
file names the numeric ``/dev`` node. The entry's ``dev`` attribute and the ``st_rdev`` returned
by ``stat(2)`` on that node contain the same major and minor. Class entries are sysfs device
objects, not symlinks to ``/dev``.

During removal the affected class entry and ``/dev`` node disappear. An fd opened before removal
continues to refer to the old device instance and never rebinds to the rescanned instance;
device-specific ioctls on that old fd return ``-ENODEV``. After rescan, new opens through the stable
path reach the new instance.

Data Conventions
================

ABI Versioning
--------------

Every ioctl argument struct carries a leading ``__u32 size`` field. Callers must set
``size = sizeof(struct ...)`` before issuing the ioctl. The kernel reads ``size`` first, then
copies ``min(user_size, kernel_size)`` bytes in. Fields the kernel knows about but the caller's
older struct does not include are zero-filled. The response is written back for
``min(user_size, kernel_size)`` bytes; if ``user_size > kernel_size``, the kernel zero-fills the
extra tail via ``clear_user()``. This supports append-only struct extension within a coordinated
release; it is not a promise that arbitrary kernel-module and userspace-library releases can be
mixed. ``slash.ko``, libslash, vrtd/VRT, and v80-smi must come from the same SLASH release.

Error Handling
--------------

All ioctls return ``0`` on success or a negative errno on failure, except for some ioctls that use
the return value as a file descriptor (described below). The standard errno values are documented
under each ioctl. Unknown ioctl command numbers return ``-ENOTTY``.

Concurrency Model
=================

The intent is that all ioctls and ``read()``/``write()`` calls in this ABI are safe to invoke
concurrently from multiple threads or processes, on the same fd or on different fds. Concurrent
calls must never corrupt kernel state, and the kernel is expected to serialize internally where
necessary.

.. note::

   The current kernel driver is not exhaustively tested for concurrent access and bugs in this
   area may exist. Treat the safety property as an intent rather than a verified guarantee.

Conceptually, a queue pair is a sequential resource: the hardware processes one ``read()`` or
``write()`` on a given qpair at a time. The kernel serializes concurrent I/O on the same qpair,
so issuing ``read()``/``write()`` from multiple threads, or via multiple ``QPAIR_GET_FD`` fds for
the same qpair, is safe but offers no throughput benefit over a single-threaded caller. For
parallel I/O, allocate multiple qpairs via ``QPAIR_ADD`` and distribute transfers across them.

Hotplug ioctls from multiple processes serialize on ``pci_lock_rescan_remove()``.
``TOGGLE_SBR`` drops this lock before calling ``pci_bridge_secondary_bus_reset()`` to avoid
deadlock with the PCI slot lock.

Card information and BARs: ``/dev/slash_ctl<N>``
================================================

The control device provides two services. First, BAR enumeration and access: callers query which
of the card's PCIe BARs are present and usable, then obtain a dma-buf fd for each BAR they wish to
memory-map for direct MMIO register access. Second, device identity: callers read the card's PCI
BDF string and vendor/device IDs to correlate the control device with a physical board and with the
matching QDMA control device.

- **Device file name:** ``/dev/slash_ctl<N>`` (e.g. ``/dev/slash_ctl0``)
- **Sysfs name:** ``slash_ctl_<PCI-BDF>`` (e.g., ``/sys/class/slash/slash_ctl_0000:61:00.2``)
- **Associated PCI function:** PF2, device ID ``10EE:50B6``
- **Permissions:** ``0600`` (owner read/write)
- **Creation:** one per card, created when PF2 is probed during module load or PCI rescan
- **File operations:** ``open``, ``release``, and ``ioctl`` — no ``read``, ``write``, or ``mmap``
  on this fd itself. MMIO access is through a dma-buf fd returned by an ioctl.

Usage
-----

Querying Device Information
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Before accessing BARs, callers typically identify the card and enumerate its available BARs using
``SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO`` and ``SLASH_CTLDEV_IOCTL_GET_BAR_INFO``. The device info
ioctl returns the BDF string and PCI IDs, which correlate this control device with the matching
QDMA device at the same BDF (function 1). The BAR info ioctl reports per-BAR metadata: whether
the BAR is present and usable for MMIO, its physical address, and its size.

.. code-block:: c

    /* Query PCI identity */
    struct slash_ioctl_device_info dev_info = { .size = sizeof(dev_info) };
    ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &dev_info);
    /* dev_info.bdf → e.g. "0000:61:00.2" */
    /* dev_info.vendor_id == 0x10EE, dev_info.device_id == 0x50B6 */

    /* Enumerate all six BARs */
    for (int i = 0; i < 6; i++) {
        struct slash_ioctl_bar_info bar_info = {
            .size       = sizeof(bar_info),
            .bar_number = i,
        };
        ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &bar_info);
        if (bar_info.usable)
            printf("BAR%d: addr=0x%016llx length=0x%llx\n",
                   i, bar_info.start_address, bar_info.length);
    }

BAR Access and MMIO
~~~~~~~~~~~~~~~~~~~

Each PCIe BAR is accessed through a dma-buf fd obtained from ``SLASH_CTLDEV_IOCTL_GET_BAR_FD``.
The fd is mapped with ``mmap()`` to obtain a pointer for direct MMIO register access. All reads
and writes through that pointer must be bracketed with ``DMA_BUF_IOCTL_SYNC`` calls on the
dma-buf fd to ensure correct memory ordering.

.. code-block:: c

    #include <linux/dma-buf.h>

    /* Obtain a dma-buf fd for BAR 0 — return value is the fd, not 0 */
    struct slash_ioctl_bar_fd_request req = {
        .size       = sizeof(req),
        .bar_number = 0,
        .flags      = O_CLOEXEC,
    };
    int bar_fd = ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
    /* req.length is now filled with the BAR size */

    void *mmio = mmap(NULL, req.length, PROT_READ | PROT_WRITE, MAP_SHARED, bar_fd, 0);

    /* MMIO write: bracket with SYNC_WRITE */
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
    ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);
    /* ... MMIO writes via mmio pointer ... */
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);

    /* MMIO read: same pattern with SYNC_READ */
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);
    /* ... MMIO reads via mmio pointer ... */
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);

    /* Teardown */
    munmap(mmio, req.length);
    close(bar_fd);

BAR mapping is **not inherited across** ``fork()``. Each child process that needs MMIO access must
obtain its own dma-buf fd via ``GET_BAR_FD``.

After a device is removed from the PCI hierarchy, mapped BAR regions remain accessible in virtual
memory. However, all physical accesses will return ``0xFFFFFFFF`` (PCIe completion timeout) and
writes are silently discarded.

After a device is removed from the PCI hierarchy, mapped BAR regions remain accessible in virtual
memory but their behavior is undefined. Userspace should treat the mapping as invalid after removal..

IOCTL Reference
---------------

All control device ioctls use magic byte ``'v'`` (``0x76``) and sequence numbers ``0x30``–``0x32``.

``SLASH_CTLDEV_IOCTL_GET_BAR_INFO``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reads BAR metadata for a single BAR index, reporting whether the BAR is present and usable for
MMIO access along with its physical address and size.

**Interface:**

.. code-block:: c

    #define SLASH_CTLDEV_IOCTL_GET_BAR_INFO _IOWR('v', 0x30, struct slash_ioctl_bar_info)

    struct slash_ioctl_bar_info {
        __u32 size;           /* [in/out] ABI version: set to sizeof(struct) */
        __u8  bar_number;     /* [in]  BAR index to query: 0–5 */
        __u8  usable;         /* [out] Non-zero if BAR is present and is MMIO */
        __u8  in_use;         /* [out] Always 0 in current implementation */
        __u8  pad0;           /* padding */
        __u64 start_address;  /* [out] Physical/bus start address of the BAR */
        __u64 length;         /* [out] Size of the BAR in bytes */
    };

**Direction:** ``_IOWR`` — userspace writes ``bar_number`` (and ``size``); the kernel writes back
``usable``, ``in_use``, ``start_address``, and ``length``.

**Preconditions:**

- ``size`` must cover at least ``length``
- ``bar_number`` must be in ``[0, 5]``

**Postconditions:**

- ``usable`` = 1 if the BAR has a non-zero start address and is ``IORESOURCE_MEM`` (MMIO type)
- ``in_use`` = 0 (reserved for future use; never set in current implementation)
- ``start_address`` = physical bus address
- ``length`` = BAR size in bytes

**Return values:**

- ``0`` — success
- ``-EFAULT`` — bad userspace pointer in ``copy_from_user`` or ``copy_to_user``
- ``-EINVAL`` — ``size`` too small, or ``bar_number`` out of ``[0, 5]``

``SLASH_CTLDEV_IOCTL_GET_BAR_FD``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Returns a new dma-buf file descriptor for the named BAR. The fd can be passed to ``mmap()`` to
obtain a pointer for direct MMIO access. The BAR size is reported back in ``length``. The fd is
returned as the ``ioctl()`` return value.

**Interface:**

.. code-block:: c

    #define SLASH_CTLDEV_IOCTL_GET_BAR_FD _IOWR('v', 0x31, struct slash_ioctl_bar_fd_request)

    struct slash_ioctl_bar_fd_request {
        __u32 size;        /* [in/out] ABI version */
        __u8  bar_number;  /* [in]  BAR index: 0–5 */
        __u8  pad0;        /* padding */
        __u16 pad1;        /* padding */
        __u32 flags;       /* [in]  fd flags: only O_CLOEXEC is honoured */
        __u64 length;      /* [out] Size of the BAR in bytes */
    };

**Direction:** ``_IOWR`` — userspace writes ``bar_number`` and ``flags``; the kernel writes back
``length`` and returns the new fd as the ``ioctl()`` return value (not as a struct field).

**Preconditions:**

- ``size`` must cover at least ``length``
- ``bar_number`` in ``[0, 5]``
- ``flags & ~O_CLOEXEC == 0`` (any other flag bits cause ``-EINVAL``)
- The specified BAR must be a usable MMIO BAR (must have an active dma-buf exporter)

**Postconditions:**

- The return value is a non-negative fd number on success.
- The fd refers to a dma-buf exporter for the named BAR and can be passed to ``mmap()``.
- ``length`` is filled with the BAR size; callers use this to size the ``mmap()`` call.

**Return values:**

- ``>= 0`` — file descriptor (success)
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small, ``bar_number`` out of range, or unsupported ``flags`` bits
- ``-ENODEV`` — BAR has no dma-buf exporter (BAR not present or not MMIO)
- Other negative errno from ``dma_buf_fd()``

``SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reads the PCI identity fields of the accessed card. Callers use this to correlate a control device
fd with a physical board and with the matching QDMA control device.

**Interface:**

.. code-block:: c

    #define SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO _IOWR('v', 0x32, struct slash_ioctl_device_info)

    #define SLASH_PCI_BDF_LEN 32

    struct slash_ioctl_device_info {
        __u32 size;                   /* [in/out] ABI version */
        char  bdf[SLASH_PCI_BDF_LEN]; /* [out] PCI BDF string, NUL-terminated, e.g. "0000:61:00.2" */
        __u16 vendor_id;              /* [out] PCI vendor ID (0x10EE for AMD/Xilinx) */
        __u16 device_id;              /* [out] PCI device ID (0x50B6 for PF2) */
        __u16 subsystem_vendor_id;    /* [out] PCI subsystem vendor ID */
        __u16 subsystem_device_id;    /* [out] PCI subsystem device ID */
    };

**Direction:** ``_IOWR`` — userspace writes ``size``; the kernel writes back all output fields.

**Preconditions:**

- ``size`` must cover at least the ``size`` field itself (``size >= sizeof(__u32)``) — otherwise
  ``-EINVAL``. This ioctl carries no input fields beyond ``size``, so the minimum is just the
  ``size`` field on its own.

**Postconditions:**

- The output is truncated to ``min(size, sizeof(struct))`` bytes. Fields whose tail lies beyond
  the user-supplied ``size`` are not written; the corresponding bytes in the user buffer are left
  untouched.
- If ``size > sizeof(struct)``, the trailing bytes of the user buffer are zero-filled.
- Within the written range, all output fields are populated and ``bdf`` is a NUL-terminated string
  in ``DDDD:BB:SS.F`` format with full domain.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small (below ``sizeof(__u32)``)

Memory transfers via QDMA: ``/dev/slash_qdma_ctl<N>``
=====================================================

The QDMA device manages DMA queue pairs for bulk data movement between host memory and the card's
on-board memory (HBM or DDR). Each queue pair is allocated with a mode (currently only MM) and a
direction mask, then started before use. An anon-inode fd obtained from the queue pair serves as
the transfer channel: host buffers are registered once, and transfer ioctls name the registered
buffer, buffer offset, device-side physical address, length, and direction.

- **Device file name:** ``/dev/slash_qdma_ctl<N>`` (e.g. ``/dev/slash_qdma_ctl0``)
- **Sysfs name:** ``slash_qdma_ctl_<PCI-BDF>`` (e.g. ``/sys/class/slash/slash_qdma_ctl_0000:61:00.1``)
- **Associated PCI function:** PF1, device ID ``10EE:50B5``
- **Permissions:** ``0600``
- **Creation:** one per card, created when PF1 is probed
- **File operations:** ``open``, ``release``, ``ioctl`` on the control fd. DMA I/O is done on
  per-qpair anon-inode fds returned by an ioctl.

The QDMA and control functions use the same board-to-``N`` map, so
``slash_qdma_ctl<N>`` is always paired with ``slash_ctl<N>``.

Usage
-----

In order to transfer data via QDMA, a queue pair must be added, started, and an I/O fd needs
to be created. The I/O fd is ioctl-only for data movement: userspace registers a host buffer,
then issues transfer ioctls that name the registered buffer, buffer offset, device-side address,
length, and direction. Full lifecycle:

.. code-block:: c

    /* Step 1: Add queue pair (MM mode, bidirectional) */
    struct slash_qdma_qpair_add add = {
        .size         = sizeof(add),
        .mode         = 0,    /* QDMA_Q_MODE_MM */
        .dir_mask     = 0x3,  /* H2C | C2H */
        .h2c_ring_sz  = 0,
        .c2h_ring_sz  = 0,
        .cmpt_ring_sz = 0,
    };
    ioctl(qdma_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add);
    uint32_t qid = add.qid;

    /* Step 2: Start the queue pair */
    struct slash_qdma_qpair_op op = { .size = sizeof(op), .qid = qid, .op = 0 };
    ioctl(qdma_fd, SLASH_QDMA_IOCTL_Q_OP, &op);  /* START */

    /* Step 3: Obtain I/O fd */
    struct slash_qdma_qpair_fd_request fd_req = {
        .size = sizeof(fd_req), .qid = qid, .flags = O_CLOEXEC
    };
    int io_fd = ioctl(qdma_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &fd_req);

    /* Step 4: Create a kernel-owned DMA buffer and mmap it for CPU access.
     * The buffer fd is returned by the ioctl; the kernel allocated the pages,
     * built the SGL, and DMA-mapped everything once. */
    struct slash_qdma_buf_create bc = { .size = sizeof(bc), .length = nbytes };
    int buf_fd = ioctl(io_fd, SLASH_QDMA_IOCTL_BUF_CREATE, &bc);
    void *host_buf = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                          buf_fd, 0);

    /* Step 5: H2C transfer to device address 0x4000000000.  The transfer
     * carries an array of per-qpair sub-transfers; a single-channel fd uses
     * one sub-transfer with qpair_index 0. */
    struct slash_qdma_transfer xfer = {
        .size = sizeof(xfer),
        .count = 1,
        .xfers[0] = {
            .qpair_index = 0,
            .direction = SLASH_QDMA_XFER_H2C,
            .buf_fd = buf_fd,
            .buf_offset = 0,
            .dev_addr = 0x4000000000LL,
            .length = nbytes,
        },
    };
    ioctl(io_fd, SLASH_QDMA_QPAIR_IOCTL_TRANSFER, &xfer);

    /* Step 6: C2H transfer from device address 0x4000000000 */
    xfer.xfers[0].direction = SLASH_QDMA_XFER_C2H;
    ioctl(io_fd, SLASH_QDMA_QPAIR_IOCTL_TRANSFER, &xfer);

    /* Step 7: Teardown — closing the buffer fd (after munmap) releases it. */
    munmap(host_buf, nbytes);
    close(buf_fd);
    close(io_fd);
    op.op = 1;  ioctl(qdma_fd, SLASH_QDMA_IOCTL_Q_OP, &op);  /* STOP */
    op.op = 2;  ioctl(qdma_fd, SLASH_QDMA_IOCTL_Q_OP, &op);  /* DEL */

The qpair fd does **not** support ``read``, ``write``, ``pread``, ``pwrite``, ``mmap``,
``poll``/``select``, or ``splice`` for data movement.  Buffer fds returned by
``SLASH_QDMA_IOCTL_BUF_CREATE`` **are** mappable with ``mmap`` (full length,
offset 0).

All transfers are synchronous and block until the transfer completes or times out. The timeout is
**10 seconds**; after expiry the call returns ``-ETIME``. Partial transfers are possible; the
return value is the number of bytes transferred, and the file position is advanced accordingly.

The buffer offset must be aligned to the buffer's page granule.  The transfer
length is an exact byte count: it must be non-zero and may end within the final
page. The transfer is backed by 4 KiB base pages, with the driver clipping the
final descriptor when the requested length is not page-multiple.
Non-page-multiple transfers are supported for exact byte-stream use cases such
as PDI programming, but they are not the speed-optimised path. Performance
critical application buffers should pad transfer lengths to full 4 KiB page
multiples whenever possible.

Multiple fds can be obtained for the same qpair via multiple ``QPAIR_GET_FD`` calls, including
from different processes. Concurrent ``read()``/``write()`` calls on the same qpair (from any
fd or thread) are serialized by the kernel and execute one at a time; for parallel I/O, allocate
additional qpairs via ``QPAIR_ADD``. See the `Concurrency Model`_ section for the full safety
contract and its current testing caveat.

The following errno values can be returned by ``read()`` and ``write()`` on the I/O fd:

.. list-table::
   :header-rows: 1

   * - Return value
     - Condition
   * - ``>= 0``
     - Bytes transferred (success; partial transfer is possible)
   * - ``-ENODEV``
     - Device shutting down, or the required direction is not enabled for this qpair
   * - ``-EINVAL``
     - Zero-length, unaligned, or out-of-range transfer
   * - ``-ENOMEM``
     - SGL allocation failure
   * - ``-EFAULT``
     - ``get_user_pages_fast`` returned fewer pages than needed (bad userspace buffer)
   * - ``-ETIME``
     - 10-second DMA timeout
   * - Other libqdma errors
     - Propagated from ``qdma_request_submit()``

Device Address Map
~~~~~~~~~~~~~~~~~~

The queue pair fd treats the file position as the device-side physical address in the
`16 TB NoC Interconnect Address Map`_. Within this address map, there are three particular
regions of interest:

.. _16 TB NoC Interconnect Address Map: https://docs.amd.com/r/en-US/am011-versal-acap-trm/16-TB-NoC-Interconnect-Address-Map

.. list-table::
   :header-rows: 1

   * - Region
     - Base
     - End (exclusive)
     - Direction
   * - HBM (64 pseudo-channels)
     - ``0x0000004000000000``
     - ``0x0000004800000000``
     - H2C and C2H
   * - DDR
     - ``0x0000060000000000``
     - ``0x0000060800000000``
     - H2C and C2H
   * - Bitstream / PDI input region
     - ``0x0000000102100000``
     - ``0x0000000142100000``
     - H2C only

FPGA Programming
~~~~~~~~~~~~~~~~

FPGA programming (loading a new bitstream/PDI) is performed as a DMA write to the bitstream
programming region (``0x102100000``) over an H2C-only MM queue pair.

IOCTL Reference
---------------

All QDMA control device ioctls use magic byte ``'v'`` (``0x76``) and sequence numbers
``0x50``-``0x53``.

Every QDMA ioctl returns ``-ENODEV`` immediately if the hardware is shutting down (``hw_shutdown``
flag set) or the QDMA handle is not open.

``SLASH_QDMA_IOCTL_INFO``
~~~~~~~~~~~~~~~~~~~~~~~~~

Queries the QDMA device's PCI identity and capabilities. ``bdf`` is always the full PF1 BDF and can
be matched with a control device by comparing the ``DDDD:BB:SS`` board portion. The capability
fields are reserved for future reporting and are currently zero.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_IOCTL_INFO _IOWR('v', 0x50, struct slash_qdma_info)

    struct slash_qdma_info {
        __u32 size;                   /* [in/out] ABI version */
        char  bdf[SLASH_PCI_BDF_LEN]; /* [out] Full PF1 BDF, e.g. "0000:61:00.1" */
        __u32 qsets_max;              /* [out] Max queue sets (currently 0) */
        __u32 msix_qvecs;             /* [out] Queue MSI-X vectors (currently 0) */
        __u32 vf_max;                 /* [out] Max VFs (currently 0) */
        __u32 caps;                   /* [out] Capability bitmask (currently 0) */
    };

**Direction:** ``_IOWR`` — userspace writes ``size``; the kernel writes back all output fields.

**Preconditions:**

- ``size`` must cover at least the ``size`` field itself (``size >= sizeof(__u32)``) — otherwise
  ``-EINVAL``. This ioctl carries no input fields beyond ``size``, so the minimum is just the
  ``size`` field on its own.

**Postconditions:**

- ``bdf`` is a NUL-terminated ``DDDD:BB:SS.1`` string with the full PCI domain.
- ``qsets_max``, ``msix_qvecs``, ``vf_max``, and ``caps`` are set to 0 in the current
  implementation.
- The output is truncated to ``min(size, sizeof(struct))`` bytes. Fields whose tail lies beyond
  the user-supplied ``size`` are not written; the corresponding bytes in the user buffer are left
  untouched.
- If ``size > sizeof(struct)``, the trailing bytes of the user buffer are zero-filled.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small (below ``sizeof(__u32)``)
- ``-ENODEV`` — device shutting down or QDMA handle not open

``SLASH_QDMA_IOCTL_QPAIR_ADD``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Allocates a new queue pair on the device. On success, the kernel-assigned queue pair ID (``qid``)
is returned in the struct and is used for all subsequent operations on this queue pair.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_IOCTL_QPAIR_ADD _IOWR('v', 0x51, struct slash_qdma_qpair_add)

    struct slash_qdma_qpair_add {
        __u32 size;          /* [in/out] ABI version */
        __u32 mode;          /* [in]  Queue mode: 0=MM (Memory Mapped), 1=ST (Streaming, not yet supported) */
        __u32 dir_mask;      /* [in]  Direction bitmask (see below) */
        __u32 mm_channel;    /* [in]  AXI-MM/NoC channel selection: 0=auto, 1=channel 0, 2=channel 1 */
        __u32 h2c_ring_sz;   /* [in]  H2C descriptor ring CSR table index: 0–15 */
        __u32 c2h_ring_sz;   /* [in]  C2H descriptor ring CSR table index: 0–15 */
        __u32 cmpt_ring_sz;  /* [in]  Completion ring CSR table index: 0–15 */
        __u32 qid;           /* [out] Kernel-assigned queue pair ID */
        __u32 aperture_size; /* [in]  0=linear MM addressing, non-zero=keyhole aperture size */
    };

Direction bitmask bits:

.. list-table::
   :header-rows: 1

   * - Bit
     - Value
     - Meaning
   * - 0
     - ``0x1``
     - H2C (host-to-card, write)
   * - 1
     - ``0x2``
     - C2H (card-to-host, read)
   * - 2
     - ``0x4``
     - CMPT (completion queue; not yet supported)

Ring size fields are QDMA Control and Status Register (CSR) table indices (0–15), not raw
descriptor counts. Index 0 maps to approximately 2049 descriptors; index 15 to approximately
16385. The caller does not control the actual descriptor count directly.

``aperture_size`` controls libqdma keyhole mode for memory-mapped queues.  A value of ``0``
keeps endpoint addressing linear and is the normal setting for DDR/HBM application buffers.
A non-zero power-of-two value enables keyhole mode: endpoint addresses wrap within that byte
aperture as the transfer advances.  Keyhole queues are intended for special endpoints such as
the PDI design-writer ingress path; ordinary application queues should leave this field ``0``.

**Direction:** ``_IOWR`` — userspace writes ``mode``, ``dir_mask``, ``mm_channel``, ring size
indices, and optionally ``aperture_size``; the kernel writes back ``qid``.

**Preconditions:**

- ``size`` must cover at least ``cmpt_ring_sz`` (the trailing input field) — otherwise ``-EINVAL``
- ``dir_mask`` must be non-zero and contain only bits ``[0, 1]``; bit 2 (CMPT) is not yet
  supported
- ``mode`` must be 0 (MM); streaming mode (1) is not yet supported
- ``mm_channel`` must be 0 (auto), 1 (channel 0), or 2 (channel 1)
- All ring size indices must be in ``[0, 15]``
- ``aperture_size`` must be 0 (linear addressing) or a power-of-two keyhole aperture size
- At most 256 concurrent queue pairs per device. The actual ceiling is lower in practice and
  depends on how many queues libqdma's resource manager makes available to the calling process
  (the 256-slot pool is shared across all PCI functions of the device).

**Postconditions:**

- ``qid`` is filled with the kernel-assigned ID (0–255), used for all subsequent operations on
  this queue pair. If ``size`` is too small to cover ``qid``, the field is silently dropped on the
  write-back but the qpair is still created — callers should always supply ``size = sizeof(struct)``
  so they can recover the assigned id.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small, or invalid ``dir_mask``, ``mode``, or ring size index
- ``-EOPNOTSUPP`` — streaming mode or completion queue requested (not yet supported)
- ``-ENOMEM`` — allocation failure
- ``-EBUSY`` — no qpair IDs available (the per-process queue ceiling has been reached)
- ``-ENODEV`` — device shutting down
- Other negative errno from libqdma's ``qdma_queue_add()``

``SLASH_QDMA_IOCTL_Q_OP``
~~~~~~~~~~~~~~~~~~~~~~~~~

Performs a lifecycle operation (start, stop, or delete) on an existing queue pair. The expected
lifecycle is: ``ADD → START → [I/O via qpair fd] → STOP → DEL``.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_IOCTL_Q_OP _IOWR('v', 0x52, struct slash_qdma_qpair_op)

    struct slash_qdma_qpair_op {
        __u32 size; /* [in/out] ABI version */
        __u32 qid;  /* [in]     Queue pair ID from QPAIR_ADD */
        __u32 op;   /* [in]     Operation: 0=START, 1=STOP, 2=DEL */
    };

Operations:

.. list-table::
   :header-rows: 1

   * - ``op``
     - Constant
     - Effect
   * - 0
     - ``SLASH_QDMA_QUEUE_OP_START``
     - Activates all HW queues in the pair. Must be called before any I/O.
   * - 1
     - ``SLASH_QDMA_QUEUE_OP_STOP``
     - Quiesces all HW queues. Required before DEL (but DEL implies STOP).
   * - 2
     - ``SLASH_QDMA_QUEUE_OP_DEL``
     - Removes all HW queues and releases the qpair entry from the xarray.

DEL is safe to call on a running queue (the kernel will stop it first), so an explicit STOP before
DEL is not strictly required but is the recommended sequence. After DEL, the qpair ID may be reused
by a subsequent ``QPAIR_ADD``. Any open anon-inode fds obtained via ``QPAIR_GET_FD`` still hold a
ref on the entry; they remain valid until closed, but the underlying hardware queues will have been
removed.

**Direction:** ``_IOWR`` — userspace writes ``qid`` and ``op``; no kernel-to-userspace data.

**Preconditions:**

- ``size`` must cover at least ``op`` (the trailing input field) — otherwise ``-EINVAL``
- ``op`` must be in ``[0, 2]``
- ``qid`` must refer to an existing queue pair

**Postconditions:**

- On START: all HW queues in the pair are active; I/O on the qpair fd is possible.
- On STOP: all HW queues are quiesced.
- On DEL: the qpair entry is removed from the xarray; the ``qid`` may be reused.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small, or ``op`` value not in ``[0, 2]``
- ``-ENOENT`` — ``qid`` not found in the device's xarray
- ``-ENODEV`` — device shutting down
- Other negative errno from libqdma queue start, stop, or remove

``SLASH_QDMA_IOCTL_QPAIR_GET_FD``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Creates a new file descriptor for data transfer.  The fd is a **collection of one or two queue
pairs** (typically one per AXI-MM/NoC channel): a transfer issued on it selects a bound queue pair
by index, so one transfer ioctl can fan across both channels.  The returned fd is ioctl-only for
data movement: it supports buffer register/unregister and transfer ioctls, but not ``read``,
``write``, ``pread``, ``pwrite``, ``mmap``, ``poll``/``select``, or ``splice`` (an optional
``io_uring`` ``uring_cmd`` async transfer path is available on capable kernels).  Multiple fds can
be obtained for the same qpair(s) via multiple calls.  The fd is returned as the ``ioctl()`` return
value.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_FD_MAX_QPAIRS 2u

    #define SLASH_QDMA_IOCTL_QPAIR_GET_FD _IOWR('v', 0x53, struct slash_qdma_qpair_fd_request)

    struct slash_qdma_qpair_fd_request {
        __u32 size;        /* [in/out] ABI version */
        __u32 qid;         /* [in]     Legacy single qpair ID; used when qpair_count == 0 */
        __u32 flags;       /* [in]     fd flags: only O_CLOEXEC is honoured */
        __u32 qpair_count; /* [in]     Number of qpair_ids (1..SLASH_QDMA_FD_MAX_QPAIRS); 0 = use qid */
        __u32 qpair_ids[SLASH_QDMA_FD_MAX_QPAIRS]; /* [in] qpair IDs; index == qpair_index */
    };

**Direction:** ``_IOWR`` — userspace writes the qpair selection and ``flags``; the kernel returns
the new fd as the ``ioctl()`` return value (not as a struct field).

**Preconditions:**

- ``size`` must cover at least ``flags`` (the trailing input field of the legacy form) — otherwise ``-EINVAL``
- The selected queue pairs must exist and be non-empty (``qpair_count == 0`` selects the single ``qid``)
- ``qpair_count`` must not exceed ``SLASH_QDMA_FD_MAX_QPAIRS``
- ``flags & ~O_CLOEXEC == 0`` (any other bits cause ``-EINVAL``)
- The queue pairs should be in the started state for I/O to work
- Each bound qpair keeps the per-qpair configuration (``mm_channel``, ``aperture_size``, ring sizes,
  directions) it was given at ``QPAIR_ADD`` time, so the two channels can be configured independently

**Postconditions:**

- The return value is a non-negative fd number on success.
- The fd holds a reference on the qpair entry, device, and the client context that owns registered
  buffers; neither can be freed while this fd is open.

**Return values:**

- ``>= 0`` — file descriptor (success)
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small, or unsupported ``flags`` bits
- ``-ENOENT`` — ``qid`` not found or qpair is empty
- ``-ENODEV`` — device shutting down
- ``-ENOMEM`` — allocation failure
- Other negative errno from ``anon_inode_getfile()`` or ``get_unused_fd_flags()``

``SLASH_QDMA_IOCTL_BUF_CREATE``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Creates a kernel-owned DMA buffer and returns a mappable fd for it. The ioctl may be issued on
either the QDMA control fd or a qpair fd of the same device. The kernel allocates ``length`` bytes
as a set of 4 KiB base pages (not physically contiguous), builds the transfer scatter-gather list,
and DMA-maps every page **once** — so the steady-state transfer path only slices the prebuilt SGL,
syncs the touched pages, and submits. Userspace maps the returned fd with ``mmap`` to obtain a CPU
pointer and passes the fd in ``struct slash_qdma_subxfer`` to move data. The buffer is bound to the
fd's QDMA device; transfers must use a qpair fd of that same device.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_IOCTL_BUF_CREATE _IOWR('v', 0x54, struct slash_qdma_buf_create)

    struct slash_qdma_buf_create {
        __u32 size;          /* [in/out] ABI version */
        __u32 flags;         /* [in]  Only O_CLOEXEC is honoured */
        __u64 length;        /* [in]  Buffer length in bytes (page multiple) */
        __u32 granule;       /* [out] Bytes per SGL descriptor (host page size) */
        __u32 transfer_hint; /* [out] enum slash_qdma_transfer_hint */
    };

**Direction:** ``_IOWR`` — issued on the control fd or a qpair fd. Userspace writes ``flags`` and
``length``; the kernel writes back ``granule`` and ``transfer_hint`` and returns the new buffer fd
as the ``ioctl()`` return value (same convention as the BAR/queue-pair fd ioctls).

The returned fd:

- is ``mmap``-able (full length, offset 0, ``MAP_SHARED``) for CPU access to the buffer;
- releases the buffer when it (and any mapping) is closed — there is no explicit unregister ioctl;
- keeps its pages (and DMA mapping) alive as long as either the fd or any mapping exists.

``transfer_hint`` is advisory and tells userspace which queue topology the kernel expects to be
best for this buffer on the current hardware. Current SLASH hardware returns
``SLASH_QDMA_TRANSFER_HINT_V80``; userspace may ignore this value. Known values are:

.. code-block:: c

    enum slash_qdma_transfer_hint {
        SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR = 1,
        SLASH_QDMA_TRANSFER_HINT_V80          = 2,
    };

``SLASH_QDMA_TRANSFER_HINT_V80`` asks userspace to apply the V80 placement-aware channel policy:
spread a transfer across both AXI-MM channels so each NoC ingress master (NMU) drives an
independent memory endpoint (NSU). The marker is opaque; the client computes the actual split from
the buffer's device address (DDR ranges are halved across the two channels, while HBM ranges are
routed by the 16 GiB half-memory boundary). ``SLASH_QDMA_TRANSFER_HINT_SINGLE_QPAIR`` keeps all
traffic on a single queue.

**Preconditions:**

- ``size`` must cover at least ``length`` (the trailing input field) — otherwise ``-EINVAL``
- ``flags`` must contain only ``O_CLOEXEC``
- ``length`` must be a non-zero multiple of the page size

**Postconditions:**

- the ``ioctl()`` return value is the new buffer fd (``>= 0``)
- ``granule`` is the per-descriptor page size (4 KiB); ``transfer_hint`` is an advisory topology hint
- the pages stay allocated and DMA-mapped until the fd and all mappings are closed and no transfer
  is in flight

**Return values:**

- ``>= 0`` — the new buffer fd (success)
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — ``size`` too small, unsupported ``flags`` bits, or misaligned/zero ``length``
- ``-ENOMEM`` — page allocation or DMA-mapping failure
- ``-ENODEV`` — device shutting down
- Other negative errno from ``anon_inode_getfile()`` or ``get_unused_fd_flags()``

The ``'v'`` ``0x55`` ioctl number is reserved (it was the removed
``SLASH_QDMA_IOCTL_BUF_UNREGISTER``; kernel buffers are now released by closing the fd).

``SLASH_QDMA_QPAIR_IOCTL_TRANSFER``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Performs a DMA transfer batch using kernel buffers. Unlike ``read``/``write``/``pread``/``pwrite``,
this ioctl is issued on a **queue-pair I/O fd** (from ``SLASH_QDMA_IOCTL_QPAIR_GET_FD``), not the
control device. The transfer carries an array of per-qpair sub-transfers; sub-transfers that target
distinct queue pairs are submitted **concurrently** (all but the last asynchronously, the last
blocking, then awaited), so a single ioctl can drive both NoC channels in parallel. No pages are
allocated or DMA-mapped on this path — that work was amortised at ``BUF_CREATE`` time — so each
sub-transfer syncs and submits the cached, pre-DMA-mapped SGL slice directly.

**Interface:**

.. code-block:: c

    #define SLASH_QDMA_QPAIR_IOCTL_TRANSFER _IOWR('v', 0x56, struct slash_qdma_transfer)

    struct slash_qdma_subxfer {
        __u32 qpair_index; /* [in] Index into the fd's bound qpairs */
        __u32 direction;   /* [in] 1=H2C (write), 2=C2H (read) */
        __s32 buf_fd;      /* [in] Kernel buffer fd from BUF_CREATE */
        __u32 pad0;        /* padding */
        __u64 buf_offset;  /* [in] Byte offset within the buffer */
        __u64 dev_addr;    /* [in] Device-side (endpoint) address */
        __u64 length;      /* [in] Number of bytes to transfer */
    };

    struct slash_qdma_transfer {
        __u32 size;   /* [in/out] ABI version */
        __u32 count;  /* [in] Number of sub-transfers (1..SLASH_QDMA_FD_MAX_QPAIRS) */
        struct slash_qdma_subxfer xfers[SLASH_QDMA_FD_MAX_QPAIRS];
    };

**Direction:** ``_IOWR`` — userspace writes all input fields; the total number of bytes transferred
across all sub-transfers is returned as the ``ioctl()`` return value (not as a struct field).

**Preconditions:**

- ``size`` must cover at least ``count`` (the trailing header field) — otherwise ``-EINVAL``
- ``count`` must be in ``[1, SLASH_QDMA_FD_MAX_QPAIRS]``
- each sub-transfer's ``qpair_index`` must be ``< `` the number of qpairs the fd owns
- each ``direction`` must be 1 (H2C) or 2 (C2H) and must be enabled on the selected queue pair
- each ``buf_fd`` must be a buffer fd (from ``BUF_CREATE``) bound to the same device as this qpair fd
- each ``buf_offset`` must be aligned to the buffer's page granule; ``length`` is an exact byte count,
  must be non-zero and ``<= UINT_MAX``, may end within the final page, and ``buf_offset + length``
  must not exceed the buffer length

**Return values:**

- ``>= 0`` — total number of bytes transferred (success)
- ``-EFAULT`` — copy failure
- ``-EBADF`` — a ``buf_fd`` is not a valid open fd
- ``-EINVAL`` — ``size``/``count`` invalid, bad ``qpair_index``/``direction``, a ``buf_fd`` that is not
  a SLASH buffer or belongs to another device, or an out-of-range / misaligned slice
- ``-ENODEV`` — device shutting down or the requested direction is not enabled on the qpair
- Other negative errno from libqdma's ``qdma_request_submit()`` (the first sub-transfer error wins)

An optional asynchronous form of this transfer is exposed via ``io_uring`` ``uring_cmd`` (opcode
``SLASH_QDMA_URING_CMD_TRANSFER``), available only on kernels built with ``CONFIG_IO_URING`` and
``uring_cmd`` support. The SQE inline command carries a single ``__u64`` userspace pointer to a
``struct slash_qdma_transfer``; the completion CQE ``res`` holds the total bytes transferred or a
negative errno. This lets many buffer transfers be kept in flight from a single thread.

Device resets and hotplugging: ``/dev/slash_hotplug``
=====================================================

The hotplug device provides privileged control over the PCIe lifecycle of SLASH cards. It supports
removing a device from the PCI hierarchy, rescanning root buses to rediscover devices, issuing a
secondary bus reset (SBR) on the upstream bridge for a full hardware reset, and an atomic
remove-and-rescan operation. These operations are used after loading a new FPGA bitstream and when
performing a full board reset.

- **Device file name:** ``/dev/slash_hotplug``
- **Sysfs name:** ``/sys/class/slash/slash_hotplug``
- **Device number:** the shared SLASH major, minor 0
- **Permissions:** ``0600``
- **Creation:** exactly one instance, created at module load, destroyed at module unload
- **File operations:** ``ioctl`` only (includes 32-bit compat path). No ``open``, ``release``,
  ``read``, ``write``, or ``mmap``.

Usage
-----

Full FPGA Reconfiguration (with secondary bus reset)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For a complete reconfiguration where the FPGA is fully reset, remove both PFs, assert a secondary bus
reset, wait for FPGA re-initialization, then rescan:

.. code-block:: c

    struct slash_hotplug_device_request req = { .size = sizeof(req) };

    /* Remove both PFs */
    snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.1");
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);
    snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);

    /* Assert SBR (blocks ~1 s internally for link retraining) */
    snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.0");  /* bus matters, not function digit */
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR, &req);

    /* Wait for FPGA re-initialization — caller responsibility */
    sleep(7);  /* 5–10 s recommended */

    /* Rescan all root buses */
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN, NULL);
    /* /dev/slash_ctl<N> and /dev/slash_qdma_ctl<N> reappear */

Hotplug Remove and Rescan
~~~~~~~~~~~~~~~~~~~~~~~~~

For a simple teardown and re-add without reset, remove by BDF then rescan:

.. code-block:: c

    /* Remove by BDF */
    struct slash_hotplug_device_request req = { .size = sizeof(req) };
    snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);

    /* Rescan */
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN, NULL);

Or atomically via HOTPLUG (remove + rescan on the same bus):

.. code-block:: c

    snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
    ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG, &req);

IOCTL Reference
---------------

All hotplug ioctls use magic byte ``'w'`` (``0x77``) and sequence numbers ``0x30``–``0x33``.

Three of the four ioctls (``REMOVE``, ``TOGGLE_SBR``, ``HOTPLUG``) share the following request
struct:

.. code-block:: c

    #define SLASH_HOTPLUG_BDF_LEN 32

    struct slash_hotplug_device_request {
        __u32 size;                        /* ABI version: set to sizeof(struct) */
        char  bdf[SLASH_HOTPLUG_BDF_LEN]; /* NUL-terminated PCI BDF, e.g. "0000:03:00.0" */
    };

The BDF format is ``DDDD:BB:SS.F`` with full domain prefix. Leading and trailing whitespace are
trimmed before parsing.

``SLASH_HOTPLUG_IOCTL_RESCAN``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Rescans all PCI root buses to discover new or reconfigured devices. Typically called after
``REMOVE`` or ``TOGGLE_SBR`` to rediscover a device.

**Interface:**

.. code-block:: c

    #define SLASH_HOTPLUG_IOCTL_RESCAN _IO('w', 0x30)

**Direction:** ``_IO`` — no argument. Pass ``NULL`` as the third argument to ``ioctl()``.

**Preconditions:** None.

**Postconditions:**

- All PCI root buses have been scanned under ``pci_lock_rescan_remove()``.
- Any new or reconfigured PCI devices are discovered and probed.

**Return values:**

- ``0`` — success (always succeeds if the kernel PCI lock can be acquired)

``SLASH_HOTPLUG_IOCTL_REMOVE``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Removes a PCI device identified by BDF from the PCI hierarchy, triggering the driver's ``.remove``
callback. The corresponding ``/dev/slash_ctl<N>`` or ``/dev/slash_qdma_ctl<N>`` node disappears.

**Interface:**

.. code-block:: c

    #define SLASH_HOTPLUG_IOCTL_REMOVE _IOW('w', 0x31, struct slash_hotplug_device_request)

**Direction:** ``_IOW`` — userspace writes the BDF; no kernel-to-userspace data.

**Preconditions:**

- ``bdf`` must be a valid, parseable ``DDDD:BB:SS.F`` string (or empty for single-device shorthand)
- ``size`` must cover the ``bdf`` field — otherwise ``-EINVAL``

**Postconditions:**

- Bus mastering is disabled on the device (``pci_clear_master()``).
- The device is removed from the PCI hierarchy (``pci_stop_and_remove_bus_device()``).
- The driver's ``.remove`` callback is invoked; associated device nodes disappear.
- A later rescan recreates the node with the same class name, ``/dev`` path, and ``dev_t``.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — malformed BDF or request ``size`` too small
- ``-ENODEV`` — device not found in PCI subsystem

``SLASH_HOTPLUG_IOCTL_TOGGLE_SBR``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Asserts a secondary bus reset (SBR) on the upstream PCIe bridge for the bus specified by BDF,
performing a full hardware reset of all endpoints on that bus. The ioctl blocks for approximately
1000 ms internally for PCIe link retraining; userspace should wait an **additional 5–10 seconds**
after the call returns before rescanning.

**Interface:**

.. code-block:: c

    #define SLASH_HOTPLUG_IOCTL_TOGGLE_SBR _IOW('w', 0x32, struct slash_hotplug_device_request)

**Direction:** ``_IOW`` — userspace writes the BDF; no kernel-to-userspace data.

**Preconditions:**

- ``size`` must cover the ``bdf`` field — otherwise ``-EINVAL``
- ``bdf`` must be a valid ``DDDD:BB:SS.F`` string; only the domain and bus number are used to
  locate the upstream bridge
- The endpoint device may have been removed before calling; the kernel resolves the bridge via the
  bus number, which persists after endpoint removal

**Postconditions:**

- Bridge config space is saved, ``PCI_BRIDGE_CTL_BUS_RESET`` is asserted for at least 2 ms,
  deasserted, and config space is restored.
- The ioctl sleeps 1000 ms for PCIe link retraining before returning.
- The PCIe link is retrained; the FPGA may still be initializing after return.

**Return values:**

- ``0`` — success (after 1000 ms delay)
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — malformed BDF or request ``size`` too small
- ``-ENODEV`` — no upstream bridge found for the specified bus

``SLASH_HOTPLUG_IOCTL_HOTPLUG``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Atomically removes and rescans a single PCI device under the PCI lock. This is equivalent to
``REMOVE`` followed immediately by ``RESCAN`` on the same parent bus, without releasing the lock
between operations. Does **not** include an SBR; use ``TOGGLE_SBR`` separately if a hardware reset
is needed.

**Interface:**

.. code-block:: c

    #define SLASH_HOTPLUG_IOCTL_HOTPLUG _IOW('w', 0x33, struct slash_hotplug_device_request)

**Direction:** ``_IOW`` — userspace writes the BDF; no kernel-to-userspace data.

**Preconditions:**

- ``size`` must cover the ``bdf`` field — otherwise ``-EINVAL``
- ``bdf`` must be a valid, parseable ``DDDD:BB:SS.F`` string
- The device and its parent bus must exist in the PCI subsystem

**Postconditions:**

- The device is removed (``pci_clear_master()`` + ``pci_stop_and_remove_bus_device()``).
- The parent bus is rescanned (``pci_rescan_bus()``); the device reappears if hardware is present.
- Both operations complete atomically under ``pci_lock_rescan_remove()``.

**Return values:**

- ``0`` — success
- ``-EFAULT`` — copy failure
- ``-EINVAL`` — malformed BDF or request ``size`` too small
- ``-ENODEV`` — device or parent bus not found
