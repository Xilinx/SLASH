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

Vivado needs two debug probe files (``.ltx``), loaded in order -- the base file
first, then the user-region file.

Base probe file (static shell)
   ``debug_nets.ltx`` describes the static shell's debug network. It is generated
   once when the static shell is built and ships inside the installed ``slashkit``
   package. Both static shells instantiate the debug hub, so pick the file matching
   the shell you built against -- ``slashkit/resources/static_shell/debug_nets.ltx``
   for the service shell, ``slashkit/resources/static_shell_compute/debug_nets.ltx``
   for the compute shell.

   Load it as ``PROBES.FILE`` and refresh. Do **not** assign it to
   ``FULL_PROBES.FILE`` -- see the warning below.

Partial probe file (user region)
   ``top_i_slash_slash_<project>_inst_0_hw_probes.ltx`` describes the ILAs in your
   design. When the ``[debug]`` section is present, the linker packages it inside
   the built ``.vbin`` (under ``images/``). Extract it with ``tar xzf <name>.vbin``.

Opening the ILAs in the Hardware Manager
========================================

Connect the Vivado Hardware Manager to the board and load the base probes file
first:

.. code-block:: tcl

   open_hw_manager
   connect_hw_server
   open_hw_target

   # The V80 presents two JTAG devices; the FPGA is xcv80, not arm_dap.
   current_hw_device [lindex [get_hw_devices xcv80*] 0]

   set_property PROBES.FILE {debug_nets.ltx} [current_hw_device]
   refresh_hw_device [current_hw_device]

Then program your ``.vbin`` and load the partial probes file:

.. code-block:: tcl

   set_property PROBES.FILE {top_i_slash_slash_<project>_inst_0_hw_probes.ltx} [current_hw_device]
   refresh_hw_device [current_hw_device]

The ILA cores declared in ``[debug]`` then appear under the device and can be
triggered and captured as usual.

Pause the host (e.g. with a sleep, or a prompt for user input) so the ILA can be armed before the kernel runs -- but place the pause
**after** any ``setFrequency()`` call, not immediately after ``.vbin`` programming.
Arming first and changing the clock afterwards breaks the capture (see the warning
below). The working order is:

1. program the ``.vbin``
2. set the user clock (host ``setFrequency()``, or ``v80-smi debug clockwiz --set``)
3. pause the host (sleep, or request user input)
4. arm the ILA and set triggers
5. release the host (wait for sleep to expire, or provide user input)

.. important::

   Construct the host-side device with programming disabled::

      vrt::Device device(bdf, vbin, /*program=*/false);

   The parameter defaults to ``true``, which reprograms the device on construction
   and wipes an ILA that has already been armed. Note that this also skips applying
   the clock frequency recorded in the ``.vbin``, so the user clock keeps whatever
   rate it already had.

.. warning::

   Load ``debug_nets.ltx`` as ``PROBES.FILE`` only. Assigning it to
   ``FULL_PROBES.FILE`` as well makes ``run_hw_ila`` fail with::

      ERROR: [Labtools 27-188] Use refresh_hw_device command, with a valid
      [debug_nets.ltx] file before running this command.

   The base-then-partial load order is still required; it is the extra
   ``FULL_PROBES.FILE`` assignment alone that breaks the core. The core still
   arms and triggers in that state, so the failure looks like an ILA whose
   waveform window never updates rather than an error at set-up time.

.. warning::

   Do **not** call ``vrt::Device::setFrequency()`` while an ILA is armed. Changing
   the user clock under an armed core leaves it able to trigger but unable to hand
   its buffer back, and the upload fails with::

      ERROR: [Xicom 50-298] ILA core [...] - ChipScope Service AxisILA upload:
      integer modulo by zero

   As with ``FULL_PROBES.FILE`` above, the core arms and triggers normally, so this
   presents as a waveform window that never updates rather than as an error when
   the clock is set. It does not depend on the frequency requested -- the same
   failure occurs whether the new rate is above or below the one the design was
   timed at.

   Set the clock *before* arming, either from the host or with
   ``v80-smi debug clockwiz -d <BDF> --set <hz> --region user``, and leave it alone
   for the rest of the session.
