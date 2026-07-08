..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

###################
Graph Devices
###################

Graph devices implement the execution side of ``vrt::graph``. A ``Graph`` is
authored once, compiled into per-device ``DGraph`` objects, then each registered
device lowers its subgraph into an executable plan.

Device Interfaces
=================

IDevice
-------

.. doxygenclass:: vrt::graph::IDevice
   :project: VRT
   :members:

IDevicePlan
-----------

.. doxygenclass:: vrt::graph::IDevicePlan
   :project: VRT
   :members:

DGraph
------

.. doxygenstruct:: vrt::graph::DGraph
   :project: VRT
   :members:

DGraphChild
-----------

.. doxygenstruct:: vrt::graph::DGraphChild
   :project: VRT
   :members:

DGraphChildRole
---------------

.. doxygenenum:: vrt::graph::DGraphChildRole
   :project: VRT

CPU Device
==========

CpuDevice
---------

.. doxygenclass:: vrt::graph::CpuDevice
   :project: VRT
   :members:

CpuKernel
---------

.. doxygenclass:: vrt::graph::CpuKernel
   :project: VRT
   :members:

CpuKernelArgs
-------------

.. doxygenclass:: vrt::graph::CpuKernelArgs
   :project: VRT
   :members:

CpuBufferView
-------------

.. doxygenstruct:: vrt::graph::CpuBufferView
   :project: VRT
   :members:

KernelSpan
----------

.. doxygenclass:: vrt::graph::KernelSpan
   :project: VRT
   :members:

ElementwiseCpuKernel
--------------------

.. doxygenclass:: vrt::graph::ElementwiseCpuKernel
   :project: VRT
   :members:

FPGA Device
===========

FpgaDevice
----------

.. doxygenclass:: vrt::graph::FpgaDevice
   :project: VRT
   :members:

FpgaKernelLocation
------------------

.. doxygenstruct:: vrt::graph::FpgaKernelLocation
   :project: VRT
   :members:

FpgaVbinSpec
------------

.. doxygenclass:: vrt::graph::fpga::FpgaVbinSpec
   :project: VRT
   :members:

FpgaImageSpec
-------------

.. doxygenstruct:: vrt::graph::fpga::FpgaImageSpec
   :project: VRT
   :members:

FpgaKernelSpec
--------------

.. doxygenstruct:: vrt::graph::fpga::FpgaKernelSpec
   :project: VRT
   :members:

FpgaKernelArgSpec
-----------------

.. doxygenstruct:: vrt::graph::fpga::FpgaKernelArgSpec
   :project: VRT
   :members:

Cross-Device Bridges
====================

IBridge
-------

.. doxygenclass:: vrt::graph::IBridge
   :project: VRT
   :members:

BridgeStepPair
--------------

.. doxygenstruct:: vrt::graph::BridgeStepPair
   :project: VRT

IBridgeOp
---------

.. doxygenclass:: vrt::graph::IBridgeOp
   :project: VRT
   :members:

CpuFpgaBridge
-------------

.. doxygenclass:: vrt::graph::CpuFpgaBridge
   :project: VRT
   :members:

BridgeRouter
------------

.. doxygenclass:: vrt::graph::BridgeRouter
   :project: VRT
   :members:

RoutedLeg
---------

.. doxygenstruct:: vrt::graph::RoutedLeg
   :project: VRT
   :members:

SemaphorePool
-------------

.. doxygenclass:: vrt::graph::SemaphorePool
   :project: VRT
   :members:

SemaphoreHandle
---------------

.. doxygenstruct:: vrt::graph::SemaphoreHandle
   :project: VRT
   :members:
