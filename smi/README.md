# v80-smi

Command-line tool for managing AMD Alveo V80 devices.  v80-smi can
enumerate boards, inspect vrtbin metadata, program devices, reset
hardware, and validate memory integrity and bandwidth.

| Command    | Purpose                                           |
|------------|---------------------------------------------------|
| `version`  | Print build version                               |
| `list`     | Enumerate V80 boards and check readiness          |
| `inspect`  | Display metadata from a vbin file on disk         |
| `query`    | Display metadata of the vbin loaded on a device   |
| `program`  | Load a vbin file onto a V80 device                |
| `reset`    | Hardware-reset a V80 board                        |
| `validate` | Reset board and test memory integrity + bandwidth |
| `debug`    | Low-level BAR, memory, and clock debug utilities   |

## Building

```sh
cmake -B build -S . -G Ninja
cmake --build build
```

CMake options:

| Option            | Default | Description                                  |
|-------------------|---------|----------------------------------------------|
| `SMI_INCLUDE_VRT` | `OFF`   | Build the bundled VRT library instead of using the system package |

Requires a C++20 compiler.

## Installing

```sh
sudo cmake --install build --prefix /usr/local
```

This installs the `v80-smi` binary to `<prefix>/bin/`.

## Commands

### version

Print build version and exit.

```
v80-smi version [-p]
```

| Flag           | Description                                              |
|----------------|----------------------------------------------------------|
| `-p,--plain`   | Print only the version number (useful in scripts)        |

```console
$ v80-smi version
SMI v1.2.3

$ v80-smi version --plain
1.2.3
```

### list

Enumerate V80 boards by scanning sysfs for matching PCI vendor/device
IDs.  Each board's readiness is checked across all three PCI functions
and the VRTD daemon.

```
v80-smi list [-l] [-s] [-j | -J]
```

| Flag               | Description                                             |
|--------------------|---------------------------------------------------------|
| `-l,--long`        | Print detailed per-PF info (vendor/device ID, driver, NUMA node, IRQ) |
| `-s,--sensors`     | Include sensor readings from VRTD (temperature, current, voltage, power) |
| `-j,--json`        | Compact JSON output                                     |
| `-J,--pretty-json` | Indented JSON output                                    |

Readiness checks per board:

- **PF0** (device 0x50B4) &mdash; expected driver: `ami`
- **PF1** (device 0x50B5) &mdash; expected driver: `slash_qdma`
- **PF2** (device 0x50B6) &mdash; expected driver: `slash_ctl`
- **VRTD** &mdash; daemon reachable and device registered

```console
$ v80-smi list
Board 0000:03:00 OK (PF0: OK) (PF1: OK) (PF2: OK) (VRTD: OK)
Board 0000:83:00 NOT READY (PF0: OK) (PF1: NOT READY: wrong driver) (PF2: OK) (VRTD: NOT READY)
```

### inspect

Display metadata from a vbin file without hardware.  Shows the target
platform, clock frequency, resource utilization, and kernel argument
maps.

```
v80-smi inspect <vbin> [-j | -J]
```

| Flag               | Description                   |
|--------------------|-------------------------------|
| `-j,--json`        | Compact JSON output           |
| `-J,--pretty-json` | Indented JSON output          |

```console
$ v80-smi inspect design.vbin
Vbin design.vbin:
    Platform: HARDWARE
    Clock frequency: 300000000
    Utilization:
        Slash: LUTs: 45032 (5.2%), FFs: 62001 (3.6%), LUTRAM: 3200 (0.7%), SRL: 1100 (0.3%), RAMB36: 48, RAMB18: 12, URAM: 0, DSP: 12
    Kernel:
        Name: increment_0
        Physical address: 0x20100000000
        Argument:
            Index: 0
            Name: size
            Type: int
            Offset: 0x10
            Range: 0x20
            Direction: Read
```

No hardware or VRTD required.

### query

