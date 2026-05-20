..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#########################
Bootstrapping with AVED
#########################

This tutorial walks an administrator through the one-time JTAG step
required to install **AVED** (the Alveo Versal Example Design) on an
AMD Alveo V80 board. AVED provides the AMC firmware and the PCIe
management function (PF0, device ID ``0x50B4``) that the SLASH stack
binds to via the ``ami`` kernel driver.

When you need this tutorial
===========================

The rest of the SLASH platform-setup flow — including
``ami_tool cfgmem_program`` for writing the SLASH static shell —
requires that ``ami`` is already bound to PF0. That, in turn, requires
a valid AVED image in the V80's OSPI flash. Follow this tutorial when:

- the V80 is **brand new** and has never had AVED programmed, or
- the V80's OSPI flash has been corrupted or wiped and PF0 no longer
  enumerates over PCIe, or ami repports errors (such as ``NO_AMC```)

Boards that already enumerate PF0 (visible as ``10ee:50b4`` in
``lspci``) can skip this tutorial and go directly to
:doc:`platform-setup`.

Prerequisites
=============

**Hardware:**

- AMD Alveo V80 installed in a PCIe slot.
- USB cable from the host to the V80's onboard USB-JTAG.

**Software:**

- AMD Vivado 2025.1.

Download the AVED Deployment Archive
=====================================

AVED is published by AMD as a prebuilt deployment archive. Download
the archive for the V80 from the AVED documentation portal:

- AVED documentation: https://xilinx.github.io/AVED/
- V80 member-portal page: https://www.xilinx.com/member/v80.html

Extract the archive on the host. The relevant files for this tutorial
are located under ``flash_setup/``:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - File
     - Purpose
   * - ``flash_setup/versal_change_boot_mode.tcl``
     - XSDB script that switches the Versal device to JTAG boot mode.
   * - ``flash_setup/v80_initialization.pdi``
     - Initialization PDI loaded over JTAG before flashing OSPI.
   * - ``flash_setup/fpt_setup_<vbnv>_<release>.pdi``
     - Flash Partition Table setup PDI written to OSPI.

Switch the V80 to JTAG Boot Mode
================================

By default the V80 boots from OSPI. To program a fresh board over
JTAG, the Versal device must first be switched to JTAG boot mode.
Source the Vivado settings, then launch ``xsdb`` and source the
``versal_change_boot_mode.tcl`` script shipped with the archive:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   xsdb

At the ``xsdb%`` prompt:

.. code-block:: tcl

   connect
   targets -set -filter {name =~ "Versal*"}
   source flash_setup/versal_change_boot_mode.tcl

The script reconfigures the boot-mode register on the Versal device
so that subsequent JTAG operations from Hardware Manager will be
accepted.

See `AVED JTAG Boot Recovery
<https://xilinx.github.io/AVED/amd_v80_gen5x8_exdes_1_20231204/AVED+JTAG+Boot+Recovery.html>`_
for the upstream reference.

Program OSPI Flash via Vivado Hardware Manager
==============================================

With the V80 in JTAG boot mode, launch Vivado Hardware Manager and
program the OSPI flash:

1. Launch Vivado and open Hardware Manager
   (*Flow* → *Open Hardware Manager*).
2. *Open Target* → *Auto Connect*. The V80 should appear as
   ``xcv80_1``.
3. Right-click ``xcv80_1`` → *Add Configuration Memory Device*.
4. Select the ``cfgmem-2048-ospi-x8-single`` part.`
5. Program the configuration memory using
   ``flash_setup/fpt_setup_<vbnv>_<release>.pdi`` together with
   ``flash_setup/v80_initialization.pdi``. For the address range
   select **Entire Configuration Memory Device**.
6. Wait for Hardware Manager to report **Flash Programming
   Completed Successfully**.

For the upstream step-by-step reference, see `AVED Updating FPT Image
in Flash <https://xilinx.github.io/AVED/amd_v80_gen5x8_24.1_20241002/AVED+Updating+FPT+Image+in+Flash.html>`_
and `AVED Device Programming
<https://xilinx.github.io/AVED/amd_v80_gen5x8_exdes_2_20240408/AVED+-+Device+Programming.html>`_.

Cold-Reboot the Host
====================

A full power cycle is required after flashing — a soft ``reboot``
will not re-read the Versal boot-mode pins. Shut the host down
completely, then power it back on:

.. code-block:: bash

   sudo shutdown -h now

After the host powers back on, the V80 boots from OSPI and AVED
becomes active.

Verify
======

Confirm that PF0 enumerates on the PCIe bus:

.. code-block:: bash

   lspci -d 10ee:50b4

You should see one entry per V80 board. If the ``ami`` driver is
already installed on this host, ``ami_tool`` will also report the
card:

.. code-block:: bash

   sudo ami_tool overview

The reported ``logic_uuid`` should match the UUID listed in the
AVED archive's ``version.json``.

.. note::

   If you are following the package build flow on a fresh board, you
   will not have the ``ami`` driver or ``ami_tool`` installed yet. That
   is expected at this stage — confirming that the board enumerates on
   PCIe via ``lspci`` is sufficient. Continue on to :doc:`platform-setup`
   to install the SLASH stack, which brings in ``ami`` and ``ami_tool``.

Next Steps
==========

With AVED bootstrapped and PF0 visible, continue with the regular
platform-setup flow:

- :doc:`platform-setup` — install the SLASH stack and program the
  SLASH static shell over PCIe with ``ami_tool cfgmem_program``.
