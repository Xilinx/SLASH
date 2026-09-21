# libslash

Userspace C library for the SLASH kernel driver.  libslash provides a
thin, type-safe wrapper around the driver's ioctl interface, covering
three areas of functionality:

| Module   | Header            | Device node / socket                    | PCI function |
|----------|-------------------|-----------------------------------------|--------------|
| Control  | `slash/ctldev.h`  | `/dev/slash_ctl<N>` or socket           | PF2          |
| QDMA     | `slash/qdma.h`    | `/dev/slash_qdma_ctl<N>` or socket      | PF1          |
| Hotplug  | `slash/hotplug.h` | `/dev/slash_hotplug` or socket          | —            |

## Socket transport

Starting with Step 12, every libslash open call (`slash_ctldev_open`,
`slash_qdma_open`, `slash_hotplug_open`) accepts either a character-device
path or an `AF_UNIX`/`SOCK_SEQPACKET` socket path interchangeably.
Transport is selected at open time using `stat(2)`:

- `S_ISSOCK` → socket transport: `connect(2)` to the daemon; all ioctls
  are forwarded as framed datagrams over the socket.
- otherwise → ioctl transport: the existing `open(O_RDWR)` + `ioctl(2)` path.

The `"@mock"` magic path for the control device is unaffected.

### Daemon socket paths

The `slash_sysemu` daemon exposes one socket per subsystem under
`/run/slash_sysemu/` (the `RuntimeDirectory` managed by systemd):

| Subsystem | Socket path                               |
|-----------|-------------------------------------------|
| Control   | `/run/slash_sysemu/slash_ctl<N>`          |
| QDMA      | `/run/slash_sysemu/slash_qdma_ctl<N>`     |
| Hotplug   | `/run/slash_sysemu/slash_hotplug`         |

Pass these paths directly to the open calls:

```c
struct slash_ctldev  *dev  = slash_ctldev_open("/run/slash_sysemu/slash_ctl0");
struct slash_qdma    *qdma = slash_qdma_open("/run/slash_sysemu/slash_qdma_ctl0");
struct slash_hotplug *hp   = slash_hotplug_open("/run/slash_sysemu/slash_hotplug");
```

The same consumer code that works against `/dev/slash_*` character devices
works unchanged over sockets — no API changes required.

### BAR file sync over sockets

When a BAR is obtained via the socket transport (`slash_bar_file_open` over a
socket-backed ctldev), the daemon returns a memfd via `SCM_RIGHTS`.
`slash_bar_file_start_write` / `slash_bar_file_end_write` and
`slash_bar_file_start_read` / `slash_bar_file_end_read` translate the
`DMA_BUF_IOCTL_SYNC` calls into `flock(2)` operations on the memfd:

| API call                  | flock operation             |
|---------------------------|-----------------------------|
| `slash_bar_file_start_write` | `flock(LOCK_EX)`          |
| `slash_bar_file_end_write`   | `flock(LOCK_UN)`          |
| `slash_bar_file_start_read`  | `flock(LOCK_SH)`          |
| `slash_bar_file_end_read`    | `flock(LOCK_UN)`          |

The daemon hands a distinct open file description (via `reopen()`) for each
`GET_BAR_FD` call, so flock semantics are correct: multiple clients each hold
their own file description on the same memfd inode, and `LOCK_EX` from one
client excludes `LOCK_SH` from another.

### Error mapping

Any transport-layer failure (daemon disconnect, send/recv error, datagram
truncation, sequence/op mismatch, timeout) maps to `errno = ENODEV` and a
`-1` / `NULL` return value, consistent with the forced-disconnect contract
defined in the architecture.

## Building

These instructions cover development on libslash itself. To install SLASH on a
machine, build and install the packages: see
[Build and Install SLASH](https://slash.readthedocs.io/en/latest/howto/install-from-packages.html).

```sh
cmake -B build -S . -G Ninja
cmake --build build
```

CMake options:

| Option                | Default | Description                    |
|-----------------------|---------|--------------------------------|
| `BUILD_SHARED_LIBS`   | `ON`    | Build a shared library         |
| `SLASH_BUILD_EXAMPLES` | `ON`   | Build example programs         |
| `SLASH_BUILD_TESTS`   | `ON`    | Build unit tests               |

## Installing

```sh
sudo cmake --install build --prefix /usr/local
```

This installs:

- Headers to `<prefix>/include/slash/`
- Library to `<prefix>/lib/libslash.so` (or `.a`)
- CMake package config to `<prefix>/lib/cmake/slash/`

Downstream projects can then use:

```cmake
find_package(slash REQUIRED)
target_link_libraries(myapp PRIVATE slash::slash)
```

## API overview

All functions follow POSIX conventions: pointer-returning functions
return `NULL` on failure, int-returning functions return `-1`.  `errno`
is set in both cases.

### Control device — BAR info and memory-mapped access

```c
#include <slash/ctldev.h>

/* Open the control device (or "@mock" for testing without hardware) */
struct slash_ctldev *dev = slash_ctldev_open("/dev/slash_ctl0");

/* Query PCI identity */
struct slash_ioctl_device_info *info = slash_device_info_read(dev);
printf("BDF: %s  vendor: 0x%04x\n", info->bdf, info->vendor_id);
slash_device_info_free(info);

/* Query and map a BAR */
struct slash_ioctl_bar_info *bi = slash_bar_info_read(dev, 0);
if (bi->usable) {
    struct slash_bar_file *bar = slash_bar_file_open(dev, 0, O_CLOEXEC);
    volatile uint32_t *regs = bar->map;

    /* Bracket MMIO accesses with dma-buf sync calls */
    slash_bar_file_start_write(bar);
    regs[0] = 0x1;
    slash_bar_file_end_write(bar);

    slash_bar_file_start_read(bar);
    uint32_t val = regs[0];
    slash_bar_file_end_read(bar);

    slash_bar_file_close(bar);
}
slash_bar_info_free(bi);

slash_ctldev_close(dev);
```

### QDMA — queue-based DMA transfers

Queue pair lifecycle: **add &rarr; start &rarr; I/O &rarr; stop &rarr; del**.

```c
#include <slash/qdma.h>

struct slash_qdma *qdma = slash_qdma_open("/dev/slash_qdma_ctl0");

/* Create a queue pair (MM mode, H2C + C2H directions) */
struct slash_qdma_qpair_add req = {
    .size        = sizeof(req),
    .mode        = 0,           /* QDMA_Q_MODE_MM */
    .dir_mask    = 0x3,         /* H2C | C2H */
    .h2c_ring_sz = 4,          /* CSR table index */
    .c2h_ring_sz = 4,
    .cmpt_ring_sz = 4,
};
slash_qdma_qpair_add(qdma, &req);
uint32_t qid = req.qid;

slash_qdma_qpair_start(qdma, qid);

/* Get an ioctl-only qpair fd for buffer transfers. */
int fd = slash_qdma_qpair_get_fd(qdma, qid, O_CLOEXEC);

/* Create a kernel-owned DMA buffer (length must be a whole number of pages)
 * and mmap it for CPU access via buf.addr.  Current SLASH hardware reports
 * SLASH_QDMA_TRANSFER_HINT_V80 in buf.transfer_hint. */
struct slash_qdma_buffer buf;
slash_qdma_qpair_buffer_create(fd, len, &buf);
/* ... fill buf.addr from the CPU for an H2C transfer ... */

/* H2C: host -> device at dev_addr */
slash_qdma_qpair_transfer(fd, buf.fd, /*buf_offset=*/0, dev_addr, len,
                          SLASH_QDMA_XFER_H2C);
/* C2H: device -> host */
slash_qdma_qpair_transfer(fd, buf.fd, 0, dev_addr, len, SLASH_QDMA_XFER_C2H);

slash_qdma_buffer_destroy(&buf);
close(fd);

slash_qdma_qpair_stop(qdma, qid);
slash_qdma_qpair_del(qdma, qid);
slash_qdma_close(qdma);
```

### Hotplug — PCIe device lifecycle

Typical FPGA reconfiguration flow:
**remove &rarr; SBR &rarr; sleep &rarr; rescan &rarr; hotplug**.

```c
#include <slash/hotplug.h>

struct slash_hotplug *hp = slash_hotplug_open(NULL); /* /dev/slash_hotplug */

slash_hotplug_remove(hp, "0000:03:00.0");
slash_hotplug_remove(hp, "0000:03:00.1");
slash_hotplug_remove(hp, "0000:03:00.2");

slash_hotplug_toggle_sbr(hp, "0000:03:00.0");  /* assert 2 ms, settle 5 s */

usleep(5000000);  /* wait for device re-init */

slash_hotplug_rescan(hp);

slash_hotplug_hotplug(hp, "0000:03:00.0");  /* remove + rescan in one step */
slash_hotplug_hotplug(hp, "0000:03:00.1");
slash_hotplug_hotplug(hp, "0000:03:00.2");

slash_hotplug_close(hp);
```

For single-device systems, pass `NULL` instead of a BDF string.

## Mock mode

The control device API supports a mock mode for testing without
hardware.  Pass `"@mock"` as the device path:

```c
struct slash_ctldev *dev = slash_ctldev_open("@mock");
```

Mock mode creates temporary backing files (in `$XDG_RUNTIME_DIR` or
`/tmp`) that simulate 64 MB BARs.  All BAR reads and writes operate
on these files instead of real MMIO.

## Tests

```sh
cmake --build build
cd build/tests && ctest
```

Tests run in mock mode and do not require hardware or the kernel module
to be loaded.  The in-process `SysemuTestServer` provides hermetic coverage
of all three socket-transport paths (ctldev, qdma, hotplug) without a daemon.

### BYO-daemon E2E tests

An opt-in suite (`tests/libslash_e2e_test.cpp`) drives a realistic consumer
flow against a real, already-running `slash_sysemu` daemon.  Every test
`GTEST_SKIP()`s cleanly when the environment variables are unset, so `ctest`
stays green with no daemon present.

**The suite never spawns the daemon itself.**  Launch it separately before
setting the variables.

| Variable           | Daemon socket path (typical)                    |
|--------------------|-------------------------------------------------|
| `SLASH_E2E_CTL`    | `/run/slash_sysemu/slash_ctl0`                  |
| `SLASH_E2E_QDMA`   | `/run/slash_sysemu/slash_qdma_ctl0`             |
| `SLASH_E2E_HOTPLUG`| `/run/slash_sysemu/slash_hotplug`               |

```sh
export SLASH_E2E_CTL=/run/slash_sysemu/slash_ctl0
export SLASH_E2E_QDMA=/run/slash_sysemu/slash_qdma_ctl0
export SLASH_E2E_HOTPLUG=/run/slash_sysemu/slash_hotplug
cd build/tests && ctest -R E2E
```

E2E coverage: ctldev device_info + bar_info + `GET_BAR_FD` mmap + flock
write/read round-trip; QDMA INFO (BDF, qsets_max), qpair add/start/get_fd,
BUF_CREATE, H2C→C2H round-trip asserting A==B; hotplug RESCAN.

## Project layout

```
libslash/
  include/slash/
    ctldev.h              Public API — control device
    qdma.h                Public API — QDMA
    hotplug.h             Public API — hotplug
    uapi/
      slash_interface.h     User-kernel ABI (ctldev + QDMA ioctls)
      slash_hotplug.h       User-kernel ABI (hotplug ioctls)
      slash_sysemu.h        Socket-protocol ABI (shared with slash_sysemu daemon)
  src/
    ctldev.c                Control device implementation (ioctl + socket)
    ctldev_mock.c           Mock-mode BAR backing
    qdma.c                  QDMA implementation (ioctl + socket)
    hotplug.c               Hotplug implementation (ioctl + socket)
    sock_transport.c/h      AF_UNIX/SOCK_SEQPACKET client transport (private)
  examples/
    01_bar/print_bar.c      Enumerate and read/write BARs
    02_test/some_tb.c       Multi-core HBM transfer testbench
  tests/
    sysemu_test_server.h/cpp  In-process SEQPACKET daemon stub (shared fixture)
    ctldev_sysemu_test.cpp    ctldev socket-transport tests
    qdma_sysemu_test.cpp      QDMA socket-transport tests
    hotplug_sysemu_test.cpp   Hotplug socket-transport tests
    libslash_e2e_test.cpp     Opt-in BYO-daemon E2E tests (skip when unset)
    ctldev_test.cpp           ctldev mock + null-arg tests
    qdma_test.cpp             QDMA mock + null-arg tests
    hotplug_test.cpp          Hotplug null-arg tests
    sock_transport_test.cpp   sock_transport unit tests
```

## License

MIT.  See the license header in `CMakeLists.txt` for the full text.

The UAPI headers under `include/slash/uapi/` are dual-licensed
`GPL-2.0-only OR MIT` so they can be included by both the GPL kernel
module and the MIT userspace library without ambiguity.
