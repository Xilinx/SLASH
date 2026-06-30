# rp1_pipeline

End-to-end RP1 graph demo against real HLS kernels. Five compute
kernels arranged as a diamond + chain, dispatched from the host as a
single 7-node RP1 graph and verified against a golden reduction.

This example combines:

- The existing example template (HLS kernels + `config.cfg` + `add_vbin`
  → a `.vrtbin`).
- `v80-smi` to program the board (standard SLASH bring-up flow).
- A custom host driver that uses VRT for kernel/buffer discovery and
  then drops to libslash for raw BAR access to hand-build the RP1
  graph in shared DDR. **Temporary** — to be replaced by libslash /
  VRT's `GraphBuilder` API once that lands.

## What the pipeline computes

For an input vector `x[i] = i` of length `N = 64`:

```
        produce      x[i] = i                  (writes X in HBM0)
           │
           ▼ X
       ┌───┴───┐
       ▼       ▼
    square    cube                              (parallel; read X, write Y / Z)
       │       │
       ▼ Y     ▼ Z   (HBM1)    (HBM2)
       └───┬───┘
           ▼
        combine     c[i] = y[i] + z[i] = i² + i³   (HBM3)
           │
           ▼ C
        reduce      result_reg = Σ c[i]            (s_axilite output)
           │
           ▼
   SCALAR_READ result_reg → signal[1]
           │
           ▼
   SIGNAL slot[0] = 0xC0FFEE_01                    (sentinel)
```

Golden value for `N = 64`:

```
Σ (i² + i³) for i ∈ [0, 64)
  = 85344 + 4064256
  = 4149600
  = 0x3F4760
```

## Files

| Path | Purpose |
|---|---|
| `hls/produce.{cpp,cfg}` | `out[i] = i` |
| `hls/square.{cpp,cfg}`  | `out[i] = in[i] * in[i]` |
| `hls/cube.{cpp,cfg}`    | `out[i] = in[i] * in[i] * in[i]` |
| `hls/combine.{cpp,cfg}` | `out[i] = a[i] + b[i]` |
| `hls/reduce.{cpp,cfg}`  | `result_reg = Σ in[i]` (s_axilite output) |
| `config.cfg` | Kernel instances + HBM channel assignment (X→HBM0, Y→HBM1, Z→HBM2, C→HBM3) |
| `CMakeLists.txt` | Builds the HLS kernels, packs them into a `pipeline_hw.vrtbin`, and links the host binary |
| `rp1_pipeline.cpp` | Host driver: VRT discovery + libslash BAR4 + hand-built RP1 graph |

## Validation surface

This single graph exercises, end to end on real silicon:

- Five `KERNEL_DISPATCH` nodes against real HLS slaves through the new
  `0x8800_0000 + 128M` LPD → NoC → user-region AXI-Lite aperture.
- Barrier AND — `combine` waits for both `square` and `cube`.
- Parallel dispatch — `square` and `cube` are in flight together
  (verified implicitly by the timing; `cq_delta=7` with both contributing).
- `check_inflight` chaining across multiple kernels.
- `SCALAR_READ` reading a kernel output register into a signal slot.
- `SIGNAL` as a sentinel gated by `SCALAR_READ`'s barrier.

## Build

You need Vitis HLS / Vivado in your environment for the kernel build
(same as the other `examples/` projects).

```bash
cd examples/rp1_pipeline
cmake -B build -S . -DSLASH_USE_REPO=ON
cmake --build build
# Outputs:
#   build/pipeline_hw.vrtbin   (kernels + bitstream + system_map.xml)
#   build/rp1_pipeline         (host driver)
```

If you only want to rebuild the host driver (e.g. while iterating on
graph construction), pass `--target rp1_pipeline` to `cmake --build`.

## Run

```bash
# 1. Program the bitstream with v80-smi.
v80-smi program --device <BDF> --image build/pipeline_hw.vrtbin

# 2. Make sure rp1.elf (built with -DRP1_POLLING_BRINGUP=ON) is loaded
#    onto R5-1 via xsdb — see examples/rp1_bringup/README.md for details.

# 3. Run the host driver.
sudo ./build/rp1_pipeline <BDF> build/pipeline_hw.vrtbin /dev/slash_ctl0
```

Expected output (truncated):

```
VRT version: ...
Kernel R5 addresses:
  produce_0    host=0x000202000000  r5=0x88000000
  square_0     host=0x000202010000  r5=0x88010000
  cube_0       host=0x000202020000  r5=0x88020000
  combine_0    host=0x000202030000  r5=0x88030000
  reduce_0     host=0x000202040000  r5=0x88040000
Buffer device addresses:
  X=0x000400000000 (HBM, 64 words)
  Y=0x000500000000
  Z=0x000600000000
  C=0x000700000000
reduce.result @ kernel offset 0x20
Submitted graph seq=1 (7 nodes), polling...
Done in 142 us
  cq_delta             = 7 (expect 7)
  slot[0] (sentinel) = 0xc0ffee01  (expect 0xc0ffee01)
  slot[1] (result)   = 0x3f4760    (golden 0x3f4760 = 4149600)
PASS
```

## Diagnostics

On any failure the host prints `cq_delta` (CQ entries this graph
produced) and the control-block fields that matter. Map by:

| `cq_delta` | Stalled at | Likely cause |
|---|---|---|
| 0 | before `produce`'s ap_done | `produce_0` doesn't respond at its R5 address (recompute the host→R5 translation; check `system_map.xml`'s `<BaseAddress>`) |
| 1 | between `produce` and `square` / `cube` | One of the m_axi paths is mis-bound; verify `config.cfg`'s `sp=` lines match the kernel's `bundle=gmem*` |
| 2 | `combine` not unblocking | `square` and `cube` are racing on HBM0 reads or one didn't complete — check `rp1_current_node` |
| 3 | `combine` stalled | Argument layout for `combine` is wrong; re-check `FunctionalArg.offset` values (HLS may have padded differently than expected) |
| 4 | `reduce` not finishing | Same as combine, or `reduce_0.in` is reading the wrong HBM channel |
| 5 | `SCALAR_READ` didn't fire | barrier wiring on node 5; very unlikely |
| 6 | `SIGNAL` didn't fire | barrier wiring on node 6; very unlikely |
| 7 | result mismatch | Compute is wrong — inspect a few words of each buffer with `v80-smi mem-poke` or DMA-read; `reduce.result` itself may be wrong, or one of the upstream kernels miscomputed |

If `slot[0]` is correct (`0xC0FFEE_01`) but `slot[1]` is not the golden
value, the graph plumbing is fine and only the *compute* is wrong —
suspect HBM channel mis-assignment causing one kernel to read stale
or zero memory.

## Constants you might need to edit

In `rp1_pipeline.cpp`, near the top:

```cpp
constexpr int      BAR_NUMBER       = 4;
constexpr uint64_t BAR_CTRL_OFFSET  = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t HOST_USER_REGION_BASE = 0x0000020200000000ULL;
constexpr uint64_t R5_USER_REGION_BASE   = 0x0000000088000000ULL;
```

These are the same constants as in `examples/rp1_bringup/rp1_bringup.c` —
adjust if your BAR layout or LPD aperture differs.
