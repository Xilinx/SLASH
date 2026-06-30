# v80-smi

Command-line tool for managing AMD Alveo V80 devices.  v80-smi can
enumerate boards, inspect vrtbin metadata, program devices, reset
hardware, write the static shell, and validate memory integrity and bandwidth.

| Command    | Purpose                                           |
|------------|---------------------------------------------------|
| `version`  | Print build version                               |
| `list`     | Enumerate V80 boards and check readiness          |
| `inspect`  | Display metadata from a vbin file on disk         |
| `query`    | Display metadata of the vbin loaded on a device   |
| `program`  | Load a vbin file onto a V80 device                |
| `reset`    | Hardware-reset a V80 board                        |
| `write-static-shell` | Write the static SLASH shell to a V80 board |
| `validate` | Reset board and test memory integrity + bandwidth |
| `debug`    | Low-level BAR, memory, clock, and hotplug debug utilities |

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

This installs the `v80-smi` binary to `<prefix>/bin/` and the JTAG helper
script to `<prefix>/share/v80-smi/`.

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
- **PF1** (device 0x50C1) &mdash; expected driver: `slash_qdma`
- **PF2** (device 0x50C2) &mdash; expected driver: `slash_ctl`
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

### write-static-shell

Write the installed static SLASH shell PDI to a V80 board.  The `--flash`
mode programs the flash-image PDI through VRTD cfgmem programming.
The `--jtag` mode programs the no-FPT PDI over JTAG with `xsdb`.

```
v80-smi write-static-shell --flash -d <BDF> [--pdi <file>]
v80-smi write-static-shell --jtag -d <BDF> [--pdi <file>] [--xsdb-target-id <id>] [--bash-source <file> ...]
v80-smi write-static-shell --jtag --no-remove-device [--pdi <file>] [--xsdb-target-id <id>] [--bash-source <file> ...]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `--flash`         | Program the flash-image PDI via VRTD cfgmem programming |
| `--jtag`          | Program the no-FPT PDI over JTAG via `xsdb`          |
| `-d,--device`     | Board address, required except with `--jtag --no-remove-device` |
| `--pdi`           | Use this PDI file instead of resolving the installed static shell PDI |
| `--no-remove-device` | Skip the pre-JTAG PCIe device removal; valid only with `--jtag` |
| `--bash-source`   | Source a Vivado/Vitis setup script before running `xsdb`; may be repeated and is valid only with `--jtag` |
| `--xsdb-target-id` | Select the `Versal xcv80` XSDB `target_id`; valid only with `--jtag` |

Both modes resolve their PDI path with `python3 -m slashkit static-shell-path`,
so setting `PYTHONPATH` can select an in-repo `slashkit`.  Use `--pdi` to bypass
that resolution during active development; the file must match the selected
mode (`--flash` expects a flash-image PDI, `--jtag` expects a no-FPT/JTAG-bootable
PDI).  JTAG mode removes the board's PCIe functions via VRTD unless
`--no-remove-device` is given, runs `/bin/bash -c 'source ...; xsdb ...'` with
`PDI_PATH` set to the selected PDI, optionally sets `V80_TARGET_ID` from
`--xsdb-target-id`, and rescans PCIe through VRTD afterward.

The command prints progress to stderr.  Flash mode reports VRTD cfgmem phases
and interval-based PDI download progress, while JTAG mode reports local stages
such as PCIe removal, `xsdb`, and PCIe rescan.

### validate

Optionally reset a board, then test HBM and DDR memory for data integrity and
bandwidth. Raw transfer modes skip reset and bypass the default VRTD buffer
path for data movement.

```
v80-smi validate -d <BDF> [-j <threads>] [-R] [--mm-channel <spec>] [--buffer-size <size>] [--offset <size>] [--starting-offset <size>] [--raw-transfer-test | --use-qdma-driver] [--ddr-only | --hbm-only] [--channel-allocation <auto|paired>] [--channel-region-stride <size>] [--ring-size-index <0-15>] [--bandwidth-iterations <N>] [--bandwidth-duration <seconds>]
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address (required), e.g. `03:00` or `0000:03:00` |
| `-j,--threads`    | Parallel buffers/threads, 1-64 (default 8). Bidirectional phases use `2 * threads` logical positions in each enabled memory space. |
| `-R,--no-reset`   | Skip the device reset step before running memory tests |
| `--mm-channel`    | AXI-MM/NoC channel per buffer queue: `auto` (default; driver stripes by `qid&1`), `0`, or `1`, or a comma-separated list with exactly one entry per buffer position (`2 x --threads` entries, e.g. `-j 1` -> `0,1`); no repeating, wrong length errors. Independent of `--channel-allocation`; also honored by `--use-qdma-driver`. |
| `--buffer-size`   | Size of each test buffer, accepting bytes or `k`/`K`/`m`/`M` suffixes (default `512M`, maximum `512M`) |
| `--offset`        | Distance between logical buffer positions (default `512M`) |
| `--starting-offset` | Offset from each memory-space base for logical position 0 (default `0`) |
| `--raw-transfer-test` | Use libslash raw QDMA transfers instead of VRTD buffers; implies `--no-reset` |
| `--use-qdma-driver` | Run the raw transfer test over the off-the-shelf Xilinx QDMA driver instead of SLASH; implies `--no-reset`; mutually exclusive with `--raw-transfer-test` |
| `--ddr-only`      | Run only DDR memory tests (skip HBM); mutually exclusive with `--hbm-only` |
| `--hbm-only`      | Run only HBM memory tests (skip DDR); mutually exclusive with `--ddr-only` |
| `--channel-allocation` | Raw-transfer-only placement: `auto` (default; mm-channel `qid&1`, linear addressing) or `paired` (couple mm-channel to a distinct memory region/NSU: even positions -> region 0/channel 0, odd -> region 1/channel 1). `paired` mirrors dma-perf `offset_ch0`/`offset_ch1` so both NoC NMUs drive independent memory endpoints. |
| `--channel-region-stride` | In `--channel-allocation paired`, byte distance between the two per-channel regions (NSU stride). Default `16G` (half the per-memory space); accepts `k`/`K`/`m`/`M`/`g`/`G`. |
| `--ring-size-index` | Raw-transfer-only descriptor-ring size index, `0`-`15`. Overrides the backend default when creating SLASH raw qpairs or starting stock-driver queues. |
| `--bandwidth-iterations` | Raw-transfer-only sustained bandwidth mode: repeat each whole-buffer transfer this many times in each bandwidth phase (default `1`). |
| `--bandwidth-duration` | Raw-transfer-only duration mode: repeat whole-buffer transfers until this many seconds have elapsed; `0` disables duration mode and uses `--bandwidth-iterations`. |

Each buffer defaults to 512 MB (one HBM/DDR allocator region).  The integrity test
writes a pattern, syncs to device, clears host memory, syncs back, and
verifies.  Each bandwidth
phase reports single-direction C2H reads, single-direction H2C writes,
and simultaneous bidirectional throughput (read, write, and total).  After
the per-memory phases, a final parallel phase drives HBM and DDR together
using `2 x <threads>` buffers for single-direction tests and `4 x <threads>`
threads for bidirectional tests; it is skipped when `--ddr-only` or
`--hbm-only` is given.  With `--raw-transfer-test`, the command bypasses
VRTD for transfers and opens the board's SLASH QDMA device directly, so
the SLASH QDMA driver node must be present.

Buffers are placed at `memory_base + starting-offset + position * offset`.
The position sequence is `0..N-1` for single-direction phases and `0..2N-1`
for bidirectional phases (reads on even positions, writes on odd positions).
`--buffer-size`, `--offset`, and `--starting-offset` must be 4 KiB-aligned,
`--offset` must be at least
`--buffer-size`, and the highest buffer must fit within the 64 x 512 MB DDR/HBM
address space. If any placement option is
specified in default VRTD mode, `validate` uses raw VRTD buffers so the exact
addresses are honored; this requires raw memory access permission.

The largest phase maps up to `4 x <threads> x <buffer-size>` of host buffers
when HBM and DDR are both enabled, or `2 x <threads> x <buffer-size>` with
`--ddr-only` or `--hbm-only`; `validate` fails early if that footprint exceeds
currently available host memory.

Raw transfer modes can repeat the bandwidth phases without changing buffer
placement. `--bandwidth-iterations` repeats each whole-buffer
transfer a fixed number of times, while `--bandwidth-duration` runs each
bandwidth phase for a wall-clock duration and counts completed whole-buffer
transfers. Integrity checks remain one-shot.
`--ring-size-index` can override the QDMA descriptor-ring size index for these
raw modes; useful A/B values for 4 KiB descriptor throughput are `0`, `11`,
`13`, and `15`.

With `--use-qdma-driver`, the command runs the same raw test over the
off-the-shelf Xilinx QDMA driver (`submodules/qdma_drv`) instead of SLASH.
smi provisions the queues itself: it raises the function's `qmax` via sysfs
if needed, creates and starts bidirectional AXI-MM queue pairs over generic
netlink (the same `xnl_pf` interface `dma-ctl` uses), then transfers over the
per-queue char devices `/dev/qdma<idx>-MM-<qid>`.  This requires the stock
`qdma-pf` driver to be bound to the board's PF (it cannot be bound at the same
time as the SLASH driver), and typically needs root to raise `qmax` and open
the queue devices.  The device memory addresses tested (HBM/DDR) are the same
AXI addresses used by the SLASH path.