Same output as `inspect`, but reads metadata from the vbin last
programmed by the user on a device.

```
v80-smi query -d <BDF> [-j | -J]
```

| Flag               | Description                                         |
|--------------------|-----------------------------------------------------|
| `-d,--device`      | Board address (required), e.g. `03:00` or `0000:03:00` |
| `-j,--json`        | Compact JSON output                                 |
| `-J,--pretty-json` | Indented JSON output                                |

Requires the device to have been programmed at least once.

### program

Load a vbin file onto a V80 device.

```
v80-smi program <vbin> -d <BDF>
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |

```console
$ v80-smi program design.vbin -d 03:00
```

### reset

Hardware-reset a V80 board.  Performs the full hotplug sequence
(remove &rarr; SBR &rarr; settle &rarr; rescan &rarr; hotplug) via the
VRTD daemon.

```
v80-smi reset -d <BDF>
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |

Requires root access and a running VRTD daemon.  The device must be
programmed with the static SLASH design.

### validate

Reset a board, then test HBM and DDR memory for data integrity and
bandwidth.

```
v80-smi validate -d <BDF> [-j <threads>]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |
| `-j,--threads`    | Parallel buffers/threads, 1-64 (default 8)           |

Each buffer is 64 MB.  The integrity test writes a pattern, syncs to
device, clears host memory, syncs back, and verifies.  The bandwidth
test runs parallel H2C writes and C2H reads.

```console
$ v80-smi validate -d 03:00
Resetting device 0000:03:00...
Testing HBM data integrity (8 regions)...
    HBM0: OK
    HBM1: OK
    ...
Testing HBM bandwidth (8 threads)...
    Write: 9832.10 MB/s
    Read:  9547.22 MB/s
Testing DDR data integrity (8 buffers)...
    DDR0: OK
    DDR1: OK
    ...
Testing DDR bandwidth (8 threads)...
    Write: 5120.45 MB/s
    Read:  4980.33 MB/s
```

Requires root access and a running VRTD daemon.

### debug bar-poke

Perform low-level BAR reads or writes for troubleshooting.

```
v80-smi debug bar-poke -d <BDF> -b <bar> (-r | -w) [-x] [-W <size>] [-c <count>] <address> [value]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |
| `-b,--bar`        | BAR number (required), range `0-5`                   |
| `-r,--read`       | Read operation (required unless `--write`)           |
| `-w,--write`      | Write operation (required unless `--read`)           |
| `-x,--hex`        | Print read output in hex                               |
| `-W,--word-size`  | Word size in bytes: `1`, `2`, `4`, or `8` (default `4`) |
| `-c,--count`      | Number of words to read (default `1`; must be `1` for write) |

Rules:

- Exactly one of `--read` or `--write` must be provided.
- `<address>` is a BAR-relative byte offset.
- `<value>` is required for `--write` and forbidden for `--read`.
- Input numbers are auto-detected: `0x...` is parsed as hex; otherwise values are parsed as base-10.
- `--hex` affects output formatting only.

Examples:

```console
$ v80-smi debug bar-poke -d 03:00 -b 4 --read 65536
0

$ v80-smi debug bar-poke -d 03:00 -b 4 --read --hex -W 4 -c 4 0x10000
0x0
0x1
0x2
0x3

$ v80-smi debug bar-poke -d 03:00 -b 4 --write --hex -W 4 0x10000 0x1
```

### debug mem-poke

Perform low-level raw memory reads or writes at device physical addresses.
This bypasses the allocator and requires raw-mem-access permission in vrtd.

