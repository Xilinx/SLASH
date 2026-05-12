# SLASH Kernel Module Interface Specification

## Overview

The SLASH kernel module (`slash.ko`) exposes AMD Alveo V80 FPGA cards to userspace through a set of
character devices. It drives two PCI physical functions per card and registers three categories of
device nodes: a per-card control device for BAR enumeration and MMIO access, a per-card QDMA
device for DMA queue management, and a single global hotplug device for PCIe lifecycle operations.

## Device Files

The module uses the Linux `miscdevice` framework, which allocates dynamic minor numbers under major
10. Userspace discovers device nodes by path, not by major/minor number.

### `/dev/slash_ctl<N>` — Control Device

- **Path pattern:** `/dev/slash_ctl0`, `/dev/slash_ctl1`, ...
- **sysfs name:** `slash_ctl_<PCI-BDF>` (e.g., `slash_ctl_0000:61:00.2`)
- **Associated PCI function:** PF2, device ID `10EE:50B6`
- **Permissions:** `0600` (owner read/write)
- **Creation:** one per card, created when PF2 is probed during module load or PCI rescan
- **File operations:** `ioctl` only — no `open` hook (miscdevice default), no `read`, `write`, or
  `mmap` on this fd itself. MMIO access is through a dma-buf fd returned by an ioctl.

The suffix `N` is assigned by a module-lifetime BDF-to-number map. The first time a given BDF is
probed, it is assigned the next available counter value; on hotplug remove and rescan, the same BDF
is reassigned the same `N`. The assignment is permanent for the module's lifetime — entries are
never freed. This stability guarantee means `/dev/slash_ctl0` always refers to the same physical
card across remove+rescan cycles.

### `/dev/slash_qdma_ctl<N>` — QDMA Control Device

- **Path pattern:** `/dev/slash_qdma_ctl0`, `/dev/slash_qdma_ctl1`, …
- **sysfs name:** `slash_qdma_ctl_<PCI-BDF>` (e.g., `slash_qdma_ctl_0000:61:00.1`)
- **Associated PCI function:** PF1, device ID `10EE:50B5`
- **Permissions:** `0600`
- **Creation:** one per card, created when PF1 is probed
- **File operations:** `open`, `release`, `ioctl` on the control fd. DMA I/O is done on per-qpair
  anon-inode fds returned by an ioctl.

Same stable-N mapping scheme as the control device, using a separate BDF-to-number map.

### `/dev/slash_hotplug` — Hotplug Singleton

- **Path:** `/dev/slash_hotplug` (literal; `SLASH_HOTPLUG_DEVICE_NAME`)
- **Permissions:** `0600`
- **Creation:** exactly one instance, created at module load, destroyed at module unload
- **File operations:** `ioctl` only (includes 32-bit compat path). No `open`, `release`, `read`,
  `write`, or `mmap`.

## Data Conventions

### ABI Versioning

Every ioctl argument struct carries a leading `__u32 size` field. Callers must set
`size = sizeof(struct ...)` before issuing the ioctl. The kernel reads `size` first, then copies
`min(user_size, kernel_size)` bytes in. Fields the kernel knows about but the caller's older struct
does not include are zero-filled. The response is written back for `min(user_size, kernel_size)`
bytes; if `user_size > kernel_size`, the kernel zero-fills the extra tail via `clear_user()`.
This allows the driver and library to evolve independently.

**Exception:** `SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO` treats `size == 0` as
`size = sizeof(struct slash_ioctl_device_info)` and always writes back the full struct.

### Error Handling

All ioctls return 0 on success or a negative errno on failure, except for two ioctls that use the
return value as a file descriptor (described below). The standard errno values are documented under
each ioctl. Unknown ioctl command numbers return `-ENOTTY`.

### fd-as-Return-Value Ioctls

Two ioctls return a new file descriptor as the `ioctl()` syscall return value rather than returning
0:

