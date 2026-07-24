..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##########
Graph API
##########

This tutorial introduces the Graph API: a way to compose CPU, FPGA, and GPU
kernels into a single dataflow graph and let the runtime handle scheduling,
buffer routing, and cross-device transfers for you. Two worked algorithms
build up the full API surface — the first covers kernels, buffers, scalars,
and kernel calls; the second adds loops and conditionals.

Prerequisites
=============

- The SLASH stack is installed (kernel module, libslash, vrtd, VRT, v80-smi).
  See :doc:`/howto/build-from-source` if building from source.
- Familiarity with the concepts from :doc:`your-first-kernel` and
  :doc:`buffers-and-memory` — this tutorial builds on HLS kernels, vbins, and
  buffer memory placement without re-explaining them.
- AMD Vivado **2025.1** and Vitis HLS **2025.1**, sourced in your shell, are
  needed for the FPGA sections of each algorithm:

  .. code-block:: bash

     source <path-to-vivado>/settings64.sh
     source <path-to-vitis-hls>/settings64.sh

  The CPU-only sections of each algorithm need none of this — they build and
  run as ordinary host code.

Algorithm 1: Graph Basics
==========================

The algorithm that we will consider for this first example is a simple, but
real, algorithm: illumination-normalized edge detection.

Given a 1-D signal (e.g. a row of pixel intensities), we want to detect sharp
transitions while compensating for the overall brightness of the signal so
that a dark image and a bright image produce comparable edge magnitudes.

The computation has three naturally parallel steps:

- **Step A** computes a local derivative: for each position ``i``,
  ``edges[i] = |input[i+1] - input[i]|``. This only needs neighboring
  elements.
- **Step B** computes the global brightness: ``level = sum(input) / n``. This
  only needs the original input.
- **Step C** normalizes the edges: ``output[i] = edges[i] * K / level``. This
  needs the results of both A and B, plus a tuning scalar ``K``.

Steps A and B are independent of each other and can run in parallel. Step C
must wait for both.

.. code-block:: cpp

   std::vector<int32_t> expectedOutput(const std::vector<int32_t>& input, int32_t K) {
       const size_t n = input.size();
       assert(n > 0);

       // Step A: local derivative. edges[i] = |input[i+1] - input[i]|, last = 0.
       std::vector<int32_t> edges(n, 0);
       for (size_t i = 0; i + 1 < n; ++i) {
           edges[i] = std::abs(input[i + 1] - input[i]);
       }

       // Step  B: global brightness. level = sum(input) / n.
       int64_t sum = 0;
       for (size_t i = 0; i < n; ++i) {
           sum += input[i];
       }
       int32_t level = static_cast<int32_t>(
           sum / static_cast<int64_t>(n));
       level = (level > 1) ? level : 1;

       // Step C: normalize the edges by brightness. output[i] = edges[i] * K / denom.
       std::vector<int32_t> output(n, 0);
       for (size_t i = 0; i < n; ++i) {
           output[i] = static_cast<int32_t>(
               static_cast<int64_t>(edges[i]) * K / level);
       }
       return output;
   }

Using the Graph API on CPU
----------------------------

The graph algorithm supports splitting work between the host CPU, one or
more FPGAs, and one or more GPUs.

For the first example, to get a handle on the Graph API, we'll be using it to
place an algorithm on the CPU first, and then we're going to move that
algorithm to the FPGA. Placing a graph algorithm on the CPU benefits from
parallel execution of the kernels, but the major advantage is that we will be
able to replace CPU kernels with FPGA or GPU kernels.

Creating the Kernels for CPU Usage
------------------------------------

The first step to consider is to split the algorithm into kernels of work
that can be executed individually. For the algorithm discussed above, steps A
and B can run in parallel on the input data, while step C can only run after
both steps A and B have produced their results. Therefore steps A, B and C
naturally correspond to kernels A, B and C.

For each kernel we must consider its inputs and outputs, in terms of scalars
and buffers. In this example we have:

.. code-block:: text

   Kernel A:
     * Inputs:
       - Buffer<int32> input
     * Outputs:
       - Buffer<int32> edges

   Kernel B:
     * Inputs:
       - Buffer<int32> input
     * Outputs:
       - Scalar<int32> level

   Kernel C:
     * Inputs:
       - Buffer<int32> edges
       - Scalar<int32> level
       - Scalar<int32> K
     * Outputs:
       - Buffer<int32> output

Note that we consider tuning variables, such as ``K``, that may need to
change between different runs of the same algorithm, as scalar inputs.

To write a kernel to run on the host CPU, we derive the ``CpuKernel`` class,
and implement its ``run()`` method, assuming the data and buffers are already
prepared for us, in the abstract ``Args`` class. We can retrieve buffers from
the ``Args`` class by name, using the ``in()`` and ``out()`` methods.

.. code-block:: cpp

   #include <vrt/graph/graph.hpp>

   using vrt::graph::CpuKernel;
   using vrt::graph::Args;

   // INCOMPLETE CpuKernel example for Step A
   class CpuEdges : public CpuKernel {
      public:
       CpuEdges() : CpuKernel("cpu_edges") {}

       void run(Args& args) override {
           auto input = args.in<int32_t>("input");
           auto edges = args.out<int32_t>("edges");

           for (size_t i = 0; i + 1 < input.size(); ++i) {
               edges[i] = std::abs(input[i + 1] - input[i]);
           }
       }
   };

However, there is one issue with this kernel class: there is no way for the
Graph runtime to know what types of data the kernel expects or produces
without running it. This information is essential for building the graph,
because the runtime must allocate correctly-typed buffers, route data between
kernels, and validate the graph's structure before any kernel fires. A kernel
that only describes its I/O inside ``run()`` cannot be safely wired up ahead
of time.

To solve this, we must add a boilerplate method in which we forward-declare
the buffers we will use during kernel execution.

