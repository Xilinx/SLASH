..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

########################
Build and Install SLASH
########################

This guide covers the supported method of installing the SLASH stack: building
the Debian or RPM packages from the repository, then installing them with
``apt`` or ``dnf``. It is the same flow used by SLASH continuous integration.

Pre-built packages are not distributed. The static shell that every hardware
design is linked against must be built by Vivado and cannot be redistributed,
so each site builds the packages once for itself. This is a long build; see
:ref:`what-the-build-does`.

The build and the installation may be performed on different machines. The
**build machine** requires the AMD toolchain and a set of development packages,
but no V80 board. The **target machine** requires a board and kernel headers,
but no toolchain. The two are frequently the same machine; the distinction is
noted where it applies.

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

Before You Start
================

Hardware and OS
---------------

- An AMD Alveo V80 board in a PCIe x8 (or wider) slot, on the target machine.
- Linux: Ubuntu LTS 22.04+, RHEL 9+ or compatible. Other distributions may work
  but are not tested.

Get the source
--------------

.. code-block:: bash

   git clone -b dev https://github.com/Xilinx/SLASH.git
   cd SLASH
   git submodule update --init submodules/AVED submodules/qdma_drv submodules/Versal-DCMAC

Name those three submodules explicitly. Do **not** clone with
``--recurse-submodules``: an optional QEMU submodule that SLASH does not build
pulls in further submodules, one of which no longer exists upstream, causing the
clone to fail. The `repository README
<https://github.com/Xilinx/SLASH#prerequisites>`_ documents this, and describes
how to enable the RP1 firmware test.

Unless stated otherwise, all commands on this page are run from the repository
root.

Build-machine dependencies
--------------------------

The build machine requires a C/C++ toolchain, a number of library headers and
the distribution packaging tools. A script installs the correct set for each
supported distribution:

.. tab-set::

   .. tab-item:: Ubuntu

      .. code-block:: bash

         sudo ./scripts/install-dev-deps-ubuntu.sh

   .. tab-item:: RHEL / Rocky Linux / AlmaLinux

      .. code-block:: bash

         sudo ./scripts/install-dev-deps-rhel.sh

These scripts account for differences between releases; Ubuntu 22.04 and 24.04,
for example, package debhelper DKMS support differently. Use them in preference
to a manually assembled package list.

.. dropdown:: Installing the dependencies manually

   The equivalent package sets, for distributions the scripts do not cover or
   where manual installation is preferred.

   .. tab-set::

      .. tab-item:: Ubuntu 22.04

         .. code-block:: bash

            sudo apt install \
              build-essential cmake ninja-build pkg-config rsync git dkms \
              debhelper dpkg-dev apt-utils \
              python3 python3-pip \
              libcli11-dev libinih-dev libjsoncpp-dev \
              libsystemd-dev libxml2-dev libzmq3-dev zlib1g-dev

      .. tab-item:: Ubuntu 24.04 / 26.04

         .. code-block:: bash

            sudo apt install \
              build-essential cmake ninja-build pkg-config rsync git dkms dh-dkms \
              debhelper dpkg-dev apt-utils \
              python3 python3-pip \
              libcli11-dev libinih-dev libjsoncpp-dev \
              libsystemd-dev libxml2-dev libzmq3-dev cppzmq-dev zlib1g-dev

      .. tab-item:: RHEL / Rocky / Alma 9

         .. code-block:: bash

            sudo dnf install \
              gcc gcc-c++ cmake make ninja-build pkg-config rsync git dkms \
              rpm-build createrepo_c systemd-rpm-macros \
              python3.11 python3.11-pip \
              cli11-devel cppzmq-devel inih-devel jsoncpp-devel \
              libxml2-devel systemd-devel \
              zeromq-devel zlib-devel

      .. tab-item:: RHEL / Rocky / Alma 10

         .. code-block:: bash

            sudo dnf install \
              gcc gcc-c++ cmake make ninja-build pkg-config rsync git dkms \
              rpm-build createrepo_c systemd-rpm-macros \
              python3 python3-pip \
              cli11-devel cppzmq-devel inih-devel jsoncpp-devel \
              libxml2-devel systemd-devel \
              zeromq-devel zlib-devel

