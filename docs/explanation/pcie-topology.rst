..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#################
PCIe Topology
#################

Each AMD Alveo V80 board exposes three PCIe Physical Functions (PFs). The
SLASH stack assigns each function a dedicated role, driver, and character
device so that management, DMA, and register access can operate independently.

Physical Functions
==================

.. code-block:: text

   ┌─────────────── V80 Board ───────────────┐
   │                                          │
   │   PF0 (.0)      PF1 (.1)      PF2 (.2)  │
   │   ami           slash_qdma    slash_ctl   │
   │   0x50B4        0x50B5        0x50B6      │
   │   Management    DMA           BAR MMIO    │
   └──────────────────────────────────────────┘

.. list-table::
   :header-rows: 1
   :widths: 10 15 15 20 40

   * - PF
     - Device ID
     - Driver
     - Device path
     - Role
   * - PF0
     - ``0x50B4``
     - ``ami``
     - (managed by AMI subsystem)
     - AVED management interface — sensor readings, board identity, firmware
       version.
   * - PF1
     - ``0x50B5``
     - ``slash_qdma``
     - ``/dev/slash_qdma_ctl<N>``
     - Queue-based DMA subsystem — H2C and C2H data transfers for buffers and
       streaming.
   * - PF2
     - ``0x50B6``
     - ``slash_ctl``
     - ``/dev/slash_ctl<N>``
     - BAR MMIO access — kernel register reads and writes via memory-mapped
       I/O.

All three PFs share vendor ID ``0x10EE`` (AMD/Xilinx).

BDF Addressing
==============

PCI devices are identified by a **BDF** (Bus:Device.Function) address in the
format ``DDDD:BB:DD.F`` — domain, bus, device, function. The three V80
functions share the same domain, bus, and device number and differ only in
the function digit:

.. code-block:: text

   0000:03:00.0   ← PF0  (ami)
   0000:03:00.1   ← PF1  (slash_qdma)
   0000:03:00.2   ← PF2  (slash_ctl)

Throughout the SLASH stack, a **board BDF** refers to the common prefix
without the function digit (e.g. ``0000:03:00``). Given a board BDF, each
component derives the full address by appending ``.0``, ``.1``, or ``.2``.

Device Discovery
================

``v80-smi list`` discovers V80 boards by scanning the sysfs PCI bus:

1. Enumerate ``/sys/bus/pci/devices/`` for entries with vendor ``0x10EE`` and
   device ``0x50B4`` (PF0).
2. Extract the board BDF from the matching entry.
3. Verify that companion functions PF1 (``0x50B5``) and PF2 (``0x50B6``)
   exist at the same bus and device.
4. Check that the correct kernel driver is bound to each function by
   inspecting the ``driver`` symlink in sysfs.
5. Query the ``vrtd`` daemon for device registration via
   ``vrtd::Session::getDeviceByBdf()``.

Readiness Checks
================

``v80-smi list`` reports four readiness indicators per board:

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Check
     - Validates
     - Failure meaning
   * - **PF0**
     - ``ami`` driver bound to ``0x50B4``
     - AMI management driver not loaded
   * - **PF1**
     - ``slash_qdma`` driver bound to ``0x50B5``
     - QDMA driver not loaded
   * - **PF2**
     - ``slash_ctl`` driver bound to ``0x50B6``
     - Control driver not loaded
   * - **VRTD**
     - Daemon has registered the board
     - ``vrtd`` not running or device not configured

All four must pass before the board can be used by VRT.

Hotplug Lifecycle
=================

FPGA reconfiguration requires removing the device from the PCI bus,
performing a Secondary Bus Reset (SBR), and re-enumerating. The ``slash``
kernel module exposes a hotplug character device at ``/dev/slash_hotplug``
with four ioctl operations:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Operation
     - Description
   * - ``REMOVE``
     - Remove a device by BDF from the PCI bus.
   * - ``TOGGLE_SBR``
     - Assert the Secondary Bus Reset on the root port (2 ms hold), deassert,
       then wait 5 s for the link to retrain.
   * - ``RESCAN``
     - Rescan the entire PCI bus to re-enumerate devices.
   * - ``HOTPLUG``
     - Atomic REMOVE + RESCAN for a single device.

A typical FPGA programming sequence follows this order:

.. code-block:: text

   1. REMOVE  PF0, PF1, PF2       ← tear down all three functions
   2. TOGGLE_SBR on root port      ← reset the FPGA, reload bitstream
   3. RESCAN                       ← re-enumerate the bus
   4. HOTPLUG each function        ← bind drivers to the new device

The ``vrtd`` daemon orchestrates this sequence through its
``ResetSequence`` hotplug operation, which is triggered by
``v80-smi reset`` or programmatically via ``vrtd::Device::hotplugOp()``.

Segmented Configuration
=======================

Each physical function operates through its own independent character device.
This separation means that the SLASH stack layers interact with different
functions for different purposes:

- **VRT** uses PF2 (``slash_ctl``) for kernel register access via BAR MMIO,
  and PF1 (``slash_qdma``) for buffer DMA transfers.
- **vrtd** uses libslash to manage all three functions and orchestrate device
  lifecycle events like hotplug and reset.
- **v80-smi** reads PF0 (``ami``) for sensor data and board identity, and
  uses PF1/PF2 for validation and programming.

There are no cross-function dependencies in userspace — each character device
can be opened, used, and closed independently.

See Also
========

- :doc:`architecture` — full SLASH stack overview.
- :doc:`/tutorials/admin/device-management` — managing V80 boards in
  practice.
- :doc:`/reference/vrtd/client-flow` — how VRT connects to ``vrtd`` for
  device access.