- `SLASH_CTLDEV_IOCTL_GET_BAR_FD` — returns a dma-buf fd for BAR MMIO access
- `SLASH_QDMA_IOCTL_QPAIR_GET_FD` — returns an anon-inode fd for QDMA queue I/O

On success, the return value is a non-negative file descriptor number. On failure, the return value
is a negative errno (not -1; callers check `ret < 0`). This is a non-standard convention that
differs from all other ioctls in this interface, which return 0 on success.

Both ioctls are declared `_IOWR` (read+write direction) even though the fd is carried in the return
value rather than a struct field.

## Control Device (`/dev/slash_ctl<N>`)

### Overview

The control device provides two services. First, BAR enumeration and access: callers query which
of the card's PCIe BARs are present and usable, then obtain a dma-buf fd for each BAR they wish to
memory-map for direct MMIO register access. Second, device identity: callers read the card's PCI
BDF string and vendor/device IDs to correlate the control device with a physical board and with the
matching QDMA control device.

### Operations

#### BAR Access Setup and Teardown

```c
/* Setup */
struct slash_ioctl_bar_fd_request req = {
    .size       = sizeof(req),
    .bar_number = 0,
    .flags      = O_CLOEXEC,
};
int bar_fd = ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
void *mmio = mmap(NULL, req.length, PROT_READ | PROT_WRITE, MAP_SHARED, bar_fd, 0);

/* MMIO write */
struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);
*(volatile uint32_t *)((char *)mmio + offset) = value;
sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);

/* Teardown */
munmap(mmio, req.length);
close(bar_fd);
```

### Ioctl Reference

All control device ioctls use magic byte `'v'` (0x76) and sequence numbers `0x30`–`0x32`.

#### `SLASH_CTLDEV_IOCTL_GET_BAR_INFO`

```c
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
```

**Behavior:** Reads BAR metadata.

**Preconditions:**
- `size` must cover at least `bar_number` (minimum size enforced by kernel)
- `bar_number` must be in `[0, 5]`

**Postconditions:**
- `usable` = 1 if the BAR has a non-zero start address and is `IORESOURCE_MEM` (MMIO type)
- `in_use` = 0 (reserved for future use; never set in current implementation)
- `start_address` = physical bus address (0 if not usable)
- `length` = BAR size in bytes (0 if not usable)

**Return values:**
- `0` — success
- `-EFAULT` — bad userspace pointer in copy_from_user or copy_to_user
- `-EINVAL` — `size` too small, or `bar_number` out of `[0, 5]`

#### `SLASH_CTLDEV_IOCTL_GET_BAR_FD`

```c
#define SLASH_CTLDEV_IOCTL_GET_BAR_FD _IOWR('v', 0x31, struct slash_ioctl_bar_fd_request)

struct slash_ioctl_bar_fd_request {
    __u32 size;        /* [in/out] ABI version */
    __u8  bar_number;  /* [in]  BAR index: 0–5 */
    __u8  pad0;        /* padding */
    __u16 pad1;        /* padding */
    __u32 flags;       /* [in]  fd flags: only O_CLOEXEC is honoured */
    __u64 length;      /* [out] Size of the BAR in bytes */
};
```

**Description:** Returns a new fd to access the BAR.

**Preconditions:**
- `size` must cover at least `flags`
- `bar_number` in `[0, 5]`
- `flags & ~O_CLOEXEC == 0` (any other flag bits cause `-EINVAL`)
- The specified BAR must be a usable MMIO BAR (must have an active dma-buf exporter)

**Postconditions:**
- The return value is a non-negative fd number on success.
- The fd refers to a dma-buf exporter for the named BAR and can be passed to `mmap()`.
- `length` is filled with the BAR size; callers use this to size the `mmap()` call.

**Return values:**
- `>= 0` — file descriptor (success)
- `-EFAULT` — copy failure
- `-EINVAL` — `size` too small, `bar_number` out of range, or unsupported `flags` bits
- `-ENODEV` — BAR has no dma-buf exporter (BAR not present or not MMIO)
- Other negative errno from `dma_buf_fd()`

#### `SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO`

```c
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
```

