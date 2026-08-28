# Graph API examples

Runnable companions to the `vrt::graph` tutorial (Graph API — a tutorial
introduction). Each subdirectory is a self-contained example with its own
`CMakeLists.txt`, HLS kernel sources, and README.

| Directory | Algorithm | Exemplified feature |
|-----------|-----------|----------------------|
| [`00_multi_image_pipeline`](00_multi_image_pipeline/README.md) | End-to-end CPU + FPGA work graph | Two exclusive-image vbins, one-call device bring-up via `Graph::addFpga(...)`, chained `PDI_LOAD` reprogram nodes, an in-place CPU kernel, and a fixed-count loop carrying state. |
| [`01_edge_detection`](01_edge_detection/README.md) | Illumination-normalized edge detection | Basic graph authoring: CPU/FPGA kernels, buffers, scalars, and a single reprogram gating three kernels (two of which run concurrently) in one vbin image. |
| [`02_sharpen_loop`](02_sharpen_loop/README.md) | Iterative sharpening with adaptive gain | Loops and conditionals: a fixed-count loop carrying state through an FPGA kernel, run in parallel with a CPU reduction, followed by a post-loop conditional. |

All three examples build the host application without Vivado (`-DBUILD_KERNELS=OFF`);
running them against real hardware additionally requires the HLS kernels to be
linked into a `.vbin` and a V80 with RP1 firmware loaded. See each example's
README for exact build and run instructions.

## Hardware acceptance sequence

Build all three host applications locally without invoking Vivado:

```bash
for example in \
  00_multi_image_pipeline \
  01_edge_detection \
  02_sharpen_loop; do
  cmake \
    -S "examples/graph/$example" \
    -B "examples/graph/$example/build" \
    -G Ninja \
    -DVRT_USE_REPO=ON \
    -DBUILD_KERNELS=OFF
  cmake --build "examples/graph/$example/build"
done
ctest \
  --test-dir examples/graph/00_multi_image_pipeline/build \
  --output-on-failure
```

Place the separately built hardware vbins next to those applications, or use
the `MULTI_BIN`, `MULTI_VBIN_A`, `MULTI_VBIN_B`, `EDGE_BIN`, `EDGE_VBIN`,
`SHARPEN_BIN`, and `SHARPEN_VBIN` environment overrides. `V80_SMI` similarly
selects the `v80-smi` executable.

On a prepared V80 host, run exactly one script:

```bash
ARTIFACT_DIR="$PWD/tmp/graph-hardware-acceptance/manual-run" \
  ./scripts/extra/test-graph-hardware.sh 0000:65:00.0
```

The script deliberately performs no reset and does not restart `vrtd`. In one
continuous device session it runs:

1. a read-only `v80-smi debug rp1-dump` preflight;
2. graph/00 with 2 iterations and input offset 0;
3. graph/00 again with 3 iterations and input offset 4096;
4. graph/01;
5. graph/02's bright/pass-through branch (`--threshold 0`);
6. graph/02's dark/boost branch (`--threshold 1000000`).

A read-only RP1 dump follows every application. Each case runs under GNU
`timeout --foreground` with a configurable `CASE_TIMEOUT` and `KILL_GRACE`.
The script forces `VRT_RP1_TRACE=1` and `VRT_RP1_RESULT=1`. It validates every
sequence-tagged result as a clean `SUCCESS` with a known image, completed
operations, ordered graph/publication timing, a non-empty trace, and zero
quiescence. It also rejects missing host-reference PASS output, too few
successful PDI trace records, missing successful `GRAPH_DONE`, trace overflow,
a result sequence that disagrees with the following read-only dump, a
non-advancing `graph_done_seq`, an incompatible protocol-v6
capability/platform identity, or terminal error/recovery evidence.
`UNREACHED_NODES` is allowed on success because an unchosen conditional branch
legitimately remains pending; the runtime's lifecycle sentinel proves that all
intended leaves joined.

Per-case logs, RP1 snapshots, escaped commands, source revision/dirty status,
input binary/vbin hashes, and final artifact hashes are written under
`ARTIFACT_DIR`. The directory must not already exist. Other configurable
controls are documented by:

```bash
./scripts/extra/test-graph-hardware.sh --help
```

The acceptance harness itself has a hardware-free test. It creates all fake
executables and vbins under the repository's ignored `tmp/` directory and
checks the success, missing/failed-result, missing-PDI,
missing-graph-completion, trace-overflow, and watchdog paths:

```bash
./scripts/test-graph-hardware-local.sh
```
