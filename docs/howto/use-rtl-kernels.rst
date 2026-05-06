..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

##################
Using RTL Kernels
##################

RTL IP following the IP-XACT standard can be used directly as kernels in SLASH.
No intermediate compiled-object container is needed — the SLASH linker consumes
IP-XACT directories as produced by the Vivado IP Integrator. This guide is a
re-synthesis of the Vitis/XRT guide "Packaging RTL Kernels" (UG1393) adapted for
SLASH. Readers familiar with Vitis/XRT should consult the
`Differences from Vitis/XRT`_ section below for a concise summary of what has
changed.

.. note::

   The interface requirements for RTL kernels in SLASH are not yet formally
   enforced. Treat the requirements stated here as strong guidelines that are
   expected to hold in practice.

Differences from Vitis/XRT
===========================

The general requirements for RTL kernels in SLASH are similar to those in Vitis/XRT
(UG1393, "Packaging RTL Kernels"), but the following terminology and concept changes
apply throughout.

.. list-table::
   :header-rows: 1
   :widths: 35 35 30

   * - Vitis/XRT
     - SLASH
     - Notes
   * - Software emulation
     - Emulation
     - Behavioural C-model-based execution
   * - Hardware emulation
     - Simulation
     - RTL Verilog simulation
   * - ``.xclbin`` device binary
     - ``.vbin`` device binary
     - See :doc:`/explanation/vrtbin-format`
   * - Compiled object file (``.xo``)
     - Raw IP-XACT directory
     - Vivado IP Integrator output; no additional packaging step needed
   * - XRT-managed vs user-managed kernel
     - No hard distinction
     - See below

**XRT-managed vs user-managed kernels**

In Vitis/XRT there is a formal distinction between XRT-managed kernels — which must
implement the ``ap_ctrl_hs`` or ``ap_ctrl_chain`` control protocol — and user-managed
kernels, which can implement any control scheme. SLASH does not enforce this distinction
as a formal category.

The VRT Kernel API provides two levels of use that can be applied to the same kernel:

- **Direct register access** via ``kernel.read(offset)`` and
  ``kernel.write(offset, value)``: works with any RTL kernel regardless of its
  control scheme.
- **High-level launch API** — ``kernel.setArg(...)``, ``kernel.call(...)``,
  ``kernel.start()``, ``kernel.wait()``: these implement the ``ap_ctrl_hs`` handshake
  protocol on top of ``read`` and ``write``. A kernel intended to be used with this
  API must expose the control register map described in
  `Control Interface Register Map`_.

There is no hard boundary between the two. A kernel may expose a control register and
be used with ``call()``/``wait()`` for convenience, or it may implement a completely
custom scheme driven entirely through ``read()``/``write()``.

Kernel Interface Requirements
==============================

To allow the SLASH linker to connect the kernel to the platform and to other kernels,
the RTL IP is expected to expose the following interfaces. All ports must be associated
with a bus interface in the IP-XACT packaging.

.. list-table::
   :header-rows: 1
   :widths: 20 25 55

   * - Port / Interface
     - Description
     - Notes
   * - Clock
     - One or more clock inputs
     - At least one clock is required. The clock can be named anything.
   * - Reset
     - Active-Low reset input
     - Optional. Should be associated with a clock via the
       ``ASSOCIATED_RESET`` property. Internally pipeline the reset to
       improve timing. The signal should be driven by a synchronous reset in
       the associated clock domain.
   * - ``interrupt``
     - Active-High interrupt
     - Optional. When used, the port name must be exactly ``interrupt``.
   * - ``s_axi_control``
     - AXI4-Lite slave control interface
     - Optional for purely data-driven kernels. When present, the interface
       name must be exactly ``s_axi_control`` (case-sensitive).
   * - ``m_axi_*``
     - AXI4 memory-mapped master
     - Optional. Must use 64-bit addresses. Must not use WRAP or FIXED burst
       types; ``AxSIZE`` should match the AXI data bus width. Memory offsets
       for each partition are supplied by the host through a register in the
       ``s_axi_control`` interface. Non-conforming logic must be wrapped or
       bridged to satisfy these requirements.
   * - ``axis``
     - AXI4-Stream
     - Optional. One-way only — bidirectional ports are not supported.

.. note::

   AMD recommends packaging AXI4 memory-mapped interfaces with
   ``HAS_BURST=0`` and ``SUPPORTS_NARROW_BURST=0`` set in the IP-level
   ``bd.tcl`` file, indicating that wrap/fixed burst types and narrow bursts
   are not used.

Control Interface Register Map
================================

The control register map is only required if the kernel is intended to be used with
the high-level VRT launch API (``setArg``, ``call``, ``start``, ``wait``). Kernels
driven exclusively through ``kernel.read()``/``kernel.write()`` may implement any
register scheme.

When the high-level API is used, the VRT runtime communicates with the kernel through
the ``ap_ctrl_hs`` protocol over the ``s_axi_control`` interface. The expected register
layout matches the XRT-managed kernel layout from UG1393:

.. list-table::
   :header-rows: 1
   :widths: 10 25 65

   * - Offset
     - Name
     - Description
   * - ``0x00``
     - Control
     - Bit 0 (``ap_start``): write 1 to begin execution. Bit 1
       (``ap_done``): reads 1 when execution is complete.
   * - ``0x04``
     - Global Interrupt Enable
     - Optional; only required if the kernel signals an interrupt to the host.
   * - ``0x08``
     - IP Interrupt Enable
     - Optional.
   * - ``0x0C``
     - IP Interrupt Status
     - Optional.
   * - ``0x10``\ +
     - Kernel arguments
     - Scalar arguments are 32 bits wide; ``m_axi`` and ``axis`` pointer
       arguments are 64 bits wide. All user-defined registers must begin at
       offset ``0x10``; offsets below this are reserved.

Kernels driven through ``read()``/``write()`` only are free to place registers at any
offset, including below ``0x10``.

Build Targets
=============

RTL kernels support the **hardware** and **simulation** build targets.

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Target
     - Supported
     - Notes
   * - Hardware
     - Yes
     - Full Vivado implementation producing a ``.vbin`` device binary
   * - Simulation
     - Yes
     - RTL simulated via the Verilog register model
   * - Emulation
     - No
     - RTL kernels do not support the emulation build target

See :doc:`/explanation/platform-modes` and
:doc:`/tutorials/user/emulation-and-simulation` for further background on build
targets.

Design Recommendations
=======================

Memory Performance
------------------

The AXI4 memory-mapped interfaces connect to the DDR and HBM memory controllers on
the platform. For best performance:

- Match the AXI data width to the native memory controller width (typically 512 bits
  on the V80).
- Do not use WRAP, FIXED, or sub-sized bursts.
- Use burst transfers as large as possible (up to the 4 KB AXI4 protocol limit).
- Avoid deasserted write strobes; these can cause ECC logic in the DDR memory
  controller to perform read-modify-write operations.
- Use pipelined AXI transactions.
- Avoid generating write address commands if the kernel cannot deliver the full write
  transaction, and avoid generating read address commands if the kernel cannot accept
  all read data without back pressure.

Quality of Results
------------------

- Pipeline all reset inputs and internally distribute resets, avoiding high-fanout nets.
- Reset only essential control-logic flip-flops.
- Consider registering input and output signals where possible.
- Account for the resource footprint of the kernel relative to the V80's capacity,
  especially if multiple kernels are instantiated.

Debug and Verification
----------------------

- Verify the RTL in a standalone testbench before integration. The AXI Verification
  IP (VIP), available in the Vivado IP catalog, can help verify AXI interfaces.
- Use simulation builds to test host-side software integration and to observe
  interactions between multiple kernels.
- ILA cores can be embedded inside RTL kernels for on-hardware debug.

Next Steps
==========

- :doc:`/tutorials/user/your-first-kernel` — HLS kernel walkthrough for comparison
- :doc:`/tutorials/user/emulation-and-simulation` — emulation and simulation builds
- :doc:`/explanation/platform-modes` — hardware, simulation, and emulation explained
- :doc:`/reference/cmake/slashtools` — ``add_vbin()`` reference
