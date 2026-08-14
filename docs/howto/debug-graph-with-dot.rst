..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

########################
Debug Graphs with DOT
########################

VRT provides Graphviz projections for the resolved, placed, routed, and
scheduled compiler stages. These renderers are intended for compiler
development and characterization; the public ``Execution`` API deliberately
does not expose compiler intermediates.

Render a Compiler Stage
=======================

Include ``<vrt/graph/render/dot.hpp>`` and pass a stage object to
``vrt::graph::render::renderToDot``:

.. code-block:: cpp

   std::string dot =
       vrt::graph::render::renderToDot(scheduled);

Write the returned text with the application's preferred file API, then use
Graphviz:

.. code-block:: bash

   dot -Tpng scheduled.dot -o scheduled.png

Choosing a View
===============

Resolved
   Inspect typed values, provenance, topology, in-place pairs, boundary
   aliases, and control results.

Placed
   Inspect operation ownership, memory locations, and value replicas.

Routed
   Inspect transfer signatures, selected mechanisms, legs, and dependency
   edges.

Scheduled
   Inspect queue-local payloads, step dependencies, logical rendezvous, and
   split-control protocols.

Use the earliest stage that contains the unexpected state. For example, a
missing dependency is a resolution issue, while an unexpected transfer leg is
a routing issue.

Diagnose FPGA Image Safety
==========================

An FPGA call must be ordered after a reprogram of the image containing its
kernel. Retain the returned ``GraphNode`` and name it in the call's
``.after`` field:

.. code-block:: cpp

   auto loaded = graph.addReprogram({.image = image});
   graph.addKernelCall({
       .kernel = kernel,
       .inputs = {{"in", input}},
       .outputs = {{"out", output}},
       .after = {loaded},
   });

If a call is nested inside structured control, author its reprogram in the
same ``RegionBuilder``.
