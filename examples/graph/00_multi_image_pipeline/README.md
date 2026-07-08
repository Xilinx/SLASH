# rp1_graph_vbin_full

End-to-end example for the VRT graph FPGA backend, authored with the
struct-literal `vrt::graph` API:

- CPU kernels and FPGA kernels in the same `vrt::graph::Graph`;
- one-call device bring-up via `Graph::addFpga(...)` (PDI staging, vbin/image
  loading, vrtd session + BAR window, RP1 readiness preflight, `FpgaDevice`);
- two exclusive user-region vbins exposed as named image handles;
- explicit reprogram nodes (`loop.addReprogram({.image = ...})`) lowering to RP1
  `PDI_LOAD`, with image safety proven at `compile()`;
- a fixed-count loop carrying `state` across iterations;
- an in-place (`inout`) CPU kernel;
- a post-loop conditional that branches on a scalar written by a CPU kernel.

This example is a runnable template for hardware bring-up. It builds the host
application without Vivado when `BUILD_KERNELS=OFF`, but executing the graph
requires a V80 with RP1 firmware and both hardware vbins.

## Graph

For each element `i`:

```text
x = i
x = x + 10                          # cpu_preprocess
repeat `iterations` (loop-carried x):
    x = x + 1                       # cpu_stage
    reprogram(imageA)               # PDI_LOAD A
    x = x + 1                       # graph_kernel_0 from imageA: out = in + 1
    if i % 10 == 0: x = x + 1       # cpu_sparse (in place, every 10th element)
    reprogram(imageB)               # PDI_LOAD B
    x = x * 2                       # graph_kernel_0 from imageB: out = in * 2
    x = x - 4                       # cpu_finalize
parity = post[0] & 1                # cpu_parity (writes a scalar)
if parity == 0: out[i] = post[i] + 100   # cpu_report
else:           out[i] = post[i] + 200   # cpu_report_odd
```

The loop carries `state` (a port named in both `.inputs` and `.outputs`): each
iteration reads `loop.input("state")` and writes `loop.output("state")`, so
`--iterations N` validates data flowing from iteration `i` into `i + 1`. The
post-loop `cpu_parity` reduces the result to one scalar that the conditional
branches on (RP1 `COND` is a single scalar decision over the whole region).

## Image safety

The user region starts with **no active image**, so every FPGA dispatch must be
gated behind a reprogram of its image:

- each FPGA dispatch declares `.after = {the reprogram that loaded its image}`;
  `compile()` checks the dispatch's image matches that reprogram's;
- a reprogram chains to the prior reprogram via `.after = {prior}`, and
  `compile()` expands that to also wait on every kernel gated behind the prior
  reprogram, so the old image fully drains before reconfiguration (kernels are
  never named in a drain edge);
- an ungated FPGA dispatch is rejected at `compile()`.

## Files

| Path | Purpose |
|------|---------|
| `hls_a/graph_kernel.cpp` | Image A FPGA kernel, increments every element. |
| `hls_b/graph_kernel.cpp` | Image B FPGA kernel, doubles every element. |
| `config_a.cfg`, `config_b.cfg` | One `graph_kernel_0` instance with two HBM-connected `m_axi` ports. |
| `rp1_graph_vbin_full.cpp` | Host graph authoring and execution. |
| `CMakeLists.txt` | Builds HLS/vbins when Vivado is available and always builds the host app. |

## Build

Against the repo tree:

```bash
cd examples/rp1_graph_vbin_full
cmake -B build -S . -DVRT_USE_REPO=ON
cmake --build build
```

Host-only build without Vivado:

```bash
cd examples/rp1_graph_vbin_full
cmake -B build -S . -DVRT_USE_REPO=ON -DBUILD_KERNELS=OFF
cmake --build build
```

With `BUILD_KERNELS=OFF`, pass vbins built elsewhere with `--vbin-a` and
`--vbin-b` when running.

## Run

Prerequisites:

- base platform loaded on the V80;
- RP1 firmware loaded and reporting `RP1_STATE_READY`;
- `vrtd` running and the current user authorized for the device;
- both vbins built for the same base platform and exclusive user-region layout.

Run from the build directory:

```bash
./rp1_graph_vbin_full --bdf 0000:65:00.0 --iterations 2 --elements 16
```

Or provide explicit vbin paths:

```bash
./rp1_graph_vbin_full \
  --bdf 0000:65:00.0 \
  --vbin-a /path/to/rp1_graph_vbin_full_a_hw.vbin \
  --vbin-b /path/to/rp1_graph_vbin_full_b_hw.vbin \
  --iterations 2 \
  --elements 16
```

Pass iff the final CPU buffer matches the host-computed reference. The program
prints the first few output values and returns non-zero on mismatch or runtime
errors.

## Notes

The FPGA graph ABI is scalar-first because `FpgaDevice` packs RP1 kernel
arguments in `IOTypeMap` order: scalar inputs, then input buffer addresses, then
output buffer addresses. The HLS kernels therefore use:

```cpp
void graph_kernel(ap_uint<64> n, const int* in, int* out);
```

A vbin-derived FPGA kernel handle spells out its element types explicitly
(`.scalarIn<uint64_t>("n").in<int32_t>("in").out<int32_t>("out")`), because the
current `system_map.xml` metadata identifies buffer ports but does not carry C++
element type.