Target-machine dependencies
---------------------------

Every machine that will run the packages needs kernel headers, so that DKMS can
compile the kernel module against the running kernel:

.. tab-set::

   .. tab-item:: Debian / Ubuntu

      .. code-block:: bash

         sudo apt install linux-headers-$(uname -r)

   .. tab-item:: RHEL / Rocky / Fedora

      .. code-block:: bash

         sudo dnf install kernel-devel-$(uname -r)

Everything else — ``dkms``, ``gcc``, the shared libraries — is declared as a
dependency by the packages themselves and pulled in automatically.

AMD tools and license
---------------------

Building the static shell requires Vivado and Vitis **2025.1** and a **Vivado
Enterprise license**. Both must be installed on the machine that runs them:
normally the build machine, or the cluster nodes when
:ref:`offloading to a cluster <building-on-a-cluster>`, in which case the build
machine does not require them.

Source both before building:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis>/settings64.sh

For ``csh``/``tcsh`` users, use ``settings64.csh``. Releases other than 2025.1
are not supported.

Verify the environment with ``which v++``; the packaging script tests for this
binary and exits if it is absent.

.. note::

   Vivado Enterprise license configuration is site-specific. Contact your
   license administrator if you are unsure how licenses are served at your
   site.

SMBus IP
--------

The SMBus IP (``xilinx.com:ip:smbus:1.1``) used for board management is
**not included** in this repository and is not bundled with Vivado. It must
be downloaded separately from the AMD member portal and placed into the
local IP repository before building:

1. Download the SMBus IP from https://www.xilinx.com/member/v80.html
   (AMD account required).
2. Copy the downloaded IP directory into
   ``linker/slashkit/resources/base/common/iprepo/``
   so that Vivado can locate it during synthesis. The directory name must
   begin with ``smbus``; a release-date suffix is permitted.

Confirm the result before starting the build:

.. code-block:: bash

   ls -d linker/slashkit/resources/base/common/iprepo/smbus*/

If this command prints no path, the packaging script stops with an error before
performing any work.

See the `AVED rebuild guide <https://xilinx.github.io/AVED/>`_ for
additional details.

.. _build-the-packages:

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

Pass ``--noninteractive`` to suppress the confirmation prompt shown when an
existing build is about to be overwritten. This is required when the build is
run detached, and is what continuous integration uses.

.. note::

   ``dpkg-buildpackage`` writes the ``.dsc`` and source tarball to the
   **parent** of the repository directory and cannot be configured otherwise.
   The directory containing the checkout must therefore be writable.

.. _what-the-build-does:

What the build does, and how long it takes
------------------------------------------

Most of the elapsed time is spent in Vivado. The script builds the base IP, then
runs ``scripts/root-design-build.sh``, which synthesises and implements **two**
static shells, the service shell and the compute shell, sequentially. Compiling
the C++ components, building the AMI package and assembling the packages take
minutes by comparison.

A reference run on a 16-core machine took 7.5 hours for the service shell and
5.5 hours for the compute shell, with approximately two further hours of
non-Vivado work, giving **17.5 hours** in total. Vivado required up to **140 GB**
of resident memory during implementation, so the machine must be sized for
memory as well as core count. Allow tens of gigabytes of scratch space in the
working tree. Run the build detached, under ``nohup``, ``tmux`` or ``screen``.

The final build step creates a Python virtual environment and builds the
``slashkit`` wheel, downloading its build backend from PyPI. This step runs on
the machine that started the script, not on the cluster when offloading, so
that machine requires outbound network access. Without it the build fails at
the end, after the Vivado work has completed.

If a prerequisite is missing, the script reports it and stops before performing
any work:

.. code-block:: text

   ERROR: v++ not found in PATH. Source Vitis 2025.1 before building:
     source <path-to-vitis>/settings64.sh

   ERROR: SMBus IP (xilinx.com:ip:smbus:1.1) not found in
   linker/slashkit/resources/base/common/iprepo/.