**Description:** Reads PCI identity fields of the accessed card.

**Preconditions:** None. `size == 0` is explicitly accepted and treated as `sizeof(struct
slash_ioctl_device_info)`.

**Postconditions:** All output fields populated. `bdf` is a NUL-terminated string in
`DDDD:BB:SS.F` format with full domain.

**Return values:**
- `0` — success
- `-EFAULT` — copy failure

### BAR MMIO Interface

BAR MMIO access is not performed on the control device fd itself. Instead, `GET_BAR_FD` returns a
dma-buf fd that is then mapped with `mmap()`.

#### Obtaining and Mapping a BAR

```c
/* Get BAR fd — return value is the fd, not 0 */
struct slash_ioctl_bar_fd_request req = {
    .size       = sizeof(req),
    .bar_number = 0,
    .flags      = O_CLOEXEC,
};
int bar_fd = ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
/* req.length is now filled with BAR size */

void *mmio = mmap(NULL, req.length, PROT_READ | PROT_WRITE, MAP_SHARED, bar_fd, 0);
```

- **Protection:** `PROT_READ | PROT_WRITE`
- **Flags:** `MAP_SHARED`
- **Offset:** any page-aligned offset within `[0, bar_length)` is accepted
- **Size:** taken from `req.length` filled by the kernel

#### Mapping Behavior

BAR mapping is **not inherited across `fork()`**. Each child process that
needs MMIO access must obtain its own dma-buf fd via `GET_BAR_FD`.

#### DMA_BUF_IOCTL_SYNC Protocol

All MMIO accesses through the mapped BAR region must be bracketed with `DMA_BUF_IOCTL_SYNC` calls
on the dma-buf fd:

```c
#include <linux/dma-buf.h>

/* Before writing to the BAR */
struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);

/* ... MMIO writes via mmio pointer ... */

/* After writing */
sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);

/* For reads: */
sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);
/* ... MMIO reads ... */
sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
ioctl(bar_fd, DMA_BUF_IOCTL_SYNC, &sync);
```

#### Post-Device-Removal Behavior

After `pci_stop_and_remove_bus_device()`, the VMA remains valid at the virtual address level. Any
physical accesses return `0xFFFFFFFF` (PCIe completion timeout). This is intended degraded behavior.

## QDMA Device (`/dev/slash_qdma_ctl<N>`)

### Overview

The QDMA device manages DMA queue pairs for bulk data movement between host memory and the card's
on-board memory (HBM or DDR). Each queue pair is allocated with a mode (MM or streaming) and a
direction mask, then started before use. An anon-inode fd obtained from the queue pair serves as
the I/O channel: `write()` performs H2C transfers, `read()` performs C2H transfers, and the file
position encodes the device-side physical address.

### Operations

#### DMA Transfer Sequence

Full lifecycle for a queue pair including H2C and C2H transfers:

```c
/* Step 1: Add queue pair (MM mode, bidirectional) */
struct slash_qdma_qpair_add add = {
    .size        = sizeof(add),
    .mode        = 0,    /* QDMA_Q_MODE_MM */
    .dir_mask    = 0x3,  /* H2C | C2H */
    .h2c_ring_sz = 0,
    .c2h_ring_sz = 0,
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

/* Step 4: H2C transfer to device address 0x4000000000 */
pwrite(io_fd, host_buf, nbytes, 0x4000000000LL);

/* Step 5: C2H transfer from device address 0x4000000000 */
pread(io_fd, host_buf, nbytes, 0x4000000000LL);

/* Step 6: Teardown */
close(io_fd);
op.op = 1;  ioctl(qdma_fd, SLASH_QDMA_IOCTL_Q_OP, &op);  /* STOP */
op.op = 2;  ioctl(qdma_fd, SLASH_QDMA_IOCTL_Q_OP, &op);  /* DEL */
```

#### FPGA Programming

FPGA programming (loading a new bitstream/PDI) is performed as a DMA write to the bitstream
programming region (`0x102100000`) over an H2C-only MM queue pair. After programming, PF2 should
be removed and rescanned because the new bitstream may present a different device identity or BAR
layout:

