..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

###############
Platform Setup
###############

This tutorial walks a system administrator through installing the SLASH
stack on a machine with an AMD Alveo V80 board — from package installation
to a validated, running system.

The recommended installation method is via pre-built packages (Debian or
RPM). If you need to build from source instead, see
:doc:`/howto/build-from-source`.

Prerequisites
=============

**Hardware:**

- AMD Alveo V80 board installed in a PCIe x8 (or wider) slot.

**Software:**

- Linux (Ubuntu LTS 22.04+, RHEL 9+ or compatible recommended).

Target-machine prerequisites
----------------------------

Every machine that will run the SLASH packages needs kernel headers
installed so that DKMS can compile the kernel module:

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         sudo apt install linux-headers-$(uname -r)

   .. tab-item:: RHEL / Rocky Linux / AlmaLinux

      .. code-block:: bash

         sudo dnf install kernel-devel-$(uname -r)

Other install-time dependencies (``dkms``, ``gcc``, libraries, etc.) are
declared by the packages themselves and will be pulled from the system
repositories automatically.

Build-machine prerequisites
---------------------------

The machine used to run the packaging scripts needs a C/C++ toolchain,
library development headers, and the packaging tools. These are only
required on the build machine, not on every target:

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         sudo apt install \
           build-essential cmake ninja-build pkg-config rsync \
           debhelper dpkg-dev apt-utils \
           python3 python3-pip \
           libcli11-dev libinih-dev libjsoncpp-dev \
           libsystemd-dev libxml2-dev libzmq3-dev zlib1g-dev

   .. tab-item:: RHEL 9 / Rocky Linux 9 / AlmaLinux 9

      .. code-block:: bash

         sudo dnf install \
           gcc gcc-c++ cmake make ninja-build pkg-config rsync \
           rpm-build createrepo_c systemd-rpm-macros \
           python3.11 python3.11-pip \
           cli11-devel cppzmq-devel inih-devel jsoncpp-devel \
           libxml2-devel systemd-devel \
           zeromq-devel zlib-devel

   .. tab-item:: RHEL 10 / Rocky Linux 10 / AlmaLinux 10

      .. code-block:: bash

         sudo dnf install \
           gcc gcc-c++ cmake make ninja-build pkg-config rsync \
           rpm-build createrepo_c systemd-rpm-macros \
           python3 python3-pip \
           cli11-devel cppzmq-devel inih-devel jsoncpp-devel \
           libxml2-devel systemd-devel \
           zeromq-devel zlib-devel

.. note::

   **How package dependencies work.** Each SLASH package declares its
   dependencies — both on system packages (e.g. ``slash-dkms`` depends on
   ``dkms``, ``gcc``, ``make``) and on other SLASH packages (e.g. ``vrtd``
   depends on ``libslash``). System-repository dependencies are resolved
   automatically by ``apt`` / ``dnf``.

   However, ``apt`` and ``dnf`` cannot resolve dependencies between local
   ``.deb`` / ``.rpm`` files unless they are hosted in a repository. When
   installing from the command line, you must pass **all** required SLASH
   packages in a single command so the package manager can satisfy them
   together. The packaging scripts generate repository metadata
   (``Packages`` / ``repodata/``) so that hosting the output directory as a
   repository avoids this limitation.

Static Shell
============

The *static shell* is the pre-built FPGA platform base that ships inside
the ``slashkit`` package. It contains the fixed platform infrastructure —
including the SMBus controller IP used for board management — that every
hardware vrtbin is linked against.

Building it requires **Vivado 2025.1** and **Vitis 2025.1**, plus a
**Vivado Enterprise license** (the SMBus IP is not available under the
standard tier). Source both tools before running the package build:

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

Build the Packages
==================

All packages — including the AMI driver package — are produced by a single
script run from the repository root. The static shell is built
automatically as part of this step. **Expect the build to take several
hours** while Vivado synthesises and implements the platform design.

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         ./scripts/package-deb.sh

      Packages are written to ``./deb/``.

   .. tab-item:: RHEL / Rocky Linux / AlmaLinux

      .. code-block:: bash

         ./scripts/package-rpm.sh

      Packages are written to ``./rpm/``.