Rebuilding packages without rebuilding the shell
------------------------------------------------

Once the static shells exist, changes to the runtime, the daemon or the CLI do
not require rebuilding them. Set ``SLASH_PKG_SKIP_ROOT_DESIGN_BUILD`` to reuse
the existing shells:

.. code-block:: bash

   SLASH_PKG_SKIP_ROOT_DESIGN_BUILD=1 scripts/package-deb.sh

This reduces the build to a few minutes. It requires
``linker/slashkit/resources/static_shell/`` and
``linker/slashkit/resources/static_shell_compute/`` to be populated already.
Setting the variable also skips the prerequisite checks described above, so a
missing shell is reported later as a packaging error rather than as a clear
diagnostic.

.. _building-on-a-cluster:

Building on a cluster
---------------------

Where a batch scheduler is available, the Vivado work can be submitted to it
while the build is orchestrated from the machine that started it. Set
``SLASH_TOOL_LAUNCHER`` to a submit wrapper and run the same script:

.. code-block:: bash

   export SLASH_TOOL_LAUNCHER=/path/to/your/submit-wrapper
   scripts/package-deb.sh --noninteractive

The submit host then does not require a local Vivado installation; the ``v++``
check is skipped when a launcher is set. Everything the build uses — the
checkout, the build directory and the toolchain — must reside on storage
visible from both hosts at identical paths.

``SLASH_ROOT_DESIGN_JOBS`` sets the Vivado job count (default 8), and
``SLASH_VIVADO_BIN`` the name of the Vivado binary where it is not ``vivado``.

.. warning::

   ``SLASH_ROOT_DESIGN_JOBS`` does **not** size the scheduler reservation. It
   sets the number of parallel jobs Vivado runs; the core count granted by the
   scheduler is determined by the wrapper. Both must be set, and to the same
   value. Requesting more Vivado jobs than the reservation has cores
   oversubscribes the node, which some sites treat as grounds for terminating
   the job.

   Wall-clock limits apply per job. Each shell is submitted separately, so each
   limit must cover a single shell — approximately 8 hours — rather than the
   17.5-hour total. Reserve approximately 150 GB of memory per job.

   The two jobs are submitted sequentially and each is waited on, so the
   orchestrating process remains idle for extended periods. Run it under
   ``nohup`` or a terminal multiplexer. Terminating it while a job is running
   leaves that job holding its reservation, and the job must then be cancelled
   manually.

A reference wrapper for IBM Spectrum LSF, providing per-step reservation sizing
and a preflight check, is supplied in ``scripts/lsf/``. The corresponding
settings are ``SLASH_LSF_CORES_static_shell`` and
``SLASH_LSF_WALLTIME_static_shell``, together with the matching
``_static_shell_compute`` pair, in ``site.conf``; copy ``site.conf.example`` and
edit it. See :doc:`offload-builds-to-a-cluster` for the full set of requirements
a wrapper must satisfy.

Install the AMI Driver
=======================

The V80 board's PF0 function (device ID ``0x50B4``) is managed by the
**AMI** (AVED Management Interface) kernel module. Install it before the
rest of the SLASH stack so it can bind to PF0 when the board enumerates over
PCIe.

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

If the board already enumerates over PCIe, verify that ``ami`` is bound to PF0:

.. code-block:: bash

   lspci -d 10ee:50b4 -k

You should see ``Kernel driver in use: ami``. If no PF0 is visible yet, continue
to `Program the Board`_ and boot the temporary JTAG image first.

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

Check whether the board is already reachable through the daemon:

.. code-block:: bash

   v80-smi list

Boards that already enumerate over PCIe should show all four readiness
indicators passing (PF0, PF1, PF2, VRTD). A new board will not yet be listed;
see `Program the Board`_ below.

Program the Board
==================

With the packages installed and ``vrtd`` running, the remaining step is to write
the SLASH static shell to the board. A board that has not previously run SLASH
requires a temporary image to be booted over JTAG first, so that it enumerates
over PCIe; a board already running AVED or an earlier SLASH shell can be flashed
directly.

