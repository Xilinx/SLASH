..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

#########################
Migrate from XRT to VRT
#########################

This guide maps XRT (Xilinx Runtime) concepts to their VRT (V80 Runtime)
equivalents. It is intended for developers familiar with host code written
against XRT for Alveo U200/U250/U280/U55C boards, and provides the
reference needed to become productive on the Alveo V80 with VRT.

.. note::

   VRT targets the AMD Alveo V80 exclusively. The API surface is smaller
   and more opinionated than XRT, so there is less to learn.

Quick Reference
===============

.. list-table::
   :header-rows: 1
   :widths: 30 40 30

   * - XRT
     - VRT
     - Header
   * - ``xrt::device``
     - ``vrt::Device``
     - ``<vrt/device.hpp>``
   * - ``xrt::kernel``
     - ``vrt::Kernel``
     - ``<vrt/kernel.hpp>``
   * - ``xrt::bo``
     - ``vrt::Buffer<T>``
     - ``<vrt/buffer.hpp>``
   * - ``xrt::run``
     - (integrated into ``vrt::Kernel``)
     - ``<vrt/kernel.hpp>``
   * - xclbin
     - vbin
     - ``<vrt/vrtbin.hpp>``
   * - ``xrt::run::set_arg``
     - ``vrt::Kernel::setArg``
     - ``<vrt/kernel.hpp>``
   * - ``xrt::bo::sync``
     - ``vrt::Buffer<T>::sync``
     - ``<vrt/buffer.hpp>``
   * - ``xrt::bo::map``
     - ``vrt::Buffer<T>::operator[]`` / ``get()``
     - ``<vrt/buffer.hpp>``
   * - xbutil
     - v80-smi
     - CLI tool
   * - ``XCL_EMULATION_MODE``
     - Built into vbin (platform auto-detected)
     - --
   * - ``xrt::kernel::group_id``
     - ``vrt::Kernel::argMemoryConfig``
     - ``<vrt/kernel.hpp>``
   * - N/A
     - ``vrt::Device::setFrequency``
     - ``<vrt/device.hpp>``

Architecture: What Changed
==========================

XRT talks to the kernel driver directly via ioctls. VRT is one component
of the broader **SLASH** platform, which inserts a **daemon** (``vrtd``)
between the application and the driver:

.. code-block:: text

   XRT:     App  -->  libxrt_core  -->  xocl/xclmgmt (kernel driver)  -->  FPGA

   SLASH:   App  -->  libvrt  -->  vrtd (daemon)  -->  slash (kernel driver)  -->  FPGA

The daemon multiplexes device access across processes, manages DMA buffer
lifetimes, and handles FPGA programming. From the user's perspective,
this layering is transparent: the application code interacts with the
same style of Device/Kernel/Buffer objects regardless of how requests are
dispatched underneath.

**What this means in practice:**

* The ``vrtd`` daemon must be running before the application starts (it
  is a systemd service).
* Multi-process access to the same device works without coordination
  from the user side.
* There is no equivalent of ``xbmgmt`` -- management operations go
  through ``vrtd`` or ``v80-smi``.

Includes
========

XRT:

.. code-block:: cpp

   #include <xrt/xrt_device.h>
   #include <xrt/xrt_kernel.h>
   #include <xrt/xrt_bo.h>

VRT:

.. code-block:: cpp

   #include <vrt/device.hpp>
   #include <vrt/kernel.hpp>
   #include <vrt/buffer.hpp>

Device Management
=================

Opening a Device
----------------

XRT opens a device by index and loads an xclbin:

.. code-block:: cpp

   auto device = xrt::device(0);
   auto uuid = device.load_xclbin("design.xclbin");

VRT opens a device by PCIe BDF and programs a vbin in one step:

.. code-block:: cpp

   vrt::Device device("d8:00", "design.vbin");

The constructor extracts the vbin archive, programs the FPGA, and parses
kernel metadata. To skip programming (when the device is already loaded):

.. code-block:: cpp

   vrt::Device device("d8:00", "design.vbin", false);

.. note::

   **BDF format:** VRT uses board-level ``BB:DD`` or ``DDDD:BB:DD`` --
   no function suffix. Copy the address directly from ``v80-smi list``
   output.

Binary Format: xclbin vs vbin
=============================

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * -
     - xclbin
     - vbin
   * - Format
     - Custom Xilinx container
     - tar archive
   * - Contents
     - Bitstream, metadata, clock info
     - PDI, system_map.xml, (optional) emu/sim executables
   * - Kernel metadata
     - Embedded XML sections
     - ``system_map.xml`` (auto-parsed)
   * - Platform variants
     - Separate files or ``--target`` flag
     - Single file; platform (hw/emu/sim) embedded in metadata

VRT auto-detects whether a vbin targets hardware, emulation, or
simulation. There is no ``XCL_EMULATION_MODE`` environment variable --
the platform is a property of the vbin itself.

You can inspect a vbin without a device:

.. code-block:: bash

   v80-smi inspect design.vbin

Kernel Execution
================

Getting a Kernel Handle
-----------------------

XRT:

.. code-block:: cpp

   auto kernel = xrt::kernel(device, uuid, "my_kernel");

VRT:

.. code-block:: cpp

   vrt::Kernel kernel(device, "my_kernel_0");

.. note::

   **Naming convention:** VRT kernel names include the instance suffix
   from the design (e.g., ``"vadd_0"``), matching what appears in
   ``system_map.xml``.

Setting Arguments and Launching
-------------------------------

XRT exposes two launch styles: a one-call form that takes the arguments
inline, and a staged form that sets arguments individually before
starting. VRT mirrors the same two styles directly on the ``Kernel``
object.

XRT -- one call, blocking:

.. code-block:: cpp

   auto run = kernel(bo_in, bo_out, size);  // set args + start
   run.wait();

XRT -- staged:

.. code-block:: cpp

   auto run = xrt::run(kernel);
   run.set_arg(0, bo_in);
   run.set_arg(1, bo_out);
   run.set_arg(2, size);
   run.start();
   run.wait();

VRT -- one call, blocking (``call`` sets the args, starts the kernel,
and waits for completion):

.. code-block:: cpp

   kernel.call(buffer_in, buffer_out, size);

VRT -- one call, non-blocking (``start`` with arguments sets them and
starts execution without blocking):

.. code-block:: cpp

   kernel.start(buffer_in, buffer_out, size);
   // ... do other work ...
   kernel.wait();

VRT -- staged with ``setArg`` + ``start`` / ``call``:

.. code-block:: cpp

   kernel.setArg(0, buffer_in);
   kernel.setArg(1, buffer_out);
   kernel.setArg(2, size);
   kernel.start();   // non-blocking; pair with kernel.wait()
   // or
   kernel.call();    // blocking equivalent of start() + wait()

Arguments can also be set by name:

.. code-block:: cpp

   kernel.setArg("input", buffer_in);
   kernel.setArg("output", buffer_out);
   kernel.setArg("size", 1024);
   kernel.call();

.. list-table::
   :header-rows: 1
   :widths: 40 20 20 20

   * - Style
     - Sets args
     - Starts
     - Waits
   * - ``kernel.call(args...)``
     - yes
     - yes
     - yes
   * - ``kernel.start(args...)``
     - yes
     - yes
     - no
   * - ``setArg(...)`` + ``kernel.call()``
     - yes
     - yes
     - yes
   * - ``setArg(...)`` + ``kernel.start()``
     - yes
     - yes
     - no

.. note::

   **Buffer arguments are resolved automatically.** When you pass a
   ``vrt::Buffer<T>`` to ``call``, ``start``, or ``setArg``, VRT extracts
   the physical address. No need to call ``.address()`` or similar.

Reading Output Registers
------------------------

XRT: Read kernel outputs via ``xrt::bo`` or register access.

VRT: Read directly from kernel registers by offset:

.. code-block:: cpp

   uint32_t result = kernel.read(0x18);

Buffer Management
=================

Creating Buffers
----------------

XRT allocates buffer objects with a memory group:

.. code-block:: cpp

   auto bo = xrt::bo(device, size_bytes, kernel.group_id(0));

VRT uses typed, element-counted buffers. Memory placement comes from
kernel metadata:

.. code-block:: cpp

   vrt::Buffer<float> buf(device, num_elements, kernel.argMemoryConfig("input"));

``argMemoryConfig()`` returns a ``MemoryConfig`` that encodes the correct
memory type (DDR, HBM, or HBM_VNOC) and HBM port for that kernel
argument -- the VRT equivalent of XRT's ``group_id()``.

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - XRT
     - VRT
   * - ``xrt::bo(device, size_bytes, group_id)``
     - ``vrt::Buffer<T>(device, num_elements, kernel.argMemoryConfig("arg"))``
   * - Size in **bytes**
     - Size in **elements** (byte size = ``num_elements * sizeof(T)``)
   * - Untyped (``void*``)
     - Typed (``T*``)

