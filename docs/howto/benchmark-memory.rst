..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##################
Benchmark Memory
##################

This guide shows how to measure HBM and DDR memory bandwidth on a V80 board
using ``v80-smi validate`` and the performance example.

Prerequisites
=============

- The SLASH stack is installed, ``vrtd`` is running, and a V80 board is
  visible in ``v80-smi list``.
- Root or sufficient permissions for device reset (``v80-smi validate``
  performs a full reset before testing).

Quick Start — v80-smi validate
================================

The fastest way to benchmark memory is with the built-in validate command:

.. code-block:: bash

   v80-smi validate -d <BDF> [-j <threads>]

- ``<BDF>`` — board address from ``v80-smi list`` (e.g. ``0000:03:00``).
- ``[threads]`` — number of parallel buffers (default: 8). Each buffer is
  64 MB (one allocator sub-region).

Example:

.. code-block:: bash

   v80-smi validate -d 0000:03:00
   v80-smi validate -d 0000:03:00 -j 4    # 4 parallel buffers

What validate Measures
========================

The command runs three phases:

1. **Device reset** — performs a full hotplug reset sequence via ``vrtd`` to
   bring the board to a clean state.

2. **HBM test** — allocates *N* buffers in HBM, then:

   - **Integrity check**: fills each buffer with an XOR pattern
     (``data[i] = i ^ seed``), syncs host-to-device, clears the host copy,
     syncs device-to-host, and verifies every word matches.
   - **Bandwidth measurement**: launches *N* threads in parallel. Each thread
     performs a full-buffer host-to-device (H2C) transfer, then a
     device-to-host (C2H) transfer. Wall-clock time is recorded for each
     direction.

3. **DDR test** — repeats the same integrity and bandwidth measurements using
   DDR memory.

Output includes per-direction bandwidth in MB/s and a pass/fail verdict for
data integrity.

Using Example 05 (perf)
=========================

The ``05_perf`` example provides a more configurable benchmark that runs
inside your own application. Build it against the repository:

.. code-block:: bash

   cd examples/05_perf
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build
   cmake --build build --target hls
   cmake --build build --target perf_hw    # or perf_emu / perf_sim

Run:

.. code-block:: bash

   ./05_perf <BDF> perf_hw.vbin

This example allocates buffers across HBM and DDR banks and measures
round-trip throughput, giving you a baseline for your own kernel designs.

Interpreting Results
====================

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - Metric
     - Direction
     - Description
   * - Write bandwidth
     - H2C (host-to-device)
     - Rate at which data is DMA'd from host memory to the device.
   * - Read bandwidth
     - C2H (device-to-host)
     - Rate at which data is DMA'd from the device back to host memory.
   * - Data integrity
     - Both
     - PASS if every word survives the round-trip; FAIL indicates a
       transfer or memory error.

HBM typically delivers significantly higher aggregate bandwidth than DDR due
to its 32 independent channels. Increasing the thread count can improve
utilisation of these parallel channels.

.. note::

   Bandwidth numbers depend on PCIe link width and generation, BIOS
   settings, IOMMU configuration, and host CPU/memory speed. Results are
   most useful for relative comparisons (e.g. before/after a configuration
   change) rather than absolute guarantees.

Tuning Parameters
=================

- **Thread count** — more parallel buffers can saturate more HBM channels,
  but returns diminish once the PCIe link is the bottleneck.
- **Buffer size** — each buffer is 64 MB by default (one allocator
  sub-region). The validate command does not expose a size parameter; use
  the VRT API directly if you need different sizes.

See Also
========

- :doc:`/explanation/memory-model` — how HBM and DDR banks are organised.
- :doc:`/tutorials/user/buffers-and-memory` — buffer allocation and
  synchronisation in application code.
- :doc:`/reference/smi/commands` — full ``v80-smi`` command reference.
