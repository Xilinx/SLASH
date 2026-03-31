..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##########
Examples
##########

SLASH includes six example projects demonstrating different VRT features.

.. list-table::
   :header-rows: 1
   :widths: 5 20 40

   * - ID
     - Name
     - Feature
   * - 00
     - axilite
     - AXI-Lite control interfaces and kernel linking
   * - 01
     - aximm
     - AXI memory-mapped kernel interfaces
   * - 02
     - chain
     - Freerunning streaming kernel chains
   * - 03
     - multiple_boards
     - Multi-device control from a single application
   * - 04
     - freq
     - Custom clock frequency targeting
   * - 05
     - perf
     - HBM/DDR memory performance benchmarking

Each example includes a ``CMakeLists.txt`` with targets for hardware (``hw``), emulation (``emu``),
and simulation (``sim``) flows. See ``examples/README.md`` in the repository for build
instructions.
