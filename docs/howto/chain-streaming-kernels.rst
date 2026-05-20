..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

###########################
Chain Streaming Kernels
###########################

This guide shows how to connect multiple HLS kernels via AXI-Stream to form a
processing pipeline where data flows kernel-to-kernel without touching device
memory.

Prerequisites
=============

- The SLASH stack is installed, ``vrtd`` is running, and a V80 board is visible.
- Familiarity with HLS kernel basics.
  See :doc:`/tutorials/user/your-first-kernel`.

Streaming Pipeline Concept
===========================

In a streaming pipeline, kernels are wired together through on-chip AXI-Stream
channels. Data bypasses device memory entirely between stages:

.. code-block:: text

   Host Memory ──► [dma_in] ──axis──► [passthrough] ──axis──► [dma_out] ──► Host Memory

- **dma_in** — reads from device memory and writes to a stream.
- **passthrough** — a freerunning kernel that processes each element as it
  arrives (in this example, a simple pass-through).
- **dma_out** — reads from a stream and writes to device memory.

Writing Streaming HLS Kernels
==============================

DMA-In Kernel (Stream Producer)
---------------------------------

The DMA-in kernel reads from a memory-mapped port and pushes each element onto
an AXI-Stream output:

.. code-block:: cpp

   void dma_in(ap_uint<64>* in, hls::stream<ap_uint<64>>& axis_out, ap_uint<32> size) {
       #pragma hls interface mode=s_axilite port=size
       #pragma hls interface mode=axis port=axis_out
       #pragma hls interface m_axi bundle=gmem0 port=in max_widen_bitwidth=64
       #pragma hls interface mode=s_axilite port=return

       for (ap_uint<32> i = 0; i < size; i++) {
           #pragma HLS PIPELINE II=1
           axis_out.write(in[i]);
       }
   }

Key pragmas:

- ``m_axi`` — memory-mapped master for the input buffer.
- ``axis`` — AXI-Stream output port.
- ``s_axilite port=return`` — allows the host to start and poll the kernel.

Freerunning Kernel (Stream Processor)
---------------------------------------

A freerunning kernel has no host control interface. It runs continuously,
processing data whenever the input stream has elements:

.. code-block:: cpp

   void passthrough(hls::stream<ap_uint<64>>& axis_in, hls::stream<ap_uint<64>>& axis_out) {
       #pragma HLS INTERFACE axis port=axis_in
       #pragma HLS INTERFACE axis port=axis_out
       #pragma HLS INTERFACE ap_ctrl_none port=return

       ap_uint<64> data;
       while (true) {
           #pragma HLS PIPELINE II=1
           if (!axis_in.empty()) {
               data = axis_in.read();
               axis_out.write(data);
           }
       }
   }

The ``ap_ctrl_none`` pragma is critical — it removes the start/done/idle
control registers, making the kernel autonomous. You do **not** call
``kernel.start()`` or ``kernel.wait()`` for freerunning kernels.

DMA-Out Kernel (Stream Consumer)
----------------------------------

The DMA-out kernel reads from a stream and writes each element to device
memory:

.. code-block:: cpp

   void dma_out(ap_uint<32> size, hls::stream<ap_uint<64>>& axis_in, ap_uint<64>* out) {
       #pragma hls interface mode=s_axilite port=size
       #pragma hls interface mode=axis port=axis_in
       #pragma hls interface m_axi bundle=gmem0 port=out max_widen_bitwidth=64
       #pragma hls interface mode=s_axilite port=return

       for (ap_uint<32> i = 0; i < size; i++) {
           #pragma HLS PIPELINE II=1
           ap_uint<64> val;
           axis_in.read(val);
           out[i] = val;
       }
   }

Linker Configuration
=====================

Connect the kernels with ``stream_connect`` directives in ``config.cfg``:

.. code-block:: ini

   [connectivity]
   nk=dma_in:1:dma_in_0
   nk=passthrough:1:passthrough_0
   nk=dma_out:1:dma_out_0

   stream_connect=dma_in_0.axis_out:passthrough_0.axis_in
   stream_connect=passthrough_0.axis_out:dma_out_0.axis_in

- ``nk`` — instantiates each kernel (same syntax as non-streaming designs).
- ``stream_connect`` — wires AXI-Stream ports between kernel instances using
  ``<instance>.<port>:<instance>.<port>`` syntax.

No ``sp=`` lines are needed for the streaming ports themselves. Only the
memory-mapped ports on ``dma_in`` and ``dma_out`` require memory mapping, which
the linker assigns automatically when no explicit ``sp=`` is given.

Host Application
=================

In the host code, only the DMA endpoint kernels need to be controlled. The
freerunning ``passthrough`` kernel is not instantiated:

.. code-block:: cpp

   vrt::Kernel dma_in(device, "dma_in_0");
   vrt::Kernel dma_out(device, "dma_out_0");
   // passthrough_0 is freerunning — no host handle needed

Allocate buffers using ``argMemoryConfig()`` so the VRT runtime automatically
selects the correct memory bank for each kernel's memory-mapped argument:

.. code-block:: cpp

   vrt::Buffer<uint64_t> buffer_in(device, size, dma_in.argMemoryConfig("in"));
   vrt::Buffer<uint64_t> buffer_out(device, size, dma_out.argMemoryConfig("out"));

Set arguments, start both DMA kernels, and verify the output:

.. code-block:: cpp

   buffer_in.sync(vrt::SyncType::HOST_TO_DEVICE);

   dma_in.setArg(0, buffer_in);
   dma_in.setArg(1, size);
   dma_out.setArg(0, size);
   dma_out.setArg(1, buffer_out);

   dma_in.start();
   dma_out.start();
   dma_in.wait();
   dma_out.wait();

   buffer_out.sync(vrt::SyncType::DEVICE_TO_HOST);

.. note::

   Both ``dma_in`` and ``dma_out`` must be started. If ``dma_out`` is not
   ready to consume data, the pipeline will stall due to back-pressure.

Build and Run
==============

Ensure you have sourced Vivado and Vitis HLS before building:

.. code-block:: bash

   source <path-to-vivado>/settings64.sh
   source <path-to-vitis-hls>/settings64.sh

.. code-block:: bash

   cd examples/02_chain
   cmake -B build -S . -G Ninja -DSLASH_USE_REPO=ON
   cmake --build build
   cmake --build build --target hls
   cmake --build build --target chain_hw    # or chain_emu / chain_sim

.. code-block:: bash

   ./02_chain <BDF> chain_hw.vbin

Replace ``<BDF>`` with your board's address from ``v80-smi list``.

Key Design Considerations
===========================

- **ap_ctrl_none** kernels cannot be started or stopped from the host. They
  run whenever data is available on their input streams.
- **Stream widths must match** between connected ports. In this example all
  three kernels use ``ap_uint<64>``.
- **Back-pressure** is handled automatically — if a downstream kernel is not
  consuming, upstream stalls.
- For multi-stage pipelines, extend the ``stream_connect`` chain in
  ``config.cfg``.

Next Steps
==========

- :doc:`/tutorials/user/your-first-kernel` — basic kernel authoring.
- :doc:`/tutorials/user/buffers-and-memory` — buffer management for DMA
  endpoints.
- :doc:`/howto/use-cmake-modules` — CMake setup for HLS and vrtbin linking.
- :doc:`/explanation/architecture` — how streaming fits in the SLASH stack.
