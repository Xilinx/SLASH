# slash_sysemu — SLASH System Emulation Daemon

`slash_sysemu` is a user-space daemon that emulates the SLASH kernel driver via
UNIX domain sockets, enabling application testing without physical hardware.

The design is documented where it lives: each subsystem's header carries the
authoritative design notes and rationale as block comments. The sections below
give the conceptual overview, glossary, component map, and MVP scope needed to
navigate them.

## Overview

Instead of the driver's character devices and `ioctl()`s, the daemon exposes
`AF_UNIX`/`SOCK_SEQPACKET` sockets with identical names and turns each `ioctl`
into a request/response datagram. Where an `ioctl` returns a file descriptor the
daemon returns success and passes the FD as `SCM_RIGHTS` ancillary data; where an
`ioctl` argument struct carries FDs, the sender passes them as ancillary data and
references them by index. libslash hides this difference: it checks the file type
and either issues an `ioctl` (real device) or a datagram exchange (socket).

A *model process* (the `vpp_sim` executable from a VBIN) models the FPGA behind a
ZeroMQ protocol; the daemon drives it and reflects its state into memfd-backed
BARs that the user maps.

### Glossary

- **System emulation** — emulating the *entire* accelerator in the host: the
  FPGA, its memory, its PCIe connection, and how VRT/VRTD and the user
  application interact with it. This daemon does system emulation. It is
  independent of, and combinable with, the two terms below.
- **FPGA emulation / FPGA simulation** — two ways to model the behaviour of the
  *programmable logic itself* in software. How the PL is modelled is orthogonal to
  system emulation. This sprint supports only the *simulation* dialect (`vpp_sim`).
- **Model process** — a process that models one FPGA's behaviour (by simulation
  or emulation) and that the daemon communicates with over ZeroMQ.

## Architecture

One daemon serves many accelerators, each identified by its board BDF (the PCI
BDF without the function suffix, e.g. `0000:61:00`). Per accelerator the daemon
tracks six components — the main+staging VBIN files, the model process, the model
control workers, a PF0 stub, the QDMA subsystem (PF1), and the BAR/device-info
subsystem (PF2) — through an Absent/Inactive/Active/Partial state machine driven
from the `slash_hotplug` socket.

Authoritative design notes live in the source. Component map (files under `src/`):

| Area | Files | What it does |
|------|-------|--------------|
| Transport / protocol | `transport.*`, `protocol.h` | SEQPACKET framing, `slash_sysemu_socket_header`, SCM_RIGHTS FD passing by index, `Result<T>`/`ErrorKind` |
| Config / CLI | `config.*` | INI + CLI parsing, `BoardBdf`, socket-path helpers |
| VBIN / system map | `vbin.*`, `system_map.*` | unpack a VBIN, parse the system map into kernels/registers, reverse register→address |
| Model client | `model_client.*` | serialized `vpp_sim` ZeroMQ dialect client |
| Model lifecycle | `vbin_store.*`, `model_process.*`, `reconfigure.*`, `worker_controller.h` | main/staging store, process launch/death, staging→main reconfiguration |
| BARs | `bar_memfd.*` | memfd-backed BAR windows with flock brackets |
| Model control workers | `model_control_workers.*` | per-kernel idle→busy→idle FSM + clock-wizard lock pin |
| PF2 (CTL) | `ctl_subsystem.*`, `ctl_ioctls.h` | `slash_ctl<N>`: BAR info/fd, device info |
| PF1 (QDMA) | `qdma_subsystem.*`, `qdma_ioctls.h` | `slash_qdma_ctl<N>`: qpair FSM, H2C/C2H transfers, reconfig aperture |
| Accelerator / hotplug | `accelerator.*`, `hotplug_subsystem.*`, `daemon.*` | per-BDF state machine, `slash_hotplug`, lifecycle lock, sd-event loop |
| Default model | `model/` | from-source default VBIN (round-trip memory, no kernels) |

The libslash socket transport (client side) lives in `driver/libslash/`.

## Scope (MVP)

This daemon is a minimum viable product. The following are intentionally **out of
scope** for this sprint; the design leaves room to add them later:

- **Virtual network setups** — persistently connecting accelerators into
  (virtual) networks.
- **Non-polling BAR interface** — the dmabuf-shaped model requires the daemon to
  poll BAR memory; a read/write-syscall interface would remove polling but needs a
  kernel-driver refactor.
