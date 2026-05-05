..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

######################
Install from Packages
######################

This guide covers installing the SLASH stack from pre-built Debian or RPM
packages. This is the recommended installation method for most users.

For building everything from source (e.g. for development or unsupported
distributions), see :doc:`build-from-source`.

.. contents:: On this page
   :depth: 2
   :local:

Package Groups
==============

SLASH is split into focused packages so you install only what you need.

Runtime packages (required on every host with a V80 board):

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Package
     - Purpose
   * - ``slash-dkms``
     - DKMS source for the ``slash`` kernel module. Compiles and installs
       ``slash.ko`` for the running kernel automatically.
   * - ``libslash``
     - Shared library for interacting with the kernel module over the
       driver's character device.
   * - ``vrtd``
     - Daemon that multiplexes device access, enforces permissions, and
       manages board state. Includes systemd units, udev rules, and
       default ``/etc/vrt/vrtd.conf``.
   * - ``libvrtd``
     - Client libraries (``libvrtd`` C wire-protocol, ``libvrtdpp`` C++
       RAII wrapper) for applications that communicate with the daemon.
   * - ``libvrt``
     - VRT C++ runtime library — the high-level API for kernels, buffers,
       and device control.
   * - ``v80-smi``
     - Board management CLI: ``list``, ``inspect``, ``program``,
       ``query``, ``reset``, ``validate``.

Development packages (required when building applications or HLS kernels):

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Package
     - Purpose
   * - ``libslash-dev``
     - Headers and CMake targets for ``libslash``.
   * - ``libvrtd-dev``
     - Headers and CMake targets for ``libvrtd`` / ``libvrtdpp``.
   * - ``libvrt-dev``
     - Headers and CMake targets for ``libvrt``.
   * - ``slashkit``
     - Python-based kernel linker that packages compiled HLS IP into
       ``.vbin`` archives. Provides the ``build_hls_dir()`` and
       ``add_vbin()`` CMake functions via the ``SlashTools`` module.

Convenience metapackages:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Package
     - Pulls in
   * - ``slash``
     - All runtime packages above except ``v80-smi`` (install separately).
   * - ``slash-dev``
     - All development packages above.
   * - ``slash-sim-emu``
     - Runtime subset for simulation/emulation hosts (no board required).
   * - ``slash-sim-emu-dev``
     - Development subset for simulation/emulation.

Build the Packages
==================

All packages — including the AMI driver package — are produced by a single
script run from the repository root:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         scripts/package-deb.sh

      Packages are written to ``./deb/``.

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         scripts/package-rpm.sh

      Packages are written to ``./rpm/``.

Both scripts call ``scripts/package-ami.sh`` internally, so the AMI package
is built and placed in the same output directory as the SLASH packages.

Install System Prerequisites
=============================

Every target machine needs kernel headers so that DKMS can compile the
kernel module:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install linux-headers-$(uname -r)

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         sudo dnf install kernel-devel-$(uname -r)

Other install-time dependencies (``dkms``, ``gcc``, libraries, etc.) are
declared by the packages themselves and will be pulled from the system
repositories automatically.

Install the AMI Driver
=======================

The V80 board's PF0 function (device ID ``0x50B4``) is managed by the
**AMI** (AVED Management Interface) kernel module. Install it before the
rest of the SLASH stack — ``vrtd`` requires AMI to be bound to PF0 to
manage the board.

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install ./deb/ami_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         sudo dnf install ./rpm/ami-<version>-1.<dist>.x86_64.rpm

.. warning::

   If AMI is already installed on this system — for example, built from
   source or installed from a separate vendor package — the generated AMI
   package may conflict with the existing installation. Either remove the
   existing AMI installation before proceeding, or skip this step and
   ensure your installed AMI version is compatible with this SLASH release.

After installation, verify that ``ami`` is bound to PF0:

.. code-block:: bash

   lspci -d 10ee:50b4 -k

You should see ``Kernel driver in use: ami``.

Install Runtime Packages
=========================