.. code-block:: cpp

   using vrt::graph::IOTypeMap;

   // COMPLETE CpuKernel example for Step A
   class CpuEdges : public CpuKernel {
      public:
       CpuEdges() : CpuKernel("cpu_edges") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .in<int32_t>("input")
               .out<int32_t>("edges");
       }

       void run(Args& args) override {
           auto input = args.in<int32_t>("input");
           auto edges = args.out<int32_t>("edges");

           for (size_t i = 0; i + 1 < input.size(); ++i) {
               edges[i] = std::abs(input[i + 1] - input[i]);
           }
       }

   };

The ``in()`` and ``out()`` methods correspond to buffers, which are the
principal data shape. Scalars can be used too, using the ``scalarIn()`` and
``scalarOut()`` methods.

.. code-block:: cpp

   // CpuKernel example for Step B
   class CpuLevel : public CpuKernel {
      public:
       CpuLevel() : CpuKernel("cpu_level") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .in<int32_t>("input")
               .scalarOut<int32_t>("level");
       }

       void run(Args& args) override {
           auto  input = args.in<int32_t>("input");
           auto& level = args.scalarOut<int32_t>("level");

           int64_t sum = 0;
           for (size_t i = 0; i < n; ++i) {
               sum += input[i];
           }

           level = static_cast<int32_t>(sum / static_cast<int64_t>(n));
           level = (level > 1) ? level : 1;
       }
   };

.. code-block:: cpp

   // CpuKernel example for Step C
   class CpuNormalize : public CpuKernel {
      public:
       CpuNormalize() : CpuKernel("cpu_normalize") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .in<int32_t>("edges")
               .scalarIn<int32_t>("level")
               .scalarIn<int32_t>("K")
               .out<int32_t>("output");
       }

       void run(Args& args) override {
           auto edges  = args.in<int32_t>("edges");
           auto level  = args.scalarIn<int32_t>("level");
           auto K      = args.scalarIn<int32_t>("K");
           auto output = args.out<int32_t>("output");

           for (size_t i = 0; i < n; ++i) {
               output[i] = static_cast<int32_t>(
                   static_cast<int64_t>(edges[i]) * K / level);
           }
       }
   };

Optimizing Space Usage with inout Buffers
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Conceptually, kernels are purely functional operations, using a list of
inputs and creating a list of outputs.

On the other hand, for many buffer operations, creating a new physical buffer
to hold the output at every step is inefficient and unnecessary, compared to
modifying it in place.

For example, the ``CpuNormalize`` kernel as written above does
``output[i] = edges[i] * K / level``, but this can more efficiently be
written as ``edges[i] = edges[i] * K / level;`` modifying the buffer in
place. This is possible because there is no other need for the original
``edges`` buffer after or in parallel with ``CpuNormalize``.

To express such a buffer modification operation, we can use the ``inout()``
function.

.. code-block:: cpp

   // Optimized CpuKernel example for Step C
   class CpuNormalize : public CpuKernel {
      public:
       CpuNormalize() : CpuKernel("cpu_normalize") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .inout<int32_t>("edges")
               .scalarIn<int32_t>("level")
               .scalarIn<int32_t>("K");
       }

       void run(Args& args) override {
           auto edges  = args.inout<int32_t>("edges");
           auto level  = args.scalarIn<int32_t>("level");
           auto K      = args.scalarIn<int32_t>("K");

           for (size_t i = 0; i < n; ++i) {
               edges[i] = static_cast<int32_t>(
                   static_cast<int64_t>(edges[i]) * K / level);
           }
       }
   };

Reusing space using ``inout`` is not always possible or efficient. For
example, ``CpuEdges`` could not reuse space in this way, because ``CpuLevel``
needs to read the same input buffer at the same time. If ``CpuEdges``
declared an inout buffer, it would implicitly wait for ``CpuLevel`` to finish
before starting. This would be handled transparently by the runtime, but it
would impact runtime performance.

Creating the Graph
~~~~~~~~~~~~~~~~~~~

We have now created three kernels, but they are still just independent
procedures, with no relationship between them — running them as a unit
requires combining them into a graph. The names for the buffers, such as
``"edges"`` or ``"input"``, are scoped per-kernel, and do not imply a
relationship between the kernels.

A Graph object contains:

- **Devices** — one or more compute targets (CPU, FPGA, GPU) that kernels
  are placed on.
- **Kernel registrations** — the set of named kernels available on each
  device.
- **Tokens** — typed graph-level scalars and buffers that flow between
  kernel calls. Input tokens carry data into the graph; output tokens carry
  results back to the caller; internal tokens (created with
  ``graph.buffer<T>`` / ``graph.scalar<T>``) are temporaries that live only
  during execution.
- **Kernel calls** — nodes that bind tokens to kernel ports and express
  data-flow edges.

First, we create the graph object, and register our kernels to the CPU
device.

.. code-block:: cpp

       // 1. Graph initialization
       auto graph = vrt::graph::Graph::withDefaults();

       // 2. Devices
       auto cpu = graph.cpu();

       // 3. Kernels
       auto edges     = cpu.add<CpuEdges>();
       auto level     = cpu.add<CpuLevel>();
       auto normalize = cpu.add<CpuNormalize>();

After we have registered our devices (in this case, just the CPU), and the
kernels, we can proceed to create the graph, out of scalar and buffer
declarations and kernel calls.

.. code-block:: cpp

       // 4. Scalars and buffers which are inputs to the graph as a whole.
       auto n = graph.scalarInput<uint64_t>("n");
       auto input = graph.input<uint32_t>("input", n);
       auto K = graph.scalarInput<uint32_t>("K");

       // 5. Scalars and buffers which are outputs to the graph as a whole.
       auto output = graph.output<uint32_t>("output", n);

       // 6. Kernel calls and temporaries
       auto edges = graph.buffer<uint32_t>("edges", n);
       graph.addKernelCall({
           .kernel  = edges,
           .inputs  = {{"input", input}},
           .outputs = {{"edges", edges}},
       });

       auto level = graph.scalar<uint32_t>("level");
       graph.addKernelCall({
           .kernel        = level,
           .inputs        = {{"input", input}},
           .outputScalars = {{"level", level}},
       });

       graph.addKernelCall({
           .kernel = normalize,
           .inouts = {{"edges", edges, output}},
           .inputScalars = {{"level", level}, {"K", K}},
       });

