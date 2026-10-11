# Examples

This directory contains the example projects for VRT. Each example demonstrates different functionalities and usage patterns of the VRT ecosystem:

| ID | Exemplified Feature | Notes |
|------|-----------|----------------|
| 0 | Linking, AXI-Lite control | |
| 1 | Kernels with AXI-MM interfaces | |
| 2 | Freerunning streaming kernels | |
| 3 | Controlling multiple V80s | Uses vrtbin of example 0 |
| 4 | Frequency targets | |
| 5 | Memory performance test | Instantiates current maximum number of kernels |
| 6 | Network interface test | Drives two network interfaces |
| `graph/00_multi_image_pipeline` | VRT graph FPGA backend (`vrt::graph`) | End-to-end HSA graph demo: CPU + FPGA kernels in one `vrt::graph::Graph`, one-call device bring-up via `Graph::addFpga(...)`, explicit `PDI_LOAD` reprogram nodes, and a fixed-count loop carrying state. See [graph/00_multi_image_pipeline/README.md](graph/00_multi_image_pipeline/README.md). |
| `graph/01_edge_detection` | `vrt::graph` tutorial, Algorithm 1 | Basic graph authoring: CPU/FPGA kernels, buffers, scalars, and one reprogram gating three kernels (two of which run concurrently) in a single vbin image. See [graph/01_edge_detection/README.md](graph/01_edge_detection/README.md). |
| `graph/02_sharpen_loop` | `vrt::graph` tutorial, Algorithm 2 | Loops and conditionals: a fixed-count loop carrying state through an FPGA kernel, run in parallel with a CPU reduction, followed by a post-loop conditional. See [graph/02_sharpen_loop/README.md](graph/02_sharpen_loop/README.md). |
| `extra/rp1_latency` | RP1 command processor latency microbenchmarks | Not a tutorial: compares the RP1 dispatch path against the legacy host-driven VRT path on one no-op kernel. Standalone build, see [extra/rp1_latency/README.md](extra/rp1_latency/README.md). |

## How to run the examples

Each example project has its own `CMakeLists.txt` that defines the build targets.

### Build targets

Each `CMakeLists.txt` provides the following targets:

- `hls`: Compile the HLS kernels in the `hls/` directory.
- `<design>_hw`: Link a hardware vrtbin for the specified project.
- `<design>_emu`: Link an emulation vrtbin for the specified project.
- `<design>_sim`: Link a simulation vrtbin for the specified project.
- `<project_name>`: Build the runtime application (e.g. `00_axilite`).

### Example usage

```bash
cd examples/00_axilite

# Configure (use -DSLASH_USE_REPO=ON when building against the local repo tree)
cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON

# Build the application
cmake --build build

# Build FPGA artefacts (requires Vivado/Vitis)
cmake --build build --target hls            # compile HLS kernels
cmake --build build --target axilite_hw     # link hardware vrtbin
cmake --build build --target axilite_emu    # link emulation vrtbin
cmake --build build --target axilite_sim    # link simulation vrtbin
```

The vrtbin files and the application executable are placed in the `build/` directory.

## How to run

The following environment variables need to be set prior to building or running any examples:

```bash
source <path-to-vivado>/settings64.sh
source <path-to-vitis-hls>/settings64.sh
```

To make the changes persistent, add the commands to `.bashrc`. Sourcing the Vivado scripts is needed for the hardware builds, whereas Vitis HLS is needed for emulation.

In order to run one of the built examples, one must identify the BDF for the V80:

```
v80-smi list
--------------------------------------------------------------------
Listing V80 devices
--------------------------------------------------------------------
V80 device found with BDF: 0000:e2:00.0
--------------------------------------------------------------------
V80 device found with BDF: 0000:21:00.0
--------------------------------------------------------------------
```

To run the example, navigate to the `build` directory. The format for running is `0x_<project_name> <BDF> <VRTBIN file>`.
