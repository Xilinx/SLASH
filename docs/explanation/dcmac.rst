..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

################
SLASH Networking
################

SLASH can attach kernels directly to Ethernet ports on the V80 through the AMD
`DCMAC <https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/dcmac.html>`_
(Versal™ Adaptive SoC 600G Channelized Multirate Ethernet Subsystem), a
hardened IP block on the device. This page explains what the DCMAC is, how SLASH
wires it into a design, the source code, and more. For a hands-on walkthrough,
see :doc:`/howto/use-dcmac`.

The DCMAC
=========

The DCMAC is a hardened Ethernet MAC (Media Access Control) block in the Versal
device. On the Alveo V80, SLASH drives it in **200GAUI-4** mode, a 200 Gb/s,
four-lane `Attachment Unit Interface <https://en.wikipedia.org/wiki/Terabit_Ethernet>`_,
protected by **RS(544)**
`Reed-Solomon <https://en.wikipedia.org/wiki/Reed%E2%80%93Solomon_error_correction>`_
`FEC <https://en.wikipedia.org/wiki/Forward_error_correction>`_ (Forward Error
Correction), the KP4 code used by PAM4 (4-level Pulse Amplitude Modulation)
signaling.

The V80 exposes **four QSFP56** (Quad Small Form-factor Pluggable) cages, and
SLASH uses two DCMAC instances. Each DCMAC instance can be bound to up to two
QSFP56 cages; this dual-cage support is implemented but not yet hardware-tested.
In the single-port configuration SLASH uses here, one instance drives one cage
as a single 200 Gb/s Ethernet port, and example 06 exercises two of the four
cages, one per instance.

Key hardware facts:

- Line rate: 200 Gb/s (200GAUI-4), RS(544) FEC, PAM4 signaling.
- GT reference clock: 322.265625 MHz.
- DCMAC core clock: 782 MHz; AXI4-Stream clock (``nclk_f``): 391 MHz nominal.
- Four QSFP56 cages and two DCMAC instances per V80.

DCMAC integration in SLASH
==========================

The DCMAC lives in the **service** shell region. Kernels never talk to the raw
transceiver (QSFP56 cages); they exchange 512-bit AXI4-Stream transactions in
packet mode with the DCMAC, and the service layer handles segmentation, FEC,
resets, and clocking.

.. code-block:: text

        V80 front panel   (QSFP0 nearest the PCIe edge → QSFP3 farthest)
        ┌─────────┬─────────┬─────────┬─────────┐
        │  QSFP0  │  QSFP1  │  QSFP2  │  QSFP3  │
        └────┬────┴────┬────┴────┬────┴────┬────┘
             │         ┊         │         ┊
  ═══════════│═════════┊═════════│═════════┊══════════  Service region
             │         ┊         │         ┊   ┊ = 2nd cage (dual, untested)
          ┌──▼─────────▼──┐   ┌──▼─────────▼──┐
          │    DCMAC0     │   │    DCMAC1     │
          │ (qsfp_0_n_1)  │   │ (qsfp_2_n_3)  │
          │ port0  port1  │   │ port0  port1  │
          └──┬────────────┘   └──┬────────────┘
      eth_0  │             eth_2 │
    512-bit  │ tx0/rx0           │ tx0/rx0
       AXIS  │                   │
  ═══════════│═══════════════════│══════════════════════  User region
          ┌──▼───────────┐    ┌──▼───────────┐
          │  producer_0  │    │  producer_1  │
          │  consumer_0  │    │  consumer_1  │
          └──────────────┘    └──────────────┘

Networking is enabled per-port from the linker configuration. Adding an
``eth_N`` entry to the ``[network]`` section of ``config.cfg`` instantiates the
corresponding DCMAC/QSFP hierarchy, and ``stream_connect`` directives attach a
kernel's AXI-Stream ports to that port's ``tx0`` (transmit) and ``rx0``
(receive) endpoints:

.. code-block:: ini

   [network]
   eth_0=1
   eth_2=1

   [connectivity]
   shell=service
   stream_connect=traffic_producer_0.axis_out:eth_0.tx0
   stream_connect=eth_0.rx0:traffic_consumer_0.axis_in

Each enabled ``eth_N`` maps to one QSFP hierarchy and one DCMAC instance:

.. list-table::
   :header-rows: 1
   :widths: 20 30 20

   * - Config port
     - QSFP hierarchy
     - DCMAC instance
   * - ``eth_0``
     - ``qsfp_0_n_1``
     - DCMAC0 port 0
   * - ``eth_2``
     - ``qsfp_2_n_3``
     - DCMAC1 port 0

This mapping is implemented by the service-region emitter in
``linker/slashkit/emit/hw/service_region/service_layer_ctx.py``, which reads the
enabled ports and builds the matching block-design hierarchy, AXI-Lite control
path, and AXI-Stream links.

Source Code
===========

The DCMAC configuration, block-design TCL, and additional logic are provided as
a submodule in SLASH. To maximize reuse across projects, they live in a
separate repository, the
`Versal-DCMAC <https://github.com/fpgasystems/Versal-DCMAC>`_ submodule,
co-authored by ETH Zurich (fpgasystems) and AMD and released under the MIT
license. SLASH checks the repository out at ``submodules/Versal-DCMAC`` and its
linker references those assets (the reset FSM, segment converters, control-port
helpers, and the ``bd_dcmac`` TCL procs) via ``dcmac_paths()`` in the
service-region emitter. In a source checkout the submodule has to be initialized
explicitly; see :doc:`/howto/use-dcmac`. Packages carry a staged copy, so an
installed SLASH needs no submodule.

The same reusable design is used by other projects, including ETH Zurich's
`Coyote <https://github.com/fpgasystems/Coyote>`_ FPGA shell.

For deeper hardware detail (the register map, reset and startup sequences,
clocking relationships, statistics, and PHY tuning such as TX swing,
pre/post-emphasis, and IBERT), see the
`Versal-DCMAC README <https://github.com/fpgasystems/Versal-DCMAC>`_ and AMD
`PG369 <https://docs.amd.com/r/en-US/pg369-dcmac>`_.

See Also
========

- :doc:`/howto/use-dcmac`: run the example 06 Ethernet loopback.
- :doc:`/explanation/architecture`: where the service region fits in the stack.
