..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

################
vrt::graph API
################

The ``vrt::graph`` API builds a structured dataflow graph from typed buffer
and scalar tokens, compiles it into per-device execution plans, and runs the
compiled snapshot repeatedly with different input values.

Graph Construction
==================

Graph
-----

.. doxygenclass:: vrt::graph::Graph
   :project: VRT
   :members:

CpuKernels
----------

.. doxygenclass:: vrt::graph::CpuKernels
   :project: VRT
   :members:

RegionBuilder
-------------

.. doxygenclass:: vrt::graph::RegionBuilder
   :project: VRT
   :members:

GraphRegion
-----------

.. doxygenclass:: vrt::graph::GraphRegion
   :project: VRT
   :members:

Execution
=========

CompiledGraph
-------------

.. doxygenclass:: vrt::graph::CompiledGraph
   :project: VRT
   :members:

Tokens and Types
================

GraphBuffer
-----------

.. doxygenclass:: vrt::graph::GraphBuffer
   :project: VRT
   :members:

GraphScalar
-----------

.. doxygenclass:: vrt::graph::GraphScalar
   :project: VRT
   :members:

ScalarType
----------

.. doxygenenum:: vrt::graph::ScalarType
   :project: VRT

BufferType
----------

.. doxygenenum:: vrt::graph::BufferType
   :project: VRT

DeviceType
----------

.. doxygenenum:: vrt::graph::DeviceType
   :project: VRT

Kernel Signatures and Bindings
==============================

KernelDescriptor
----------------

.. doxygenstruct:: vrt::graph::KernelDescriptor
   :project: VRT
   :members:

IOTypeMap
---------

.. doxygenstruct:: vrt::graph::IOTypeMap
   :project: VRT
   :members:

ScalarPort
~~~~~~~~~~

.. doxygenstruct:: vrt::graph::ScalarPort
   :project: VRT
   :members:

BufferPort
~~~~~~~~~~

.. doxygenstruct:: vrt::graph::BufferPort
   :project: VRT
   :members:

RWBufferPort
~~~~~~~~~~~~

.. doxygenstruct:: vrt::graph::RWBufferPort
   :project: VRT
   :members:

IOMap
-----

.. doxygenclass:: vrt::graph::IOMap
   :project: VRT
   :members:

Struct-Literal Authoring
========================

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

ImageRef
--------

.. doxygenstruct:: vrt::graph::ImageRef
   :project: VRT
   :members:

ReprogramCallSpec
-----------------

.. doxygenstruct:: vrt::graph::ReprogramCallSpec
   :project: VRT
   :members:

BufferArg
---------

.. doxygenstruct:: vrt::graph::BufferArg
   :project: VRT
   :members:

InoutArg
--------

.. doxygenstruct:: vrt::graph::InoutArg
   :project: VRT
   :members:

ScalarArg
---------

.. doxygenstruct:: vrt::graph::ScalarArg
   :project: VRT
   :members:

TripCount
---------

.. doxygenstruct:: vrt::graph::TripCount
   :project: VRT
   :members:

KernelCallSpec
--------------

.. doxygenstruct:: vrt::graph::KernelCallSpec
   :project: VRT
   :members:

LoopBuildSpec
-------------

.. doxygenstruct:: vrt::graph::LoopBuildSpec
   :project: VRT
   :members:

ConditionalBuildSpec
--------------------

.. doxygenstruct:: vrt::graph::ConditionalBuildSpec
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

FpgaKernelBuilder
-----------------

.. doxygenclass:: vrt::graph::FpgaKernelBuilder
   :project: VRT
   :members:

FpgaImageHandle
---------------

.. doxygenclass:: vrt::graph::FpgaImageHandle
   :project: VRT
   :members:

FpgaHandle
----------

.. doxygenclass:: vrt::graph::FpgaHandle
   :project: VRT
   :members:

Control Flow
============

CompareOp
---------

.. doxygenenum:: vrt::graph::CompareOp
   :project: VRT

ConditionOperand
----------------

.. doxygenclass:: vrt::graph::ConditionOperand
   :project: VRT
   :members:

Condition
---------

.. doxygenclass:: vrt::graph::Condition
   :project: VRT
   :members:

LoopTripCount
-------------

.. doxygenclass:: vrt::graph::LoopTripCount
   :project: VRT
   :members:

BoundarySide
------------

.. doxygenenum:: vrt::graph::BoundarySide
   :project: VRT

ScalarBoundaryMapping
---------------------

.. doxygenstruct:: vrt::graph::ScalarBoundaryMapping
   :project: VRT
   :members:

BufferBoundaryMapping
---------------------

.. doxygenstruct:: vrt::graph::BufferBoundaryMapping
   :project: VRT
   :members:

BoundaryMappings
----------------

.. doxygenstruct:: vrt::graph::BoundaryMappings
   :project: VRT
   :members:

LoopKind
--------

.. doxygenenum:: vrt::graph::LoopKind
   :project: VRT

ControlOutputPlacementHints
---------------------------

.. doxygenstruct:: vrt::graph::ControlOutputPlacementHints
   :project: VRT
   :members:

LoopSpec
--------

.. doxygenstruct:: vrt::graph::LoopSpec
   :project: VRT
   :members:

LoopOp
------

.. doxygenstruct:: vrt::graph::LoopOp
   :project: VRT
   :members:

ConditionalSpec
---------------

.. doxygenstruct:: vrt::graph::ConditionalSpec
   :project: VRT
   :members:

ConditionalOp
-------------

.. doxygenstruct:: vrt::graph::ConditionalOp
   :project: VRT
   :members:

KernelOp
--------

.. doxygenstruct:: vrt::graph::KernelOp
   :project: VRT
   :members:

ReprogramSpec
-------------

.. doxygenstruct:: vrt::graph::ReprogramSpec
   :project: VRT
   :members:

ReprogramOp
-----------

.. doxygenstruct:: vrt::graph::ReprogramOp
   :project: VRT
   :members:

SubgraphBoundaryOp
------------------

.. doxygenstruct:: vrt::graph::SubgraphBoundaryOp
   :project: VRT
   :members:
