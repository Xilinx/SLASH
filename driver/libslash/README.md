# libslash

Userspace C library for the SLASH kernel driver.  libslash provides a
thin, type-safe wrapper around the driver's ioctl interface, covering
three areas of functionality:

| Module   | Header            | Device node              | PCI function |
|----------|-------------------|--------------------------|--------------|
| Control  | `slash/ctldev.h`  | `/dev/slash_ctl<N>`      | PF2          |
| QDMA     | `slash/qdma.h`    | `/dev/slash_qdma_ctl<N>` | PF1          |
| Hotplug  | `slash/hotplug.h` | `/dev/slash_hotplug`     | —            |

## Building

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

/* Get an fd for data transfer — read() = C2H, write() = H2C */
int fd = slash_qdma_qpair_get_fd(qdma, qid, O_CLOEXEC);
write(fd, buf, len);   /* H2C */
read(fd, buf, len);    /* C2H */
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
cd build && ctest
```

Tests run in mock mode and do not require hardware or the kernel module
to be loaded.

## Project layout

```
libslash/
  include/slash/
    ctldev.h              Public API — control device
    qdma.h                Public API — QDMA
    hotplug.h             Public API — hotplug
    uapi/
      slash_interface.h   User-kernel ABI (ctldev + QDMA ioctls)
      slash_hotplug.h     User-kernel ABI (hotplug ioctls)
  src/
    ctldev.c              Control device implementation
    ctldev_mock.c         Mock-mode BAR backing
    qdma.c                QDMA implementation
    hotplug.c             Hotplug implementation
  examples/
    01_bar/print_bar.c    Enumerate and read/write BARs
    02_test/some_tb.c     Multi-core HBM transfer testbench
  tests/
    slash_mock_tests.c    Unit tests (mock mode)
```

## License

MIT.  See the license header in `CMakeLists.txt` for the full text.

The UAPI headers under `include/slash/uapi/` are dual-licensed
`GPL-2.0-only OR MIT` so they can be included by both the GPL kernel
module and the MIT userspace library without ambiguity.
