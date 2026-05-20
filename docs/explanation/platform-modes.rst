..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#################
Platform Modes
#################

VRT supports three execution platforms. The same application source code runs
on all three — the platform is determined by the vrtbin file, not by the
application.

.. list-table::
   :header-rows: 1
   :widths: 15 30 25 30

   * - Platform
     - Transport
     - Build target
     - Use case
   * - **Hardware**
     - PCIe BAR + QDMA
     - ``hw``
     - Production runs on a physical V80 board
   * - **Emulation**
     - ZeroMQ IPC to C-model
     - ``emu``
     - Functional verification without FPGA hardware
   * - **Simulation**
     - Verilog register map
     - ``sim``
     - Cycle-accurate RTL simulation

Platform Selection
==================

The platform is encoded inside the vrtbin archive. When VRT extracts the
archive, it reads the ``<Platform>`` element from ``system_map.xml``:

- ``"Hardware"`` → ``vrt::Platform::HARDWARE``
- ``"Emulation"`` → ``vrt::Platform::EMULATION``
- ``"Simulation"`` → ``vrt::Platform::SIMULATION``

Your application can query the active platform at runtime:

.. code-block:: cpp

   if (device.getPlatform() == vrt::Platform::EMULATION) {
       // emulation-specific logic
   }

Building for Each Platform
==========================

Each example provides three vrtbin targets via CMake:

.. code-block:: cmake

   add_vbin(TARGET "axilite_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "axilite_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "axilite_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

The application executable is the same regardless of platform — only the vrtbin
file passed at runtime differs.

Hardware
========

On hardware, VRT communicates with the V80 board through the full SLASH stack:

- **Register access** — BAR MMIO via PF2 (kernel arguments, control, status).
- **Data transfer** — QDMA via PF1 (buffer sync between host and device memory).
- **Device management** — vrtd daemon (programming, reset, multi-tenancy).

This is the only platform that exercises the physical FPGA. It requires:

- A V80 board installed in the host.
- The ``slash`` kernel module loaded.
- The ``vrtd`` daemon running.

Emulation
=========

Emulation replaces the FPGA with a software C-model of the HLS kernels. VRT
communicates with the emulated kernels over ZeroMQ IPC sockets.

Advantages:

- No FPGA hardware required.
- Fast iteration — recompile the C-model instead of running synthesis.
- Full functional verification of kernel logic.

Limitations:

- Timing is not modelled — performance measurements are not meaningful.
- HLS kernels must include at least one AXI4-Lite interface.
- Freerunning streaming kernel chains (e.g. example ``02_chain``) are not
  supported in emulation.

Simulation
==========

Simulation runs the kernel RTL in a Verilog simulator. VRT accesses the
simulated design through a register-map interface.

Advantages:

- Cycle-accurate behaviour.
- Can catch timing and protocol issues that emulation misses.

Limitations:

- Significantly slower than emulation.
- Requires AMD Vivado and a simulator licence.
- Memory roundtrip fidelity may differ from hardware (floating-point
  representation in the simulator can introduce NaN artefacts that the
  application must handle).