```c
/* Remove PF2 */
struct slash_hotplug_device_request req = { .size = sizeof(req) };
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);

/* Rescan to rediscover PF2 */
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN, NULL);
```

User applications should retry opening the new `/dev/slash_ctl<N>` path up to 10 times with
500 ms delays to allow udev to set permissions on the new device node.

### Ioctl Reference

All QDMA control device ioctls use magic byte `'v'` (0x76) and sequence numbers `0x50`–`0x53`.

Every QDMA ioctl returns `-ENODEV` immediately if the hardware is shutting down (`hw_shutdown` flag
set) or the QDMA handle is not open.

#### `SLASH_QDMA_IOCTL_INFO`

```c
#define SLASH_QDMA_IOCTL_INFO _IOWR('v', 0x50, struct slash_qdma_info)

struct slash_qdma_info {
    __u32 size;       /* [in/out] ABI version */
    __u32 qsets_max;  /* [out] Max queue sets (currently always 0) */
    __u32 msix_qvecs; /* [out] MSI-X vectors for queues (currently always 0) */
    __u32 vf_max;     /* [out] Max VFs (currently always 0) */
    __u32 caps;       /* [out] Capability bitmask (currently always 0) */
};
```

All output fields are currently zero. This ioctl is a placeholder for future capability reporting.
Callers should issue it during initialization but make no decisions based on the returned
values in the current implementation.

**Return values:** `0`, `-EFAULT`, or `-ENODEV`.

#### `SLASH_QDMA_IOCTL_QPAIR_ADD`

```c
#define SLASH_QDMA_IOCTL_QPAIR_ADD _IOWR('v', 0x51, struct slash_qdma_qpair_add)

struct slash_qdma_qpair_add {
    __u32 size;          /* [in/out] ABI version */
    __u32 mode;          /* [in]  Queue mode: 0=MM (Memory Mapped), 1=ST (Streaming) */
    __u32 dir_mask;      /* [in]  Direction bitmask (see below) */
    __u32 h2c_ring_sz;   /* [in]  H2C descriptor ring CSR table index: 0–15 */
    __u32 c2h_ring_sz;   /* [in]  C2H descriptor ring CSR table index: 0–15 */
    __u32 cmpt_ring_sz;  /* [in]  Completion ring CSR table index: 0–15 */
    __u32 qid;           /* [out] Kernel-assigned queue pair ID */
};
```

Direction bitmask bits:

| Bit | Value | Meaning                   |
|-----|-------|---------------------------|
| 0   | `0x1` | H2C (host-to-card, write) |
| 1   | `0x2` | C2H (card-to-host, read)  |
| 2   | `0x4` | CMPT (completion queue)   |

Ring size fields are QDMA Control and Status Register (CSR) table indices (0–15), not raw descriptor counts.
Index 0 maps to approximately 2049 descriptors; index 15 to approximately 16385. The caller does not control the
actual descriptor count directly.

**Preconditions:**
- `dir_mask` must be non-zero and contain only bits `[0, 2]`
- `mode` must be 0 or 1
- All ring size indices must be in `[0, 15]`
- At most 256 concurrent queue pairs per device

**Postconditions:** `qid` is filled with the kernel-assigned ID (0–255), used for all subsequent
operations on this queue pair.

**Return values:**
- `0` — success
- `-EFAULT` — copy failure
- `-EINVAL` — invalid `dir_mask`, `mode`, or ring size index
- `-ENOMEM` — allocation failure
- `-EBUSY` — all 256 qpair IDs in use
- `-ENODEV` — device shutting down
- Other negative errno from libqdma's `qdma_queue_add()`

#### `SLASH_QDMA_IOCTL_Q_OP`

```c
#define SLASH_QDMA_IOCTL_Q_OP _IOWR('v', 0x52, struct slash_qdma_qpair_op)

struct slash_qdma_qpair_op {
    __u32 size; /* [in/out] ABI version */
    __u32 qid;  /* [in]     Queue pair ID from QPAIR_ADD */
    __u32 op;   /* [in]     Operation: 0=START, 1=STOP, 2=DEL */
};
```

