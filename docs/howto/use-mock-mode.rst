..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

################
Use Mock Mode
################

This guide shows how to use the libslash mock mode to test BAR access code
without a physical V80 board.

Prerequisites
=============

- libslash built and installed (or available in the build tree).
- No V80 hardware required.

What is Mock Mode?
==================

Mock mode provides an in-memory substitute for a V80 control device. When
you open a device with the special path ``"@mock"``, libslash creates a
temporary backing file instead of talking to the kernel driver. This lets
you develop and test BAR read/write logic on any Linux machine — including
CI runners with no FPGA hardware.

Opening a Mock Device
=====================

Pass the sentinel string ``"@mock"`` to ``slash_ctldev_open()``:

.. code-block:: c

   #include <slash/ctldev.h>

   struct slash_ctldev *ctldev = slash_ctldev_open("@mock");
   if (!ctldev) {
       perror("mock open failed");
       return -1;
   }

The returned handle behaves like a real control device for BAR operations.

What Mock Mode Provides
=======================

- **BAR 0** is usable with a size of 64 MB.
- A temporary backing file is created in ``$XDG_RUNTIME_DIR`` (or ``/tmp``
  as fallback) and memory-mapped with ``MAP_SHARED``.
- Full 32-bit and 64-bit read/write through the mapped pointer.
- ``slash_device_info_read()`` returns a zeroed BDF (``0000:00:00.0``).
- ``slash_bar_info_read(ctldev, 0)`` reports BAR 0 as usable with
  ``length = 64 * 1024 * 1024``.

What Mock Mode Does Not Provide
================================

- **BARs 1–5** are reported as unusable (zero length).
- **QDMA** queue pairs — no DMA transfers.
- **Hotplug** operations — no PCIe reset or rescan.
- **Real PCIe enumeration** — no sysfs interaction.
- **Sensor readings** — no AMI management interface.
- **DMA-buf sync** — ``slash_bar_file_start_write()`` and
  ``slash_bar_file_end_write()`` are no-ops on mock devices.

Example: Read and Write BAR Registers
========================================

.. code-block:: c

   #include <slash/ctldev.h>
   #include <fcntl.h>
   #include <assert.h>
   #include <stdint.h>

   int main(void) {
       /* Open mock device */
       struct slash_ctldev *ctldev = slash_ctldev_open("@mock");
       assert(ctldev);

       /* Query BAR 0 properties */
       struct slash_ioctl_bar_info *info = slash_bar_info_read(ctldev, 0);
       assert(info->usable);
       assert(info->length == 64 * 1024 * 1024);
       free(info);

       /* Map BAR 0 for read/write */
       struct slash_bar_file *bar = slash_bar_file_open(ctldev, 0, O_RDWR);
       assert(bar);

       /* Write and read back through the mapped region */
       uint32_t *regs = (uint32_t *)bar->map;
       regs[0] = 0xDEADBEEF;
       assert(regs[0] == 0xDEADBEEF);

       regs[4] = 0xA5A5A5A5;
       assert(regs[4] == 0xA5A5A5A5);

       /* Clean up — backing file is deleted automatically */
       slash_bar_file_close(bar);
       slash_ctldev_close(ctldev);
       return 0;
   }

Using Mock Mode in Tests
========================

The libslash test suite in ``driver/libslash/tests/`` uses mock mode for all
its unit tests. This pattern works well for CI pipelines:

1. Open the device with ``"@mock"`` — no hardware detection needed.
2. Exercise BAR read/write logic against the 64 MB mapped region.
3. Verify register values, offsets, and access patterns.
4. Clean up — the temporary file is unlinked when the device is closed.

Since mock devices require no kernel module or daemon, tests can run in
unprivileged containers and in any CI environment.

Backing File Internals
======================

When a mock device is opened, libslash:

1. Generates a random filename (``slash.mock.<random>`` in
   ``$XDG_RUNTIME_DIR`` or ``/tmp``).
2. Creates the file with ``O_RDWR | O_CREAT | O_EXCL`` and mode ``0600``.
3. Truncates to 64 MB.
4. Maps with ``mmap(PROT_READ | PROT_WRITE, MAP_SHARED)``.

On close, the file is ``munmap``'d and ``unlink``'d. If the process
terminates abnormally, the file remains in the temp directory but occupies
only as much disk space as was actually written (sparse file).

See Also
========

- :doc:`/explanation/architecture` — how libslash fits in the SLASH stack.
- :doc:`/reference/libslash-api/ctldev` — full control device API reference.
- :doc:`/explanation/pcie-topology` — the real PCIe functions that mock mode
  replaces.
