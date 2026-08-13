# 01_edge_detection

Runnable companion to the `vrt::graph` tutorial's Algorithm 1
(illumination-normalized edge detection), authored with the struct-literal
`vrt::graph` API.

For each element `i` of a 1-D signal:

```text
edges[i]  = |input[i+1] - input[i]|        # edges_kernel     (Step A)
level     = max(1, sum(input) / n)         # level_kernel     (Step B)
output[i] = edges[i] * K / level           # normalize_kernel (Step C, in place)
```

Steps A and B each read only the graph-level `input` buffer, so they can run
concurrently on the FPGA queue; step C consumes both `edges` and `level`, so
it implicitly waits for both. All three kernels are packed into a single vbin
image, so one `graph.addReprogram({.image = image})` node gates every FPGA
dispatch below it.

## Files

| Path | Purpose |
|------|---------|
| `hls/edges_kernel.cpp` | Step A: local derivative, `m_axi` in/out ports. |
| `hls/level_kernel.cpp` | Step B: brightness reduction to a scalar output. |
| `hls/normalize_kernel.cpp` | Step C: in-place gain normalization. |
| `config.cfg` | One instance of each kernel, four HBM-connected `m_axi` ports. |
| `01_edge_detection.cpp` | Host graph authoring and execution. |
| `CMakeLists.txt` | Builds the HLS kernels/vbin when Vivado is available and always builds the host app. |

## Build

Against the repo tree:

```bash
cd examples/graph/01_edge_detection
cmake -B build -S . -DVRT_USE_REPO=ON
cmake --build build
```

Host-only build without Vivado:

```bash
cd examples/graph/01_edge_detection
cmake -B build -S . -DVRT_USE_REPO=ON -DBUILD_KERNELS=OFF
cmake --build build
```

With `BUILD_KERNELS=OFF`, pass a vbin built elsewhere with `--vbin` when
running.

## Run

Prerequisites:

- base platform loaded on the V80;
- RP1 firmware loaded and reporting `RP1_STATE_READY`;
- `vrtd` running and the current user authorized for the device.

Run from the build directory:

```bash
./edge_detection --bdf 0000:65:00.0 --k 16 --elements 16
```

Pass iff the FPGA-computed output matches the host-computed reference. The
program prints the first few output values and returns non-zero on mismatch
or runtime errors.

## Notes

The FPGA graph ABI is scalar-first because `FpgaDevice` packs RP1 kernel
arguments in `IOTypeMap` order: scalar inputs, then scalar outputs, then
input buffer addresses, then output buffer addresses (in-place ports collapse
onto their input slot). `level_kernel`'s scalar output is exposed as an
`s_axilite`-mode pointer argument rather than an `m_axi` port, so the runtime
can read it back as a register after the kernel completes instead of staging
it through DDR.

The read-only `input` fans out from HBM0 to a second replica on HBM2 through
the transparent host/QDMA fallback. The mutable `edges` buffer stays on HBM1
for both `edges_kernel` and the in-place `normalize_kernel`.
