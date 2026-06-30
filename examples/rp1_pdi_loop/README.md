# rp1_pdi_loop

End-to-end demonstration of RP1's `PDI_LOAD` opcode (`0x0030`). The
host stages two user-region partial PDIs in DDR and submits a single
RP1 graph that alternates between them in a loop. Each iteration:

```
PDI_LOAD(A)  ->  KERNEL_DISPATCH @ R5 0x88000000  ->  SCALAR_READ +0x10 -> slot 0
PDI_LOAD(B)  ->  KERNEL_DISPATCH @ R5 0x88000000  ->  SCALAR_READ +0x10 -> slot 1
RERUN -> LOOP head
```

Two HLS kernels share the **same** AXI-Lite signature
(`void pdi_kernel(ap_uint<32>* out)`) but differ in the magic constant
they write to their `out` register on `ap_start`:

| Kernel | Magic | Output slot |
|--------|-------|-------------|
| variant A | `0xAAAA_AAAA` | signal slot 0 |
| variant B | `0xBBBB_BBBB` | signal slot 1 |

`PDI_LOAD` performs **partial reconfiguration of just the user region**
without disturbing the static region (NoC, BAR layout, RPU subsystem,
RP1 firmware on R5-1), so RP1 stays alive and dispatching between
swaps. Both vbins are linked against the same base platform, which is
the precondition for the swap to be safe.

