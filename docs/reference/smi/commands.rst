..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#####################
Command Reference
#####################

``v80-smi`` is the command-line system management interface for AMD Alveo V80
boards. Running ``v80-smi`` with no subcommand prints usage help.

Device Addressing
=================

Several commands accept a ``-d/--device`` option that takes a **BDF**
(Bus:Device.Function) address. The following formats are supported:

.. list-table::
   :header-rows: 1
   :widths: 30 30

   * - Format
     - Example
   * - ``BB:DD`` (short)
     - ``03:00``
   * - ``BB:DD.F`` (short with function)
     - ``03:00.0``
   * - ``DDDD:BB:DD`` (domain:bus:device)
     - ``0000:03:00``
   * - ``DDDD:BB:DD.F`` (full)
     - ``0000:03:00.0``

Commands
========

version
-------

Print the v80-smi version and exit.

.. code-block:: text

   v80-smi version [-p|--plain]

.. option:: -p, --plain

   Print only the version in ``x.y.z`` format with no prefix. Useful for
   scripting.

list
----

Enumerate V80 boards visible on the system and report their readiness status.

.. code-block:: text

   v80-smi list [-j|--json] [-J|--pretty-json] [-l|--long] [-s|--sensors]

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

.. option:: -l, --long

   Include additional information (PCI IDs, driver status).

.. option:: -s, --sensors

   Include sensor readings (temperature, power). Requires the vrtd daemon to be
   running.

inspect
-------

Display metadata from a vrtbin file on disk without programming it onto a
device.

.. code-block:: text

   v80-smi inspect <vbin> [-j|--json] [-J|--pretty-json]

.. option:: vbin

   Path to the vrtbin file. **Required.**

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

query
-----

Display the metadata of the vrtbin currently loaded on a device.

.. code-block:: text

   v80-smi query -d <BDF> [-j|--json] [-J|--pretty-json]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -j, --json

   Output as compact JSON.

.. option:: -J, --pretty-json

   Output as indented JSON.

program
-------

Load a vrtbin file onto a device, programming the FPGA.

.. code-block:: text

   v80-smi program <vbin> -d <BDF>

.. option:: vbin

   Path to the vrtbin file. **Required.**

.. option:: -d, --device <BDF>

   Board address. **Required.**

reset
-----

Perform a hardware reset of a V80 board. This executes a full PCIe secondary
bus reset and rescan (hotplug) sequence.

.. code-block:: text

   v80-smi reset -d <BDF>

.. option:: -d, --device <BDF>

   Board address. **Required.**

validate
--------

Run memory integrity and bandwidth tests against a board's HBM and DDR
subsystems.

.. code-block:: text

   v80-smi validate -d <BDF> [-j|--threads <N>]

.. option:: -d, --device <BDF>

   Board address. **Required.**

.. option:: -j, --threads <N>

   Number of parallel buffers/threads for the validation test (1–64, default 8).
