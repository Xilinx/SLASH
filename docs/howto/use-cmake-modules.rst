..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#####################
Use CMake Modules
#####################

This guide shows how to set up a new CMake project that compiles HLS
kernels and links vrtbin archives using the SLASH CMake modules.

Installed SLASH (Recommended)
=============================

If SLASH is installed system-wide (via ``sudo make install``), use
``find_package`` to import the modules and the VRT library:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(my_project LANGUAGES CXX)
   set(CMAKE_CXX_STANDARD 20)

   find_package(vrt REQUIRED CONFIG)
   find_package(SlashTools REQUIRED)

   # Your application
   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE vrt::vrt)

``find_package(SlashTools)`` makes ``build_hls()``, ``build_hls_dir()``,
and ``add_vbin()`` available. It also includes ``FindVivado`` automatically.

Source-Tree SLASH
=================

When developing against the SLASH repository without installing, add the
CMake module path and VRT as a subdirectory:

.. code-block:: cmake

   option(SLASH_USE_REPO "Build against local repo tree" OFF)

   if(SLASH_USE_REPO)
     get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
     list(APPEND CMAKE_MODULE_PATH "${REPO_ROOT}/cmake")
     include(SlashTools)
     add_subdirectory(${REPO_ROOT}/vrt ${CMAKE_CURRENT_BINARY_DIR}/vrt)
     set(_VRT_LIBS vrt)
   else()
     find_package(vrt REQUIRED CONFIG)
     find_package(SlashTools REQUIRED)
     set(_VRT_LIBS vrt::vrt)
   endif()

   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE ${_VRT_LIBS})

Configure with:

.. code-block:: bash

   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON

HLS Kernel Directory
====================

``build_hls_dir()`` expects a directory containing matched pairs of
``<kernel>.cpp`` and ``<kernel>.cfg`` files:

.. code-block:: text

   hls/
   ├── my_kernel.cpp
   ├── my_kernel.cfg
   ├── other_kernel.cpp
   └── other_kernel.cfg

Add this to your ``CMakeLists.txt``:

.. code-block:: cmake

   set(DEVICE "xcv80-lsva4737-2MHP-e-S" CACHE STRING "Target device")

   build_hls_dir(
     TARGET      hls
     ROOT        "${CMAKE_CURRENT_SOURCE_DIR}/hls"
     DEVICE      "${DEVICE}"
     KERNELS     my_kernel other_kernel
     OUT_KERNELS _KERNELS
   )

The ``_KERNELS`` variable receives the list of compiled kernel IP paths,
which you pass to ``add_vbin()`` below.

See :doc:`/reference/cmake/buildhls` for the full ``build_hls_dir()`` and
``build_hls()`` reference.

Linking Vrtbin Archives
=======================

Create a linker configuration file (``config.cfg``):

.. code-block:: ini

   [connectivity]
   nk=my_kernel:1:my_kernel_0
   nk=other_kernel:1:other_kernel_0
   sp=my_kernel_0.m_axi_gmem0:HBM1

Then add vrtbin targets for each platform:

.. code-block:: cmake

   set(CFG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config.cfg")

   add_vbin(TARGET "design_hw"  PLATFORM "hw"  CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "design_emu" PLATFORM "emu" CFG "${CFG_FILE}" KERNELS ${_KERNELS})
   add_vbin(TARGET "design_sim" PLATFORM "sim" CFG "${CFG_FILE}" KERNELS ${_KERNELS})

See :doc:`/reference/cmake/slashtools` for the full ``add_vbin()``
reference.

Locating Vivado and Vitis
=========================

Before configuring or building, source the Vivado and Vitis HLS environment in
your shell:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. SLASH has been
built and tested against **Vivado/Vitis 2025.1**; using other versions may
cause breakage.

The SLASH CMake modules will then automatically find Vivado and Vitis on
``PATH``.

Build Sequence
==============

.. code-block:: bash

   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON   # or without flag if SLASH is installed
   cmake --build build                                 # build the host application
   cmake --build build --target hls                    # compile HLS kernels (requires Vitis HLS)
   cmake --build build --target design_hw              # link hardware vrtbin
   cmake --build build --target design_emu             # link emulation vrtbin

The host application and HLS compilation are independent — you can build
them in either order. The vrtbin targets depend on the HLS kernels.