Operations:

| `op` | Constant                     | Effect                                                                     |
|------|------------------------------|----------------------------------------------------------------------------|
| 0    | `SLASH_QDMA_QUEUE_OP_START`  | Activates all HW queues in the pair. Must be called before any I/O.       |
| 1    | `SLASH_QDMA_QUEUE_OP_STOP`   | Quiesces all HW queues. Required before DEL (but DEL implies STOP).        |
| 2    | `SLASH_QDMA_QUEUE_OP_DEL`    | Removes all HW queues and releases the qpair entry from the xarray.        |

The expected lifecycle is: `ADD → START → [I/O via qpair fd] → STOP → DEL`. DEL is safe to call on
a running queue (the kernel will stop it first), so an explicit STOP before DEL is not strictly
required but is the recommended sequence.

After DEL, the qpair ID may be reused by a subsequent QPAIR_ADD. Any open anon-inode fds obtained
via QPAIR_GET_FD still hold a ref on the entry; they will remain valid until closed, but the
underlying hardware queues will have been removed.

**Return values:**
- `0` — success
- `-EFAULT` — copy failure
- `-EINVAL` — `op` value not in `[0, 2]`
- `-ENOENT` — `qid` not found in the device's xarray
- `-ENODEV` — device shutting down
- Other negative errno from libqdma queue start, stop, or remove

#### `SLASH_QDMA_IOCTL_QPAIR_GET_FD`

```c
#define SLASH_QDMA_IOCTL_QPAIR_GET_FD _IOWR('v', 0x53, struct slash_qdma_qpair_fd_request)

struct slash_qdma_qpair_fd_request {
    __u32 size;   /* [in/out] ABI version */
    __u32 qid;    /* [in]     Queue pair ID (must exist and be non-empty) */
    __u32 flags;  /* [in]     fd flags: only O_CLOEXEC is honoured */
};
```

Creates a new file descriptor to transfer data between the host and the card. The returned
file descriptor supports `read`, `write`, `pread`, `pwrite`, `lseek`, and release. It does **not**
support `mmap`, `poll`/`select`, or `splice`.

The fd holds a reference on both the qpair entry and the device. Neither can be freed while this fd
is open. Multiple fds can be obtained for the same qpair via multiple calls.

**Preconditions:**
- `qid` must refer to an existing, non-empty queue pair
- `flags & ~O_CLOEXEC == 0` (any other bits cause `-EINVAL`)
- The queue pair should be in the started state for I/O to work

**Return values:**
- `>= 0` — file descriptor (success)
- `-EFAULT` — copy failure
- `-EINVAL` — unsupported `flags` bits
- `-ENOENT` — `qid` not found or qpair is empty
- `-ENODEV` — device shutting down
- `-ENOMEM` — allocation failure
- Other negative errno from `anon_inode_getfile()` or `get_unused_fd_flags()`

### Queue Pair fd Interface

The fd returned by `SLASH_QDMA_IOCTL_QPAIR_GET_FD` is an anon-inode character device that supports
`read`, `write`, `pread`, `pwrite`, and `lseek`. It does **not** support `mmap`, `poll`/`select`,
or `splice`.

#### File Position as Device Address

The file position is interpreted as the device-side address (FPGA DDR/HBM address). Each `read()`
or `write()` uses and advances this position:

```c
/* Write to FPGA DDR at address 0x1000 */
pwrite(qpair_fd, src_buf, nbytes, 0x1000);

/* Read from FPGA DDR at address 0x1000 */
pread(qpair_fd, dst_buf, nbytes, 0x1000);

/* Or using lseek + read/write */
lseek(qpair_fd, 0x1000, SEEK_SET);
write(qpair_fd, src_buf, nbytes);
```

