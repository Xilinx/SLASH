..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

################
vrtbin Format
################

A vrtbin (``.vbin``) file is the deployment artefact in SLASH. It packages
everything needed to program and interact with a V80 design into a single
archive.

Overview
========

A vrtbin is a **gzip-compressed tar archive** produced by the SLASH linker
(``slashkit``) via the ``add_vbin()`` CMake function. VRT extracts the archive
at runtime when you construct a ``vrt::Device``.

.. code-block:: text

   ┌───────────────────────────────┐
   │          .vbin file           │  gzip-compressed tar
   ├───────────────────────────────┤
   │  system_map.xml              │  always present — design metadata
   │  *.pdi                       │  hardware only — FPGA bitstream(s)
   │  vpp_emu                     │  emulation only — C-model executable
   │  emu_manifest.json           │  emulation only — argument routing
   │  vpp_sim                     │  simulation only — simulator launcher
   │  report_utilization.xml      │  optional — FPGA resource usage
   └───────────────────────────────┘

Archive Contents
================

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - File
     - Present
     - Purpose
   * - ``system_map.xml``
     - Always
     - Design metadata: platform, clock frequency, kernel descriptions,
       register maps, memory connections, and streaming connections.
   * - ``*.pdi``
     - Hardware
     - FPGA bitstream file(s). If multiple PDI files exist, VRT prefers
       ``design.pdi``.
   * - ``vpp_emu``
     - Emulation
     - Compiled C-model executable of the HLS kernels.
   * - ``emu_manifest.json``
     - Emulation
     - Maps kernel arguments to emulation call types (scalar, buffer) and
       routing for register read-back.
   * - ``vpp_sim``
     - Simulation
     - Verilog simulator wrapper executable.
   * - ``report_utilization.xml``
     - Optional
     - FPGA resource utilisation report (LUTs, FFs, BRAMs, URAMs, DSPs).

system_map.xml
==============

The ``system_map.xml`` file is the most important entry in the archive. It
drives VRT's runtime behaviour — kernel discovery, argument routing, memory
port mapping, and platform detection all come from this file.

Schema
------

.. code-block:: xml

   <SystemMap>
     <Platform>Hardware</Platform>
     <ClockFrequency>250000000</ClockFrequency>

     <ServiceLayer>
       <Ethernet enabled="true">
         <eth index="0" />
       </Ethernet>
       <VIRT>
         <interface index="0" connection="eth0" />
       </VIRT>
     </ServiceLayer>

     <Kernel>
       <Name>increment_0</Name>
       <BaseAddress>0x20100000000</BaseAddress>
       <Range>0x1000</Range>

       <register offset="0x00" name="CTRL" access="RW"
                 description="Control register" range="32" />
       <register offset="0x10" name="size" access="W"
                 description="Number of elements" range="32" />

       <functional_args>
         <arg idx="0" name="size" type="scalar"
              offset="0x10" range="32" r="0" w="1" />
         <arg idx="1" name="in" type="buffer"
              offset="0x18" range="64" r="1" w="1"
              port="m_axi_gmem0" />
       </functional_args>

       <connection port="m_axi_gmem0" target="HBM1" />
     </Kernel>
   </SystemMap>

Key elements:

``<Platform>``
   One of ``Hardware``, ``Emulation``, or ``Simulation``. VRT maps this to
   the ``vrt::Platform`` enum and selects the appropriate back-end.

``<ClockFrequency>``
   Kernel clock frequency in Hz (e.g. ``250000000`` for 250 MHz).

``<Kernel>``
   One block per kernel instance. Contains the instance name, base address,
   register definitions, functional argument metadata, and memory port
   connections.

``<functional_args>``
   Each ``<arg>`` describes a kernel argument: index, name, type
   (``scalar`` or ``buffer``), register offset, range in bits, read/write
   flags, and the associated AXI port name (for buffer arguments).

``<connection>``
   Maps an AXI memory-mapped port to a physical memory target (e.g.
   ``HBM1``, ``DDR0``). VRT uses this to determine the correct
   ``MemoryConfig`` for buffer allocation.

How VRT Uses the Vrtbin
=======================

When you construct ``vrt::Device(bdf, vrtbinPath)``:

1. **Extract** — ``Vrtbin::extract()`` decompresses the gzip archive into a
   temporary cache directory.

2. **Discover** — VRT locates ``system_map.xml``, PDI files, and
   emulation/simulation executables within the extracted tree.

3. **Parse** — the XML parser reads ``system_map.xml`` to build kernel
   objects with register maps, argument metadata, and memory configurations.

4. **Select platform** — the ``<Platform>`` value determines whether VRT
   uses PCIe BAR access (hardware), ZeroMQ to ``vpp_emu`` (emulation), or
   ZeroMQ to ``vpp_sim`` (simulation).

5. **Program** — on hardware, VRT programs the FPGA with the PDI
   bitstream(s) via the vrtd daemon. On emulation/simulation, it launches
   the model executable in a background thread.

Inspecting a Vrtbin
===================

Use ``v80-smi inspect`` to display a vrtbin's metadata without programming
a device:

.. code-block:: bash

   v80-smi inspect my_design.vbin

This prints the platform, clock frequency, kernel names, argument lists,
and memory connections parsed from ``system_map.xml``.

You can also examine the raw archive contents:

.. code-block:: bash

   tar tzf my_design.vbin

See :doc:`/reference/smi/commands` for the full ``inspect`` command
reference.

Creating a Vrtbin
=================

Vrtbin archives are produced by the SLASH linker (``slashkit``) through the
CMake ``add_vbin()`` function:

.. code-block:: cmake

   add_vbin(TARGET "my_design_hw" PLATFORM "hw" CFG "config.cfg" KERNELS ${_KERNELS})

The linker reads compiled HLS kernel IP (``component.xml`` files) and a
connectivity configuration (``config.cfg``) to produce the archive. One
target is created per platform (``hw``, ``emu``, ``sim``).

See :doc:`/reference/cmake/slashtools` for the full ``add_vbin()``
reference and :doc:`/tutorials/user/your-first-kernel` for a worked
example.