- **FPGA-emulation-model support** — emulation models cannot be polled for kernel
  state asynchronously, so only `vpp_sim` simulation models are supported.
- **Hardened model-process isolation** — the model executable is untrusted user
  code and should be sandboxed further.
- **Persisting HBM/DDR across reconfiguration** — buffer contents are owned by the
  model process and are lost when it is replaced.

## Building

### Normal build (no sanitisers)

```sh
cmake -S . -B build/normal -DSLASH_SYSEMU_BUILD_TESTS=ON
cmake --build build/normal -j
```

### AddressSanitizer build

```sh
cmake -S . -B build/asan -DSLASH_SYSEMU_BUILD_TESTS=ON -DENABLE_ASAN=ON
cmake --build build/asan -j
```

### UndefinedBehaviourSanitizer build

```sh
cmake -S . -B build/ubsan -DSLASH_SYSEMU_BUILD_TESTS=ON -DENABLE_UBSAN=ON
cmake --build build/ubsan -j
```

### Combined ASan + UBSan build

```sh
cmake -S . -B build/aubsan -DSLASH_SYSEMU_BUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build/aubsan -j
```

## Running the tests

```sh
ctest --test-dir build/normal --output-on-failure
ctest --test-dir build/asan   --output-on-failure
ctest --test-dir build/ubsan  --output-on-failure
ctest --test-dir build/aubsan --output-on-failure
```

## Coverage report

Build with coverage instrumentation, then invoke the `coverage` target:

```sh
cmake -S . -B build/coverage -DSLASH_SYSEMU_BUILD_TESTS=ON -DENABLE_COVERAGE=ON
cmake --build build/coverage -j
cmake --build build/coverage --target coverage
```

The HTML report is written to `build/coverage/coverage_html/index.html`.

Note: `ENABLE_COVERAGE` is mutually exclusive with `ENABLE_ASAN` and
`ENABLE_UBSAN`; CMake will error if both are set simultaneously.

## Configuration

The daemon requires an INI-format configuration file that lists the emulated
accelerators.  Pass it with `-c` / `--config`.

### Sample configuration file

```ini
# Each [device.<BDF>] section declares one emulated accelerator.
# The BDF must be in "DDDD:BB:DD" format (no function suffix).

[device.0000:61:00]

[device.0000:62:00]
vbin_path = /path/to/design.vbin # Override the default VBIN to instantiate the accelerator with
```

### CLI reference

```
slash_sysemud -c <config> [options]

  -c, --config <file>     INI configuration file (required, must exist)
  -d, --base-dir <path>   Base directory for the sockets. Must be an absolute
                          path. Default: $RUNTIME_DIRECTORY (i.e. /run/slash_sysemu
                          under systemd), else /run/slash_sysemu.
      --default-vbin <f>  Daemon-wide default VBIN used to bootstrap a fresh
                          accelerator that has no main.vbin yet. Precedence
                          (low → high): compiled-in installed default,
                          $SLASH_SYSEMU_DEFAULT_VBIN, then this flag.
      --version           Print version and exit
      --help              Print help and exit
```

Socket ownership and permissions are **not** daemon options: under systemd the
daemon runs as `User=`/`Group=` and the sockets inherit that identity, with their
mode coming from the unit's `UMask=`. The daemon performs no chown/chmod itself.

The daemon creates the following sockets under `<base-dir>`:

| Socket              | Purpose                                              |
|---------------------|------------------------------------------------------|
| `slash_ctl<N>`      | BAR enumeration, MMIO access, PCI device identity    |
| `slash_qdma_ctl<N>` | DMA queue pairs; bulk data movement                  |
| `slash_hotplug`     | Daemon-level hotplug control (single instance)       |

`<N>` is the zero-based index of the accelerator in the configuration file.

### Validation

`parse_cli` enforces these constraints and returns a clear error if violated:

- `--config` must point to an existing file containing at least one
  `[device.*]` section.
- `--base-dir` must be an absolute path (starts with `/`).
- Board BDFs must be in `DDDD:BB:DD` format; function suffixes (`.2`) are
  rejected.
- Duplicate board BDFs within a config file are rejected.

## Running the daemon

```sh
./build/normal/src/slash_sysemud -c /etc/slash_sysemu/config.ini
```

Send `SIGINT` or `SIGTERM` to shut it down cleanly.
