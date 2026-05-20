# vrtd — V80 Runtime Daemon

vrtd is a systemd-managed daemon written in C (gnu11) that multiplexes
access to AMD Alveo V80 FPGA devices managed by the SLASH kernel
module. It communicates with client applications over an AF_UNIX
socket using a binary request/response protocol, passing device file
descriptors out-of-band via `SCM_RIGHTS`.

## Architecture

vrtd sits between the client libraries and the low-level driver stack:

```
libvrtdpp  (C++ RAII wrapper)
libvrtd    (C wire-protocol client library)
──────── AF_UNIX / SOCK_SEQPACKET ────────
vrtd       (this daemon)
libslash   (driver wrapper)
Linux kernel module (slash)
AMD Alveo V80 hardware
```

Key responsibilities:

- Sysfs-based automatic device discovery at startup
- Per-client DMA buffer management (HBM and DDR) with automatic
  cleanup on disconnect
- FPGA bitstream programming via the design writer subsystem
- Clock frequency control via AXI clock wizard
- PCIe hotplug (secondary bus reset) support
- Role-based access control with per-device granularity
- Systemd integration: socket activation, watchdog, sd-event loop,
  journal logging

## Directory layout

| Path | Contents |
|------|----------|
| `src/` | Daemon source code (C11) |
| `include/vrtd/` | Public wire-protocol headers shared with libvrtd |
| `libvrtd/` | C client library for the vrtd wire protocol |
| `libvrtdpp/` | C++ RAII wrapper around libvrtd |
| `conf/` | Default configuration file (`vrtd.conf`) |
| `systemd/` | systemd service and socket unit files |
| `sysusers/` | systemd-sysusers configuration (vrtd user/group) |
| `udev/` | udev rules for device permissions |
| `cmake/` | CMake config-file templates |

## Building

**Prerequisites:** libslash must be installed first (or built in-tree
with `-DVRTD_INCLUDE_LIBSLASH=ON`). System dependencies:

```bash
sudo apt install cmake pkg-config libsystemd-dev libinih-dev
```

**Build:**

```bash
cd vrt/vrtd
cmake -B build -S . -G Ninja
cmake --build build
sudo cmake --install build
```

## Running

```bash
# Manual
sudo vrtd

# Production (systemd)
sudo systemctl enable --now vrtd
```

The daemon reads its configuration from `/etc/vrt/vrtd.conf` (see
`conf/vrtd.conf` for the default format). The configuration file uses
an INI-style format to define roles, user permissions, and per-device
access rules.

## Minimum toolchain versions

The baseline is Ubuntu 22.04 LTS/RHEL 9. Lower versions may work but are
untested and may break in any update.

| Dependency | Minimum version |
|------------|-----------------|
| CMake | 3.22.1 |
| GCC | 11.4.0 |
| glibc | 2.35 |
| Linux | 5.15.0 |
| libsystemd | 249.11 |

Developers contributing to vrtd are encouraged to make use of useful
extensions and capabilities as long as they are supported by the
versions listed above.

## Coding conventions

**Read this section before writing C code for this daemon.**

This daemon is not written in standard/POSIX C, but instead leans
heavily on C11, libsystemd, glibc features, Linux syscall features,
and GNU compiler extensions (also supported by Clang). The goal is not
to write a portable application, but a modern systemd daemon using all
the tools at our disposal.

The language version is **C11** with GNU extensions (`-std=gnu11`).
C23 features should not be used unless they are available as GNU
extensions.

All `.c` source files must start with `#define _GNU_SOURCE` after the
copyright header and before including any other headers.

For the complete coding style guide — error handling, macros, ownership
rules, RAII patterns, GNU extensions, naming, and formatting — see
**[STYLE.md](STYLE.md)**.

## License

MIT — see [LICENSE](../../LICENSE).
