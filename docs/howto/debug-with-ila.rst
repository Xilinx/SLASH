..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

########################################
Debugging Kernels with ILA Debug Cores
########################################

SLASH can insert an AXIS Integrated Logic Analyzer (ILA) into the user region so
that kernel interfaces can be observed on hardware from the Vivado Hardware
Manager. Nets to probe are declared in the linker ``config.cfg``; the linker
instantiates the ILA and routes it to the debug hub that the base shell exposes.

Declaring debug nets
====================

Add a ``[debug]`` section to ``config.cfg`` with one ``net=`` entry per interface
to probe, using ``<instance>.<port>`` syntax. Up to 16 nets are supported. See
``examples/00_axilite/config.cfg``:

.. code-block:: ini

   [debug]
   net=increment_0.axis_out
   net=increment_0.m_axi_gmem0
   net=accumulate_0.s_axi_control

Build the design for hardware as usual (for example ``slashkit link -p hw`` or the
CMake ``add_vbin()`` flow). No other configuration is required.

Probe files
===========

Vivado needs two debug probe files (``.ltx``) to open the ILAs, and they must be
loaded in order — the full (base) file first, then the partial (user-region) file.

Full probe file (base shell)
   ``debug_nets.ltx`` describes the static shell's debug network. It is generated
   once when the static shell is built and ships inside the installed ``slashkit``
   package. Both static shells instantiate the debug hub, so pick the file matching
   the shell you built against — ``slashkit/resources/static_shell/debug_nets.ltx``
   for the service shell, ``slashkit/resources/static_shell_compute/debug_nets.ltx``
   for the compute shell.

Partial probe file (user region)
   ``top_i_slash_slash_<project>_inst_0_hw_probes.ltx`` describes the ILAs in your
   design. When the ``[debug]`` section is present, the linker packages it inside
   the built ``.vbin`` (under ``images/``). Extract it with ``tar xzf <name>.vbin``.

Opening the ILAs in the Hardware Manager
========================================

Before the device is programmed with your ``.vbin``, connect the Vivado Hardware
Manager to the board and specify the full probes file first:

.. code-block:: tcl

   open_hw_manager
   connect_hw_server
   open_hw_target
   current_hw_device [get_hw_devices]

   # Full (base) probes must be set before the partial probes.
   set_property PROBES.FILE      {debug_nets.ltx}                                   [current_hw_device]
   set_property FULL_PROBES.FILE {debug_nets.ltx}                                   [current_hw_device]
   refresh_hw_device [current_hw_device]

Then program your ``.vbin`` and specify the partial probes file:

.. code-block:: tcl

   set_property PROBES.FILE      {top_i_slash_slash_<project>_inst_0_hw_probes.ltx} [current_hw_device]
   refresh_hw_device [current_hw_device]

The ILA cores declared in ``[debug]`` then appear under the device and can be
triggered and captured as usual. It is recommended to pause host code execution immediately
after ``.vbin`` programming to allow for ILA trigger set-up before continuing execution.

.. note::

   Vivado requires the full debug probe file to be specified before any partial
   design probe file. Loading the partial probes without the base ``debug_nets.ltx``
   first will fail.
