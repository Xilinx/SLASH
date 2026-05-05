..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##################
Your First Kernel
##################

This tutorial walks through writing an HLS kernel from scratch, compiling it
with Vitis HLS, linking it into a vrtbin, and running it on a V80 board. By the
end you will understand every file in a minimal SLASH project.

Prerequisites
=============

- The SLASH stack is installed (kernel module, libslash, vrtd, VRT, v80-smi).
  See :doc:`/howto/build-from-source` if building from source.
- AMD Vivado **2025.1** and Vitis HLS **2025.1** are installed and sourced in
  your shell:

  .. code-block:: bash

     source <path-to-vivado>/settings64.sh
     source <path-to-vitis-hls>/settings64.sh

  For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. Using versions
  other than 2025.1 may cause breakage.

- A V80 board is installed and visible (``v80-smi list``), or you plan to use
  simulation/emulation.

Anatomy of an HLS Kernel
=========================

An HLS kernel is a C/C++ function with interface pragmas that tell Vitis HLS
how to map arguments to hardware ports. Here is the ``increment`` kernel from
``examples/00_axilite/hls/increment.cpp``:

.. code-block:: cpp

   #include <ap_fixed.h>
   #include <hls_stream.h>

   void increment(ap_uint<32> size, float* in, hls::stream<float>& axis_out) {
   #pragma hls interface mode=s_axilite port=size
   #pragma hls interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64
   #pragma hls interface axis port=axis_out
   #pragma hls interface mode=s_axilite port=return

       for(ap_uint<32> i = 0; i < size; i++) {
       #pragma hls pipeline II=1
           float data = in[i] + 1;
           axis_out.write(data);
       }
   }

Each pragma controls a different aspect of the hardware interface:

``s_axilite``
   Exposes the argument as a memory-mapped register on the AXI-Lite control
   bus. The host sets these via ``Kernel::setArg()`` before launching the
   kernel.

``m_axi``
   Maps a pointer argument to an AXI memory-mapped master port. The
   ``bundle=gmem0`` name becomes the port name visible in the linker
   configuration (prefixed with ``m_axi_``). ``max_widen_bitwidth=64``
   limits data-path widening for this port.

``axis``
   Maps an ``hls::stream`` argument to an AXI-Stream port. Streams connect
   kernels directly without going through device memory.

``s_axilite port=return``
   Required on every SLASH kernel. It creates the AP_START / AP_DONE / AP_IDLE
   control registers that VRT uses to start and poll the kernel.

``pipeline II=1``
   Instructs HLS to pipeline the loop body with an initiation interval of one
   clock cycle — one new iteration begins every cycle.

HLS Configuration File
=======================

Each kernel needs a ``.cfg`` file that tells Vitis HLS how to compile it.
Here is ``increment.cfg``:

.. code-block:: ini

   part=xcv80-lsva4737-2MHP-e-S

   [hls]
   flow_target=vivado

   syn.top=increment
   syn.file=increment.cpp
   clock=4ns

   package.output.format=ip_catalog
   package.output.syn=false

``part``
   The FPGA part string for the V80 board.

``syn.top``
   The top-level function name in the C++ source.

``syn.file``
   The source file to compile.

``clock``
   The target clock period. ``4ns`` corresponds to 250 MHz.

``package.output.format``
   Must be ``ip_catalog`` so the output can be consumed by the SLASH linker.

``package.output.syn``
   Set to ``false`` to skip RTL synthesis during HLS (the linker handles it).

Linker Configuration
====================

The linker configuration file (``config.cfg``) describes how kernels are
instantiated, connected, and mapped to memory. Here is
``examples/00_axilite/config.cfg``:

.. code-block:: ini

   [connectivity]
   nk=accumulate:1:accumulate_0
   nk=increment:1:increment_0
   stream_connect=increment_0.axis_out:accumulate_0.axis_in
   sp=increment_0.m_axi_gmem0:HBM1

``nk``
   Instantiates a kernel. Format: ``<kernel_name>:<count>:<instance_name>``.
   The instance name is what you pass to ``vrt::Kernel(device, "increment_0")``.

``stream_connect``
   Wires an AXI-Stream output port on one kernel instance to an input port on
   another. Format: ``<src_instance>.<port>:<dst_instance>.<port>``.

