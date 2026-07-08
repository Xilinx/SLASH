# 02_sharpen_loop

Runnable companion to the `vrt::graph` tutorial's Algorithm 2 (iterative
sharpening with adaptive gain), exercising the two structured-authoring
building blocks not covered by
[`01_edge_detection`](../01_edge_detection/README.md): loops and
conditionals.

For a 1-D signal:

```text
repeat `iterations` (loop-carried state x, on FPGA):
    x = x + alpha * (2*x[i] - x[i-1] - x[i+1])   # sharpen_kernel, edge-replicated boundaries

in parallel with the loop (on CPU):
    level = max(1, sum(original input) / n)      # cpu_level

after both finish:
    if level >= threshold: output = x            # cpu_passthrough
    else:                  output = x * boost    # cpu_boost
```

- **The loop** carries `state` (a port named in both `.inputs` and
  `.outputs` of `graph.addLoop({...})`): each iteration reads
  `loop.input("state")` and writes `loop.output("state")`, dispatching
  `sharpen_kernel` on the FPGA. The reprogram gating that dispatch is
  authored *inside* the loop body (`loop.addReprogram(...)`), next to the
  dispatch it gates.
- **`cpu_level`** reads only the graph's original `input` -- never the
  loop's carried state -- so the compiler is free to run it concurrently
  with the FPGA loop for its entire duration.
- **The conditional** (`graph.addConditional({...})`) branches on a scalar
  (`brightness`) written by `cpu_level`; both branches must produce the same
  `output` port, so the never-taken branch's kernel exists purely to satisfy
  that contract (`cpu_passthrough`'s body is empty).

## Files

| Path | Purpose |
|------|---------|
| `hls/sharpen_kernel.cpp` | One Laplacian sharpening step, dispatched once per loop iteration. |
| `config.cfg` | One `sharpen_kernel` instance, two HBM-connected `m_axi` ports. |
| `02_sharpen_loop.cpp` | Host graph authoring (loop + conditional) and execution. |
| `CMakeLists.txt` | Builds the HLS kernel/vbin when Vivado is available and always builds the host app. |

## Build

Against the repo tree:

```bash
cd examples/graph/02_sharpen_loop
cmake -B build -S . -DVRT_USE_REPO=ON
cmake --build build
```

Host-only build without Vivado:

```bash
cd examples/graph/02_sharpen_loop
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
./sharpen_loop --bdf 0000:65:00.0 --iterations 4 --alpha 1 --threshold 50 --boost 2 --elements 16
```

Pass iff the final output matches the host-computed reference. The program
prints the first few output values and returns non-zero on mismatch or
runtime errors.

## Notes

Only `sharpen_kernel` moved to the FPGA; `cpu_level`, `cpu_passthrough`, and
`cpu_boost` are ordinary `CpuKernel` subclasses registered via
`graph.cpu().add<T>()`, exactly as in `01_edge_detection`. `brightness` and
`sharpened` cross the loop and conditional region boundaries with no extra
plumbing -- both are ordinary token references resolved by the compiler
through the authored `.inputs` / `.outputs` port maps.