The final step of graph creation is to compile the graph. Compiling the
graph validates the structure (every input token has exactly one producer,
no cycles exist, all buffer types are consistent), lowers the kernel call
graph into a per-device execution plan, and returns an executable snapshot
called a ``CompiledGraph``. The original ``Graph`` object is not consumed and
can be compiled again after further edits.

.. code-block:: cpp

       auto exec = graph.compile();

Running the Graph
~~~~~~~~~~~~~~~~~~

Now that we have compiled the graph, we are ready to run it. We can run the
compiled graph as many times as we want, using different data.

Running the compiled graph is a 3-step process:

1. Set data for all graph-level input buffers and scalars.
2. Run the graph using ``run()``.
3. Read graph-level output buffers and scalars.

.. code-block:: cpp

       // Step 1: Inputs
       const std::vector<uint32_t> input = generateInput();
       const uint32_t K = generateK();

       exec.writeScalar("n", input.size());
       exec.write("input", input);
       exec.writeScalar("K", K);

       // Step 2: Run
       exec.run();

       // Step 3: Outputs
       std::vector<uint32_t> output;

       exec.read("output", output);

Using the Graph Algorithm on FPGA
------------------------------------

The graph we built above runs entirely on the CPU. The FPGA accelerates
kernels by offloading compute-intensive buffer operations onto the Alveo V80
fabric.

Moving a kernel from CPU to FPGA requires three things:

1. Writing the kernel in HLS C++.
2. Compiling it into a ``.vbin`` (a loadable FPGA image archive).
3. Declaring the FPGA kernel handle in the host application and wiring it
   into the graph in place of the CPU kernel.

The Graph runtime handles all the logistics — DMA transfers, PDI
programming, scheduling — transparently.

Writing the Kernels in HLS
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

An FPGA kernel for the Graph API is a plain ``extern "C"`` function with HLS
interface pragmas. The conventions mirror the CPU kernel's port names:
buffer ports become ``m_axi`` ports, scalar ports become ``s_axilite``
ports, and the kernel name must match what you will declare in the host
application.

Below is a self-contained kernel that implements step A (local derivative):

.. code-block:: cpp

   // hls/edges_kernel.cpp
   #include <ap_int.h>

   extern "C" void edges_kernel(ap_uint<64> n, const ap_int<32>* in, ap_int<32>* edges) {
   #pragma HLS interface mode=s_axilite port=n
   #pragma HLS interface mode=s_axilite port=return
   #pragma HLS interface m_axi bundle=gmem0 port=in    max_widen_bitwidth=64
   #pragma HLS interface m_axi bundle=gmem1 port=edges max_widen_bitwidth=64

       for (ap_uint<64> i = 0; i + 1 < n; ++i) {
           ap_int<32> diff = in[i + 1] - in[i];
           edges[i] = diff < 0 ? ap_int<32>(-diff) : ap_int<32>(diff);
       }
       edges[n - 1] = 0;  // last element has no right neighbour
   }

A few important points:

- **Scalar ports** (``n``) use ``mode=s_axilite``. The function return must
  also be ``s_axilite``; this is required by the runtime to detect
  completion.
- **Buffer ports** (``in``, ``edges``) use ``m_axi`` with separate bundles
  so reads and writes can proceed simultaneously.
- Port names must exactly match the names you declare when building the
  kernel handle in the host application (see below).

Step B (global brightness) reduces a buffer to a scalar. Both scalar inputs
and scalar outputs use ``s_axilite``; the Graph runtime handles forwarding
the value to any kernel that consumes it.

.. code-block:: cpp

   // hls/level_kernel.cpp
   #include <ap_int.h>

   extern "C" void level_kernel(ap_uint<64> n, const ap_int<32>* in, ap_int<32>* level) {
   #pragma HLS interface mode=s_axilite port=n
   #pragma HLS interface mode=s_axilite port=level   // scalar output
   #pragma HLS interface mode=s_axilite port=return
   #pragma HLS interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64

       ap_int<64> sum = 0;
       for (ap_uint<64> i = 0; i < n; ++i) {
           sum += in[i];
       }
       ap_int<32> result = (ap_int<32>)(sum / (ap_int<64>)n);
       *level = (result > 1) ? result : ap_int<32>(1);
   }

Step C reads ``level`` as a scalar input and normalizes the edges in place:

.. code-block:: cpp

   // hls/normalize_kernel.cpp
   #include <ap_int.h>

   extern "C" void normalize_kernel(ap_uint<64> n, ap_int<32> K, ap_int<32> level, ap_int<32>* edges) {
   #pragma HLS interface mode=s_axilite port=n
   #pragma HLS interface mode=s_axilite port=K
   #pragma HLS interface mode=s_axilite port=level
   #pragma HLS interface mode=s_axilite port=return
   #pragma HLS interface m_axi bundle=gmem0 port=edges max_widen_bitwidth=64

       for (ap_uint<64> i = 0; i < n; ++i) {
           edges[i] = (ap_int<32>)((ap_int<64>)edges[i] * K / level);
       }
   }

Each ``.cpp`` file needs a matching HLS configuration file that specifies the
target part and build settings:

.. code-block:: ini

   # hls/edges_kernel.cfg
   part=xcv80-lsva4737-2MHP-e-S

   [hls]
   flow_target=vivado

   syn.top=edges_kernel
   syn.file=edges_kernel.cpp
   clock=5ns

   package.output.format=ip_catalog

Use the same structure for ``level_kernel.cfg`` and ``normalize_kernel.cfg``,
changing ``syn.top`` and ``syn.file`` accordingly.

Creating the vbin
~~~~~~~~~~~~~~~~~~

A ``.vbin`` is a loadable FPGA image archive that packages one or more
compiled HLS kernels together with a static shell PDI for the Alveo V80. The
``slashkit`` linker, exposed through the ``add_vbin`` CMake macro, handles
this step.