`lseek` uses `default_llseek`, so `SEEK_SET`, `SEEK_CUR`, and `SEEK_END` all work. `pread` and
`pwrite` are supported (`FMODE_PREAD | FMODE_PWRITE` are set).

#### Transfer Semantics

- `write(fd, buf, count)` — performs an H2C (host-to-card) DMA transfer.s
- `read(fd, buf, count)` — performs a C2H (card-to-host) DMA transfer.
- On success, the return value is the number of bytes transferred. Partial transfers are possible.
- On success, the file position is advanced by bytes transferred.

#### Blocking Behavior

All transfers are synchronous and block until the transfer completes or times out. The timeout is
**10 seconds**. After 10 seconds without completion, the call returns a negative errno (typically `-ETIME`).

#### Transfer Size Limits

TODO: Identify transfer size limitations

#### Error Codes on the Queue Pair fd

| errno / return | Condition |
|----------------|-----------|
| `>= 0` | Bytes transferred (success; partial transfer is possible) |
| `-ENODEV` | Device shutting down, or the required direction is not enabled for this qpair |
| `-EINVAL` | Zero-length transfer (`count` results in 0 pages) |
| `-ENOMEM` | SGL allocation failure |
| `-EFAULT` | `get_user_pages_fast` returned fewer pages than needed (bad userspace buffer) |
| `-ETIME` | 10-second DMA timeout |
| Other libqdma errors | Propagated from `qdma_request_submit()` |

### Device Address Map

The QDMA queue pair fd treats the file position as the device-side physical address. Three regions
can be targeted by `read()` and `write()`:

| Region | Base | End (exclusive) | Direction |
|--------|------|-----------------|-----------|
| Bitstream / PDI | `0x0000000102100000` | `0x0000000142100000` | H2C only |
| HBM (64 pseudo-channels) | `0x0000004000000000` | `0x0000004800000000` | H2C and C2H |
| DDR | `0x0000060000000000` | `0x0000060800000000` | H2C and C2H |

#### HBM and DDR

Both regions use the same two-level layout: 64 regions of 512 MiB, each subdivided into 8
subregions of 64 MiB.

|                    | HBM                  | DDR                   |
|--------------------|----------------------|-----------------------|
| Base               | `0x4000000000`       | `0x60000000000`       |
| Regions            | 64 (HBM0–HBM63)      | 64                    |
| Region size        | 512 MiB (`0x20000000`) | 512 MiB (`0x20000000`) |
| Subregions/region  | 8                    | 8                     |
| Subregion size     | 64 MiB (`0x4000000`) | 64 MiB (`0x4000000`)  |

Address of region N, subregion K (N ∈ [0, 63], K ∈ [0, 7]):

```
HBM: 0x4000000000  + N × 0x20000000 + K × 0x4000000
DDR: 0x60000000000 + N × 0x20000000 + K × 0x4000000
```

**HBM pseudo-channel map:**

| Channel | Base           | End (exclusive) |
|---------|----------------|-----------------|
| HBM0    | `0x4000000000` | `0x4020000000`  |
| HBM1    | `0x4020000000` | `0x4040000000`  |
| …       | …              | …               |
| HBM31   | `0x43E0000000` | `0x4400000000`  |
| HBM32   | `0x4400000000` | `0x4420000000`  |
| …       | …              | …               |
| HBM63   | `0x47E0000000` | `0x4800000000`  |

TODO: DMA sync granularity and alignment constraints to be identified.

#### Bitstream / PDI Programming Region

| Field               | Value                  |
|---------------------|------------------------|
| Base address        | `0x0000000102100000`   |
| Maximum size        | 1 GiB (`0x40000000`)  |
| Direction           | H2C write-only         |
| Host buffer alignment | 4096 bytes           |

## Hotplug Device (`/dev/slash_hotplug`)

### Overview

The hotplug device provides privileged control over the PCIe lifecycle of SLASH cards. It supports
removing a device from the PCI hierarchy, rescanning root buses to rediscover devices, issuing a
secondary bus reset (SBR) on the upstream bridge for a full hardware reset, and an atomic
remove-and-rescan operation. These are used after loading a new FPGA bitstream (which requires
reprobing PF2) and when performing a full board reset.