:doc:`/tutorials/admin/platform-setup` describes both procedures, together with
verification of the kernel module and configuration of non-root user access.

On completion, ``v80-smi list`` reports the board with all four readiness
indicators passing.

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
- CMake package files for ``slash``, ``vrt``, ``vrtd`` and ``SlashTools``, in
  the distribution's library directory — ``/usr/lib/x86_64-linux-gnu/cmake/`` on
  Debian and Ubuntu, ``/usr/lib64/cmake/`` on RHEL and derivatives. Both are on
  CMake's default search path, so ``find_package`` needs no hint
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

Removal
-------

``scripts/uninstall-slash.sh`` returns the machine to an uninstalled state. It
stops ``vrtd``, unloads the kernel modules and removes all SLASH packages, in
that order. The order is significant: removing the packages first would unload
the module while a board is still bound to it.

.. code-block:: bash

   sudo scripts/uninstall-slash.sh

Pass ``--device`` with the board's PCIe address to detach the board from the bus
as well, which is required before reprogramming or reseating it:

.. code-block:: bash

   sudo scripts/uninstall-slash.sh --device 0000:21:00

``--dry-run`` reports the actions that would be taken without performing them;
``--help`` lists the remaining options. The script may be run repeatedly: each
step is skipped where there is nothing to do.

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

Building Individual Components
==============================

This section covers development work on SLASH itself, and is not an alternative
installation method. When iterating on the daemon or the CLI, rebuilding a
single component takes seconds where a package build takes hours. To install
SLASH on a machine, follow the procedure above.

Each component is a standalone CMake project:

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Component
     - Directory
     - Notes
   * - ``slash`` module
     - ``driver/``
     - Plain ``make``, not CMake. Produces ``slash.ko``.
   * - ``libslash``
     - ``driver/libslash/``
     - No SLASH dependencies. Build this first.
   * - ``vrtd``
     - ``vrt/vrtd/``
     - Needs ``libslash``. Also produces ``libvrtd`` and ``libvrtdpp``.
       ``-DVRTD_INCLUDE_LIBSLASH=ON`` builds ``libslash`` as a subdirectory
       instead of using the installed one.
   * - ``VRT``
     - ``vrt/``
     - Needs ``vrtd``. ``-DVRT_INCLUDE_VRTD=ON`` builds it as a subdirectory.
   * - ``v80-smi``
     - ``smi/``
     - Needs VRT, and a C++20 compiler — the rest of the tree is C++17.
       ``-DSMI_INCLUDE_VRT=ON`` builds VRT as a subdirectory.

Built in that order, each component finds its dependencies already installed:

.. code-block:: bash

   cd vrt/vrtd
   cmake -B build -S . -G Ninja
   cmake --build build
   sudo cmake --install build

The kernel module is the exception:

.. code-block:: bash

   cd driver
   make
   sudo insmod slash.ko

It takes ``qdma_num_threads=N`` (default 8) and
``qdma_debugfs_path=/sys/kernel/debug`` as module parameters.

The ``-DXXX_INCLUDE_YYY=ON`` options allow a component to be built without
installing its dependencies system-wide, which is normally preferable on a
development machine.

.. warning::

   A component installed in this way and the packaged copy of the same
   component occupy the same paths, and the most recently installed copy takes
   precedence. ``apt`` and ``dnf`` hold no record of files installed by CMake,
   so those files remain after the packages are removed. Use one method or the
   other on a given machine. To return to packages after building manually, run
   ``sudo scripts/uninstall-slash.sh`` and delete any remaining files it
   reports.

Each component's ``README.md`` documents its own options, tests and layout in
more detail.

Examples
--------

The examples are standalone CMake projects, and can be built against the
repository tree rather than against an installed SLASH:

.. code-block:: bash

   cd examples/00_axilite
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build

Without ``-DSLASH_USE_REPO=ON`` they build against the installed packages.
Building the FPGA artefacts requires Vivado and Vitis 2025.1 to be sourced, as
described above. See :doc:`/tutorials/user/your-first-kernel`.
