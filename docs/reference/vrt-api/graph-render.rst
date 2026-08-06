..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

######################
Graph DOT Rendering
######################

The ``vrt::graph::render`` helpers project compiler stage IR as Graphviz DOT
text. They are compiler-development tools; opaque public executions do not
expose these stages.

Resolved Graph
==============

.. doxygenfunction:: vrt::graph::render::renderToDot(const ResolvedGraph& graph)
   :project: VRT

Placed Graph
============

.. doxygenfunction:: vrt::graph::render::renderToDot(const PlacedGraph& graph)
   :project: VRT

Routed Graph
============

.. doxygenfunction:: vrt::graph::render::renderToDot(const RoutedGraph& graph)
   :project: VRT

Scheduled Graph
===============

.. doxygenfunction:: vrt::graph::render::renderToDot(const ScheduledGraph& graph)
   :project: VRT
