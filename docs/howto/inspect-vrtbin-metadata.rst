..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##########################
Inspect vrtbin Metadata
##########################

This guide shows how to use ``v80-smi inspect`` and ``v80-smi query`` to
examine kernel information, clock frequency, and resource utilisation from a
vrtbin file or a live device.

Prerequisites
=============

- ``v80-smi`` is installed and on your ``PATH``.
- You have a vrtbin file to inspect, **or** a V80 board with a loaded design.
- See :doc:`/howto/build-from-source` for installation instructions.

Inspect a vrtbin File
=====================

Pass a vrtbin file path to ``v80-smi inspect``:

.. code-block:: bash

   v80-smi inspect my_design.vbin

Example output:

.. code-block:: text

   Vbin my_design.vbin:
      Platform: HARDWARE
      Clock frequency: 200000000
      Utilization:
         slash: LUTs: 42310 (4.81%), FFs: 53792 (3.06%), ...
      Kernel:
         Name: increment_0
         Physical address: 0x202000000000
         Argument:
            Index: 0
            Name: data
            Type: int*
            Offset: 16
            Range: 64
            Direction: ReadWrite

The output shows:

- **Platform** — ``HARDWARE``, ``EMULATION``, or ``SIMULATION``.
- **Clock frequency** — the design clock in Hz.
- **Utilisation** — FPGA resource usage (hardware builds only).
- **Kernels** — each kernel instance with its physical address and arguments.

JSON Output
-----------

Use ``-J`` for pretty-printed JSON or ``-j`` for compact JSON (useful for
scripting):

.. code-block:: bash

   v80-smi inspect my_design.vbin -J

.. code-block:: json

   {
      "clock_frequency": "0xbebc200",
      "kernels": {
         "increment_0": {
            "name": "increment_0",
            "address": "0x202000000000",
            "args": [
               {
                  "index": "0x0",
                  "name": "data",
                  "type": "int*",
                  "offset": "0x10",
                  "range": "0x40",
                  "direction": "ReadWrite"
               }
            ]
         }
      }
   }

.. note::

   Numeric fields (``clock_frequency``, ``address``, ``offset``, ``range``)
   are encoded as hexadecimal strings in the JSON output to avoid integer
   precision issues.

Query a Live Device
===================

To read the metadata of the design currently loaded on a device, use
``v80-smi query``:

.. code-block:: bash

   v80-smi query -d 03:00

The output format is identical to ``inspect``, but the data comes from the
device's system map rather than a file on disk.

.. note::

   The device must have been programmed with a vrtbin (via ``v80-smi program``
   or the VRT API). If no design is loaded, the command will report an error.

Use ``v80-smi list`` to discover the BDF addresses of your boards.

Understanding the Arguments
===========================

Each kernel argument entry contains:

- **Index** — positional index in the HLS function signature.
- **Name** — the C++ parameter name from the HLS source.
- **Type** — the C++ type (e.g. ``int*``, ``unsigned int``).
- **Offset** — register offset within the kernel's AXI-Lite control block.
- **Range** — bit width of the argument register.
- **Direction** — ``Read``, ``Write``, or ``ReadWrite``.

Understanding Utilisation
=========================

Hardware vrtbin files include a utilisation report showing how much of the
FPGA fabric the design consumes:

- **LUTs** — Look-Up Tables (combinational logic).
- **FFs** — Flip-Flops (sequential logic).
- **LUTRAM** — LUT-based distributed RAM.
- **SRL** — Shift Register LUTs.
- **RAMB36 / RAMB18** — Block RAM tiles.
- **URAM** — UltraRAM blocks.
- **DSP** — Digital Signal Processing slices.

Each metric shows the absolute count and, when available, the percentage of
total resources used.

Next Steps
==========

- :doc:`/explanation/vrtbin-format` — understand the vrtbin archive structure.
- :doc:`/reference/smi/commands` — full ``v80-smi`` command reference.
- :doc:`/tutorials/admin/device-management` — device management workflows.