When installing from local package files, list all packages explicitly so
that the package manager can satisfy the inter-package dependencies in a
single transaction:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install \
           ./deb/slash-dkms_<version>_all.deb \
           ./deb/libslash_<version>_amd64.deb \
           ./deb/vrtd_<version>_amd64.deb \
           ./deb/libvrtd_<version>_amd64.deb \
           ./deb/libvrt_<version>_amd64.deb \
           ./deb/v80-smi_<version>_amd64.deb \
           ./deb/slashkit_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky / Fedora

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

   The ``slash`` metapackage and metapackage-based installs
   (``sudo apt install slash``) only work when the packages are served
   from a configured APT or DNF/YUM repository. Installing a bare
   metapackage ``.deb`` or ``.rpm`` from a local file will fail because
   the package manager cannot resolve its dependencies against local
   files.

After installation, DKMS automatically compiles and inserts the kernel
module for the running kernel. Verify it loaded:

.. code-block:: bash

   lsmod | grep slash

Start and Enable the Daemon
============================

The ``vrtd`` package installs a systemd service and socket. Enable it so
that it starts on boot and is running now:

.. code-block:: bash

   sudo systemctl enable --now vrtd

Check that the board is reachable through the daemon:

.. code-block:: bash

   v80-smi list

You should see one entry per V80 board with all four readiness indicators
passing (PF0, PF1, PF2, VRTD).

Program the Board
==================

.. note::

   This step assumes the AMI driver is already bound to PF0
   (``10ee:50b4``). If your V80 has never been programmed with AVED — for
   example, a brand-new board — first complete
   :doc:`/tutorials/admin/bootstrap-aved` to install AVED via JTAG.

After installing the packages, the board's flash memory must be programmed
with the static shell before the system can be used. This step is required:

- on the **first install** of SLASH, and
- when **upgrading** to a version that changes the static shell (noted in
  the release notes).

It is **not** required after crashes, daemon restarts, or other normal
operations — SLASH reads from flash but never writes to it during regular use.

Program the primary flash partition (replace ``<BDF>`` with the bus address
from ``v80-smi list``, e.g. ``03:00``):

.. code-block:: bash

   # For Ubuntu 22.04
   sudo ami_tool cfgmem_program -d <BDF> -t primary -p 0 \
      -i /usr/lib/python3.10/dist-packages/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi
      
   # For Rocky 9
   sudo ami_tool cfgmem_program -d <BDF> -t primary -p 0 \
      -i /usr/lib/python3.9/site-packages/slashkit/resources/static_shell/amd_v80_gen5x8_25.1.pdi

After programming completes, reboot the system for the new flash contents
to take effect:

.. code-block:: bash

   sudo reboot

Install Development Packages
==============================

If you are writing applications against the VRT API or compiling HLS
kernels, install the development metapackage:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install \
           ./deb/libslash-dev_<version>_amd64.deb \
           ./deb/libvrtd-dev_<version>_amd64.deb \
           ./deb/libvrt-dev_<version>_amd64.deb \
           ./deb/slashkit_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         sudo dnf install \
           ./rpm/libslash-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrtd-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrt-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/slashkit-<version>-1.<dist>.x86_64.rpm

This installs:

- C++ headers under ``/usr/include/vrt/``, ``/usr/include/vrtd/``, and
  ``/usr/include/slash/``
- CMake package files under ``/usr/lib/cmake/``
- The ``slashkit`` linker and the ``SlashTools`` CMake module

CMake projects can then discover VRT with:

.. code-block:: cmake

   find_package(vrt REQUIRED CONFIG)
   target_link_libraries(my_app PRIVATE vrt::vrt)

Before building HLS kernels or vrtbin files, source the Vivado and Vitis HLS
environment in your shell:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

For ``csh``/``tcsh`` shells, use ``settings64.csh`` instead. SLASH has been
built and tested against **Vivado/Vitis 2025.1**; using other versions may
cause breakage.

See :doc:`use-cmake-modules` for details on using the CMake integration.

Install Emulation / Simulation Packages
========================================