Writing Data to a Buffer
------------------------

XRT:

.. code-block:: cpp

   auto host_ptr = bo.map<float*>();
   for (int i = 0; i < n; i++) host_ptr[i] = i;

VRT buffers are directly subscriptable:

.. code-block:: cpp

   for (int i = 0; i < n; i++) buf[i] = static_cast<float>(i);

You can also get a raw pointer if needed:

.. code-block:: cpp

   float* ptr = buf.get();

Synchronizing Buffers
---------------------

XRT:

.. code-block:: cpp

   bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
   // ... run kernel ...
   bo.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

VRT:

.. code-block:: cpp

   buf.sync(vrt::SyncType::HOST_TO_DEVICE);
   // ... run kernel ...
   buf.sync(vrt::SyncType::DEVICE_TO_HOST);

Memory Types
------------

VRT exposes three memory types through ``MemoryRangeType``:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - VRT Memory Type
     - Description
   * - ``MemoryRangeType::DDR``
     - DDR memory
   * - ``MemoryRangeType::HBM``
     - HBM with explicit port (0-63)
   * - ``MemoryRangeType::HBM_VNOC``
     - HBM via Virtual Network-on-Chip (auto-distributed)

When using ``kernel.argMemoryConfig()``, the correct type and port are
selected automatically from the design metadata. This is the recommended
approach.

CLI: xbutil vs v80-smi
======================

.. list-table::
   :header-rows: 1
   :widths: 25 35 40

   * - Task
     - XRT (xbutil)
     - VRT (v80-smi)
   * - List devices
     - ``xbutil examine``
     - ``v80-smi list``
   * - Detailed device info
     - ``xbutil examine -d <bdf>``
     - ``v80-smi list -l``
   * - Program device
     - ``xbutil program -d <bdf> -u <xclbin>``
     - ``v80-smi program <vbin> -d <BDF>``
   * - Reset device
     - ``xbutil reset -d <bdf>``
     - ``v80-smi reset -d <BDF>``
   * - Validate device
     - ``xbutil validate``
     - ``v80-smi validate -d <BDF>``
   * - Inspect binary
     - ``xclbinutil --info -i <xclbin>``
     - ``v80-smi inspect <vbin>``
   * - Query loaded design
     - --
     - ``v80-smi query -d <BDF>``
   * - JSON output
     - --
     - Add ``-j`` or ``-J`` to most commands
   * - Version
     - ``xbutil --version``
     - ``v80-smi version``

CMake Integration
=================

XRT:

.. code-block:: cmake

   find_package(XRT REQUIRED)
   target_link_libraries(myapp PRIVATE XRT::xrt_coreutil)

VRT:

.. code-block:: cmake

   find_package(vrt REQUIRED CONFIG)
   target_link_libraries(myapp PRIVATE vrt::vrt)

Full example:

.. code-block:: cmake

   cmake_minimum_required(VERSION 3.20)
   project(my_v80_app LANGUAGES CXX)

   set(CMAKE_CXX_STANDARD 20)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)

   find_package(vrt REQUIRED CONFIG)

   add_executable(my_app main.cpp)
   target_link_libraries(my_app PRIVATE vrt::vrt)

See :doc:`use-cmake-modules` for the full CMake module reference.

Multi-Device
============

XRT:

.. code-block:: cpp

   auto dev0 = xrt::device(0);
   auto dev1 = xrt::device(1);

VRT uses BDF strings instead of indices:

.. code-block:: cpp

   vrt::Device fpga0("e2:00", "design.vbin");
   vrt::Device fpga1("21:00", "design.vbin");

Each device is fully independent -- separate kernels, buffers, and
frequencies:

.. code-block:: cpp

   vrt::Kernel k0(fpga0, "vadd_0");
   vrt::Kernel k1(fpga1, "vadd_0");

   vrt::Buffer<int> buf0(fpga0, 1024, k0.argMemoryConfig("in"));
   vrt::Buffer<int> buf1(fpga1, 1024, k1.argMemoryConfig("in"));

Use ``v80-smi list`` to discover available board addresses. The format
is ``BB:DD`` (no function suffix) -- copy the address directly from the
command output. See :doc:`use-multiple-boards` for more detail.

Clock Frequency Control
=======================

VRT exposes runtime clock frequency control, which has no direct XRT
equivalent:

.. code-block:: cpp

   std::cout << "Current: " << device.getFrequency() << " Hz\n";
   std::cout << "Max:     " << device.getMaxFrequency() << " Hz\n";

   device.setFrequency(300000000);  // 300 MHz