### Operations

#### Full FPGA Reconfiguration (with SBR)

For a complete reconfiguration where the FPGA is fully reset:

```c
struct slash_hotplug_device_request req = { .size = sizeof(req) };

/* Remove both PFs */
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.1");
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);

/* Assert SBR (blocks ~1 s internally for link retraining) */
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.0");  /* bus/function matters, not function digit */
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR, &req);

/* Wait for FPGA re-initialization — caller responsibility */
sleep(7);  /* 5–10 s recommended */

/* Rescan all root buses */
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN, NULL);
/* /dev/slash_ctl<N> and /dev/slash_qdma_ctl<N> reappear */
```

#### Hotplug Remove and Rescan

For a simple teardown and re-add without reset:

```c
/* Remove by BDF */
struct slash_hotplug_device_request req = { .size = sizeof(req) };
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req);

/* Rescan */
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN, NULL);
```

Or atomically via HOTPLUG (remove + rescan on the same bus):

```c
snprintf(req.bdf, sizeof(req.bdf), "0000:61:00.2");
ioctl(hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG, &req);
```

### Ioctl Reference

All hotplug ioctls use magic byte `'w'` (0x77) and sequence numbers `0x30`–`0x33`.

The device request struct used by REMOVE, TOGGLE_SBR, and HOTPLUG:

```c
#define SLASH_HOTPLUG_BDF_LEN 32

struct slash_hotplug_device_request {
    __u32 size;                        /* ABI version; 0 is accepted (treated as sizeof) */
    char  bdf[SLASH_HOTPLUG_BDF_LEN]; /* NUL-terminated PCI BDF, e.g. "0000:03:00.0" */
};
```

The BDF format is `DDDD:BB:SS.F` with full domain prefix. Leading and trailing whitespace are trimmed
before parsing. If `bdf` is empty, the only tracked device is targeted (returns
`-EOPNOTSUPP` if multiple devices are tracked, `-ENODEV` if none).

#### `SLASH_HOTPLUG_IOCTL_RESCAN`

```c
#define SLASH_HOTPLUG_IOCTL_RESCAN _IO('w', 0x30)
```

**Direction:** `_IO` (no argument — pass NULL as the third argument to `ioctl()`).

**Description:** Acquires `pci_lock_rescan_remove()`, iterates over all PCI root buses, and
calls `pci_rescan_bus()` on each. Any new or reconfigured PCI devices on any root bus are
discovered and probed. Releases the lock.

**Return values:** `0` (always succeeds if the kernel PCI lock can be acquired).

#### `SLASH_HOTPLUG_IOCTL_REMOVE`

```c
#define SLASH_HOTPLUG_IOCTL_REMOVE _IOW('w', 0x31, struct slash_hotplug_device_request)
```

**Direction:** `_IOW` (write: userspace sends struct, no kernel-to-userspace data).

**Description:**
1. Looks up the device by BDF via `pci_get_domain_bus_and_slot()`.
2. Acquires `pci_lock_rescan_remove()`.
3. Calls `pci_clear_master()` to disable bus mastering.
4. Calls `pci_stop_and_remove_bus_device()`, which removes the device from the PCI hierarchy and
   triggers the driver's `.remove` callback. The corresponding `/dev/slash_ctl<N>` or
   `/dev/slash_qdma_ctl<N>` node disappears.
5. Releases lock and `pci_dev_put()`.

**Return values:**
- `0` — success
- `-EFAULT` — copy failure
- `-EINVAL` — malformed BDF or request `size` too small
- `-ENODEV` — device not found in PCI subsystem

#### `SLASH_HOTPLUG_IOCTL_TOGGLE_SBR`

```c
#define SLASH_HOTPLUG_IOCTL_TOGGLE_SBR _IOW('w', 0x32, struct slash_hotplug_device_request)
```

**Direction:** `_IOW`

