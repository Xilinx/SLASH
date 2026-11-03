..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

#################################
Place Kernels in a Specific SLR
#################################

This guide explains how to constrain kernel instances to a super logic region
(SLR) of the V80 and how the linker assigns ``MEM`` ports to constrained
instances.

Prerequisites
=============

- The SLASH stack is installed.
- Familiarity with the linker configuration file.
  See :doc:`/tutorials/user/your-first-kernel`.

Overview
========

The V80 contains three SLRs, and the user region of each shell spans more than
one of them. Without constraints, Vivado places kernel instances anywhere in the
user region. The ``[floorplan]`` section of ``config.cfg`` restricts an instance
to the part of the user region that lies in a given SLR.

Constraining Instances to an SLR
================================

Each ``slr=`` entry assigns one or more instances to an SLR. Instance names are
separated by commas, dots, or spaces, as in ``nk=``:

.. code-block:: ini

   [connectivity]
   nk=perf:3:perf_0.perf_1.perf_2

   [floorplan]
   slr=perf_0,perf_1:SLR1
   slr=perf_2:SLR0

The valid SLRs depend on the shell:

.. list-table::
   :header-rows: 1

   * - Shell
     - Valid SLRs
   * - ``compute``
     - ``SLR0``, ``SLR1``, ``SLR2``
   * - ``service``
     - ``SLR1``, ``SLR2``

The linker rejects an unknown instance, an SLR that is not valid for the shell,
and an instance assigned to more than one SLR.

In the service shell, the user region does not extend into SLR0, and its SLR1
portion is a narrow strip along the SLR2 boundary. Instances constrained to
``SLR1`` on the service shell must therefore be small.

Implementation Behavior
=======================

For each SLR in use, the linker creates a hard pblock nested in the user-region
pblock and covering only the part of the user region in that SLR. The pblock
contains:

- the constrained kernel instances, and
- the reset register that drives those instances.

The interconnect that the linker generates around kernel ports, such as
SmartConnects and width or clock converters, is not constrained.

``MEM`` Port Assignment
=======================

Each shell provides eight ``MEM`` ports, ``MEM0`` to ``MEM7``. Each port has a
fixed location in one SLR:

.. list-table::
   :header-rows: 1

   * - Shell
     - SLR0
     - SLR1
     - SLR2
   * - ``compute``
     - ``MEM0`` to ``MEM2``
     - ``MEM3`` to ``MEM6``
     - ``MEM7``
   * - ``service``
     - None
     - ``MEM0``, ``MEM1``
     - ``MEM2`` to ``MEM7``

The linker assigns a port to each AXI interface that targets ``MEM`` in the
following order of precedence:

1. An explicit index, ``sp=<instance>.<port>:MEM<i>``, selects port ``i``. If
   the instance is constrained to a different SLR, the linker issues a warning
   and keeps port ``i``.
2. For an instance constrained to an SLR, an unindexed ``MEM`` is assigned
   round-robin among the ``MEM`` ports in that SLR.
3. Any other unindexed ``MEM`` is assigned round-robin among all eight ports.

The linker reads the ``MEM`` port locations from the installed static shell.
Static shells built before these locations were fixed do not provide them. With
such a shell, the linker issues a warning and applies rule 3 to constrained
instances; the SLR pblocks are still applied.

Choosing Between ``HBM`` and ``MEM`` Ports
==========================================

The ``HBM0`` to ``HBM63`` ports connect along the HBM edge at the top of SLR2
and provide the highest bandwidth. Instances that use them primarily are best
placed in SLR2. ``MEM`` ports provide lower bandwidth, but reach HBM and DDR
from every SLR in the user region.