All three kernels go into a single image. This is the preferred arrangement:
fewer images means fewer reprogram steps at runtime, and the linker can use
any unused PL resources across kernels in the same image.

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(edge_detection LANGUAGES CXX)

   set(CMAKE_CXX_STANDARD 17)

   find_package(vrt REQUIRED CONFIG)
   find_package(SlashTools REQUIRED)

   set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

   build_hls_dir(TARGET hls_edges     ROOT hls DEVICE "${DEVICE}" KERNELS edges_kernel
                 OUT_KERNELS _KERNELS_EDGES)
   build_hls_dir(TARGET hls_level     ROOT hls DEVICE "${DEVICE}" KERNELS level_kernel
                 OUT_KERNELS _KERNELS_LEVEL)
   build_hls_dir(TARGET hls_normalize ROOT hls DEVICE "${DEVICE}" KERNELS normalize_kernel
                 OUT_KERNELS _KERNELS_NORM)

   add_vbin(
     TARGET    edge_detection_hw
     PLATFORM  hw
     CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg"
     KERNELS   ${_KERNELS_EDGES} ${_KERNELS_LEVEL} ${_KERNELS_NORM}
   )

   add_executable(${PROJECT_NAME} main.cpp)
   target_link_libraries(${PROJECT_NAME} PRIVATE vrt::vrt)

The ``config.cfg`` linker configuration assigns each kernel's memory ports to
HBM banks. Ports that exchange the same graph buffer must use the same bank:
both readers of ``input`` use HBM0, while the ``edges`` producer and consumer
use HBM1. Scalar ports do not need ``sp=`` entries:

.. code-block:: ini

   # config.cfg
   [connectivity]
   nk=edges_kernel:1
   nk=level_kernel:1
   nk=normalize_kernel:1

   sp=edges_kernel_0.m_axi_gmem0:HBM0
   sp=edges_kernel_0.m_axi_gmem1:HBM1
   sp=level_kernel_0.m_axi_gmem0:HBM0
   sp=normalize_kernel_0.m_axi_gmem0:HBM1

Build the vbin:

.. code-block:: bash

   cmake -S . -B build
   cmake --build build
   cmake --build build --target edge_detection_hw

The file ``edge_detection_hw.vbin`` is the artifact needed at runtime.

Modifying the Code to Use the FPGA Kernels
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

With the vbin in hand, the host application needs three additions compared
to the CPU-only version: registering the FPGA device, declaring typed FPGA
kernel handles, and gating each FPGA dispatch behind an explicit
``addReprogram`` node so the Graph compiler can prove which image is active.

The ``Graph::addFpga()`` call folds all hardware setup into a single step:
it opens a vrtd session, stages the PDI bytes into DDR via QDMA, loads the
vbin, and returns an ``FpgaHandle`` whose ``image()`` method yields typed
image handles.

.. code-block:: cpp

   #include <chrono>
   #include <vrt/graph/graph.hpp>

   using namespace std::chrono_literals;
   using vrt::graph::Graph;
   using vrt::graph::GraphBuffer;
   using vrt::graph::GraphScalar;

   int main(int argc, char** argv) {
       const std::string bdf    = argv[1];
       const std::string vbin   = argv[2];  // edge_detection_hw.vbin

       // 1. Graph initialization
       Graph graph = Graph::withDefaults();

       // 2. Devices
       auto fpga  = graph.addFpga({
           .bdf     = bdf,
           .images  = {{"image", vbin}},
           .waitTimeout = 30s,
       });
       auto image = fpga.image("image");

       // 3. Kernels
       auto fpgaEdges = image.kernel("edges_kernel_0")
                           .scalarIn<uint64_t>("n")
                           .in<int32_t>("in")
                           .out<int32_t>("edges");

       auto fpgaLevel = image.kernel("level_kernel_0")
                           .scalarIn<uint64_t>("n")
                           .in<int32_t>("in")
                           .scalarOut<int32_t>("level");

       auto fpgaNorm = image.kernel("normalize_kernel_0")
                           .scalarIn<uint64_t>("n")
                           .scalarIn<int32_t>("K")
                           .scalarIn<int32_t>("level")
                           .inout<int32_t>("edges");

       // 4. Scalars and buffers which are inputs to the graph as a whole.
       GraphScalar n     = graph.scalarInput<uint64_t>("n");
       GraphScalar K     = graph.scalarInput<int32_t>("K");
       GraphBuffer input = graph.input<int32_t>("input", n);

       // 5. Scalars and buffers which are outputs to the graph as a whole.
       GraphBuffer output = graph.output<int32_t>("output", n);

       // 6. Kernel calls and temporaries.
       //    Kernels A and B run in parallel; C runs after both.
       auto r = graph.addReprogram({.image = image});

       GraphBuffer edges = graph.buffer<int32_t>("edges", n);
       GraphScalar level = graph.scalar<int32_t>("level");

       graph.addKernelCall({
           .kernel       = fpgaEdges,
           .inputScalars = {{"n", n}},
           .inputs       = {{"in", input}},
           .outputs      = {{"edges", edges}},
           .after        = {r},
       });

       graph.addKernelCall({
           .kernel        = fpgaLevel,
           .inputScalars  = {{"n", n}},
           .inputs        = {{"in", input}},
           .outputScalars = {{"level", level}},
           .after         = {r},
       });

       graph.addKernelCall({
           .kernel       = fpgaNorm,
           .inputScalars = {{"n", n}, {"K", K}, {"level", level}},
           .inouts       = {{"edges", edges, output}},
           .after        = {r},
       });

       auto exec = graph.compile();

       // Step 1: Inputs
       exec.writeScalar(n, static_cast<uint64_t>(input_data.size()));
       exec.writeScalar(K, 16);
       exec.write(input, input_data);

       // Step 2: Run
       exec.run();

       // Step 3: Outputs
       std::vector<int32_t> result(input_data.size());
       exec.read(output, result);
   }

Extra: Generating .dot Diagrams
----------------------------------

The Graph API can render the authored graph and each compiled per-device
execution plan as a Graphviz ``.dot`` file for debugging and documentation.

Include the render header:

.. code-block:: cpp

   #include <vrt/graph/render/dot.hpp>

Render the high-level authored graph before or after compilation:

.. code-block:: cpp

   // Write the authored graph (kernel nodes, data-flow edges, control flow structure).
   vrt::graph::render::writeToDotFile(graph, "graph.dot");

