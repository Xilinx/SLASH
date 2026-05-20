..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

############
BuildHLS
############

``BuildHLS.cmake`` provides CMake functions for compiling HLS C++ kernels using
Vitis HLS. It is automatically included by :doc:`slashtools`, so you do not
need to include it separately.

build_hls()
===========

Compiles a single HLS kernel from a C++ source file and configuration.

.. code-block:: cmake

   build_hls(
     TARGET  <target_name>
     CPP     <source_file>
     CFG     <config_file>
     DEVICE  <part_string>
     [OUT_DIR <output_directory>]
   )

Parameters
----------

``TARGET`` *(required)*
   CMake target name for this kernel build.

``CPP`` *(required)*
   Path to the HLS C++ source file.

``CFG`` *(required)*
   Path to the Vitis HLS configuration file (``.cfg``).

``DEVICE`` *(required)*
   FPGA part string, e.g. ``xcv80-lsva4737-2MHP-e-S``.

``OUT_DIR`` *(optional)*
   Output directory. Defaults to ``CMAKE_CURRENT_BINARY_DIR``.

Output Properties
-----------------

After the target is created, two properties are set on it:

``HLS_BUILD_DIR``
   The build directory containing HLS outputs.

``HLS_COMPONENT_XML``
   Path to the generated ``component.xml`` file. This is the input for
   ``add_vbin()``.

The same values are also exported as ``${TARGET}_BUILD_DIR`` and
``${TARGET}_COMPONENT_XML`` in the calling scope.

build_hls_dir()
===============

Compiles multiple HLS kernels from a directory containing paired
``<name>.cpp`` and ``<name>.cfg`` files.

.. code-block:: cmake

   build_hls_dir(
     TARGET      <target_name>
     ROOT        <directory>
     DEVICE      <part_string>
     KERNELS     <kernel_names...>
     [OUT_DIR     <output_directory>]
     [OUT_IP_REPO <variable_name>]
     [OUT_KERNELS <variable_name>]
   )

Parameters
----------

``TARGET`` *(required)*
   Umbrella CMake target name. Individual kernel targets are created as
   ``${TARGET}_${kernel_name}``.

``ROOT`` *(required)*
   Directory containing kernel source files. For each name listed in
   ``KERNELS``, the directory must contain ``<name>.cpp`` and ``<name>.cfg``.

``DEVICE`` *(required)*
   FPGA part string.

``KERNELS`` *(required)*
   List of kernel names to compile (without file extensions).

``OUT_DIR`` *(optional)*
   Output directory. Defaults to ``${CMAKE_CURRENT_BINARY_DIR}/${TARGET}``.

``OUT_IP_REPO`` *(optional)*
   Variable name to receive the IP repository output path.

``OUT_KERNELS`` *(optional)*
   Variable name to receive the list of ``component.xml`` paths for all
   compiled kernels. Pass this to ``add_vbin(KERNELS ...)``.

build_hls_clean()
=================

Creates a CMake target that removes HLS build artefacts.

.. code-block:: cmake

   build_hls_clean(
     TARGET  <target_name>
     DEVICE  <part_string>
     [ROOT   <directory>]
     [EXTRA_GLOBS <patterns...>]
   )

Parameters
----------

``TARGET`` *(required)*
   Name for the clean target.

``DEVICE`` *(required)*
   Part string, used to match ``build_*.${DEVICE}`` directories.

``ROOT`` *(optional)*
   Directory to clean. Defaults to ``CMAKE_CURRENT_LIST_DIR``.

``EXTRA_GLOBS`` *(optional)*
   Additional glob patterns to remove.

The generated clean target removes: build directories, log files, ``.Xil``
directories, CMake cache files, and any patterns specified via
``EXTRA_GLOBS``.

HLS Configuration File Format
==============================

Each kernel requires a ``.cfg`` file for Vitis HLS. Example
(``increment.cfg``):

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
   Top-level function name in the C++ source.

``syn.file``
   Source file to compile.

``clock``
   Target clock period (e.g. ``4ns`` = 250 MHz).

``package.output.format``
   Must be ``ip_catalog`` for the SLASH linker.

``package.output.syn``
   Set to ``false`` to defer RTL synthesis to the linker.

Example
=======

A complete CMakeLists.txt using all three functions:

.. code-block:: cmake

   set(DEVICE "xcv80-lsva4737-2MHP-e-S")

   build_hls_dir(
     TARGET      hls
     ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
     DEVICE      "${DEVICE}"
     KERNELS     increment accumulate
     OUT_KERNELS _KERNELS
   )

   add_vbin(TARGET "my_design_hw" PLATFORM "hw"
            CFG "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg"
            KERNELS ${_KERNELS})

   build_hls_clean(TARGET hls_clean DEVICE "${DEVICE}")

Build with:

.. code-block:: bash

   cmake --build build --target hls            # compile all HLS kernels
   cmake --build build --target my_design_hw   # link into a hardware vrtbin
   cmake --build build --target hls_clean      # remove HLS build artefacts