Both scripts call ``scripts/package-ami.sh`` internally, so the AMI package
is built and placed in the same output directory automatically.

Install the AMI Driver
======================

The V80 board's PF0 function (device ID ``0x50B4``) is managed by the
**AMI** (AVED Management Interface) kernel module. AMI should be installed
along with the rest of the SLASH stack.

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         sudo apt install ./deb/ami_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky Linux / AlmaLinux

      .. code-block:: bash

         sudo dnf install ./rpm/ami-<version>-1.<dist>.x86_64.rpm

.. warning::

   If AMI is already installed on this system — for example, built from
   source or installed from a separate vendor package — the generated AMI
   package may conflict with the existing installation. Either remove the
   existing AMI installation before proceeding, or skip this step and
   ensure your installed AMI version is compatible with this SLASH release.

Verify that the ``ami`` driver is bound to PF0 after installation:

.. code-block:: bash

   lspci -d 10ee:50b4 -k

The output should show ``Kernel driver in use: ami``.

Install SLASH Packages
======================

Install the full runtime stack in one command by listing all packages:

.. tab-set::

   .. tab-item:: Ubuntu (.deb)

      .. code-block:: bash

         sudo apt install \
           ./deb/slash-dkms_<version>_all.deb \
           ./deb/libslash_<version>_amd64.deb \
           ./deb/vrtd_<version>_amd64.deb \
           ./deb/libvrtd_<version>_amd64.deb \
           ./deb/libvrt_<version>_amd64.deb \
           ./deb/v80-smi_<version>_amd64.deb \
           ./deb/slashkit_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky Linux / AlmaLinux (.rpm)

      .. code-block:: bash

         sudo dnf install \
           ./rpm/slash-dkms-<version>-1.<dist>.noarch.rpm \
           ./rpm/libslash-<version>-1.<dist>.x86_64.rpm \
           ./rpm/vrtd-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrtd-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrt-<version>-1.<dist>.x86_64.rpm \
           ./rpm/v80-smi-<version>-1.<dist>.x86_64.rpm \
           ./rpm/slashkit-<version>-1.<dist>.x86_64.rpm

.. note::

   If you also need to write or compile kernels (HLS development),
   install the development packages as well:

   .. tab-set::

      .. tab-item:: Ubuntu

         .. code-block:: bash

            sudo apt install \
              ./deb/libslash-dev_<version>_amd64.deb \
              ./deb/libvrtd-dev_<version>_amd64.deb \
              ./deb/libvrt-dev_<version>_amd64.deb

      .. tab-item:: RHEL / Rocky Linux / AlmaLinux

         .. code-block:: bash

            sudo dnf install \
              ./rpm/libslash-devel-<version>-1.<dist>.x86_64.rpm \
              ./rpm/libvrtd-devel-<version>-1.<dist>.x86_64.rpm \
              ./rpm/libvrt-devel-<version>-1.<dist>.x86_64.rpm

   This installs ``slashkit`` (the kernel linker) and development headers.

Package Overview
----------------

The following table summarises what each package provides:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Package
     - Contents
   * - ``slash-dkms``
     - Kernel module source + DKMS configuration. Compiles and installs
       ``slash.ko`` for the running kernel automatically.
   * - ``libslash``
     - Shared library for interacting with the kernel module.
   * - ``libslash-dev``
     - Development headers and CMake modules for ``libslash``.
   * - ``vrtd``
     - The ``vrtd`` daemon, systemd units, udev rules, and default
       configuration. Multiplexes device access and enforces permissions.
   * - ``libvrtd``
     - Wire-protocol client libraries (``libvrtd``, ``libvrtdpp``) for
       communicating with the daemon.
   * - ``libvrtd-dev``
     - Development headers and CMake modules for ``libvrtd``.
   * - ``libvrt``
     - The VRT C++ runtime library (``libvrt``).
   * - ``libvrt-dev``
     - Development headers and CMake modules for ``libvrt``.
   * - ``v80-smi``
     - Board management CLI tool.
   * - ``slashkit``
     - Python-based kernel linker for producing ``.vbin`` artefacts.
   * - ``slash``
     - Metapackage: pulls in all runtime packages.
   * - ``slash-dev``
     - Metapackage: pulls in all development headers and CMake modules.
   * - ``slash-sim-emu``
     - Metapackage: pulls in runtime packages for simulation and emulation
       (no kernel module or daemon).
   * - ``slash-sim-emu-dev``
     - Metapackage: pulls in development packages for simulation and
       emulation.