Requirements depend on the selected mode: the default path needs VRTD and root
for reset unless `--no-reset` is used; `--raw-transfer-test` needs the SLASH
QDMA driver node; `--use-qdma-driver` needs a build with
`SMI_ENABLE_QDMA_DRIVER_BACKEND=ON` and the stock QDMA driver bound to the
board.

```console
$ v80-smi validate -d 03:00
Resetting device 0000:03:00...
Testing HBM data integrity (8 regions)...
    8/8 OK
Testing HBM read bandwidth (8 threads)...
    Read: 9547.22 MB/s
Testing HBM write bandwidth (8 threads)...
    Write: 9832.10 MB/s
Testing HBM bidirectional bandwidth (16 threads)...
    Read:  9210.15 MB/s
    Write: 9475.81 MB/s
    Total: 18685.96 MB/s
Testing DDR data integrity (8 buffers)...
    8/8 OK
Testing DDR read bandwidth (8 threads)...
    Read: 4980.33 MB/s
Testing DDR write bandwidth (8 threads)...
    Write: 5120.45 MB/s
Testing DDR bidirectional bandwidth (16 threads)...
    Read:  4860.12 MB/s
    Write: 5012.34 MB/s
    Total: 9872.46 MB/s
Testing HBM+DDR read bandwidth (16 threads)...
    Read: 11890.55 MB/s
Testing HBM+DDR write bandwidth (16 threads)...
    Write: 12450.78 MB/s
Testing HBM+DDR bidirectional bandwidth (32 threads)...
    Read:  11340.12 MB/s
    Write: 12020.34 MB/s
    Total: 23360.46 MB/s
```

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

### debug hotplug-op

Perform low-level PCIe hotplug operations through the vrtd hotplug-op API.

```
v80-smi debug hotplug-op --op rescan
v80-smi debug hotplug-op -d <BDF> --op <remove|hotplug> [--function <N>]
v80-smi debug hotplug-op -d <BDF> --op toggle-sbr --function <N>
```

| Flag              | Description                                          |
|-------------------|------------------------------------------------------|
| `-d,--device`     | Board address, e.g. `03:00` or `0000:03:00`; required except for `rescan` |
| `--op`            | Operation: `rescan`, `remove`, `toggle-sbr`, or `hotplug` |
| `--function`      | PCI function number, range `0-7`; defaults to all PFs for `remove` and `hotplug`; required for `toggle-sbr` |

Rules:

- `rescan` is device-independent and rejects both `--device` and `--function`.
- `remove` and `hotplug` default to all V80 PFs (PF0, PF1, PF2) so vrtd does not leave dangling PFs behind.
- Passing `--function` to `remove` or `hotplug` targets only that PCI function.
- `toggle-sbr` requires `--function` because it uses a single PF BDF to find the upstream bridge.
- `reset-sequence` is not exposed here; use `v80-smi reset` for the full board reset flow.
- `rescan` is unauthenticated in vrtd.
- Other operations require `pcie-hotplug` permission in the user's vrtd role.

Examples:

```console
$ v80-smi debug hotplug-op --op rescan
hotplug_op=rescan

$ v80-smi debug hotplug-op -d 03:00 --op remove
hotplug_op=remove bdf=0000:03:00 function=all

$ v80-smi debug hotplug-op -d 03:00 --op remove --function 2
hotplug_op=remove bdf=0000:03:00 function=2
```

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
| libslash   | Raw SLASH QDMA backend for `validate --raw-transfer-test` |
| slashkit   | Static-shell PDI path resolution for `write-static-shell` |
| xsdb       | JTAG programming backend for `write-static-shell --jtag` |
| qdma_nl.h  | Optional stock QDMA-driver backend (`SMI_ENABLE_QDMA_DRIVER_BACKEND=ON`) |

## Project layout

```
smi/
  src/
    smi.cpp           Entry point and subcommand dispatch
    list.cpp/hpp      Board enumeration via sysfs
    inspect.cpp/hpp   Vbin metadata inspection and device query
    program.cpp/hpp   Device programming
    reset.cpp/hpp     Hardware reset via VRTD
    write_static_shell.cpp/hpp  Static shell flash/JTAG programming
    validate.cpp/hpp  Memory integrity and bandwidth testing
    raw_transfer.hpp  Shared raw QDMA host mapping and transfer helpers
    qdma_driver_backend.cpp/hpp  Optional stock QDMA-driver validate backend
    debug/bar_poke.cpp/hpp  BAR read/write debug command
    debug/mem_poke.cpp/hpp  Raw device memory read/write command
    debug/clockwiz.cpp/hpp  Clock read/set debug command
    debug/hotplug.cpp/hpp   PCIe hotplug debug command
    bdf.hpp           BDF address parser
    utils.hpp         Formatting and output utilities
  resources/
    versal_flash_pdi.tcl  JTAG PDI programming script installed with v80-smi
```

## License

MIT — see [LICENSE](../LICENSE).
