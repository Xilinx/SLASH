# rp1_bringup_vrt

VRT-graph port of `examples/rp1_bringup`'s `diamond` stage. Ships its
own four-kernel bitstream and submits the canonical diamond DAG plus a
trailing sentinel `SIGNAL` through `vrt::graph::Graph` +
`vrt::graph::FpgaDevice` instead of hand-laying `rp1_node_t` packets.
This is the canonical regression test for the phase-1 VRT → RP1
integration on real silicon.

Topology:

```
      A
     / \
    B   C        four KERNEL_DISPATCH against bringup_kernel_{0..3}
     \ /
      D
      |
    SIGNAL       sentinel: writes 0xD1A1D0DD into slot 0
```

## Files

| Path | Purpose |
|---|---|
| `hls/bringup_kernel.{cpp,cfg}` | Trivial `bringup_kernel(size, in*)` — same AXI-Lite signature as `examples/00_axilite/hls/increment.cpp` but without the AXI-Stream output. With `size=0` the loop is a no-op so `ap_done` fires immediately. |
| `config.cfg` | `nk=bringup_kernel:4` plus four `sp=` lines binding each instance's `m_axi_gmem0` to HBM0. |
| `CMakeLists.txt` | Builds the HLS IP, links `rp1_bringup_vrt_hw.vbin`, and builds the host binary. |
| `rp1_bringup_vrt.cpp` | Host driver: opens BAR4 via vrtd, builds the diamond on `vrt::graph::Graph`, submits via `FpgaDevice`, verifies sentinel + CQ count. |

## Prerequisites

1. **Bitstream.** The base platform must provide the RP1 firmware-loading
   path, the host-visible BAR4 → DDR aperture at offset 64 MiB (where
   the RP1 control block at R5 `0x3000_0000` lives), and the RPU → NoC
   → user-region AXI-Lite path. This example's `add_vbin` call adds
   four kernel instances on top of that platform.
2. **Firmware.** Build `rp1.elf` with `RP1_POLLING_BRINGUP=ON` so the
   R5 spins on `graph_seq` instead of `wfi`'ing while the GCQ doorbell
   is still unwired:
   ```bash
   cd linker/resources/aved/rp1
   cmake -S . -B build-bringup -DRP1_POLLING_BRINGUP=ON
   cmake --build build-bringup
   # produces build-bringup/rp1.elf
   ```
3. **Load `rp1.elf` onto R5-1 via xsdb.** After load, the firmware
   should sit in `RP1_STATE_READY` with `heartbeat` advancing. Confirm
   with `examples/rp1_bringup dump /dev/slash_ctl0` before going
   further.
4. **vrtd running.** This example obtains BAR4 through `vrtd::Session`
   (it does not open `/dev/slash_ctl*` directly), so the daemon must
   be up and the calling user must have device access in
   `/etc/vrt/vrtd.conf`.

## Build

You need Vitis HLS / Vivado in your environment for the kernel build
(same as the other `examples/` projects). To skip Vivado and only
build the host binary (e.g. while iterating on the C++ driver), pass
`-DBUILD_KERNELS=OFF`.

### Against the repo tree (no installed packages required)

```bash
cd examples/rp1_bringup_vrt
cmake -B build -S . -DVRT_USE_REPO=ON
cmake --build build
# Outputs:
#   build/rp1_bringup_vrt_hw.vbin   (kernels + bitstream + system_map.xml)
#   build/rp1_bringup_vrt           (host driver)
```

### Against the installed `vrt` package

```bash
cd examples/rp1_bringup_vrt
cmake -B build -S .
cmake --build build
```

### Host-only build (no Vivado required)

```bash
cmake -B build -S . -DBUILD_KERNELS=OFF
cmake --build build
# Outputs:
#   build/rp1_bringup_vrt   (host driver only — you'll need a vbin from elsewhere)
```

## Run

```bash
# 1. Program the bitstream onto the board (standard SLASH bring-up flow).
v80-smi program --device <BDF> --image build/rp1_bringup_vrt_hw.vbin

# 2. Make sure rp1.elf is loaded onto R5-1 — see "Prerequisites" above.
#    Verify with the C tool's dump subcommand:
sudo ./<path-to>/rp1_bringup dump /dev/slash_ctl0
#    Expect magic=0x53515231 (SQR1), rp1_state=READY, heartbeat advancing.

# 3. Run the host driver.
./build/rp1_bringup_vrt                       # vrtd defaults, device index 0
./build/rp1_bringup_vrt --socket /run/vrtd.sock --device-index 0
./build/rp1_bringup_vrt --bdf 0000:65:00.0    # pick by PCI BDF
```

