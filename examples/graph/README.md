# Graph API examples

Runnable companions to the `vrt::graph` tutorial (Graph API — a tutorial
introduction). Each subdirectory is a self-contained example with its own
`CMakeLists.txt`, HLS kernel sources, and README.

| Directory | Algorithm | Exemplified feature |
|-----------|-----------|----------------------|
| [`00_multi_image_pipeline`](00_multi_image_pipeline/README.md) | End-to-end CPU + FPGA work graph | Two exclusive-image vbins, one-call device bring-up via `Graph::addFpga(...)`, chained `PDI_LOAD` reprogram nodes, an in-place CPU kernel, and a fixed-count loop carrying state. |
| [`01_edge_detection`](01_edge_detection/README.md) | Illumination-normalized edge detection | Basic graph authoring: CPU/FPGA kernels, buffers, scalars, and a single reprogram gating three kernels (two of which run concurrently) in one vbin image. |
| [`02_sharpen_loop`](02_sharpen_loop/README.md) | Iterative sharpening with adaptive gain | Loops and conditionals: a fixed-count loop carrying state through an FPGA kernel, run in parallel with a CPU reduction, followed by a post-loop conditional. |

Both examples build the host application without Vivado (`-DBUILD_KERNELS=OFF`);
running them against real hardware additionally requires the HLS kernels to be
linked into a `.vbin` and a V80 with RP1 firmware loaded. See each example's
README for exact build and run instructions.