Convert to a PNG using the ``dot`` command-line tool from Graphviz:

.. code-block:: bash

   dot -Tpng graph.dot -o graph.png
   dot -Tpng dgraph_cpu.dot -o dgraph_cpu.png
   dot -Tpng dgraph_fpga_0.dot -o dgraph_fpga_0.png

Or open interactively with ``xdot``:

.. code-block:: bash

   xdot graph.dot

The two views are complementary:

- ``renderToDot(graph)`` shows the user-authored graph: kernel nodes grouped
  by device and region, solid edges for data dependencies, dashed edges for
  explicit ``afterOps`` ordering. This is useful for reviewing the graph
  structure before running anything.
- ``renderToDot(dgraph)`` shows the compiled per-device plan: only nodes
  assigned to that device, including any bridge operations the compiler
  injected to transfer buffers across device boundaries. This is useful for
  understanding what the FPGA or CPU queue will actually execute.

For a deeper debugging workflow, including how to render every compiled
per-device plan and inspect reprogram ordering, see
:doc:`/howto/debug-graph-with-dot`.

Extra: Splitting Kernels Across Multiple vbins
--------------------------------------------------

The single-image approach above is generally preferable, and should be your
starting point. Splitting kernels across multiple images is only necessary
when the combined resource utilization of all kernels exceeds what the FPGA
fabric can accommodate in a single partial reconfiguration region.

When a split is necessary, keep two principles in mind:

1. **Minimize the number of images.** Each image boundary introduces a
   reprogram step at runtime. Reprogram is not free — so the fewer the
   images, the better.
2. **Place a boundary where there is already a natural sequential barrier in
   the graph.** A reprogram stalls the FPGA queue, so you lose less if the
   barrier already exists for data-flow reasons. Conversely, splitting
   between two kernels that could otherwise run in parallel forces them to
   be sequential and reduces throughput.

For this algorithm, the natural split point is between the two phases: steps
A and B can run in parallel, then step C must wait for both. If the three
kernels do not fit in a single image, A and B go into one image and C goes
into another — preserving the parallelism that already exists within each
phase.

The CMakeLists changes are straightforward: replace the single ``add_vbin``
with two, one per phase:

.. code-block:: cmake

     # Image 1: kernels A and B run in parallel.
     add_vbin(
       TARGET    edge_detection_ab_hw
       PLATFORM  hw
       CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config_ab.cfg"
       KERNELS   ${_KERNELS_EDGES} ${_KERNELS_LEVEL}
     )

     # Image 2: kernel C runs after both A and B.
     add_vbin(
       TARGET    edge_detection_c_hw
       PLATFORM  hw
       CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config_c.cfg"
       KERNELS   ${_KERNELS_NORM}
     )

In the host application, register both images with ``addFpga`` and chain the
reprogram nodes:

.. code-block:: cpp

       auto fpga   = graph.addFpga({
           .bdf    = bdf,
           .images = {{"imageAB", vbinAB}, {"imageC", vbinC}},
           .waitTimeout = 30s,
       });
       auto imageAB = fpga.image("imageAB");
       auto imageC  = fpga.image("imageC");

       // Assign kernels to their respective images.
       auto fpgaEdges = imageAB.kernel("edges_kernel_0") ...;
       auto fpgaLevel = imageAB.kernel("level_kernel_0") ...;
       auto fpgaNorm  = imageC.kernel("normalize_kernel_0") ...;

       // Two reprogram nodes chained: rC waits for all kernels behind rAB to finish
       // before the PDI is overwritten.
       auto rAB = graph.addReprogram({.image = imageAB});
       auto rC  = graph.addReprogram({.image = imageC, .after = {rAB}});

       // Kernel calls are otherwise identical to the single-image version,
       // with fpgaEdges and fpgaLevel gated behind rAB, and fpgaNorm behind rC.

The rest of the graph authoring — token declarations, ``addKernelCall``
bodies, compile/run/read — is unchanged from the single-image version.

Algorithm 2: Loops and Conditionals
======================================

Algorithm 1 introduced kernels, buffers, scalars, and kernel calls. This
second algorithm adds the two remaining building blocks of the Graph API:
loops and conditionals.

We'll compute an iterative sharpening operation on a 1-D signal, gated by an
adaptive gain that depends on how bright the signal is:

- **The loop**: repeatedly apply a discrete Laplacian sharpening step to the
  signal, ``K`` times, each iteration reading the previous iteration's
  result. This is a loop that carries state across iterations.
- **In parallel with the loop**: compute the average brightness of the
  *original* signal — the same reduction as Algorithm 1's Kernel B. Because
  this step never touches the loop's carried state, it can run independently
  of it.
- **The conditional**: once both the loop and the brightness computation are
  done, apply one of two gains depending on whether the signal is bright or
  dark.

.. code-block:: cpp

   std::vector<int32_t> expectedOutput(const std::vector<int32_t>& input,
                                       uint32_t K, int32_t alpha,
                                       int32_t threshold, int32_t boost) {
       const size_t n = input.size();
       assert(n > 0);

       // Loop: K iterations of discrete Laplacian sharpening (edge-replicated boundaries).
       std::vector<int32_t> s = input;
       for (uint32_t iter = 0; iter < K; ++iter) {
           std::vector<int32_t> next(n);
           for (size_t i = 0; i < n; ++i) {
               int32_t left  = (i == 0)     ? s[i] : s[i - 1];
               int32_t right = (i + 1 == n) ? s[i] : s[i + 1];
               int32_t lap   = 2 * s[i] - left - right;
               next[i] = s[i] + alpha * lap;
           }
           s = std::move(next);
       }

       // In parallel with the loop: brightness of the ORIGINAL input.
       int64_t sum = 0;
       for (size_t i = 0; i < n; ++i) sum += input[i];
       int32_t level = static_cast<int32_t>(sum / static_cast<int64_t>(n));
       level = (level > 1) ? level : 1;

       // Conditional: adaptive gain based on brightness.
       std::vector<int32_t> output(n);
       if (level >= threshold) {
           for (size_t i = 0; i < n; ++i) output[i] = s[i];         // bright: pass through
       } else {
           for (size_t i = 0; i < n; ++i) output[i] = s[i] * boost; // dark: boost
       }
       return output;
   }

