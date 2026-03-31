..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

######################
Set Clock Frequency
######################

This guide explains how to configure the kernel clock frequency at build time
and adjust it at runtime using the VRT Device API.

Prerequisites
=============

- The SLASH stack is installed.
- A V80 board is visible (``v80-smi list``) and ``vrtd`` is running.
- Familiarity with building a SLASH application.
  See :doc:`/tutorials/user/your-first-kernel`.

Where Clock Frequency Is Set
=============================

Clock frequency can be specified at three levels. Each level overrides the
previous one at its respective stage.

Build-Time: HLS Configuration
------------------------------

The HLS ``.cfg`` file sets the synthesis target clock period:

.. code-block:: ini

   clock=2ns

A period of ``2ns`` targets 500 MHz; ``4ns`` targets 250 MHz. This value
drives the timing constraints during HLS compilation.

Build-Time: Linker Configuration
---------------------------------

The ``config.cfg`` ``[clock]`` section sets the frequency recorded in the
vrtbin's ``system_map.xml``:

.. code-block:: ini

   [clock]
   krnl=vadd_0
   freqhz=400000000

This value (in Hz) is what the design reports as its clock frequency when
inspected with ``v80-smi inspect``.

Runtime: VRT Device API
------------------------

After opening a device, you can read and change the frequency from your host
application:

.. code-block:: cpp

   std::cout << "Current frequency: " << device.getFrequency() << " Hz\n";
   std::cout << "Max frequency:     " << device.getMaxFrequency() << " Hz\n";

   device.setFrequency(300000000);  // 300 MHz

   std::cout << "New frequency:     " << device.getFrequency() << " Hz\n";

Build and Run the Example
==========================

Ensure you have sourced Vivado and Vitis HLS before building:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

Example ``04_freq`` demonstrates all three levels:

.. code-block:: bash

   cd examples/04_freq
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build
   cmake --build build --target hls
   cmake --build build --target freq_hw    # or freq_emu / freq_sim

Run:

.. code-block:: bash

   ./04_freq <BDF> freq_hw.vbin

Replace ``<BDF>`` with your board's address from ``v80-smi list``.

Frequency Guidelines
=====================

- Frequencies are always specified in **Hz** (e.g. ``300000000`` for 300 MHz).
- Do not exceed the value returned by ``getMaxFrequency()``.
- Lower frequencies can improve timing closure for complex designs.
- Higher frequencies increase throughput but may cause timing violations.
- The user's ``vrtd`` role must include the ``clock`` permission.
  See :doc:`/reference/vrtd/configuration`.

Next Steps
==========

- :doc:`/reference/vrt-api/device` — full Device API reference.
- :doc:`/howto/use-cmake-modules` — CMake project setup.
- :doc:`/reference/vrtd/configuration` — permission keys including ``clock``.