Program the Board
=================

.. note::

   This step assumes the AMI driver is already bound to PF0
   (``10ee:50b4``). If your V80 has never been programmed with AVED — for
   example, a brand-new board — first complete :doc:`bootstrap-aved` to
   install AVED via JTAG.

After installing the packages, the board's flash memory must be programmed
with the static shell before the system can be used. This step is required:

- on the **first install** of SLASH, and
- when **upgrading** to a version that changes the static shell (noted in
  the release notes).

It is **not** required after crashes, daemon restarts, or other normal
operations — SLASH reads from flash but never writes to it during regular use.

Program the primary flash partition (replace ``<BDF>`` with the bus address
from ``lspci -d 10ee:``, e.g. ``03:00``):

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         sudo ami_tool cfgmem_program -d <BDF> -t primary -p 0 \
            -i /usr/lib/python3.10/dist-packages/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi

   .. tab-item:: RHEL 9 / Rocky Linux 9 / AlmaLinux 9

      .. code-block:: bash

         sudo ami_tool cfgmem_program -d <BDF> -t primary -p 0 \
            -i /usr/lib/python3.9/site-packages/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi

   .. tab-item:: RHEL 10 / Rocky Linux 10 / AlmaLinux 10

      .. code-block:: bash

         sudo ami_tool cfgmem_program -d <BDF> -t primary -p 0 \
            -i /usr/lib/python3.12/site-packages/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi

After programming completes, reboot the system for the new flash contents
to take effect:

.. code-block:: bash

   sudo reboot

Verify the Kernel Module
========================

DKMS compiles and loads ``slash.ko`` automatically on package install.
To confirm the module is loaded:

.. code-block:: bash

   lsmod | grep slash
   dmesg | grep slash

You should see one line in ``lsmod`` and, in ``dmesg``, messages for each
V80 PCI function discovered.

Each V80 board exposes three PCI functions:

.. list-table::
   :header-rows: 1
   :widths: 15 20 25 40

   * - Function
     - Device ID
     - Driver
     - Purpose
   * - PF0
     - ``0x50B4``
     - ``ami``
     - AVED management interface
   * - PF1
     - ``0x50B5``
     - ``slash_qdma``
     - Queue-based DMA subsystem
   * - PF2
     - ``0x50B6``
     - ``slash_ctl``
     - BAR MMIO access (register reads/writes)

Check that all three appear with their drivers bound:

.. code-block:: bash

   lspci -d 10ee: -k

Start the vrtd Daemon
=====================

The ``vrtd`` package installs a systemd service and socket. Enable it so
that it starts on boot and is running now:

.. code-block:: bash

   sudo systemctl enable --now vrtd

Verify the daemon is reachable:

.. code-block:: bash

   v80-smi list

Each board should show all four readiness checks passing (PF0, PF1, PF2,
VRTD).

Validate the Board
==================

Run the built-in memory integrity and bandwidth test:

.. code-block:: bash

   v80-smi validate -d <BDF>

Replace ``<BDF>`` with the bus address shown by ``v80-smi list``
(e.g. ``03:00``). This tests both HBM and DDR subsystems. A passing result
confirms the hardware, drivers, and daemon are all working correctly.

User Access
===========

By default, only ``root`` and members of the ``vrtadmin`` group have full
device access. To grant a user access:

.. code-block:: bash

   sudo usermod -aG vrtadmin <username>

The user must log out and back in for the group change to take effect.

For fine-grained permission control (per-device, per-operation), edit
``/etc/vrt/vrtd.conf``. See :doc:`/reference/vrtd/configuration` for the
full configuration reference.

Next Steps
==========

- :doc:`device-management` — list, program, reset, and validate devices.
- :doc:`vrtd-configuration` — customise daemon permissions and roles.
- :doc:`/tutorials/user/getting-started` — run your first application.
