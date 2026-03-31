..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##################
VRT Enumerations
##################

This page documents the public enumerations and supporting types in the VRT
API.

Platform
========

Defined in ``vrt/utils/platform.hpp``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Value
     - Description
   * - ``Platform::HARDWARE``
     - Physical FPGA device via PCIe.
   * - ``Platform::EMULATION``
     - Software C-model via ZeroMQ IPC.
   * - ``Platform::SIMULATION``
     - Cycle-accurate Verilog simulation.
   * - ``Platform::UNKNOWN``
     - Unspecified or invalid platform.

.. code-block:: cpp

   if (device.getPlatform() == vrt::Platform::EMULATION) {
       // emulation-specific logic
   }

See :doc:`/explanation/platform-modes` for a full description of each mode.

SyncType
========

Defined in ``vrt/buffer.hpp``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Value
     - Description
   * - ``SyncType::HOST_TO_DEVICE``
     - DMA write — transfer data from host memory to device memory (H2C).
   * - ``SyncType::DEVICE_TO_HOST``
     - DMA read — transfer data from device memory to host memory (C2H).

.. code-block:: cpp

   buf.sync(vrt::SyncType::HOST_TO_DEVICE);   // write to device
   kernel.start();
   kernel.wait();
   buf.sync(vrt::SyncType::DEVICE_TO_HOST);   // read results back

MemoryRangeType
===============

Defined in ``vrt/allocator/allocator.hpp``.

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Value
     - Description
   * - ``MemoryRangeType::HBM``
     - High Bandwidth Memory. Requires an explicit port number (0–63) in the
       ``Buffer`` constructor or a ``MemoryConfig`` with ``hbmPort`` set.
   * - ``MemoryRangeType::DDR``
     - DDR system memory. Single address space, no port required.
   * - ``MemoryRangeType::HBM_VNOC``
     - HBM via the Virtual Network-on-Chip. The allocator automatically
       places the buffer across HBM channels — no explicit port required.

.. code-block:: cpp

   // DDR buffer — no port needed
   vrt::Buffer<float> ddr(device, 1024, vrt::MemoryRangeType::DDR);

   // HBM buffer — explicit port required
   vrt::Buffer<float> hbm(device, 1024, vrt::MemoryRangeType::HBM, 3);

   // HBM VNOC — auto-placed, no port
   vrt::Buffer<float> vnoc(device, 1024, vrt::MemoryRangeType::HBM_VNOC);

See :doc:`/explanation/memory-model` for details on memory types and
allocation strategies.

HBMRegion
=========

Defined in ``vrt/allocator/allocator.hpp``.

Underlying type: ``uint64_t``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Value
     - Description
   * - ``HBM0`` – ``HBM63``
     - Individual HBM pseudo-channels. ``HBM0 = 0``, ``HBM1 = 1``, …,
       ``HBM63 = 63``.
   * - ``NON_HBM``
     - Sentinel value (``UINT64_MAX``) indicating no specific HBM region.

Users typically do not reference ``HBMRegion`` directly. Pass a port number
to the ``Buffer`` constructor instead.

ProgramType
===========

Defined in ``vrt/device.hpp``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Value
     - Description
   * - ``ProgramType::FLASH``
     - Program the device via flash memory (default).
   * - ``ProgramType::JTAG``
     - Program the device via JTAG interface.

.. code-block:: cpp

   vrt::Device device(bdf, vrtbinPath, true, vrt::ProgramType::FLASH);

StreamDirection
===============

Defined in ``vrt/qdma/qdma_connection.hpp``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Value
     - Description
   * - ``StreamDirection::HOST_TO_DEVICE``
     - Streaming data flow from host to device (H2C).
   * - ``StreamDirection::DEVICE_TO_HOST``
     - Streaming data flow from device to host (C2H).

Used internally by ``QdmaConnection`` and ``StreamingBuffer``. Most users
interact with streaming via ``vrt::StreamingBuffer<T>`` rather than
referencing ``StreamDirection`` directly.

MemoryConfig
============

Defined in ``vrt/allocator/allocator.hpp``.

A plain struct describing the memory type and optional HBM port for a
buffer. Obtain it from kernel metadata rather than constructing it manually:

.. code-block:: cpp

   vrt::Kernel kernel(device, "my_kernel");
   vrt::MemoryConfig config = kernel.portMemoryConfig("m_axi_gmem0");
   vrt::Buffer<float> buf(device, 1024, config);

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Field
     - Description
   * - ``MemoryRangeType type``
     - ``DDR``, ``HBM``, or ``HBM_VNOC``.
   * - ``std::optional<uint8_t> hbmPort``
     - Set only when ``type == HBM``. Contains the HBM port number (0–63).
