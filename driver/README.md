# SLASH kernel module

## Testing

The test suite requires a physical V80 to be present and the module to be
loaded into a running kernel.

### Prerequisites

- A kernel built with `CONFIG_GCOV_KERNEL=y` (only needed for coverage runs).
- `lcov` and `genhtml` installed (only needed for coverage runs).
- The BDF identifier of the V80 card (e.g. `0000:03:00`).
    - You may be able to retrieve the BDF identifier by running `v80-smi list`

### Running the tests manually

Build the module and the test suite:

```sh
make          # builds slash.ko
make -C tests/ all
```

Load the module and rescan the PCI bus so the device nodes appear:

```sh
sudo insmod ./slash.ko
echo 1 | sudo tee /sys/bus/pci/rescan > /dev/null
```

Run the kselftest suite (must be run as root):

```sh
sudo make -C tests/ run
```

The suite produces TAP output. Each test fixture automatically tears down
queue pairs on failure, so a failing test does not leave the device in a
broken state.

#### Optional: override the DMA target address

The `write_read_verify` test defaults to DMA address `0x0`. Set
`SLASH_TEST_DMA_ADDR` to use a different address:

```sh
sudo SLASH_TEST_DMA_ADDR=0x100000000 make -C tests/ run
```

### Running with code-coverage instrumentation

`test_module.sh` automates the full build → load → test → coverage cycle:

```sh
./test_module.sh <BBBB:DD:FF>
```

Replace `<BBBB:DD:FF>` with the BDF of the V80 (e.g. `0000:03:00`).

The script:
1. Checks that the running kernel has `CONFIG_GCOV_KERNEL=y`.
2. Builds `slash.ko` with gcov instrumentation (`make GCOV=1`).
3. Builds the test suite.
4. Removes any currently-loaded `slash` module.
5. Resets the gcov counters.
6. Inserts the module and rescans the PCI bus.
7. Runs the full kselftest suite.
8. Removes the module.
9. Captures coverage with `lcov` and generates an HTML report in `coverage/`.

Open `coverage/index.html` in a browser to browse line-level coverage.
