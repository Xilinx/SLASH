..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

######################
Using the Graph API
######################

The graph API authors a typed hardware-style netlist and compiles it into an
``Execution``. Applications declare ``GraphBuffer`` and ``GraphScalar`` tokens,
then connect them to named kernel ports with struct literals or ``IOMap``.

This resembles Verilog intentionally: tokens are wires, kernel calls are
instances, and named bindings connect the two. Compilation validates the whole
region before resolving dependencies. A consumer may therefore be written
before its producer; textual authoring order is not execution order.

Author a CPU Pipeline
=====================

Register CPU kernels with ``Graph::cpu()``, declare typed tokens, and bind
them to the kernel's named ports:

.. code-block:: cpp

   vrt::graph::Graph graph = vrt::graph::Graph::withDefaults();
   auto add_one = graph.cpu().elementwise<std::int32_t>(
       "add_one", [](std::int32_t value) { return value + 1; });

   auto size = graph.scalarInput<std::uint64_t>("size");
   auto input = graph.input<std::int32_t>("input", size);
   auto output = graph.output<std::int32_t>("output", size);
   graph.addKernelCall({
       .kernel = add_one,
       .inputs = {{"in", input}},
       .outputs = {{"out", output}},
   });

Compile and Run
===============

``compile()`` validates and lowers the graph, returning an execution object.
Write and read through the tokens retained during authoring:

.. code-block:: cpp

   auto execution = graph.compile();
   std::vector<std::int32_t> values{1, 2, 3, 4};
   execution.writeScalar(
       size, static_cast<std::uint64_t>(values.size()));
   execution.write(input, values);
   execution.run();

   std::vector<std::int32_t> result(values.size());
   execution.read(output, result);

Structured Control
==================

``addLoop`` and ``addConditional`` create nested ``RegionBuilder`` scopes.
Values cross their boundaries through named input and output ports:

.. code-block:: cpp

   auto iterations =
       graph.scalarInput<std::uint32_t>("iterations");
   auto repeated =
       graph.buffer<std::int32_t>("repeated", size);
   auto body = graph.addLoop({
       .count = iterations,
       .inputs = {{"state", output}},
       .outputs = {{"state", repeated}},
   });
   body.addKernelCall({
       .kernel = add_one,
       .inputs = {{"in", body.input("state")}},
       .outputs = {{"out", body.output("state")}},
   });

Predicates are built from ``GraphScalar`` tokens. Both conditional branches
must produce every declared output port.

FPGA Images
===========

``Graph::addFpga`` returns an owning ``FpgaHandle``. Look up an image, create
typed kernel handles from it, and order FPGA calls after the corresponding
reprogram node:

.. code-block:: cpp

   auto fpga = graph.addFpga(spec);
   auto image = fpga.image("image_a");
   vrt::graph::KernelHandle kernel =
       image.kernel("process").in<std::int32_t>("in")
                              .out<std::int32_t>("out");
   auto loaded = graph.addReprogram({.image = image});
   graph.addKernelCall({
       .kernel = kernel,
       .inputs = {{"in", input}},
       .outputs = {{"out", output}},
       .after = {loaded},
   });

For lower-level authoring, ``Graph::rootRegion()``, ``GraphRegion``, and
``IOMap`` expose the same named connections directly. Both public surfaces
snapshot into the same compiler IR and have identical execution semantics.