Writing the Algorithm in CPU Kernels
---------------------------------------

Creating the Kernels for CPU Usage
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The loop body is its own kernel — a single sharpening step. Note that, as in
Algorithm 1, the kernel doesn't need an explicit element-count port; it reads
the buffer's length directly from the span it's given.

.. code-block:: cpp

   using vrt::graph::CpuKernel;
   using vrt::graph::IOTypeMap;

   // One iteration of the sharpening stencil.
   class CpuSharpen : public CpuKernel {
      public:
       CpuSharpen() : CpuKernel("cpu_sharpen") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .in<int32_t>("in")
               .scalarIn<int32_t>("alpha")
               .out<int32_t>("out");
       }

       void run(Args& args) override {
           auto in    = args.in<int32_t>("in");
           auto alpha = args.scalarIn<int32_t>("alpha");
           auto out   = args.out<int32_t>("out");

           const size_t n = in.size();
           for (size_t i = 0; i < n; ++i) {
               int32_t left  = (i == 0)     ? in[i] : in[i - 1];
               int32_t right = (i + 1 == n) ? in[i] : in[i + 1];
               int32_t lap   = 2 * in[i] - left - right;
               out[i] = in[i] + alpha * lap;
           }
       }
   };

The brightness kernel is identical in shape to Algorithm 1's Kernel B — it
only ever reads the graph's original input, never the loop's carried state:

.. code-block:: cpp

   class CpuLevel : public CpuKernel {
      public:
       CpuLevel() : CpuKernel("cpu_level") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}.in<int32_t>("input").scalarOut<int32_t>("level");
       }

       void run(Args& args) override {
           auto  input = args.in<int32_t>("input");
           auto& level = args.scalarOut<int32_t>("level");

           int64_t sum = 0;
           for (size_t i = 0; i < input.size(); ++i) sum += input[i];
           level = static_cast<int32_t>(sum / static_cast<int64_t>(input.size()));
           level = (level > 1) ? level : 1;
       }
   };

Each branch of the conditional gets its own kernel. The bright branch needs
no further processing — but both branches of a conditional must still
produce every declared output port, so the kernel exists purely to satisfy
that contract, and its body is empty:

.. code-block:: cpp

   // Bright branch: the signal is already well-lit, so no gain adjustment is needed.
   class CpuPassthrough : public CpuKernel {
      public:
       CpuPassthrough() : CpuKernel("cpu_passthrough") {}
       IOTypeMap ioTypeMap() const override { return IOTypeMap{}.inout<int32_t>("data"); }
       void run(Args&) override {}
   };

   // Dark branch: scale the signal up by a fixed boost factor.
   class CpuBoost : public CpuKernel {
      public:
       CpuBoost() : CpuKernel("cpu_boost") {}

       IOTypeMap ioTypeMap() const override {
           return IOTypeMap{}
               .in<int32_t>("in")
               .scalarIn<int32_t>("boost")
               .out<int32_t>("out");
       }

       void run(Args& args) override {
           auto in    = args.in<int32_t>("in");
           auto boost = args.scalarIn<int32_t>("boost");
           auto out   = args.out<int32_t>("out");
           for (size_t i = 0; i < in.size(); ++i) out[i] = in[i] * boost;
       }
   };

Authoring the Loop
~~~~~~~~~~~~~~~~~~~~

A loop is authored as its own nested region: ``graph.addLoop({...})``
returns a ``RegionBuilder`` for that region, on which further kernel calls
are authored just like at the top level.

- ``.count`` is the trip count — an integer graph scalar.
- A port name present in both ``.inputs`` and ``.outputs`` is
  *loop-carried*: on the first iteration ``loop.input(port)`` reads the
  ``.inputs`` token; on every iteration the body must write
  ``loop.output(port)``; and once the loop finishes, the final iteration's
  value is published to the ``.outputs`` token.

.. code-block:: cpp

       auto sharpened = graph.buffer<int32_t>("sharpened", n);
       {
           auto loop = graph.addLoop({
               .count   = K,
               .inputs  = {{"state", input}},
               .outputs = {{"state", sharpened}},
           });

           loop.addKernelCall({
               .kernel       = sharpen,
               .inputScalars = {{"alpha", alpha}},
               .inputs       = {{"in", loop.input("state")}},
               .outputs      = {{"out", loop.output("state")}},
           });
       }

Here ``state`` starts out as ``input``; each iteration reads the previous
iteration's output and produces the next one; after ``K`` iterations the
result lands in ``sharpened``. Note that ``alpha`` — a graph-level input
declared outside the loop — is referenced directly inside the loop body: a
root-scope scalar can be captured by a kernel call in a nested region without
any extra wiring.

Authoring the Conditional
~~~~~~~~~~~~~~~~~~~~~~~~~~~

A conditional is authored similarly, but produces *two* branch regions — one
per outcome. ``.condition`` takes a ``Condition``, most simply built by
comparing a ``GraphScalar`` against a constant or another scalar. Both
branches must produce every port listed in ``.outputs``.

.. code-block:: cpp

       auto brightness = graph.scalar<int32_t>("brightness");
       graph.addKernelCall({
           .kernel        = level,
           .inputs        = {{"input", input}},
           .outputScalars = {{"level", brightness}},
       });

       auto [thenBranch, elseBranch] = graph.addConditional({
           .condition = (brightness >= threshold),
           .inputs    = {{"signal", sharpened}},
           .outputs   = {{"signal", output}},
       });

       thenBranch.addKernelCall({
           .kernel = passthrough,
           .inouts = {{"data", thenBranch.input("signal"), thenBranch.output("signal")}},
       });

       elseBranch.addKernelCall({
           .kernel       = boost,
           .inputScalars = {{"boost", boostBy}},
           .inputs       = {{"in", elseBranch.input("signal")}},
           .outputs      = {{"out", elseBranch.output("signal")}},
       });

Note that ``level``'s kernel call above sits outside both the loop and the
conditional, at the graph's top level — it reads ``input`` (available from
the start) and writes ``brightness``, with no dependency on ``sharpened``.
The scheduler is therefore free to run it concurrently with the loop; both
must finish before the conditional, which depends on both, can start.