**Description:**
1. Parses the BDF to extract the domain and bus number.
2. Acquires `pci_lock_rescan_remove()`, finds the `pci_bus` for that bus number, takes a ref on
   `bus->self` (the upstream PCIe bridge), then releases the lock.
3. Calls `pci_bridge_secondary_bus_reset(bridge)`: saves bridge config space, asserts
   `PCI_BRIDGE_CTL_BUS_RESET` for at least 2 ms, deasserts, restores config space.
4. Sleeps **1000 ms** for PCIe link retraining.
5. Releases the bridge ref and returns.

The 1000 ms delay is internal to the ioctl. After it returns, the PCIe link is retrained but the
FPGA may still be initializing. Userspace should wait an **additional 5–10 seconds** before
rescanning.

The endpoint device may have been removed before calling TOGGLE_SBR; the kernel resolves the bridge
via the bus number, which persists after endpoint removal.

**Return values:**
- `0` — success (after 1000 ms delay)
- `-EFAULT` — copy failure
- `-EINVAL` — malformed BDF
- `-ENODEV` — no upstream bridge found for the specified bus

#### `SLASH_HOTPLUG_IOCTL_HOTPLUG`

```c
#define SLASH_HOTPLUG_IOCTL_HOTPLUG _IOW('w', 0x33, struct slash_hotplug_device_request)
```

**Direction:** `_IOW`

**Description:**
1. Looks up the device by BDF, records its parent `pci_bus`.
2. Acquires `pci_lock_rescan_remove()`.
3. `pci_clear_master()` + `pci_stop_and_remove_bus_device()` + `pci_dev_put()`.
4. `pci_rescan_bus(parent_bus)` to rediscover the device on the same bus.
5. Releases lock.

This performs remove and rescan atomically under the PCI lock. It does **not** include an SBR. Use
`TOGGLE_SBR` separately before `HOTPLUG` if a hardware reset is needed.

**Return values:**
- `0` — success
- `-EFAULT` — copy failure
- `-EINVAL` — malformed BDF
- `-ENODEV` — device or parent bus not found

## Device Enumeration

Glob `/dev/slash_ctl*` to find all control device nodes. For each discovered node:

1. Open `/dev/slash_ctl<N>` with `O_RDWR`.
2. Issue `SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO` to read the PCI BDF and vendor/device IDs.
3. Use the BDF (stripped of the `.F` function digit) to locate the matching QDMA device by
   globbing `/sys/class/misc/slash_qdma_ctl_<bus:device-prefix>.*` and reading the `uevent` file.
4. Open `/dev/slash_qdma_ctl<N>` with `O_RDWR`.
5. Issue `SLASH_QDMA_IOCTL_INFO` to read QDMA capabilities (currently all zero).
6. Issue `SLASH_CTLDEV_IOCTL_GET_BAR_INFO` for BARs 0–5 to discover which BARs are usable.
7. Issue `SLASH_CTLDEV_IOCTL_GET_BAR_FD` for each usable BAR to obtain a mappable fd.

The hotplug singleton can be opened directly:

```c
int hp_fd = open("/dev/slash_hotplug", O_RDWR | O_CLOEXEC);
```

## Concurrency Model

### Concurrent Access Safety

TODO: To be expanded/specified: What may be concurrently accessed, what not?

- Generally, all ioctl operations are safe.
- Multiple concurrent `read()`/`write()` on the same qpair fd are not recommended and are not
  tested. Each call submits a synchronous libqdma request; concurrent requests to the same hardware
  queue handle may race inside libqdma.
- Multiple processes may each hold their own fd to the same qpair via separate calls to
  `QPAIR_GET_FD`.
- Hotplug ioctls from multiple processes serialize on `pci_lock_rescan_remove()`.
  `TOGGLE_SBR` drops this lock before calling `pci_bridge_secondary_bus_reset()` to avoid deadlock
  with the PCI slot lock.

### Notifications

No custom uevents or netlink notifications are emitted, and there is no poll-able event queue.
Userspace must discover new nodes by watching `/dev/` via udev or polling.
