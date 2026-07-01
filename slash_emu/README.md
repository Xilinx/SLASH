# slash_emu — SLASH System Emulation Daemon

`slash_emu` is a user-space daemon that emulates the SLASH kernel driver via
UNIX domain sockets, enabling application testing without physical hardware.
See `/SLASH/architecture.md` for the full design document.

## Building

### Normal build (no sanitisers)

```sh
cmake -S . -B build/normal -DSLASH_EMU_BUILD_TESTS=ON
cmake --build build/normal -j
```

### AddressSanitizer build

```sh
cmake -S . -B build/asan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_ASAN=ON
cmake --build build/asan -j
```

### UndefinedBehaviourSanitizer build

```sh
cmake -S . -B build/ubsan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_UBSAN=ON
cmake --build build/ubsan -j
```

### Combined ASan + UBSan build

```sh
cmake -S . -B build/aubsan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
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
cmake -S . -B build/coverage -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_COVERAGE=ON
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
# Per-accelerator keys may be added in future steps (e.g. vbin_path).

[device.0000:62:00]
```

### CLI reference

```
slash_emu_daemon -c <config> [options]

  -c, --config <file>     INI configuration file (required, must exist)
  -d, --base-dir <path>   Base directory for sockets (default: /run/slash_emu,
                          must be an absolute path)
  -u, --uid <name|id>     Socket owner UID — user name or numeric ID
                          (default: vrtd, falls back to current UID if absent)
  -g, --gid <name|id>     Socket owner GID — group name or numeric ID
                          (default: vrt, falls back to current GID if absent)
  -m, --mode <octal>      Socket permission mode in octal
                          (default: 600; max: 7777)
      --version           Print version and exit
      --help              Print help and exit
```

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
- `--mode` must be valid octal digits in [0, 7777].
- `--uid` / `--gid` that are explicitly specified must resolve to a known
  user/group; the built-in defaults `vrtd` / `vrt` fall back to the current
  process uid/gid with a warning if those accounts do not exist.

## Running the daemon

```sh
./build/normal/src/slash_emu_daemon -c /etc/slash_emu/config.ini
```

Send `SIGINT` or `SIGTERM` to shut it down cleanly.