See :doc:`set-clock-frequency` for more detail.

Emulation and Simulation
========================

XRT: Set ``XCL_EMULATION_MODE=hw_emu`` or ``sw_emu`` and run with an
emulation xclbin.

VRT: Build the design for the target platform (hw, emu, or sim) and use
the corresponding vbin. The platform is auto-detected -- no environment
variable needed:

.. code-block:: cpp

   // Same host code for all three. The vbin determines the platform.
   vrt::Device device(bdf, "design_emu.vbin");

   // Check which platform is active:
   if (device.getPlatform() == vrt::Platform::EMULATION) {
       std::cout << "Running in emulation mode\n";
   }

Platform values: ``vrt::Platform::HARDWARE``, ``vrt::Platform::EMULATION``,
``vrt::Platform::SIMULATION``.

See :doc:`/explanation/platform-modes` for further background.

Logging
=======

VRT has a built-in logger with configurable verbosity:

.. code-block:: cpp

   #include <vrt/utils/logger.hpp>

   vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::DEBUG);

Log levels: ``NONE``, ``WARN``, ``ERROR``, ``INFO``, ``DEBUG``.

Complete Example: Vector Add
============================

Here is a minimal vadd host program showing the full XRT-to-VRT
translation.

XRT version:

.. code-block:: cpp

   #include <xrt/xrt_device.h>
   #include <xrt/xrt_kernel.h>
   #include <xrt/xrt_bo.h>

   int main() {
       auto device = xrt::device(0);
       auto uuid = device.load_xclbin("vadd.xclbin");
       auto kernel = xrt::kernel(device, uuid, "vadd");

       auto bo_a = xrt::bo(device, 1024 * sizeof(int), kernel.group_id(0));
       auto bo_b = xrt::bo(device, 1024 * sizeof(int), kernel.group_id(1));
       auto bo_c = xrt::bo(device, 1024 * sizeof(int), kernel.group_id(2));

       auto a = bo_a.map<int*>();
       auto b = bo_b.map<int*>();
       for (int i = 0; i < 1024; i++) { a[i] = i; b[i] = i; }

       bo_a.sync(XCL_BO_SYNC_BO_TO_DEVICE);
       bo_b.sync(XCL_BO_SYNC_BO_TO_DEVICE);

       auto run = xrt::run(kernel);
       run.set_arg(0, bo_a);
       run.set_arg(1, bo_b);
       run.set_arg(2, bo_c);
       run.set_arg(3, 1024);
       run.start();
       run.wait();

       bo_c.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
       auto c = bo_c.map<int*>();
       // verify c[i] == 2*i
   }

VRT version:

.. code-block:: cpp

   #include <vrt/device.hpp>
   #include <vrt/kernel.hpp>
   #include <vrt/buffer.hpp>

   int main() {
       vrt::Device device("d8:00", "vadd.vbin");
       vrt::Kernel vadd(device, "vadd_0");

       vrt::Buffer<int> a(device, 1024, vadd.argMemoryConfig("a"));
       vrt::Buffer<int> b(device, 1024, vadd.argMemoryConfig("b"));
       vrt::Buffer<int> c(device, 1024, vadd.argMemoryConfig("c"));

       for (int i = 0; i < 1024; i++) { a[i] = i; b[i] = i; }

       a.sync(vrt::SyncType::HOST_TO_DEVICE);
       b.sync(vrt::SyncType::HOST_TO_DEVICE);

       // One-call blocking form (set args + start + wait):
       vadd.call(a, b, c, 1024);

       // Equivalent staged form:
       //   vadd.setArg("a", a);
       //   vadd.setArg("b", b);
       //   vadd.setArg("c", c);
       //   vadd.setArg("size", 1024);
       //   vadd.start();   // non-blocking
       //   vadd.wait();

       c.sync(vrt::SyncType::DEVICE_TO_HOST);
       // verify c[i] == 2*i
   }

**Key differences at a glance:**

* Device by BDF, not index. Programming and xclbin-loading combined into
  the constructor.
* No UUID. Kernel lookup by name only.
* No separate ``xrt::run`` object. ``call``/``start``/``setArg``/``wait``
  live on ``Kernel`` directly.
* Buffers are typed and element-counted. Memory placement via
  ``argMemoryConfig()``.
* Explicit ``sync()`` with enum direction instead of ``XCL_BO_SYNC_*``
  macros.
* Device and buffer cleanup is automatic via RAII, same as XRT.
