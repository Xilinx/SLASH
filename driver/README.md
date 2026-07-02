# SLASH kernel module

## Module parameters

Exposed under `/sys/module/slash/parameters/` (all writable at runtime; see
`modinfo slash.ko`):

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `qdma_num_threads` | uint | 8 | Number of libqdma worker threads. |
| `qdma_debugfs_path` | charp | disabled | debugfs mount path for libqdma. |

### A/B testing NoC channel bandwidth

The AXI-MM / NoC channel is chosen per queue pair when it is added (the
`mm_channel` field of the qpair-add ioctl, `enum slash_qdma_mm_channel`):
`auto` stripes queues across both channels by `qid & 1`, while `0` / `1` pin a
queue to a single channel. Every queue creator carries this setting, so it can
be driven per buffer to check whether both PCIe NMUs (NoC channels) actually
contribute bandwidth. With `v80-smi validate`:

```sh
# All queues on NoC channel 0 (NMU S00)
sudo v80-smi validate -d <BDF> --raw-transfer-test --no-reset --mm-channel 0

# All queues on NoC channel 1 (NMU S01)
sudo v80-smi validate -d <BDF> --raw-transfer-test --no-reset --mm-channel 1

# Split across both channels (qid & 1)
sudo v80-smi validate -d <BDF> --raw-transfer-test --no-reset --mm-channel auto

# Explicit per-buffer split (even positions -> channel 0, odd -> channel 1)
sudo v80-smi validate -d <BDF> --raw-transfer-test --no-reset --mm-channel 0,1
```

Debug builds with `SLASH_QDMA_OP_DEBUG=1` log each queue's selected
`mm_channel` when it is added. If the split run is no faster than a single
forced channel, traffic is not being spread across both NMUs. The per-queue
setting affects every queue created through this driver (both the VRTD buffer
path and `--raw-transfer-test`); the off-the-shelf Xilinx QDMA driver path
(`--use-qdma-driver`) honors `--mm-channel` through its own channel attribute.

## Testing

The test suite requires a physical V80 to be present and the module to be
loaded into a running kernel.

## Local libqdma patches

SLASH carries small patches for the pinned `libqdma` submodule under
`driver/patches/`. The driver `Makefile` applies them before building, and
`make clean` attempts to revert them so the submodule working copy returns to
its pristine pinned state. DKMS packages include the same patch directory and
depend on `patch(1)`.

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