Putting it all together, in the same numbered style as Algorithm 1:

.. code-block:: cpp

       // 1. Graph initialization
       auto graph = vrt::graph::Graph::withDefaults();

       // 2. Devices
       auto cpu = graph.cpu();

       // 3. Kernels
       auto sharpen     = cpu.add<CpuSharpen>();
       auto level       = cpu.add<CpuLevel>();
       auto passthrough = cpu.add<CpuPassthrough>();
       auto boost       = cpu.add<CpuBoost>();

       // 4. Scalars and buffers which are inputs to the graph as a whole.
       auto n         = graph.scalarInput<uint64_t>("n");
       auto input     = graph.input<int32_t>("input", n);
       auto K         = graph.scalarInput<uint32_t>("K");
       auto alpha     = graph.scalarInput<int32_t>("alpha");
       auto threshold = graph.scalarInput<int32_t>("threshold");
       auto boostBy   = graph.scalarInput<int32_t>("boost");

       // 5. Scalars and buffers which are outputs to the graph as a whole.
       auto output = graph.output<int32_t>("output", n);

       // 6. Kernel calls and temporaries.
       auto sharpened = graph.buffer<int32_t>("sharpened", n);
       {
           auto loop = graph.addLoop({
               .count   = K,
               .inputs  = {{"state", input}},
               .outputs = {{"state", sharpened}},
           });

           loop.addKernelCall({
               .kernel       = sharpen,
               .inputScalars = {{"alpha", alpha}},
               .inputs       = {{"in", loop.input("state")}},
               .outputs      = {{"out", loop.output("state")}},
           });
       }

       auto brightness = graph.scalar<int32_t>("brightness");
       graph.addKernelCall({
           .kernel        = level,
           .inputs        = {{"input", input}},
           .outputScalars = {{"level", brightness}},
       });

       auto [thenBranch, elseBranch] = graph.addConditional({
           .condition = (brightness >= threshold),
           .inputs    = {{"signal", sharpened}},
           .outputs   = {{"signal", output}},
       });

       thenBranch.addKernelCall({
           .kernel = passthrough,
           .inouts = {{"data", thenBranch.input("signal"), thenBranch.output("signal")}},
       });

       elseBranch.addKernelCall({
           .kernel       = boost,
           .inputScalars = {{"boost", boostBy}},
           .inputs       = {{"in", elseBranch.input("signal")}},
           .outputs      = {{"out", elseBranch.output("signal")}},
       });

Running the Graph
~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

       auto exec = graph.compile();

       // Step 1: Inputs
       const std::vector<int32_t> inputData = generateInput();
       exec.writeScalar(n, static_cast<uint64_t>(inputData.size()));
       exec.write(input, inputData);
       exec.writeScalar(K, 20u);
       exec.writeScalar(alpha, 1);
       exec.writeScalar(threshold, 50);
       exec.writeScalar(boostBy, 2);

       // Step 2: Run
       exec.run();

       // Step 3: Outputs
       std::vector<int32_t> result(inputData.size());
       exec.read(output, result);

Rewriting the Kernels for FPGA Offload
------------------------------------------

The loop is the computationally heavy part of this algorithm — it re-scans
the whole signal ``K`` times. The brightness reduction and the two branch
kernels are each a single pass over the signal, cheap enough to leave on the
CPU. Moving only ``cpu_sharpen`` to the FPGA also preserves the parallelism
from the golden model: ``cpu_level`` still runs on the CPU, and since it no
longer competes with the loop for CPU time, it runs alongside the FPGA loop
for the loop's entire duration.

Writing the Kernel in HLS
~~~~~~~~~~~~~~~~~~~~~~~~~~~

As in Algorithm 1, scalar ports use ``s_axilite`` and buffer ports use
``m_axi`` with separate bundles. The only kernel that needs rewriting is the
sharpening step; ``level``, ``passthrough``, and ``boost`` stay exactly as
written above.

.. code-block:: cpp

   // hls/sharpen_kernel.cpp
   #include <ap_int.h>

   extern "C" void sharpen_kernel(ap_uint<64> n, ap_int<32> alpha,
                                  const ap_int<32>* in, ap_int<32>* out) {
   #pragma HLS interface mode=s_axilite port=n
   #pragma HLS interface mode=s_axilite port=alpha
   #pragma HLS interface mode=s_axilite port=return
   #pragma HLS interface m_axi bundle=gmem0 port=in  max_widen_bitwidth=64
   #pragma HLS interface m_axi bundle=gmem1 port=out max_widen_bitwidth=64

       if (n == 0) return;

       // Loop-carried buffers may alias in and out. Retain original neighbour
       // values in a sliding window so an in-place call remains correct.
       ap_int<32> left = in[0];
       ap_int<32> current = in[0];
       for (ap_uint<64> i = 0; i < n; ++i) {
           ap_int<32> right = (i + 1 == n) ? current : in[i + 1];
           ap_int<32> lap = 2 * current - left - right;
           out[i] = current + alpha * lap;
           left = current;
           current = right;
       }
   }

.. code-block:: ini

   # hls/sharpen_kernel.cfg
   part=xcv80-lsva4737-2MHP-e-S

   [hls]
   flow_target=vivado

   syn.top=sharpen_kernel
   syn.file=sharpen_kernel.cpp
   clock=5ns

   package.output.format=ip_catalog

Creating the vbin
~~~~~~~~~~~~~~~~~~

A single kernel, a single image:

.. code-block:: cmake

   find_package(vrt REQUIRED CONFIG)
   find_package(SlashTools REQUIRED)

   set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

   build_hls_dir(TARGET hls_sharpen ROOT hls DEVICE "${DEVICE}" KERNELS sharpen_kernel
                 OUT_KERNELS _KERNELS_SHARPEN)

   add_vbin(
     TARGET    sharpen_hw
     PLATFORM  hw
     CFG       "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg"
     KERNELS   ${_KERNELS_SHARPEN}
   )

