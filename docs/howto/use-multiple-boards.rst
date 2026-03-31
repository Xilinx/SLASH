..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

######################
Use Multiple Boards
######################

This guide shows how to control multiple V80 boards from a single application
using separate ``vrt::Device`` instances.

Prerequisites
=============

- Two or more V80 boards installed and visible via ``v80-smi list``.
- The SLASH stack is installed and ``vrtd`` is running.
- A vrtbin file built for the target design.
- See :doc:`/tutorials/user/getting-started` for an introduction to the VRT
  workflow.

Identifying Your Boards
========================

Run ``v80-smi list`` to discover the BDF address of each board:

.. code-block:: bash

   v80-smi list

Each board is listed with a unique BDF (Bus:Device.Function). Note the BDF for
every board you intend to use.

Opening Multiple Devices
========================

Create a separate ``vrt::Device`` for each board. Both can load the same
vrtbin:

.. code-block:: cpp

   vrt::Device fpga0("e2:00.0", "my_design.vrtbin", false, vrt::ProgramType::FLASH);
   vrt::Device fpga1("21:00.0", "my_design.vrtbin", false, vrt::ProgramType::FLASH);

.. note::

   Replace the BDF strings with the addresses reported by ``v80-smi list`` on
   your system. The addresses above are examples.

Each ``Device`` manages its own connection to the ``vrtd`` daemon and its own
FPGA state.

Per-Device Kernels and Buffers
===============================

Kernels and buffers are scoped to the device that created them. Create
separate instances for each board:

.. code-block:: cpp

   // Kernels on fpga0
   vrt::Kernel increment0(fpga0, "increment_0");
   vrt::Kernel accumulate0(fpga0, "accumulate_0");

   // Same kernel names, but on fpga1
   vrt::Kernel increment1(fpga1, "increment_0");
   vrt::Kernel accumulate1(fpga1, "accumulate_0");

   // Buffers — one per device
   vrt::Buffer<float> buffer0(fpga0, size, increment0.argMemoryConfig("in"));
   vrt::Buffer<float> buffer1(fpga1, size, increment1.argMemoryConfig("in"));

You cannot pass a buffer allocated on one device to a kernel on another.

Running Kernels Independently
===============================

Each device operates independently. You can run kernels on different boards
sequentially or concurrently:

.. code-block:: cpp

   buffer0.sync(vrt::SyncType::HOST_TO_DEVICE);
   buffer1.sync(vrt::SyncType::HOST_TO_DEVICE);

   // Run on fpga0
   increment0.start(size, buffer0.getPhysAddr());
   accumulate0.start(size);
   increment0.wait();
   accumulate0.wait();

   // Run on fpga1
   increment1.start(size, buffer1.getPhysAddr());
   accumulate1.start(size);
   increment1.wait();
   accumulate1.wait();

To maximise throughput, start kernels on both boards before waiting:

.. code-block:: cpp

   increment0.start(size, buffer0.getPhysAddr());
   increment1.start(size, buffer1.getPhysAddr());
   // Both boards are now running in parallel
   increment0.wait();
   increment1.wait();

Each device can also have its own clock frequency:

.. code-block:: cpp

   fpga0.setFrequency(200000000);
   fpga1.setFrequency(200000000);

Cleanup
=======

Call ``cleanup()`` on each device when done:

.. code-block:: cpp

   fpga0.cleanup();
   fpga1.cleanup();

Complete Example
================

Example ``03_multiple_boards`` demonstrates this pattern using the
``increment`` and ``accumulate`` kernels from example ``00_axilite``.

.. code-block:: bash

   cd examples/03_multiple_boards
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build

.. note::

   This example reuses the vrtbin from example ``00_axilite``. Build that
   example first to produce the vrtbin file.

Next Steps
==========

- :doc:`/reference/vrt-api/device` — full Device API reference.
- :doc:`/tutorials/user/buffers-and-memory` — buffer management in depth.
- :doc:`set-clock-frequency` — frequency tuning per device.