Pass iff:

- signal slot 0 reads back `0xD1A1D0DD`;
- exactly 5 CQ entries are emitted (4 kernels + 1 sentinel signal).

Expected output:

```
[rp1_bringup_vrt] connected to vrtd @ /run/vrtd.sock, device=<...> (<BDF>)
[rp1_bringup_vrt] compiled diamond (A=0x88000000 B=0x88010000 C=0x88020000 D=0x88030000), submitting...
PASS: slot[0]=0xd1a1d0dd cq_delta=5 state=1
```

## Kernel addresses

The four R5 addresses are deterministic from the linker's instance
placement: 64 KiB-aligned, alphabetical instance name order, starting
at host `0x0202'0000'0000` (see
[linker/src/emit/hw/tcl_gen.py](../../linker/src/emit/hw/tcl_gen.py)
`min_align=0x0001_0000` and
[linker/src/emit/hw/user_region/addr_ctx.py](../../linker/src/emit/hw/user_region/addr_ctx.py)).
Converting to the R5 side via
`r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000`:

| Instance | host `BaseAddress` | R5 addr |
|---|---|---|
| `bringup_kernel_0` | `0x0202'0000'0000` | `0x8800'0000` |
| `bringup_kernel_1` | `0x0202'0001'0000` | `0x8801'0000` |
| `bringup_kernel_2` | `0x0202'0002'0000` | `0x8802'0000` |
| `bringup_kernel_3` | `0x0202'0003'0000` | `0x8803'0000` |

These match the constants hardcoded near the top of
`rp1_bringup_vrt.cpp` and `examples/rp1_bringup/rp1_bringup.c`. If you
fork the kernel signature, change the instance count, or add other
kernel types ahead of `bringup_kernel` in alphabetical order, **verify
against the `system_map.xml` packed into the built vbin** and update
both source files to match. Quick check:

```bash
unzip -p build/rp1_bringup_vrt_hw.vbin system_map.xml | head -40
# Look for <Kernel Name="bringup_kernel_0"><BaseAddress>...
```

## What this validates beyond `examples/rp1_bringup`

In addition to everything the C tool's `diamond` validates (barrier
AND on D, parallel B/C dispatch, multi-kernel inflight chaining), this
example exercises:

- `vrt::graph::Graph` authoring (`addNode` / `afterNodes` / `IOMap`);
- `vrt::graph::GraphCompiler` lowering of a 4-kernel DAG into a
  per-device `DGraph`;
- `vrt::graph::FpgaDevice::compilePlan()` translating
  `CompiledKernelNode`s into `RP1_OP_KERNEL_DISPATCH` packets with
  bucket-0 barrier allocation and a trailing sentinel `RP1_OP_SIGNAL`;
- `vrt::graph::fpga::Rp1Submitter` programming the control block,
  staging args, bumping `graph_seq`, and polling `graph_done_seq`;
- `vrt::graph::fpga::Rp1BarWindow` routing every BAR4 access through
  `vrtd::BarFile::getPtr<T>()` so the dma-buf SYNC_START/SYNC_END
  contract is honoured.

If this passes on silicon and the C tool's `diamond` against the same
vbin did too, the VRT → RP1 path is end-to-end functional.

## Diagnostics

On failure, the tool dumps the current control block (state, error
code, current node, cq_write_idx, heartbeat).

| Field | Meaning |
|---|---|
| `rp1_state=RUNNING` | graph stalled mid-execution; some kernel didn't fire |
| `rp1_state=ERROR`   | firmware bailed; check `rp1_error_code` (1=inflight full, 2=kernel timeout) |
| `rp1_state=READY`   | graph completed but sentinel didn't fire — DDR write visibility / barrier wiring bug |
| `cq_write_idx` delta | how far through the diamond the graph got (see [examples/rp1_bringup/README.md](../rp1_bringup/README.md) `cq_delta` table) |