.. code-block:: ini

   # config.cfg
   [connectivity]
   nk=sharpen_kernel:1

   sp=sharpen_kernel_0.m_axi_gmem0:HBM0
   sp=sharpen_kernel_0.m_axi_gmem1:HBM0

The loop carries one physical buffer across iterations, so both kernel ports
must address the same HBM bank. The sliding-window implementation above also
keeps the stencil correct when ``in`` and ``out`` are that same buffer.

Modifying the Code to Use the FPGA Kernel
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #include <chrono>
   #include <vrt/graph/graph.hpp>

   using namespace std::chrono_literals;
   using vrt::graph::Graph;
   using vrt::graph::GraphBuffer;
   using vrt::graph::GraphScalar;

   int main(int argc, char** argv) {
       const std::string bdf  = argv[1];
       const std::string vbin = argv[2];  // sharpen_hw.vbin

       // 1. Graph initialization
       Graph graph = Graph::withDefaults();

       // 2. Devices
       auto fpga  = graph.addFpga({
           .bdf     = bdf,
           .images  = {{"image", vbin}},
           .waitTimeout = 30s,
       });
       auto image = fpga.image("image");

       // 3. Kernels -- the loop body moves to FPGA; the rest stay on the CPU.
       auto fpgaSharpen = image.kernel("sharpen_kernel_0")
                             .scalarIn<uint64_t>("n")
                             .scalarIn<int32_t>("alpha")
                             .in<int32_t>("in")
                             .out<int32_t>("out");

       auto cpu         = graph.cpu();
       auto level       = cpu.add<CpuLevel>();
       auto passthrough = cpu.add<CpuPassthrough>();
       auto boost       = cpu.add<CpuBoost>();

       // 4. Scalars and buffers which are inputs to the graph as a whole.
       auto n         = graph.scalarInput<uint64_t>("n");
       auto input     = graph.input<int32_t>("input", n);
       auto K         = graph.scalarInput<uint32_t>("K");
       auto alpha     = graph.scalarInput<int32_t>("alpha");
       auto threshold = graph.scalarInput<int32_t>("threshold");
       auto boostBy   = graph.scalarInput<int32_t>("boost");

       // 5. Scalars and buffers which are outputs to the graph as a whole.
       auto output = graph.output<int32_t>("output", n);

       // 6. Kernel calls and temporaries.

       // The loop now dispatches to the FPGA. As with any FPGA dispatch, it must
       // be gated behind a reprogram of its image -- and because the dispatch
       // lives inside the loop body, the reprogram must be authored there too;
       // a reprogram at the graph's top level would not gate a kernel nested
       // inside a loop or conditional.
       auto sharpened = graph.buffer<int32_t>("sharpened", n);
       {
           auto loop = graph.addLoop({
               .count   = K,
               .inputs  = {{"state", input}},
               .outputs = {{"state", sharpened}},
           });

           auto r = loop.addReprogram({.image = image});

           loop.addKernelCall({
               .kernel       = fpgaSharpen,
               .inputScalars = {{"n", n}, {"alpha", alpha}},
               .inputs       = {{"in", loop.input("state")}},
               .outputs      = {{"out", loop.output("state")}},
               .after        = {r},
           });
       }

       // Unchanged: `level` still reads the original `input`, so it keeps
       // running on the CPU for the whole time the FPGA loop is iterating.
       auto brightness = graph.scalar<int32_t>("brightness");
       graph.addKernelCall({
           .kernel        = level,
           .inputs        = {{"input", input}},
           .outputScalars = {{"level", brightness}},
       });

       // Unchanged: the conditional and both branches stay on the CPU.
       auto [thenBranch, elseBranch] = graph.addConditional({
           .condition = (brightness >= threshold),
           .inputs    = {{"signal", sharpened}},
           .outputs   = {{"signal", output}},
       });

       thenBranch.addKernelCall({
           .kernel = passthrough,
           .inouts = {{"data", thenBranch.input("signal"), thenBranch.output("signal")}},
       });

       elseBranch.addKernelCall({
           .kernel       = boost,
           .inputScalars = {{"boost", boostBy}},
           .inputs       = {{"in", elseBranch.input("signal")}},
           .outputs      = {{"out", elseBranch.output("signal")}},
       });

       auto exec = graph.compile();

       // Step 1: Inputs
       const std::vector<int32_t> inputData = generateInput();
       exec.writeScalar(n, static_cast<uint64_t>(inputData.size()));
       exec.write(input, inputData);
       exec.writeScalar(K, 20u);
       exec.writeScalar(alpha, 1);
       exec.writeScalar(threshold, 50);
       exec.writeScalar(boostBy, 2);

       // Step 2: Run
       exec.run();

       // Step 3: Outputs
       std::vector<int32_t> result(inputData.size());
       exec.read(output, result);
   }

A few points worth noting:

- **Only ``cpu_sharpen`` moved.** ``CpuLevel``, ``CpuPassthrough``, and
  ``CpuBoost`` are the exact same classes from the CPU-only version, still
  registered the same way via ``graph.cpu().add<T>()``.
- **The reprogram lives inside the loop body**, next to the dispatch it
  gates — the same rule Algorithm 1 established for any FPGA kernel applies
  equally inside a loop or conditional region.
- **``brightness`` and ``sharpened`` cross device boundaries with no extra
  plumbing.** ``brightness`` is produced by a CPU kernel at the graph's top
  level and consumed by the top-level conditional; ``sharpened`` is
  produced by the FPGA loop and consumed by CPU kernels inside the
  conditional's branches. Both are ordinary token references.
- **The parallelism is unchanged.** ``level`` still depends only on
  ``input``, so moving the loop to FPGA doesn't change what runs
  concurrently — the CPU is free to compute brightness for the entire
  duration of the loop.

Next Steps
==========

- :doc:`/reference/vrt-api/graph` — full API reference for ``Graph``,
  ``GraphBuffer``, ``GraphScalar``, and related types.
- :doc:`buffers-and-memory` — DDR vs HBM memory placement, used implicitly
  by FPGA-backed graph buffers.
- :doc:`/explanation/graph-api-architecture` — how the Graph compiler, device
  abstraction, and cross-device bridges fit into the rest of the SLASH stack.
