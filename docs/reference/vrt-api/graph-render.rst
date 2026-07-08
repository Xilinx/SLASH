..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

######################
Graph DOT Rendering
######################

The ``vrt::graph::render`` helpers export both authored graphs and compiled
per-device graphs as Graphviz DOT text. Use them to inspect inferred dataflow,
explicit ``after`` dependencies, nested control regions, and bridge-injected
device plans.

renderToDot(Graph)
==================

.. doxygenfunction:: vrt::graph::render::renderToDot(const Graph& graph)
   :project: VRT

renderToDot(DGraph)
===================

.. doxygenfunction:: vrt::graph::render::renderToDot(const DGraph& dgraph)
   :project: VRT

writeToDotFile(Graph)
=====================

.. doxygenfunction:: vrt::graph::render::writeToDotFile(const Graph& graph, const std::string& path)
   :project: VRT

writeToDotFile(DGraph)
======================

.. doxygenfunction:: vrt::graph::render::writeToDotFile(const DGraph& dgraph, const std::string& path)
   :project: VRT
