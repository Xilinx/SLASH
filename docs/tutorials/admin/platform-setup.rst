..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

####################
Bring Up a V80 Board
####################

This tutorial takes a machine on which the SLASH packages are already installed
and brings it into service: verifying the kernel module and daemon, granting
user access, programming the board with the static shell, and validating it.

If the packages are not yet installed, begin with
:doc:`/howto/install-from-packages`, which covers building and installing them.
This tutorial covers the steps performed afterwards, on the machine containing
the board.

Prerequisites
=============

**Hardware:**

- AMD Alveo V80 board installed in a PCIe x8 (or wider) slot.

**Software:**

- Linux (Ubuntu LTS 22.04+, RHEL 9+ or compatible recommended).
- The SLASH runtime packages installed, and ``vrtd`` enabled — see
  :doc:`/howto/install-from-packages`.

Verify the Kernel Module
========================

DKMS compiles and loads ``slash.ko`` automatically on package install.
To confirm the module is loaded:

.. code-block:: bash

   lsmod | grep slash
   dmesg | grep slash

You should see one line in ``lsmod``. If the board already enumerates over
PCIe, ``dmesg`` should also show messages for each V80 PCI function
discovered.

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
     - ``0x50C1``
     - ``slash_qdma``
     - Queue-based DMA subsystem
   * - PF2
     - ``0x50C2``
     - ``slash_ctl``
     - BAR MMIO access (register reads/writes)

The legacy device IDs ``0x50B5`` (PF1) and ``0x50B6`` (PF2) are still accepted
as a fallback for cards carrying a pre-compute-platform bitstream.

For boards already visible over PCIe, check that all three functions appear
with their drivers bound:

.. code-block:: bash

   lspci -d 10ee: -k

Verify the Daemon
=================

Check that ``vrtd`` is running and reachable:

.. code-block:: bash

   v80-smi list

Boards that already enumerate over PCIe should show all four readiness checks
passing (PF0, PF1, PF2, VRTD). A new board that has never been programmed will
not yet be listed; this is expected and is addressed by `Program the Board`_
below.

If the command reports that it cannot reach the daemon, enable it:

.. code-block:: bash

   sudo systemctl enable --now vrtd

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

.. note::
   Access permissions must be granted before the programming step.

Program the Board
=================

Program the board with the SLASH static shell. New users should complete both
steps below:

A. boot a temporary image over JTAG so the board enumerates over PCIe, then
B. use PCIe to write the permanent image to the on-board flash.

If the board is already running AVED or a previous SLASH static shell, skip
step A and perform only step B. In normal operation, repeat step B only when
upgrading to a release that changes the static shell, as noted in the release
notes. It is **not** required after crashes, daemon restarts, or other normal
operations — SLASH reads from flash but never writes to it during regular use.

No system restart is required after either step.

A. Write a Temporary Image via JTAG Boot
----------------------------------------

This step loads the no-FPT static-shell PDI over JTAG. The image is temporary:
it lets the board enumerate over PCIe so that step B can write the permanent
flash image.

A1. Identify the XSDB Target if Multiple Boards Are Present
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

If the host has more than one V80 connected over USB-JTAG, use ``xsdb`` to find
the ``target_id`` for the ``Versal xcv80`` device you want to program:

.. code-block:: text

   source <path-to-vitis>/settings64.sh
   xsdb
   connect
   targets

Use the number from the matching ``Versal xcv80`` target.

A2. Write via JTAG
~~~~~~~~~~~~~~~~~~

If there is a single connected V80, run:

.. code-block:: bash

   source <path-to-vitis>/settings64.sh
   v80-smi write-static-shell --jtag --no-remove-device

For multiple connected V80s, pass the XSDB target ID from step A1:

.. code-block:: bash

   source <path-to-vitis>/settings64.sh
   v80-smi write-static-shell --jtag --no-remove-device --xsdb-target-id <XSDB_TARGET_ID>

B. Flash the Permanent Image via PCIe
-------------------------------------

B1. Identify the Board in ``v80-smi list``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After step A completes, or if the board was already running a previous
version of SLASH, find the board's PCIe bus address:

.. code-block:: bash

   v80-smi list

B2. Write via PCIe
~~~~~~~~~~~~~~~~~~

Program the primary flash partition (replace ``<BDF>`` with the bus address
shown by ``v80-smi list``, e.g. ``03:00``):

.. code-block:: bash

   v80-smi write-static-shell --flash -d <BDF>

The command resolves the packaged static shell PDI, programs it through VRTD,
and resets the board into the programmed partition. No host reboot or system
restart is required.

Validate the Board
==================

Run the built-in memory integrity and bandwidth test:

.. code-block:: bash

   v80-smi validate -d <BDF>

Replace ``<BDF>`` with the bus address shown by ``v80-smi list``
(e.g. ``03:00``). This tests both HBM and DDR subsystems. A passing result
confirms the hardware, drivers, and daemon are all working correctly.

Next Steps
==========

- :doc:`device-management` — list, program, reset, and validate devices.
- :doc:`vrtd-configuration` — customise daemon permissions and roles.
- :doc:`/tutorials/user/getting-started` — run your first application.
