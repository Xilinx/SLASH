..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

########################
Debug Graphs with DOT
########################

This guide shows how to render a ``vrt::graph`` graph to Graphviz ``.dot``
files so you can inspect inferred data dependencies, explicit ``after`` edges,
nested loop/conditional regions, and compiled per-device execution plans.

Prerequisites
=============

- Your application uses the Graph API and includes ``<vrt/graph/render/dot.hpp>``.
- Graphviz is installed if you want to convert ``.dot`` files into images:

  .. code-block:: bash

     dot -V

- ``xdot`` is optional, but useful for interactive inspection.

Render the Authored Graph
=========================

Render the authored graph before or after ``compile()``:

.. code-block:: cpp

   #include <vrt/graph/render/dot.hpp>

   // graph is a vrt::graph::Graph you have finished authoring.
   vrt::graph::render::writeToDotFile(graph, "graph.dot");

The authored graph view shows the structure you wrote:

- kernel nodes grouped by device and graph region;
- nested clusters for loop bodies and conditional branches;
- solid edges for token data dependencies;
- dashed edges for explicit ``after`` ordering constraints, such as FPGA
  reprogram gating.

Convert the output to a PNG:

.. code-block:: bash

   dot -Tpng graph.dot -o graph.png

Or open it interactively:

.. code-block:: bash

   xdot graph.dot

Render Compiled Per-Device Graphs
=================================

After compilation, render each per-device ``DGraph`` from the
``CompiledGraph`` snapshot:

.. code-block:: cpp

   auto exec = graph.compile();

   for (const auto& dgraph : exec.dgraphs()) {
       const std::string path = "dgraph_" + dgraph.deviceId + ".dot";
       vrt::graph::render::writeToDotFile(dgraph, path);
   }

The compiled view is lower level than the authored graph:

- each file contains only nodes assigned to that device;
- bridge-injected nodes are visible where data or ordering crosses devices;
- device-copy nodes are visible where the compiler inserted an intra-FPGA
  HBM-bank transfer using the host/QDMA fallback;
- child DGraphs appear under compiled loop and conditional nodes;
- device-specific execution order is easier to inspect than in the authored
  view.

Convert the compiled views the same way:

.. code-block:: bash

   dot -Tpng dgraph_cpu.dot -o dgraph_cpu.png
   dot -Tpng dgraph_fpga:0.dot -o dgraph_fpga_0.png

Read the Views Together
=======================

Use the authored graph first when you want to understand whether the graph you
wrote has the expected shape. Use the compiled per-device graph when you want
to understand what each backend will execute.

For the loop-and-conditional sharpening example in
:doc:`/tutorials/user/graph-api`, the authored graph should show:

- a top-level graph with the input, brightness, loop, conditional, and output
  flow;
- a loop body region containing the sharpening kernel;
- two conditional branch regions, one for the pass-through branch and one for
  the boost branch;
- a data edge from the loop output to the conditional input;
- a data edge from the brightness scalar to the conditional predicate.

If the sharpening loop is moved to FPGA and the brightness/branch kernels stay
on CPU, the compiled views separate that structure:

- the FPGA DGraph contains the loop-body dispatch and any required reprogram
  node;
- the CPU DGraph contains the brightness kernel and conditional branches;
- bridge nodes appear where the sharpened signal crosses from FPGA back to CPU.

Diagnose Ungated FPGA Dispatches
================================

FPGA kernel dispatches must be ordered after a reprogram node for the image
that contains the kernel. If ``compile()`` rejects a graph because an FPGA
dispatch is not gated by a reprogram, render the authored graph and check the
dashed ``after`` edges.

The correct pattern is:

.. code-block:: cpp

   auto r = graph.addReprogram({.image = image});

   graph.addKernelCall({
       .kernel = fpgaKernel,
       .inputs = {{"in", input}},
       .outputs = {{"out", output}},
       .after = {r},
   });

In ``graph.dot``, the reprogram node should have a dashed edge to the FPGA
kernel dispatch. If the dispatch is nested inside a loop or conditional, the
reprogram must be authored in the same region as the dispatch it gates.

Next Steps
==========

- :doc:`/tutorials/user/graph-api` — build and run complete CPU/FPGA graph
  examples.
- :doc:`/reference/vrt-api/graph-render` — DOT rendering API reference.
- :doc:`/explanation/graph-api-architecture` — how authored graphs compile
  into per-device plans.