> **Status (this branch): not yet exercised on silicon.** All build
> artefacts are in place and the host binary builds against the libvrt
> headers. Bring-up on a real V80 is the next step. See [Caveats](#caveats)
> for the known PR-race that this first cut deliberately does not
> work around.

## Files

| Path | Purpose |
|------|---------|
| `hls_a/pdi_kernel.{cpp,cfg}` | Variant A: writes `0xAAAA_AAAA` to its `out` AXI-Lite register. |
| `hls_b/pdi_kernel.{cpp,cfg}` | Variant B: writes `0xBBBB_BBBB`. Same signature as A. |
| `config_a.cfg`, `config_b.cfg` | `nk=pdi_kernel:1`. No `sp=` lines (kernel is AXI-Lite only). |
| `CMakeLists.txt` | Two `build_hls_dir` + `add_vbin` calls (`rp1_pdi_loop_a_hw.vbin`, `rp1_pdi_loop_b_hw.vbin`) and the host binary. |
| `rp1_pdi_loop.cpp` | Host driver: extracts both partials, QDMA-stages them, hand-packs the LOOP graph, verifies. |

## Prerequisites

1. **SLASH base platform pre-flashed** on the V80 (the usual
   `linker/resources/aved/build_all.sh` output containing
   `top_wrapper.pdi`, `amc.elf`, and the boot stub). `PDI_LOAD` only
   reconfigures the user region; everything else must already be in
   place.

2. **RP1 firmware loaded onto R5-1 via xsdb**, built with the
   bring-up polling flag set (the GCQ doorbell is not wired yet):

   ```bash
   cd linker/resources/aved/rp1
   cmake -S . -B build-bringup -DRP1_POLLING_BRINGUP=ON
   cmake --build build-bringup
   # produces build-bringup/rp1.elf, then load via xsdb
   ```

   After load, verify with `examples/rp1_bringup dump /dev/slash_ctl0`
   that `magic == 0x53515231` (`"SQR1"`), `rp1_state == 1 (READY)`, and
   `heartbeat` is advancing.

3. **vrtd running** and the calling user in the device-access list.
   See `vrt/vrtd/README.md`.

4. **Vivado + Vitis HLS in the environment** for the kernel build. To
   iterate on the host driver without Vivado, pass `-DBUILD_KERNELS=OFF`.

## Build

### Against the repo tree (no installed packages required)

```bash
cd examples/rp1_pdi_loop
cmake -B build -S . -DVRT_USE_REPO=ON
cmake --build build
# Outputs:
#   build/rp1_pdi_loop_a_hw.vbin   (kernel A bitstream)
#   build/rp1_pdi_loop_b_hw.vbin   (kernel B bitstream)
#   build/rp1_pdi_loop             (host driver)
```

### Against the installed `vrt` package

```bash
cd examples/rp1_pdi_loop
cmake -B build -S .
cmake --build build
```

### Host-only build (no Vivado required)

Useful while iterating on `rp1_pdi_loop.cpp`:

```bash
cmake -B build -S . -DVRT_USE_REPO=ON -DBUILD_KERNELS=OFF
cmake --build build
# Output: build/rp1_pdi_loop  (you'll need vbins from a separate Vivado build)
```

## Run

```bash
# 1. (Optional) Pre-load one of the vbins via v80-smi if you want to skip
#    --program inside the tool:
v80-smi program --device <BDF> --image build/rp1_pdi_loop_a_hw.vbin

# 2. Ensure rp1.elf is loaded onto R5-1 -- see "Prerequisites" above.
sudo ./<path-to>/rp1_bringup dump /dev/slash_ctl0
#    Expect magic=0x53515231 (SQR1), rp1_state=READY, heartbeat advancing.

# 3. Run the host driver.
./build/rp1_pdi_loop --bdf 0000:65:00.0
./build/rp1_pdi_loop --bdf 0000:65:00.0 --iterations 8
./build/rp1_pdi_loop --bdf 0000:65:00.0 --no-program       # skip step (1)'s reprogram
./build/rp1_pdi_loop --bdf 0000:65:00.0 \
                     --vbin-a path/to/A.vbin \
                     --vbin-b path/to/B.vbin
```

Pass iff:

- signal slot 0 reads back `0xAAAA_AAAA` (last A kernel ran);
- signal slot 1 reads back `0xBBBB_BBBB` (last B kernel ran);
- signal slot 2 reads back `0xD1A1_D0DD` (sentinel: graph finished);
- `cq_write_idx` advanced by exactly `1 + iterations * 7 + 1 + 1` (init
  `SIGNAL` + `iterations` body cycles of `PDI + KD + SR + PDI + KD + SR
  + RERUN` + LOOP exit + sentinel `SIGNAL`).

Expected output for `--iterations 4` (the default):

```
[rp1_pdi_loop] extracting partial PDI from rp1_pdi_loop_a_hw.vbin ...
[rp1_pdi_loop] extracting partial PDI from rp1_pdi_loop_b_hw.vbin ...
[rp1_pdi_loop] PDI sizes: A=<size> B=<size> bytes
[rp1_pdi_loop] opening device 0000:65:00.0 (program=yes) ...
[rp1_pdi_loop] staged A partial PDI: <size> bytes @ DDR 0x...
[rp1_pdi_loop] staged B partial PDI: <size> bytes @ DDR 0x...
[rp1_pdi_loop] waiting for RP1 firmware to be READY...
[rp1_pdi_loop] submitting 10-node graph (iterations=4, physA=0x..., physB=0x...), polling...
[rp1_pdi_loop] slot[0] (kernel A) = 0xaaaaaaaa  (expect 0xaaaaaaaa)
[rp1_pdi_loop] slot[1] (kernel B) = 0xbbbbbbbb  (expect 0xbbbbbbbb)
[rp1_pdi_loop] slot[2] (sentinel)  = 0xd1a1d0dd  (expect 0xd1a1d0dd)
[rp1_pdi_loop] cq_delta = 31  (expect 31)
PASS: PDI_LOAD loop ran 4 body iterations (8 partial reconfigurations) cleanly.
```

## Graph layout

The graph is hand-packed to demonstrate the raw RP1 opcodes directly. (The
`vrt::graph::FpgaDevice` backend — `vrt/src/graph/device/fpga_device.cpp` —
can now author `PDI_LOAD` / `LOOP` graphs itself; see
`examples/rp1_graph_vbin_full` for the high-level API. This example
deliberately stays at the packet level.) The 10 nodes are:

| Idx | Opcode | Bucket-await | Bucket-set | Payload |
|----:|--------|:-:|:-:|---------|
| 0 | `SIGNAL`           | (0,0x00) | (0,0x01) | slot 31 := 0  (LOOP sentinel) |
| 1 | `LOOP`             | (0,0x01) | (0,0x02) | body=[2..8], max_iter=N, cond=signal[31] EQ 0xFFFFFFFF (never), clear=[1..1] |
| 2 | `PDI_LOAD`         | (1,0x00) | (1,0x01) | addr = phys_A |
| 3 | `KERNEL_DISPATCH`  | (1,0x01) | (1,0x02) | R5 0x88000000, arg_count=0 |
| 4 | `SCALAR_READ`      | (1,0x02) | (1,0x04) | source=0x88000010, slot 0 |
| 5 | `PDI_LOAD`         | (1,0x04) | (1,0x08) | addr = phys_B |
| 6 | `KERNEL_DISPATCH`  | (1,0x08) | (1,0x10) | R5 0x88000000, arg_count=0 |
| 7 | `SCALAR_READ`      | (1,0x10) | (1,0x20) | source=0x88000010, slot 1 |
| 8 | `RERUN`            | (1,0x20) | (1,0x40) | target=node 1 |
| 9 | `SIGNAL`           | (0,0x02) | (0,0x04) | slot 2 := 0xD1A1_D0DD |

Bucket 0 carries the init -> LOOP -> sentinel edges (and **must not** be
cleared by the LOOP — see `linker/resources/aved/rp1/src/rp1_loop.c`
lines 289-316). Bucket 1 carries the body bits and is cleared each
iteration via the LOOP's `bucket_clear` range. The RERUN sits inside
the body range (`body_end=8`) so its `DONE` state is reset each
iteration along with the rest of the body — without this, RERUN would
fire exactly once and the loop would deadlock from iteration 2 onward.

The LOOP's exit condition is `signal[31] EQ 0xFFFFFFFF`. Node 0
initialises slot 31 to `0`; nothing else touches it; the comparison
therefore never matches and the loop exits exclusively via
`max_iterations`. This matches the pattern in
`linker/resources/aved/rp1/src/rp1_graph_test.c`'s `test_loop_decrement`
(adapted from "exit when slot hits zero" to "never exit by signal").

## Caveats

### 1. PR race between `PDI_LOAD` and `KERNEL_DISPATCH`

The PMC's IPI ACK only signals **"request consumed"**, not **"fabric
ready"**.  After `rp1_pdi_load()` returns, the PMC continues loading
the PDI asynchronously (see
`linker/resources/aved/rp1/ARCHITECTURE.md` lines 207-227 and
`linker/resources/aved/rp1/src/rp1_pdi.c`'s docstring). The graph
in this example dispatches the next kernel immediately, so on real
silicon the dispatch may race the PR completion.

The architecture's recommended `AWAIT_SEMAPHORE` pattern requires an
opcode that doesn't yet exist in
`driver/libslash/include/slash/uapi/rp1_protocol.h`. The cleanest
in-graph fence will be a `COND` + `RERUN` poll on the new kernel's
`ap_idle` bit via `SCALAR_READ` of `<kernel_base>+0x00`; that's
deferred to a follow-up CL. Once that lands, insert one fence node
between every `PDI_LOAD` and the following `KERNEL_DISPATCH`.

### 2. `vrt::Buffer` rounds DDR allocations up to 64 MiB

vrtd's DDR allocator works in 64 MiB chunks
(`vrt/vrtd/src/allocator.c` lines 74-133). Both partial-PDI buffers
will reserve ~64 MiB each even though each blob is typically far
smaller. Acceptable for a demo; if you need tight DDR usage, slice
fewer Buffers manually via the lower-level `UntypedBuffer` API.

### 3. AXI-Lite offset of the `out` register is HLS-determined

`rp1_pdi_loop.cpp` reads `kKernelR5 + 0x10` via `SCALAR_READ` to
capture the kernel's output. HLS conventionally lays scalar I/O ports
out starting at `+0x10` after the four-word `ap_ctrl` region
(`+0x00..+0x0F`), so a single `s_axilite` output scalar normally lands
at `+0x10`. Confirm by inspecting the driver header generated for
`pdi_kernel` (look at `xpdi_kernel_hw.h` or
`pdi_kernel_hw.h` in the HLS build directory):

```bash
ls build/hls_a/build_pdi_kernel.*/hls/impl/ip/drivers/*/src/x*_hw.h
grep -E 'CTRL_ADDR_OUT_DATA|XPDI_KERNEL_CONTROL_ADDR_OUT_DATA' \
     build/hls_a/build_pdi_kernel.*/hls/impl/ip/drivers/*/src/x*_hw.h
```

If the offset differs from `0x10`, update `kKernelOutOffset` near the
top of `rp1_pdi_loop.cpp` and rebuild the host binary (no FPGA rebuild
required).

### 4. Same R5 address by design

Both partial PDIs each instantiate exactly one `pdi_kernel`. The linker
places the first (only) user-region kernel at host `0x0202_0000_0000`
-> R5 `0x8800_0000` in both designs (see `linker/src/emit/hw/tcl_gen.py`
`min_align=0x0001_0000` and `linker/src/emit/hw/user_region/addr_ctx.py`).
A single `KERNEL_DISPATCH(0x88000000)` opcode therefore targets whichever
variant happens to be currently loaded. If you fork the kernel
signature or add other kernels before `pdi_kernel` alphabetically,
verify against the `system_map.xml` packed into each built vbin and
update `kKernelR5` in `rp1_pdi_loop.cpp`.

### 5. Base-platform match is required

Both vbins must be linked against the **same base platform** as the
bitstream programmed onto the board, so the static region (NoC,
BAR layout, RPU subsystem, RP1 firmware on R5-1) survives untouched
across the `PDI_LOAD` swap. The `CMakeLists.txt` here uses the same
`PLATFORM hw` and `DEVICE` for both `add_vbin` calls, which satisfies
this implicitly.

## Diagnostics

On failure (or timeout), the tool dumps the RP1 control block.
Useful fields:

| Field | Meaning |
|-------|---------|
| `rp1_state == RUNNING` | graph stalled mid-execution |
| `rp1_state == ERROR`   | firmware bailed; check `rp1_error_code` |
| `rp1_error_code == 1`  | `ERR_INFLIGHT_FULL` (more than 32 in-flight kernels) |
| `rp1_error_code == 2`  | `ERR_KERNEL_TIMEOUT` (a `KERNEL_DISPATCH` didn't see `ap_done`) |
| `rp1_error_code == 3`  | `ERR_PDI_TIMEOUT` (PMC didn't ACK the IPI within the budget) |
| `cq_write_idx`         | how far the graph got — compare against `cq_start` printed on submit |

`rp1_error_code == 3` is the most likely failure mode for this
example today: the PMC may take longer than the default 10M poll
cycles on first power-on, or the partial PDI may be malformed. Bump
`timeout_cycles` in the PDI_LOAD payload (`rp1_pdi_loop.cpp`'s
`makePdiLoad` helper) if needed.