``sp``
   Maps an AXI memory-mapped port to a physical memory resource. Format:
   ``<instance>.<port>:<memory>``. Valid memories include ``HBM0``–``HBM63``,
   ``DDR0``–``DDR3``, and ``MEM``.

CMake Build Setup
=================

SLASH provides CMake modules for compiling HLS kernels and linking vrtbins.
With SLASH installed, use ``find_package`` to import them:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(my_project LANGUAGES CXX)
   set(CMAKE_CXX_STANDARD 20)

   find_package(vrt REQUIRED CONFIG)
   find_package(SlashTools REQUIRED)

   # --- HLS kernels ---
   set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

   build_hls_dir(
     TARGET      hls
     ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
     DEVICE      "${DEVICE}"
     KERNELS     increment accumulate
     OUT_KERNELS _KERNELS
   )

   # --- VBIN targets ---
   set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
   add_vbin(TARGET "my_design_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "my_design_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "my_design_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

   # --- Executable ---
   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE vrt::vrt)

``find_package(SlashTools)`` makes ``build_hls_dir()`` and ``add_vbin()``
available, and also locates Vivado and Vitis automatically.

``build_hls_dir()`` compiles every kernel in the ``hls/`` directory. It
expects ``<name>.cpp`` and ``<name>.cfg`` file pairs for each kernel listed in
``KERNELS``. The compiled IP paths are stored in ``_KERNELS``.

``add_vbin()`` invokes the SLASH linker (``slashkit``) to produce a ``.vbin``
archive from the compiled kernels and the connectivity configuration. One
target is created per platform (``hw``, ``emu``, ``sim``).

See :doc:`/reference/cmake/slashtools` and :doc:`/reference/cmake/buildhls`
for full function reference.

Build and Run
=============

Ensure you have sourced Vivado and Vitis HLS before building (see
`Prerequisites`_).

.. code-block:: bash

   cmake -B build -S . -G Ninja
   cmake --build build                              # build the host application
   cmake --build build --target hls                 # compile HLS kernels (requires Vitis HLS)
   cmake --build build --target my_design_hw        # link into a hardware vrtbin

Run the application:

.. code-block:: bash

   v80-smi list                               # find your board's BDF
   ./my_app 03:00 my_design_hw.vbin           # run with BDF and vrtbin

For emulation (no FPGA required):

.. code-block:: bash

   cmake --build build --target my_design_emu
   ./my_app 03:00 my_design_emu.vbin

Creating Your Own Project
=========================

To start a project outside the SLASH repository, create a directory with the
following layout:

.. code-block:: text

   my_project/
   ├── CMakeLists.txt
   ├── config.cfg
   ├── main.cpp
   └── hls/
       ├── my_kernel.cpp
       └── my_kernel.cfg

**CMakeLists.txt** — use ``find_package`` to locate the installed SLASH modules:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(my_project LANGUAGES CXX)
   set(CMAKE_CXX_STANDARD 20)

   find_package(vrt REQUIRED CONFIG)
   find_package(SlashTools REQUIRED)

   set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

   build_hls_dir(
     TARGET      hls
     ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
     DEVICE      "${DEVICE}"
     KERNELS     my_kernel
     OUT_KERNELS _KERNELS
   )

   set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
   add_vbin(TARGET "my_design_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "my_design_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "my_design_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE vrt::vrt)

**config.cfg** — a minimal connectivity configuration:

.. code-block:: ini

   [connectivity]
   nk=my_kernel:1:my_kernel_0
   sp=my_kernel_0.m_axi_gmem0:HBM1

**Build sequence:**

.. code-block:: bash

   cmake -B build -S . -G Ninja
   cmake --build build                              # build the host application
   cmake --build build --target hls                 # compile HLS kernels
   cmake --build build --target my_design_hw        # hardware vrtbin
   cmake --build build --target my_design_emu       # emulation vrtbin
   cmake --build build --target my_design_sim       # simulation vrtbin

See :doc:`/howto/use-cmake-modules` for the full CMake setup reference.

Next Steps
==========

- :doc:`buffers-and-memory` — learn about DDR vs HBM memory and buffer
  management.
- :doc:`/reference/cmake/slashtools` — full ``add_vbin()`` reference.
- :doc:`/reference/cmake/buildhls` — full ``build_hls()`` and
  ``build_hls_dir()`` reference.
- :doc:`/explanation/platform-modes` — run the same code in emulation or
  simulation.