To develop or run kernels in emulation or simulation without a physical
V80 board, install the ``slash-sim-emu`` subset:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install \
           ./deb/libslash_<version>_amd64.deb \
           ./deb/libvrtd_<version>_amd64.deb \
           ./deb/libvrt_<version>_amd64.deb

      For building emu/sim kernels, also install:

      .. code-block:: bash

         sudo apt install \
           ./deb/libslash-dev_<version>_amd64.deb \
           ./deb/libvrtd-dev_<version>_amd64.deb \
           ./deb/libvrt-dev_<version>_amd64.deb \
           ./deb/slashkit_<version>_amd64.deb

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         sudo dnf install \
           ./rpm/libslash-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrtd-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrt-<version>-1.<dist>.x86_64.rpm

      For building emu/sim kernels, also install:

      .. code-block:: bash

         sudo dnf install \
           ./rpm/libslash-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrtd-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrt-devel-<version>-1.<dist>.x86_64.rpm \
           ./rpm/slashkit-<version>-1.<dist>.x86_64.rpm

No board and no kernel module are required on emulation/simulation hosts.
The daemon is still needed if any component connects to ``vrtd``, but you
can point applications at the emulation platform directly.

See :doc:`/tutorials/user/emulation-and-simulation` for a walkthrough.

Upgrade and Removal
====================

.. note::

   If the new version changes the static shell, re-program the board flash
   after upgrading the packages. See `Program the Board`_ above.

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      Re-run ``scripts/package-deb.sh`` to produce the new packages, then
      reinstall with ``apt install`` — apt handles upgrades transparently
      when given local ``.deb`` files:

      .. code-block:: bash

         sudo apt install \
           ./deb/ami_<new-version>_amd64.deb \
           ./deb/slash-dkms_<new-version>_all.deb \
           ./deb/libslash_<new-version>_amd64.deb \
           ./deb/vrtd_<new-version>_amd64.deb \
           ./deb/libvrtd_<new-version>_amd64.deb \
           ./deb/libvrt_<new-version>_amd64.deb \
           ./deb/v80-smi_<new-version>_amd64.deb \
           ./deb/slashkit_<new-version>_amd64.deb

      To remove all SLASH and AMI packages:

      .. code-block:: bash

         sudo apt remove ami slash-dkms libslash libvrtd libvrt \
                         v80-smi slashkit vrtd
         sudo apt autoremove

   .. tab-item:: RHEL / Rocky / Fedora

      Re-run ``scripts/package-rpm.sh``, then upgrade:

      .. code-block:: bash

         sudo dnf upgrade \
           ./rpm/ami-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/slash-dkms-<new-version>-1.<dist>.noarch.rpm \
           ./rpm/libslash-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/vrtd-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrtd-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/libvrt-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/v80-smi-<new-version>-1.<dist>.x86_64.rpm \
           ./rpm/slashkit-<new-version>-1.<dist>.x86_64.rpm

      To remove:

      .. code-block:: bash

         sudo dnf remove ami slash-dkms libslash libvrtd libvrt v80-smi slashkit vrtd

.. note::

   Removing ``slash-dkms`` automatically removes the kernel module from
   DKMS management and unloads it if currently loaded.

Troubleshooting
===============

**Kernel module did not load after install**

   DKMS compiles during package installation. If headers were missing at
   that point, install them and rebuild:

   .. code-block:: bash

      sudo apt install linux-headers-$(uname -r)   # Debian/Ubuntu
      sudo dkms build slash/0.1
      sudo dkms install slash/0.1

**vrtd fails to start**

   Check the journal for errors:

   .. code-block:: bash

      sudo journalctl -u vrtd --no-pager

   Common causes: kernel module not loaded, or board not detected by the
   OS (check ``lspci -d 10ee:``).

**v80-smi list shows no boards**

   Verify the module is loaded (``lsmod | grep slash``) and that the
   daemon is running (``systemctl status vrtd``).

**Permission denied**

   The user must be in the ``vrtadmin`` group:

   .. code-block:: bash

      sudo usermod -aG vrtadmin <username>

   Log out and back in for the change to take effect.
