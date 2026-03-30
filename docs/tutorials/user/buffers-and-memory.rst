..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

####################
Buffers and Memory
####################

This tutorial explains how to allocate device memory, transfer data between
host and device, and choose the right memory type for your workload.

Memory Types on the V80
========================

The AMD Alveo V80 board has two distinct memory subsystems:

**DDR**
   A single, large-capacity address space. Suitable for bulk data that does not
   require high bandwidth. Selected with ``MemoryRangeType::DDR``.

**HBM (High Bandwidth Memory)**
   64 pseudo-channels (HBM0–HBM63) offering very high aggregate bandwidth.
   Each channel is accessed independently. There are two ways to use HBM:

   - **Port-based** — ``MemoryRangeType::HBM`` with an explicit port number.
     The buffer is allocated on a specific HBM channel and the kernel accesses
     that channel directly. This gives the full bandwidth of the channel, but
     restricts access to that HBM region only — the kernel cannot reach other
     HBM channels through this port. The kernel's ``sp=`` directive in the
     linker configuration must map the port to the same channel.
   - **VNOC (Virtual NoC)** — ``MemoryRangeType::HBM_VNOC``. The buffer is
     allocated across multiple HBM channels and accessed through the on-chip
     VNOC interconnect. This allows the kernel to reach the entire HBM memory
     space regardless of which channel holds the data, but is bottlenecked by
     the lower bandwidth of the VNOC compared to a direct HBM port connection.

The linker configuration determines which memory each kernel port is connected
to. For example, ``sp=increment_0.m_axi_gmem0:HBM1`` maps the
``m_axi_gmem0`` port of ``increment_0`` to HBM channel 1.

Creating Buffers
================

``Buffer<T>`` is a typed, host-accessible buffer backed by device memory. There
are three ways to construct one.

From a MemoryConfig (recommended)
----------------------------------

.. code-block:: cpp

   // By kernel argument name (recommended)
   vrt::Buffer<float> buffer(device, size, increment.argMemoryConfig("in"));

   // By AXI port name
   vrt::Buffer<float> buffer(device, size, increment.portMemoryConfig("m_axi_gmem0"));

Both methods read the vrtbin metadata and return a ``MemoryConfig`` struct with
the correct memory type and HBM port (if applicable), ensuring the buffer
automatically matches the kernel's linker configuration.

``Kernel::argMemoryConfig()`` is recommended because argument names are part of
the kernel's public interface.
``Kernel::portMemoryConfig()`` requires knowing the internal AXI port name (e.g.
``m_axi_gmem0``), which is an implementation detail of the HLS pragma.

With explicit HBM port
-----------------------

.. code-block:: cpp

   vrt::Buffer<uint32_t> buffer(device, size, vrt::MemoryRangeType::HBM, 1);

Allocates on HBM channel 1 specifically. Use this when you need to control
placement directly. The port number must match the ``sp=`` mapping in the linker
configuration.

With DDR
--------

.. code-block:: cpp

   vrt::Buffer<uint32_t> buffer(device, size, vrt::MemoryRangeType::DDR);

Allocates in the DDR address space.

.. note::

   Constructing a buffer with ``MemoryRangeType::HBM`` but *without* a port
   number throws ``std::invalid_argument``. If you want aggregated HBM
   bandwidth without specifying a channel, use ``MemoryRangeType::HBM_VNOC``
   instead.

Host-Device Data Transfer
=========================

Data moves between host and device memory with ``sync()``:

.. code-block:: cpp

   // Fill the buffer on the host side
   for (uint32_t i = 0; i < size; i++) {
       buffer[i] = static_cast<float>(i);
   }

   // Transfer host -> device
   buffer.sync(vrt::SyncType::HOST_TO_DEVICE);

   // ... run kernels ...

   // Transfer device -> host
   buffer.sync(vrt::SyncType::DEVICE_TO_HOST);

   // Read results
   float result = buffer[0];

On hardware, ``sync()`` triggers a DMA transfer through the QDMA subsystem. In
emulation and simulation, buffer data is exchanged over ZeroMQ — the same API
works transparently on all platforms.

Accessing Buffer Data
=====================

``Buffer<T>`` provides array-style access on the host side:

.. code-block:: cpp

   buffer[i] = 42.0f;          // write element i
   float val = buffer[i];      // read element i
   float* raw = buffer.get();  // raw pointer to the host buffer

``operator[]`` performs bounds checking and throws ``std::out_of_range`` if the
index exceeds the buffer size.

The host-side array is only a local copy. Changes are not visible on the device
until you call ``sync(HOST_TO_DEVICE)``, and device-side results are not visible
on the host until you call ``sync(DEVICE_TO_HOST)``.

Complete Example
================

Putting it all together — the typical buffer workflow:

.. code-block:: cpp

   #include <vrt/device.hpp>
   #include <vrt/kernel.hpp>
   #include <vrt/buffer.hpp>

   vrt::Device device(bdf, vrtbinFile);
   vrt::Kernel increment(device, "increment_0");

   // Allocate using the kernel's port configuration
   uint32_t size = 1024;
   vrt::Buffer<float> buffer(device, size, increment.argMemoryConfig("in"));

   // Fill data
   for (uint32_t i = 0; i < size; i++) {
       buffer[i] = static_cast<float>(i);
   }

   // Transfer to device
   buffer.sync(vrt::SyncType::HOST_TO_DEVICE);

   // Launch kernel
   increment.setArg(0, size);
   increment.setArg(1, buffer);
   increment.start();
   increment.wait();

   // Transfer results back
   buffer.sync(vrt::SyncType::DEVICE_TO_HOST);

   // Read results
   for (uint32_t i = 0; i < size; i++) {
       std::cout << buffer[i] << std::endl;
   }

   device.cleanup();

Common Patterns
===============

**Buffer is move-only.**
   ``Buffer<T>`` cannot be copied — only moved. This prevents accidental
   double-free of device memory.

   .. code-block:: cpp

      vrt::Buffer<float> a(device, size, vrt::MemoryRangeType::DDR);
      vrt::Buffer<float> b = std::move(a);  // OK
      // vrt::Buffer<float> c = b;          // compile error

**Call cleanup() after you are done with buffers.**
   ``device.cleanup()`` releases all device resources. Buffers should not be
   used after cleanup.

**HBM port mismatches throw at construction.**
   If you create a buffer on HBM channel 3 but the kernel port is mapped to
   channel 1 in the linker configuration, the transfer will target the wrong
   memory region. Use ``portMemoryConfig()`` to avoid this.

Next Steps
==========

- :doc:`/explanation/memory-model` — deeper look at the DDR/HBM memory
  subsystems and the buddy allocator.
- :doc:`your-first-kernel` — write your own HLS kernel from scratch.
- :doc:`/explanation/architecture` — understand the full SLASH stack.
