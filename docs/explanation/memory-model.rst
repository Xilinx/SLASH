..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##############
Memory Model
##############

The AMD Alveo V80 board has two distinct memory subsystems — DDR and HBM —
each with different capacity, bandwidth, and access characteristics. This
document explains how SLASH models these subsystems and how the runtime
allocator manages device memory.

DDR Memory
==========

The V80 has a single DDR address space, accessed through the QDMA subsystem
(PCIe Physical Function 1). DDR offers large capacity and is suitable for bulk
data storage where bandwidth is not the primary concern.

In VRT, DDR memory is selected with ``MemoryRangeType::DDR``:

.. code-block:: cpp

   vrt::Buffer<float> buffer(device, size, vrt::MemoryRangeType::DDR);

In the linker configuration, DDR is referenced as ``DDR0``:

.. code-block:: ini

   sp=offset_0.m_axi_gmem0:DDR0

HBM (High Bandwidth Memory)
============================

The V80 includes HBM organized as 64 pseudo-channels (HBM0–HBM63). Each
channel provides independent bandwidth, and the aggregate bandwidth across all
channels is substantially higher than DDR.

There are two access modes:

Port-Based Access
-----------------

``MemoryRangeType::HBM`` with an explicit port number allocates on a specific
HBM channel. The kernel port must be mapped to the same channel via the
``sp=`` directive in the linker configuration.

.. code-block:: cpp

   // Allocate on HBM channel 1
   vrt::Buffer<uint32_t> buffer(device, size, vrt::MemoryRangeType::HBM, 1);

.. code-block:: ini

   # Linker config must match
   sp=increment_0.m_axi_gmem0:HBM1

Internally, the port number maps to an ``HBMRegion`` enum value (``HBM0``
through ``HBM63``) and the allocation type is set to ``BufferAllocType::Hbm``.

.. note::

   Constructing a buffer with ``MemoryRangeType::HBM`` but *without* a port
   throws ``std::invalid_argument``. HBM always requires an explicit channel
   unless you use VNOC.

VNOC (Virtual NoC) Access
--------------------------

``MemoryRangeType::HBM_VNOC`` allocates across multiple HBM channels using the
on-chip Virtual Network-on-Chip, aggregating bandwidth without requiring the
application to manage individual channels.

.. code-block:: cpp

   vrt::Buffer<float> buffer(device, size, vrt::MemoryRangeType::HBM_VNOC);

The allocation type is set to ``BufferAllocType::HbmVnoc`` and no specific
``HBMRegion`` is selected.

MemoryConfig and Port Mapping
==============================

Rather than specifying memory types and ports manually, the recommended
approach is to use ``MemoryConfig`` — a struct that carries both the
``MemoryRangeType`` and an optional HBM port number:

.. code-block:: cpp

   struct MemoryConfig {
       MemoryRangeType type;
       std::optional<uint8_t> hbmPort;
   };

Obtain a ``MemoryConfig`` from the kernel:

.. code-block:: cpp

   // By port name
   vrt::MemoryConfig config = kernel.portMemoryConfig("m_axi_gmem0");

   // By argument name
   vrt::MemoryConfig config = kernel.argMemoryConfig("in");

These methods parse the ``system_map.xml`` inside the vrtbin to determine
which memory type and channel the kernel port is connected to. The returned
config can be passed directly to the ``Buffer<T>`` constructor:

.. code-block:: cpp

   vrt::Buffer<float> buffer(device, size, kernel.argMemoryConfig("in"));

This ensures the buffer allocation always matches the linker configuration.

Buddy Allocator
===============

On hardware, VRT uses a three-tier buddy-system allocator to manage device
memory efficiently. Each tier handles a different size range:

**SmallBlock** (4 KB – 2 MB)
   Managed by ``BuddySuperblockBase<12, 21>``. Allocations are carved from a
   2 MB superblock using power-of-two splitting.

**MediumBlock** (2 MB – 64 MB)
   Managed by ``BuddySuperblockBase<21, 26>``. Allocations are carved from a
   64 MB superblock.

**LargeBlock** (> 64 MB)
   Allocated directly from vrtd as a standalone DMA buffer, bypassing the
   buddy system.

When a buffer is allocated:

1. The size is rounded up to the nearest power of two.
2. The allocator searches for the smallest available block that fits.
3. If the available block is larger than needed, it is split in half
   repeatedly until the target size is reached. The unused halves are returned
   to the free list.

When a buffer is freed:

1. The allocator checks if the freed block's *buddy* (the other half from the
   original split) is also free.
2. If so, the two halves are coalesced back into a single larger block.
3. This continues up the hierarchy until no more buddies can be merged.

This approach minimises fragmentation while keeping allocation and deallocation
fast.

Platform Differences
====================

The memory model is designed to be transparent across all three SLASH
platforms, but the underlying mechanisms differ:

**Hardware**
   Real DMA allocations through the vrtd daemon, libslash, and the kernel
   driver. ``sync()`` triggers QDMA transfers between host and device memory.
   The buddy allocator manages physical address space.

**Emulation**
   Fake physical addresses are assigned starting at ``0x4000000000`` (HBM) and
   ``0x60000000000`` (DDR). Buffer data is exchanged with the C-model via
   ZeroMQ IPC. No real DMA occurs.

**Simulation**
   Same fake address scheme as emulation. Buffer data is exchanged with the
   Verilog simulation via ZeroMQ. The address windows match the simulation
   memory map configured in the linker's ``run_pre.tcl``.

In all cases, the ``Buffer<T>`` API (construction, ``sync()``, ``operator[]``)
is identical. Application code does not need to change when switching
platforms.
