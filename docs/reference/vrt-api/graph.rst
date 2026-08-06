..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

################
vrt::graph API
################

The public graph API is a typed hardware-style authoring and execution
surface. Applications retain buffer and scalar tokens for execution I/O;
compiler identities and stage IR remain internal.

Authoring
=========

Graph
-----

.. doxygenclass:: vrt::graph::Graph
   :project: VRT
   :members:

RegionBuilder
-------------

.. doxygenclass:: vrt::graph::RegionBuilder
   :project: VRT
   :members:

GraphRegion
-----------

.. doxygentypedef:: vrt::graph::GraphRegion
   :project: VRT

CpuKernels
----------

.. doxygenclass:: vrt::graph::CpuKernels
   :project: VRT
   :members:

KernelHandle
------------

.. doxygenstruct:: vrt::graph::KernelHandle
   :project: VRT
   :members:

GraphNode
---------

.. doxygenstruct:: vrt::graph::GraphNode
   :project: VRT
   :members:

Call Specifications
-------------------

.. doxygenstruct:: vrt::graph::KernelCallSpec
   :project: VRT
   :members:

.. doxygenstruct:: vrt::graph::ReprogramCallSpec
   :project: VRT
   :members:

.. doxygenstruct:: vrt::graph::LoopBuildSpec
   :project: VRT
   :members:

.. doxygenstruct:: vrt::graph::ConditionalBuildSpec
   :project: VRT
   :members:

Port Arguments
--------------

.. doxygenstruct:: vrt::graph::BufferArg
   :project: VRT
   :members:

.. doxygenstruct:: vrt::graph::ScalarArg
   :project: VRT
   :members:

.. doxygenstruct:: vrt::graph::InoutArg
   :project: VRT
   :members:

Graph Tokens
============

.. doxygenclass:: vrt::graph::GraphBuffer
   :project: VRT
   :members:

.. doxygenclass:: vrt::graph::GraphScalar
   :project: VRT
   :members:

Execution
=========

.. doxygenclass:: vrt::graph::Execution
   :project: VRT
   :members:

Kernel ABI Metadata
===================

IOTypeMap
---------

.. doxygenstruct:: vrt::graph::IOTypeMap
   :project: VRT
   :members:

IOMap
-----

.. doxygentypedef:: vrt::graph::IOMap
   :project: VRT

ScalarPort
----------

.. doxygenstruct:: vrt::graph::ScalarPort
   :project: VRT
   :members:

BufferPort
----------

.. doxygenstruct:: vrt::graph::BufferPort
   :project: VRT
   :members:

RWBufferPort
------------

.. doxygenstruct:: vrt::graph::RWBufferPort
   :project: VRT
   :members:

FPGA Authoring
==============

FpgaImageSource
---------------

.. doxygenstruct:: vrt::graph::FpgaImageSource
   :project: VRT
   :members:

FpgaSpec
--------

.. doxygenstruct:: vrt::graph::FpgaSpec
   :project: VRT
   :members:

FpgaHandle
----------

.. doxygenclass:: vrt::graph::FpgaHandle
   :project: VRT
   :members:

FpgaImageHandle
---------------

.. doxygenclass:: vrt::graph::FpgaImageHandle
   :project: VRT
   :members:

FpgaKernelBuilder
-----------------

.. doxygenclass:: vrt::graph::FpgaKernelBuilder
   :project: VRT
   :members:

Control Predicates
==================

CompareOp
---------

.. doxygenenum:: vrt::graph::CompareOp
   :project: VRT

Condition
---------

.. doxygenclass:: vrt::graph::Condition
   :project: VRT
   :members:
