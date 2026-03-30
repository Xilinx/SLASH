..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

###############
Getting Started
###############

This tutorial walks through building and running your first SLASH application
using example ``00_axilite``.

Prerequisites
=============

Before you begin, ensure:

- The SLASH stack is installed (kernel module, libslash, vrtd, VRT, v80-smi).
  The recommended way is to install pre-built packages — see
  :doc:`/tutorials/admin/platform-setup`. To build from source instead, see
  :doc:`/howto/build-from-source`.
- A V80 board is installed and visible (run ``v80-smi list`` to check).
- The ``vrtd`` daemon is running (``sudo systemctl enable --now vrtd``).
- AMD Vivado **2025.1** and Vitis HLS **2025.1** are installed and sourced in
  your shell (for building FPGA artefacts). Source the environment before
  building:

  .. code-block:: bash

     source <path-to-vivado>/settings64.sh
     source <path-to-vitis-hls>/settings64.sh

  For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. Using versions
  other than 2025.1 may cause breakage.

What the Example Does
=====================

Example ``00_axilite`` demonstrates AXI-Lite control interfaces. It deploys two
HLS kernels onto a V80 board:

- **increment** — reads a buffer of floats from device memory, adds 1.0 to each
  element, and writes the result back.
- **accumulate** — sums all elements and returns the total via an AXI-Lite
  output register.

The host application generates random data, sends it to the device, runs both
kernels, and verifies the result against a golden model.

Build the Example
=================

.. code-block:: bash

   cd examples/00_axilite
   cmake -B build -S . -G Ninja
   cmake --build build

This builds the host application. To also build the FPGA artefacts (HLS kernels
and hardware vrtbin), ensure you have sourced Vivado and Vitis HLS first (see
`Prerequisites`_):

.. code-block:: bash

   cmake --build build --target hls          # compile HLS kernels
   cmake --build build --target axilite_hw   # link into a hardware vrtbin

For emulation (no FPGA required):

.. code-block:: bash

   cmake --build build --target axilite_emu  # link into an emulation vrtbin

Run the Example
===============

Identify your board's BDF address:

.. code-block:: bash

   v80-smi list

Run the application with the BDF and the vrtbin file:

.. code-block:: bash

   ./00_axilite <BDF> <path-to-vrtbin>

For example:

.. code-block:: bash

   ./00_axilite 03:00 axilite_hw.vbin

Expected output:

.. code-block:: text

   VRT Version: 0.1.0
   Generating data...
   Time taken for waits: <N> us
   Expected: <value>
   Got: <value>
   Absolute error: <small number> (effective tolerance ...)
   Test passed!

Understanding the Code
======================

The key VRT calls in ``00_axilite.cpp``:

**1. Open a device and load the vrtbin:**

.. code-block:: cpp

   vrt::Device device(bdf, vrtbinFile);

This connects to vrtd, opens the board at the given BDF, and programs the FPGA
with the design from the vrtbin file. The platform (hardware, emulation, or
simulation) is determined automatically from the vrtbin contents.

**2. Create kernel handles:**

.. code-block:: cpp

   vrt::Kernel accumulate(device, "accumulate_0");
   vrt::Kernel increment(device, "increment_0");

Each kernel is looked up by name from the loaded design.

**3. Allocate a device buffer:**

.. code-block:: cpp

   vrt::Buffer<float> buffer(device, size, increment.argMemoryConfig("in"));

This allocates ``size`` floats in device memory. The memory configuration
(DDR vs HBM, address range) is taken from the kernel argument named ``"in"``.

**4. Transfer data to the device:**

.. code-block:: cpp

   buffer.sync(vrt::SyncType::HOST_TO_DEVICE);

DMA transfers the host-side buffer contents to device memory.

**5. Set arguments and launch kernels:**

.. code-block:: cpp

   increment.setArg(0, size);
   increment.setArg(1, buffer);
   increment.start();

Arguments are written to the kernel's AXI-Lite registers. ``start()`` sets the
AP_START bit. ``wait()`` polls until the kernel signals completion.

**6. Read a result register:**

.. code-block:: cpp

   uint32_t val = accumulate.read(0x18);

Reads a 32-bit value from the kernel's AXI-Lite register space at offset
``0x18``.

**7. Clean up:**

.. code-block:: cpp

   device.cleanup();

Releases device resources.

Next Steps
==========

- :doc:`your-first-kernel` — write your own HLS kernel from scratch.
- :doc:`buffers-and-memory` — learn about DDR vs HBM and streaming buffers.
- :doc:`/explanation/architecture` — understand the full SLASH stack.
- :doc:`/explanation/platform-modes` — run the same code in emulation or
  simulation.
