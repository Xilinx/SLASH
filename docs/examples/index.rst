..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##########
Examples
##########

SLASH includes classic VRT examples plus Graph API examples demonstrating
heterogeneous graph execution.

.. list-table::
   :header-rows: 1
   :widths: 12 28 60

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
   * - 06
     - dcmac
     - 200 Gb/s DCMAC Ethernet loopback between two QSFP56 ports
   * - graph/00
     - multi_image_pipeline
     - End-to-end CPU + FPGA graph with two exclusive-image vbins, one-call
       FPGA bring-up, chained reprogram nodes, an in-place CPU kernel, and a
       fixed-count loop carrying state.
   * - graph/01
     - edge_detection
     - Basic Graph API authoring: CPU/FPGA kernels, buffers, scalars, and a
       single reprogram gating three kernels in one vbin image.
   * - graph/02
     - sharpen_loop
     - Loops and conditionals: a fixed-count FPGA loop carrying state, running
       alongside a CPU reduction, followed by a post-loop conditional.
   * - extra
     - rp1_latency
     - Latency microbenchmarks rather than a tutorial: the RP1 command
       processor dispatch path against the legacy host-driven VRT path, using
       the same no-op kernel on the same board.

Each example includes a ``CMakeLists.txt`` with targets for hardware (``hw``), emulation (``emu``),
and simulation (``sim``) flows. See ``examples/README.md`` in the repository for build
instructions. The ``extra/`` projects are the exception: they build standalone against an
installed or in-repo VRT and link a hardware vbin only.
