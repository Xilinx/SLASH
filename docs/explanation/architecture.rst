..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##############
Architecture
##############

SLASH is organised as a layered stack. Each layer has a single responsibility
and communicates with adjacent layers through well-defined interfaces.

Stack Overview
==============

.. code-block:: text

   ┌─────────────────────────────────────────────┐
   │              User Application               │  C++17
   ├─────────────────────────────────────────────┤
   │            VRT  (libvrt)                    │  C++17  ─ MIT
   ├─────────────────────────────────────────────┤
   │          libvrtd++  (C++ RAII wrapper)      │  C++20  ─ MIT
   ├─────────────────────────────────────────────┤
   │          libvrtd    (C wire-protocol)       │  C11    ─ MIT
   ├──────────────── AF_UNIX ────────────────────┤
   │          vrtd       (daemon)                │  C11    ─ MIT
   ├─────────────────────────────────────────────┤
   │          libslash   (driver wrapper)        │  C      ─ MIT
   ├─────────────────────────────────────────────┤
   │       Linux kernel module  (slash)          │  C      ─ GPLv2
   ├─────────────────────────────────────────────┤
   │          AMD Alveo V80 Hardware             │
   └─────────────────────────────────────────────┘

Two additional components sit alongside the stack:

- **v80-smi** — command-line system management interface for listing, programming,
  resetting, and validating V80 boards.
- **slashkit** — Python-based toolchain that links HLS kernels into
  *vrtbin* archives for deployment.

Layer Descriptions
==================

User Application
----------------

Your C++ program. It uses the VRT API to open a device, load a vrtbin, allocate
buffers, launch kernels, and read results. The same source code runs unchanged
on hardware, emulation, and simulation platforms (see :doc:`/explanation/platform-modes`).

VRT (libvrt)
------------

The V80 RunTime library. VRT is the primary API surface:

- ``vrt::Device`` — opens a board by BDF, loads a vrtbin, exposes kernels and
  memory configuration.
- ``vrt::Kernel`` — represents a hardware kernel; supports argument setting,
  start, wait, and register read/write.
- ``vrt::Buffer<T>`` — typed device memory with host synchronisation
  (``HOST_TO_DEVICE`` / ``DEVICE_TO_HOST``).
- ``vrt::StreamingBuffer<T>`` — QDMA streaming I/O for kernel ports.
- ``vrt::Vrtbin`` — extracts and inspects vrtbin archives.

VRT transparently selects the correct back-end (PCIe BAR, ZeroMQ, or Verilog
register map) based on the platform encoded in the vrtbin's ``system_map.xml``.

libvrtd++ and libvrtd
---------------------

Client libraries for communicating with the vrtd daemon:

- **libvrtd** (C) — wire-protocol client over ``AF_UNIX`` /
  ``SOCK_SEQPACKET``. Exposes typed request/response helpers and fd passing
  via ``SCM_RIGHTS``.
- **libvrtd++** (C++) — RAII/exception wrapper. ``vrtd::Session``,
  ``vrtd::Device``, ``vrtd::Bar``, ``vrtd::BarFile`` manage connection
  lifetime automatically.

vrtd (daemon)
-------------

The V80 Runtime Daemon multiplexes access to FPGA devices and enforces
permission rules for multi-tenancy. It listens on a Unix domain socket and
translates client requests into libslash calls. Configuration is through
``vrtd.conf`` (see :doc:`/reference/vrtd/configuration`).

libslash
--------

A thin C wrapper around the Linux kernel driver's ioctl interface. It exposes
three modules:

- **Control** (``slash/ctldev.h``) — BAR MMIO access via PF2.
- **QDMA** (``slash/qdma.h``) — queue-based DMA via PF1.
- **Hotplug** (``slash/hotplug.h``) — PCIe secondary bus reset and rescan.

Linux Kernel Module
-------------------

The ``slash`` kernel module manages two PCI functions on each V80 board:

- **PF1** (``slash_qdma``) — queue-based DMA subsystem.
- **PF2** (``slash_ctl``) — BAR MMIO access for register reads and writes.

PF0 (``ami``) is the AVED management interface, managed by a separate driver.

Initialisation order: QDMA → Hotplug → PCIe. Teardown is reversed.

Typical Execution Flow
======================

The following sequence shows a minimal hardware run using VRT:

.. code-block:: text

   1.  vrt::Device device(bdf, vrtbinFile);
       │
       ├─ Extract vrtbin archive (gzipped tar)
       ├─ Parse system_map.xml → determine platform
       ├─ Connect to vrtd → open device → program FPGA
       └─ Discover kernels and memory configuration

   2.  vrt::Kernel kernel(device, "kernel_name");
       └─ Look up kernel in the loaded design

   3.  vrt::Buffer<float> buf(device, size, kernel.argMemoryConfig("in"));
       └─ Allocate device memory (DDR or HBM) via QDMA

   4.  buf.sync(vrt::SyncType::HOST_TO_DEVICE);
       └─ DMA transfer: host → device

   5.  kernel.setArg(0, size);
       kernel.setArg(1, buf);
       kernel.start();
       └─ Write arguments to AXI-Lite registers, then set AP_START

   6.  kernel.wait();
       └─ Poll AP_DONE / AP_IDLE

   7.  uint32_t result = kernel.read(0x18);
       └─ Read result register via BAR MMIO
