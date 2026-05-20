..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

####################
Build from Source
####################

This guide covers building all SLASH components from the repository.

Prerequisites
=============

- **CMake** 3.20 or later
- **C++ compiler** with C++17 support (GCC 9+, Clang 10+) — v80-smi requires
  C++20
- **C compiler** with C11 support
- **Linux kernel headers** (for the kernel module)
- **pkg-config**

Library dependencies:

- **libxml2** — XML parsing (vrtbin ``system_map.xml``)
- **ZeroMQ** (libzmq) — emulation/simulation IPC
- **JsonCpp** — JSON manifest and command handling
- **zlib** — vrtbin archive decompression
- **libsystemd** — vrtd daemon integration
- **inih** — INI configuration parsing (vrtd)

On Debian/Ubuntu:

.. code-block:: bash

   sudo apt install cmake pkg-config ninja-build \
     libxml2-dev libzmq3-dev libjsoncpp-dev zlib1g-dev \
     libsystemd-dev libinih-dev libcli11-dev \
     linux-headers-$(uname -r) \
     python3

Build Order
===========

Components must be built in dependency order:

.. code-block:: text

   1. Linux kernel module (slash)
   2. libslash
   3. vrtd  (depends on libslash)
   4. VRT   (depends on vrtd)
   5. v80-smi (depends on VRT)

Alternatively, VRT can build vrtd as a CMake subdirectory automatically.

Linux Kernel Module
===================

.. code-block:: bash

   cd driver
   make
   sudo insmod slash.ko

Optional module parameters:

- ``qdma_num_threads=N`` — number of libqdma worker threads (default: 8).
- ``qdma_debugfs_path=/sys/kernel/debug`` — enable QDMA debugfs diagnostics.

vrtd (Daemon)
=============

.. code-block:: bash

   cd vrt/vrtd
   cmake -B build -S . -G Ninja
   cmake --build build

This produces:

- ``libvrtd`` — C wire-protocol client library.
- ``libvrtdpp`` — C++ RAII wrapper library.
- ``vrtd`` — the daemon executable.

Install:

.. code-block:: bash

   sudo cmake --install build

VRT (Runtime Library)
=====================

.. code-block:: bash

   cd vrt
   cmake -B build -S . -G Ninja
   cmake --build build

If vrtd is not installed system-wide, VRT will build it as a subdirectory
automatically. To force this behaviour:

.. code-block:: bash

   cmake -B build -S . -G Ninja -DFETCHCONTENT_FULLY_DISCONNECTED=OFF

Install:

.. code-block:: bash

   sudo cmake --install build

v80-smi
=======

Requires C++20 and a built VRT library.

.. code-block:: bash

   cd smi
   cmake -B build -S . -G Ninja
   cmake --build build

Install:

.. code-block:: bash

   sudo cmake --install build

slashkit — Static Shell
==============================

After installing ``v80-smi``, the linker's static shell must be built
before hardware vrtbins can be linked. The static shell is the pre-built
FPGA platform base that every hardware vrtbin is linked against. It contains
platform IP — including the SMBus controller used for board management —
that requires a **Vivado Enterprise license** to build.

Source Vivado **2025.1** and Vitis **2025.1** and ensure a Vivado Enterprise
license is configured for your site:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis>/settings64.sh

For ``csh``/``tcsh`` users:

.. code-block:: csh

   source <path-to-vivado>/settings64.csh
   source <path-to-vitis>/settings64.csh

.. note::

   Vivado Enterprise license configuration is site-specific. Contact your
   license administrator if you are unsure how licenses are served at your
   site.

The SMBus IP (``xilinx.com:ip:smbus:1.1``) used for board management is
**not included** in this repository and is not bundled with Vivado. It must
be downloaded separately from the AMD member portal and placed into the
local IP repository before building:

1. Download the SMBus IP from https://www.xilinx.com/member/v80.html
   (AMD account required).
2. Copy the downloaded IP directory into ``linker/slashkit/resources/base/iprepo/``
   so that Vivado can locate it during synthesis.

See the `AVED rebuild guide <https://xilinx.github.io/AVED/>`_ for
additional details.

Then run the linker install script from the repository root:

.. code-block:: bash

   bash scripts/root-design-build.sh

**This step takes several hours** — it runs full Vivado synthesis and
implementation to produce the static shell artifacts.

Examples
========

Each example is a standalone CMake project. To build against the local
repository tree (without installing SLASH first):

.. code-block:: bash

   cd examples/00_axilite
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build

To build against installed SLASH packages:

.. code-block:: bash

   cmake -B build -S . -G Ninja
   cmake --build build

Building FPGA artefacts (HLS kernels and vrtbin files) requires AMD Vivado
**2025.1** and Vitis HLS **2025.1**. Source the environment before building:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. Using versions
other than 2025.1 may cause breakage.

The CMake ``SlashTools`` module provides:

- ``build_hls_dir()`` — compile HLS kernels from a directory.
- ``add_vbin()`` — link kernels into a vrtbin for a target platform
  (``hw``, ``emu``, or ``sim``).