```
v80-smi debug mem-poke -d <BDF> (-r | -w) [-x] [-W <size>] [-c <count>] <address> [value] [-f <path>]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |
| `-r,--read`       | Read operation (required unless `--write`)           |
| `-w,--write`      | Write operation (required unless `--read`)           |
| `-x,--hex`        | Hex output in read mode; hex text/hexdump file mode with `-f` |
| `-W,--word-size`  | Word size in bytes: `1`, `2`, `4`, or `8` (default `4`) |
| `-c,--count`      | Number of words (default `1`)                        |
| `-f,--file`       | File path for file-mode read/write                   |

Rules:

- Exactly one of `--read` or `--write` must be provided.
- `<address>` is a device physical address.
- In scalar mode (no `--file`):
    - `--write` requires `<value>` and `--count` must be `1`.
    - `--read` forbids `<value>`.
    - Address must be aligned to word size.
- In file mode (`--file`):
    - `<value>` is forbidden.
    - Byte count is exactly `word-size * count`.
    - With `--hex`: file is parsed/emitted as hex text (hexdump-compatible).
    - Without `--hex`: file is raw binary.

Examples:

```console
$ v80-smi debug mem-poke -d 03:00 --read --hex -W 4 -c 4 0x40000000
0x3f800000
0x40000000
0x40400000
0x40800000

$ v80-smi debug mem-poke -d 03:00 --write --hex -W 4 0x40000000 0x3f800000

$ v80-smi debug mem-poke -d 03:00 --write -W 4 -c 256 -f input.bin 0x40000000
```

### debug clockwiz

Read or set clock rates through the vrtd clock-op API.

```
v80-smi debug clockwiz -d <BDF> (--get | --set <rate_hz>) [--region <region>] [-x]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |
| `--get`           | Read current clock rate for selected region          |
| `--set`           | Set requested clock rate in Hz for selected region   |
| `--region`        | Clock region: `user` or `service` (default `user`)   |
| `-x,--hex`        | Print `--get` output in hex                          |

Rules:

- Exactly one of `--get` or `--set` must be provided.
- `--set` value is in Hz and must be greater than zero.
- `--hex` is valid only with `--get`.
- `--set` prints both requested and achieved frequencies.

Examples:

```console
$ v80-smi debug clockwiz -d 03:00 --get
300000000

$ v80-smi debug clockwiz -d 03:00 --get --region service --hex
0x11e1a300

$ v80-smi debug clockwiz -d 03:00 --set 300000000 --region user
requested_hz=300000000
achieved_hz=300000000
```

Requires a running VRTD daemon and clock permission in the user's role.

## Device addressing

All commands that accept a `-d,--device` option support four BDF
(Bus:Device.Function) formats:

| Format          | Example         | Notes                       |
|-----------------|-----------------|-----------------------------|
| `DDDD:BB:DD`    | `0000:03:00`    | Board-level, no function    |
| `BB:DD`         | `03:00`         | Short form                  |
| `DDDD:BB:DD.F`  | `0000:03:00.0`  | Full with PCI function (not recommended)     |
| `BB:DD.F`       | `03:00.0`       | Domain defaults to `0000` (not recommended)   |

All forms are normalised to board-level `DDDD:BB:DD`.  If a PCI
function digit is supplied it is accepted but ignored with a warning,
since v80-smi always operates at board granularity.

## Dependencies

| Dependency | Purpose                                          |
|------------|--------------------------------------------------|
| libvrt     | VRT runtime library (device, kernel, vrtbin APIs) |
| vrtd       | Runtime daemon (sensors, reset, validate, query)  |

## Project layout

```
smi/
  src/
    smi.cpp           Entry point and subcommand dispatch
    list.cpp/hpp      Board enumeration via sysfs
    inspect.cpp/hpp   Vbin metadata inspection and device query
    program.cpp/hpp   Device programming
    reset.cpp/hpp     Hardware reset via VRTD
    validate.cpp/hpp  Memory integrity and bandwidth testing
    debug/bar_poke.cpp/hpp  BAR read/write debug command
    debug/mem_poke.cpp/hpp  Raw device memory read/write command
    debug/clockwiz.cpp/hpp  Clock read/set debug command
    bdf.hpp           BDF address parser
    utils.hpp         Formatting and output utilities
```

## License

MIT — see [LICENSE](../LICENSE).
