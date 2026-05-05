..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##############
SlashTools
##############

``SlashTools.cmake`` is the primary CMake module for building SLASH projects.
It provides the ``add_vbin()`` function for linking HLS kernels into vrtbin
archives, and automatically includes :doc:`buildhls` for HLS kernel
compilation.

Including SlashTools
====================

When SLASH is installed system-wide:

.. code-block:: cmake

   find_package(SlashTools REQUIRED)

When building from the SLASH source tree:

.. code-block:: cmake

   list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
   include(SlashTools)

``SlashTools`` automatically includes ``BuildHLS.cmake`` and locates Vivado via
``FindVivado.cmake``.

add_vbin()
==========

Links compiled HLS kernels into a ``.vbin`` archive for a given platform.

.. code-block:: cmake

   add_vbin(
     TARGET   <target_name>
     PLATFORM <platform>
     CFG      <config_file>
     KERNELS  <kernel_xml_paths...>
   )

Parameters
----------

``TARGET`` *(required)*
   Name of the CMake custom target. The output file will be
   ``${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.vbin``.

``PLATFORM`` *(required)*
   Target platform. One of:

   - ``hw`` — hardware (real V80 board)
   - ``emu`` — emulation (C-model, no FPGA required)
   - ``sim`` — simulation (Verilog register map)

``CFG`` *(required)*
   Path to the linker configuration file containing ``[connectivity]``
   directives (``nk=``, ``stream_connect=``, ``sp=``).

``KERNELS`` *(required)*
   List of HLS component XML paths. These are typically produced by
   ``build_hls_dir()`` via the ``OUT_KERNELS`` parameter.

Operating Modes
----------------

``add_vbin()`` supports two modes for locating the SLASH linker:

**Installed mode** (preferred)
   If ``slashkit`` is found on ``PATH``, the function invokes it directly.

**Source-tree mode**
   If ``SLASH_REPO_ROOT`` is set (or auto-detected from the module's location),
   the function invokes ``linker/src/main.py`` via Python 3.

If neither mode is available, CMake emits a ``FATAL_ERROR``.

Configuration Variables
========================

``SLASH_REPO_ROOT``
   Path to the SLASH repository root. Auto-detected when the CMake module is
   located inside the repository tree.

``SLASHKIT_EXECUTABLE``
   Path to the installed ``slashkit`` linker. Auto-detected via ``find_program()``.

Example
=======

From ``examples/00_axilite/CMakeLists.txt``:

.. code-block:: cmake

   build_hls_dir(
     TARGET      hls
     ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
     DEVICE      "${DEVICE}"
     KERNELS     increment accumulate
     OUT_KERNELS _KERNELS
   )

   set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")
   add_vbin(TARGET "axilite_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "axilite_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "axilite_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

Build with:

.. code-block:: bash

   cmake --build build --target hls           # compile HLS kernels
   cmake --build build --target axilite_hw    # link hardware vrtbin
   cmake --build build --target axilite_emu   # link emulation vrtbin
