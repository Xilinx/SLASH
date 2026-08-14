..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##############################
Use DCMAC (Ethernet Loopback)
##############################

This guide walks through example ``06_dcmac``, which sends raw Ethernet traffic
between two 200 Gb/s DCMAC (Versal™ Adaptive SoC 600G Channelized Multirate
Ethernet Subsystem) ports and counts the beats received. For background on how
the DCMAC is wired into a SLASH design, see :doc:`/explanation/dcmac`.

Prerequisites
=============

- The SLASH stack is installed, ``vrtd`` is running, and a V80 board is visible
  (``v80-smi list``).
- A V80 with two QSFP56 cages externally wired to DCMAC0 port 0 (``eth_0``) and
  DCMAC1 port 0 (``eth_2``).
- Vivado and Vitis HLS **2025.1** or newer, sourced in your shell.
- Familiarity with HLS kernel basics.
  See :doc:`/tutorials/user/your-first-kernel`.

Physical Setup
==============

Example 06 forms a loopback **between two ports**: whatever leaves ``eth_0`` must
arrive at ``eth_2``, and vice versa. Connect QSFP cage 0 and QSFP cage 2 with a
single 200 Gb/s cable (a QSFP56 DAC or an AOC/fiber pair). On the V80, **QSFP0 is
the cage closest to the PCIe connector / motherboard**; the cages number outward
from there.

.. code-block:: text

   traffic_producer_0 ──► eth_0.tx0 ──┐        ┌── eth_2.rx0 ──► traffic_consumer_1
                                      │  cable │
   traffic_consumer_0 ◄── eth_0.rx0 ──┘        └── eth_2.tx0 ◄── traffic_producer_1

.. note::

   A single-port loopback module would also work electrically, but 200 Gb/s
   loopback modules are hard to source. A **switch will not work**: example 06
   emits raw Ethernet with no MAC/IP addressing, so there is nothing for a
   switch to forward. Use a direct cable between the two cages.

.. note::

   The two ports need not be on the same board. Example 06 works equally well
   with one port on one V80 cabled to a port on a second, independent V80; the
   raw Ethernet frames flow point-to-point either way.

Enabling Ethernet in the Linker
===============================

Ports are turned on in the ``[network]`` section of ``config.cfg``. Each
``eth_N=1`` instantiates one DCMAC/QSFP hierarchy. Kernel AXI-Stream ports are
attached to the port's ``tx0`` (transmit) and ``rx0`` (receive) endpoints with
``stream_connect``:

.. code-block:: ini

   [network]
   eth_0=1
   eth_2=1

   [connectivity]
   shell=service
   nk=traffic_producer:2:traffic_producer_0.traffic_producer_1
   nk=traffic_consumer:2:traffic_consumer_0.traffic_consumer_1

   stream_connect=traffic_producer_0.axis_out:eth_0.tx0
   stream_connect=traffic_producer_1.axis_out:eth_2.tx0

   stream_connect=eth_0.rx0:traffic_consumer_0.axis_in
   stream_connect=eth_2.rx0:traffic_consumer_1.axis_in

- ``shell=service`` selects the service shell region that hosts the DCMAC.
- ``eth_0`` maps to DCMAC0 port 0, ``eth_2`` to DCMAC1 port 0
  (see :doc:`/explanation/dcmac`).
- ``tx0`` / ``rx0`` are the MAC's stream endpoints, wired just like any other
  ``stream_connect`` (see :doc:`/howto/chain-streaming-kernels`).

The Kernels
===========

The producer emits ``flits`` beats of 512-bit data onto its stream. It also
takes a ``dest`` argument (a TDEST tag), but example 06 leaves it unused, since
the loopback path and the consumer ignore it:

.. code-block:: cpp

   typedef ap_axiu<512, 1, 1, 3> pkt;

   void traffic_producer(hls::stream<pkt>& axis_out,
                         ap_uint<32> flits, ap_uint<3> dest) {
       #pragma HLS INTERFACE mode=s_axilite port=flits  bundle=control
       #pragma HLS INTERFACE mode=s_axilite port=dest   bundle=control
       #pragma HLS INTERFACE mode=s_axilite port=return bundle=control
       // ... write `flits` beats, set last on the final beat ...
   }

The consumer is **freerunning** (``ap_ctrl_none``): it counts every received
beat into an AXI-Lite-readable ``rx_flits`` register and never stops, so the
host does not start or wait on it:

.. code-block:: cpp

   void traffic_consumer(hls::stream<pkt>& axis_in, ap_uint<32>& rx_flits) {
       #pragma HLS INTERFACE ap_ctrl_none port=return
       // ... increment rx_flits on every beat read from axis_in ...
   }

Host Application
================

The host only drives the two producers: ``traffic_producer_0`` sends 101 flits
and ``traffic_producer_1`` sends 85. The consumers run on their own:

.. code-block:: cpp

   vrt::Device device(bdf, vrtbinFile);
   vrt::Kernel traffic_producer_0(device, "traffic_producer_0");
   vrt::Kernel traffic_producer_1(device, "traffic_producer_1");

   traffic_producer_0.start(101, 0);   // 101 flits; second arg `dest` is unused
   traffic_producer_0.wait();
   traffic_producer_1.start(85, 1);    // 85 flits
   traffic_producer_1.wait();

   device.cleanup();

Build and Run
=============

Source Vivado and Vitis HLS first, then build the HLS kernels and a hardware
vbin:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

.. code-block:: bash

   cd examples/06_dcmac
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build
   cmake --build build --target hls
   cmake --build build --target dcmac_hw    # or dcmac_emu / dcmac_sim

.. code-block:: bash

   ./06_dcmac <BDF> dcmac_hw.vbin

Replace ``<BDF>`` with your board's address from ``v80-smi list``.

.. tip::

   You do not have to rebuild example 06 to exercise the DCMAC. The default
   image shipped with the service and user regions already contains the
   networking logic, so you can load it and drive the ports directly with the
   low-level Python driver described below.

Verifying the Link
==================

Traffic only flows once both DCMAC links are up. The C++ example is intentionally
minimal: it sends frames and relies on the consumers' ``rx_flits`` counters. For
link-up status, per-port statistics, and IP/UDP bring-up, use the lower-level
Python driver shipped with the linker resources:

.. code-block:: text

   linker/slashkit/resources/dcmac/driver/network_end2end_test.py

It initializes both DCMACs, checks ``link_up``, and (with ``--udp``) configures
MAC/IP addresses and the socket table before generating traffic.

Next Steps
==========

- :doc:`/explanation/dcmac`: how the DCMAC is instantiated and where the HDL
  comes from.
- :doc:`/howto/chain-streaming-kernels`: AXI-Stream ``stream_connect`` basics.
