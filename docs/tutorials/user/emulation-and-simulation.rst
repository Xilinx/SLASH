..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

############################
Emulation and Simulation
############################

This tutorial walks through building and running a SLASH application in
emulation and simulation modes. Neither mode requires FPGA hardware — your
application runs against a software model of the kernels instead.

For a conceptual overview of the three platforms see
:doc:`/explanation/platform-modes`.

Prerequisites
=============

- The SLASH stack is installed (at minimum VRT and the CMake modules).
  See :doc:`/howto/build-from-source` if building from source.
- AMD Vivado **2025.1** and Vitis HLS **2025.1** are installed and sourced in
  your shell:

  .. code-block:: bash

     source <path-to-vivado>/settings64.sh
     source <path-to-vitis-hls>/settings64.sh

  For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. Using versions
  other than 2025.1 may cause breakage.

- For simulation: a Vivado-supported Verilog simulator licence.
- No V80 board is required.

When to Use Each Mode
=====================

.. list-table::
   :header-rows: 1
   :widths: 15 25 25 35

   * - Mode
     - Speed
     - Accuracy
     - Best for
   * - **Emulation**
     - Fast
     - Functional only
     - Rapid iteration on kernel logic
   * - **Simulation**
     - Slow
     - Cycle-accurate RTL
     - Catching timing and protocol bugs

Use emulation first to verify functional correctness, then simulation when
you need cycle-accurate behaviour.

Build an Emulation Vrtbin
=========================

Ensure you have sourced Vivado and Vitis HLS before building (see
`Prerequisites`_).

Using the ``00_axilite`` example:

.. code-block:: bash

   cd examples/00_axilite
   cmake -B build -S . -G Ninja
   cmake --build build                              # build the host application
   cmake --build build --target hls                 # compile HLS kernels (requires Vitis HLS)
   cmake --build build --target axilite_emu         # link into an emulation vrtbin

The ``axilite_emu`` target invokes the SLASH linker (``slashkit``) with
``PLATFORM "emu"``. The resulting ``.vbin`` file contains:

- ``system_map.xml`` with ``<Platform>Emulation</Platform>``
- ``vpp_emu`` — a compiled C-model executable of the HLS kernels
- ``emu_manifest.json`` — argument routing metadata

Run in Emulation
================

Run the same application binary, passing the emulation vrtbin:

.. code-block:: bash

   ./00_axilite 03:00 axilite_emu.vbin

The BDF argument (``03:00``) is still required for API compatibility but no
hardware is accessed. Under the hood VRT:

1. Extracts the vrtbin and reads ``system_map.xml``.
2. Detects ``Platform::EMULATION``.
3. Launches the ``vpp_emu`` process in a background thread.
4. Connects to the C-model via ZeroMQ on ``tcp://localhost:5555``.
5. Translates ``setArg()``, ``start()``, ``wait()``, and ``sync()`` calls
   into JSON commands sent over ZeroMQ.

The application output should match hardware results exactly.

Build and Run in Simulation
===========================

.. code-block:: bash

   cmake --build build --target axilite_sim  # link into a simulation vrtbin
   ./00_axilite 03:00 axilite_sim.vbin

The simulation vrtbin contains a ``vpp_sim`` executable (a Verilog
simulator wrapper) and a ``system_map.xml`` with
``<Platform>Simulation</Platform>``.

VRT launches the simulator and communicates via ZeroMQ in the same way as
emulation. Simulation is significantly slower — expect minutes rather than
seconds for even simple designs.

Querying the Platform at Runtime
================================

Your application can check the active platform and adjust behaviour:

.. code-block:: cpp

   vrt::Device device(bdf, vrtbinPath);

   if (device.getPlatform() == vrt::Platform::EMULATION) {
       std::cout << "Running in emulation mode" << std::endl;
   }

   if (device.getPlatform() == vrt::Platform::SIMULATION) {
       std::cout << "Running in simulation mode" << std::endl;
   }

See :doc:`/reference/vrt-api/enums` for all ``vrt::Platform`` values.

How Buffers Work in Emulation and Simulation
============================================

In emulation and simulation, ``vrt::Buffer<T>`` allocates host memory
instead of device memory. Fake physical addresses are assigned automatically
so that kernel argument routing works transparently:

- **Emulation** — ``sync(HOST_TO_DEVICE)`` sends the buffer contents over
  ZeroMQ to the C-model. ``sync(DEVICE_TO_HOST)`` fetches the results
  back.
- **Simulation** — the same pattern applies, using the simulator's memory
  model.

No changes to your buffer code are needed. The same ``sync()`` calls work
on all three platforms.

Known Limitations
=================

**Emulation:**

- Timing is not modelled — performance measurements are not meaningful.
- HLS kernels must include at least one AXI4-Lite interface.

**Simulation:**

- Significantly slower than emulation.
- Requires AMD Vivado and a simulator licence.
- Floating-point representation in the simulator can introduce NaN artefacts.
  Your application should handle these with relaxed comparison tolerances.

Next Steps
==========

- :doc:`/explanation/platform-modes` — conceptual overview of all three
  platforms.
- :doc:`/explanation/vrtbin-format` — what is inside a vrtbin archive.
- :doc:`your-first-kernel` — write and build your own HLS kernel.
