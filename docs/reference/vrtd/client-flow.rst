..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#############
Client Flow
#############

``vrtd`` (the *V80 Runtime Daemon*) multiplexes access to SLASH-managed FPGA
devices and enforces permission rules for multi-tenancy. Applications talk to
``vrtd`` over a Unix domain socket via:

- **C API**: *libvrtd* (``<vrtd/vrtd.h>``)
- **C++ wrapper**: *libvrtd++* (``vrtd::Session``, ``vrtd::Device``, ``vrtd::Bar``,
  ``vrtd::BarFile``)

Pipeline
========

.. code-block:: text

   +-----------+     +----------+     +-----------+     +---------+     +--------+
   |  libvrt   | <-- | libvrtd++| <-- |  libvrtd  | <-- |  vrtd   | <-- |libslash|
   +-----------+     +----------+     +-----------+     +---------+     +--------+
                                             AF_UNIX / SOCK_SEQPACKET
                                             sendmsg/recvmsg (+SCM_RIGHTS)

Roles
-----

- **SLASH kernel module / libslash**: low-level device control.
- **vrtd**: daemon that arbitrates access and permissions (multi-tenant).
- **libvrtd (C)**: wire protocol client; exposes typed requests/responses.
- **libvrtd++ (C++)**: safer RAII/exception wrapper on top of libvrtd.

Quick Start (C++)
=================

Minimal program that opens a session, grabs device 0, opens BAR 0, and reads a
``uint32_t`` via RAII:

.. code-block:: cpp

   #include <vrtd/session.hpp>
   #include <vrtd/device.hpp>
   #include <vrtd/bar.hpp>
   #include <vrtd/bar_file.hpp>
   #include <cstdint>
   #include <iostream>

   int main() {
     try {
       vrtd::Session s;  // connects to the standard socket path

       auto n = s.getNumDevices();
       if (n == 0) {
         std::cout << "No devices\n";
         return 0;
       }

       vrtd::Device d = s.getDevice(0);
       vrtd::Bar    b = d.getBar(0);

       vrtd::BarFile bf = b.openBarFile();

       auto p = bf.getPtr<std::uint32_t>(vrtd::BarFile::Direction::Read, /*address=*/0);
       std::uint32_t value = *p;  // read via volatile
       (void)value;

       bf.close();
     } catch (const vrtd::Error& e) {
       std::cerr << "vrtd error: " << e.what() << "\n";
       return 1;
     }
     return 0;
   }

Quick Start (C)
===============

Same operation using the C API with explicit bracketing for memory access:

.. code-block:: c

   #include <vrtd/vrtd.h>
   #include <slash/ctldev.h>
   #include <stdint.h>
   #include <stdio.h>
   #include <unistd.h>

   int main() {
     int fd = vrtd_connect(VRTD_STANDARD_PATH);
     if (fd < 0) { perror("vrtd_connect"); return 1; }

     uint32_t num = 0;
     if (vrtd_get_num_devices(fd, &num) != VRTD_RET_OK || num == 0) {
       fprintf(stderr, "no devices or error\n"); close(fd); return 1;
     }

     struct slash_bar_file bf = {0};
     enum vrtd_ret r = vrtd_open_bar_file(fd, /*dev=*/0, /*bar=*/0, &bf);
     if (r != VRTD_RET_OK) {
       fprintf(stderr, "open bar failed: %d\n", (int)r); close(fd); return 1;
     }

     slash_bar_file_start_read(&bf);
     volatile uint32_t *p = (volatile uint32_t*)((volatile uint8_t*)bf.map + 0);
     uint32_t value = *p;
     slash_bar_file_end_read(&bf);
     (void)value;

     vrtd_close_bar_file(&bf);
     close(fd);
     return 0;
   }

End-to-End Flow
===============

This section walks the common path from connection to BAR memory access,
showing the C and C++ entry points side-by-side.

1) Connect
----------

- **C**: ``int fd = vrtd_connect(VRTD_STANDARD_PATH);``
  Returns ``fd >= 0`` on success (caller owns and must ``close(fd)``),
  or ``-1`` with ``errno`` set on failure.

- **C++**: ``vrtd::Session s;`` or ``vrtd::Session s{"/run/vrtd.sock"};``
  Throws ``vrtd::Error(VRTD_RET_BAD_CONN)`` on failure.
  RAII — destructor calls ``close()``. Thread-safe (internal mutex).

2) Discover Devices
-------------------

- **C**:

  - ``vrtd_get_num_devices(fd, &count)`` — returns ``VRTD_RET_OK`` on success.
  - ``vrtd_get_device_info(fd, dev_index, &info)`` — fills
    ``struct vrtd_device_info`` (name + PCI BDF/IDs).
  - ``vrtd_get_device_by_bdf(fd, "0000:65:00.0", &dev_index)`` — lookup by BDF.

- **C++**:

  - ``uint32_t n = s.getNumDevices();``
  - ``vrtd::Device d = s.getDevice(i);`` — throws ``vrtd::Error(VRTD_RET_NOEXIST)``
    if out of range.
  - Accessors: ``d.getNum()``, ``d.getName()``, ``d.getBdf()``.
  - Any ``Device`` becomes invalid if its originating ``Session`` is closed or moved.

3) BAR Metadata
---------------

- **C**: ``vrtd_get_bar_info(fd, dev, bar, &info)`` fills
  ``struct slash_ioctl_bar_info`` with usability flag, start address, and
  length (bytes).

- **C++**: ``vrtd::Bar b = d.getBar(bar_index);``
  Query: ``b.isUsable()``, ``b.isInUse()``, ``b.getStartAddress()``,
  ``b.getLength()`` (bytes, physical).

4) Obtain BAR FD
----------------

- **C**: ``vrtd_get_bar_fd(fd, dev, bar, &bar_fd, &len)`` receives ``bar_fd``
  via ``SCM_RIGHTS``. Caller owns and must ``close(bar_fd)``.

- **C++**: Not called directly — ``Bar::openBarFile()`` handles this internally.

5) Map the BAR
--------------

- **C**: ``vrtd_open_bar_file(fd, dev, bar, &bf)`` fills ``struct slash_bar_file``
  with ``bf.fd``, ``bf.map``, and ``bf.len``. Unmap with ``vrtd_close_bar_file(&bf)``.

- **C++**: ``vrtd::BarFile bf = b.openBarFile();`` — RAII; owns FD + mapping.
  ``bf.close()`` or destructor releases resources.

6) Access BAR Memory
--------------------

- **C**:

  - Read: ``slash_bar_file_start_read(&bf);`` … access via ``volatile`` pointer
    into ``bf.map`` … ``slash_bar_file_end_read(&bf);``
  - Write: ``slash_bar_file_start_write(&bf);`` … access …
    ``slash_bar_file_end_write(&bf);``
  - Use ``(volatile uint8_t*)bf.map + offset`` to compute addresses.

- **C++**:

  - ``auto p = bf.getPtr<T>(vrtd::BarFile::Direction::Read, offset);``
    Returns a move-only ``vrtd::BarFilePtr<T>`` that brackets the operation and
    ends it on destruction.
  - Only one operation (read or write) may be active at a time per ``BarFile``.
  - Raw pointer: ``bf.getRawPtr(offset)`` — caller must manually bracket with
    ``slash_bar_file_start_*`` / ``_end_*``.

Error Model
===========

- **C** functions return ``vrtd_ret``. Check for ``VRTD_RET_OK`` before using
  outputs.
- **C++** methods throw ``vrtd::Error``; transport failures map to
  ``VRTD_RET_BAD_CONN``. ``vrtd::Error::what()`` returns a static,
  human-readable string.
- Local misuse in ``BarFile`` / ``BarFilePtr`` throws ``std::runtime_error``.

Common return codes:

- ``VRTD_RET_OK`` — success.
- ``VRTD_RET_BAD_LIB_CALL`` — bad library usage (e.g. null out-pointer).
- ``VRTD_RET_BAD_CONN`` — broken transport (socket errors, daemon not running).
- ``VRTD_RET_BAD_REQUEST`` — malformed request.
- ``VRTD_RET_INVALID_ARGUMENT`` — invalid argument.
- ``VRTD_RET_NOEXIST`` — resource does not exist (e.g. out-of-range index).
- ``VRTD_RET_INTERNAL_ERROR`` — daemon-side failure; check vrtd logs.
- ``VRTD_RET_AUTH_ERROR`` — permission denied by role configuration.

Thread Safety
=============

- **Session / Device / Bar (C++)**: public methods are thread-safe (internal
  mutex). Object validity is tied to the originating session — closing or
  moving a session invalidates previously obtained ``Device`` / ``Bar`` values.
- **BarFile / BarFilePtr (C++)**: not thread-safe. Only one read or write
  operation may be active at a time. Re-entrant ``getPtr()`` calls throw.

Lifetime and Moves (C++)
=========================

- Moving or closing a ``Session`` invalidates all ``Device`` and ``Bar`` objects
  obtained from it (subsequent calls throw).
- ``BarFile`` is move-only. Ensure all ``BarFilePtr`` instances have been
  destroyed before calling ``bf.close()`` or letting the destructor run,
  otherwise an exception is thrown.

Wire Protocol
=============

- Transport: ``AF_UNIX`` + ``SOCK_SEQPACKET``.
- Messages: request/response headers (size, opcode, seqno) + body.
- FD passing: responses may carry a file descriptor via ``SCM_RIGHTS``
  (e.g. for BAR file access).
- Size limits: request body must not exceed ``VRTD_MSG_MAX_SIZE`` minus headers.
- Generic escape hatch: ``vrtd_raw_request`` sends arbitrary opcodes; prefer
  typed helpers for normal use.

Troubleshooting
===============

- **Out-of-range device index** → ``VRTD_RET_NOEXIST`` (C) or ``vrtd::Error`` (C++).
- **Session closed/moved, then using Device/Bar** → throws (invalid lifetime).
- **Two concurrent ``getPtr()`` calls on the same BarFile** → throws re-entrancy error.
- **Transport errors** (socket down, daemon not running) → map to
  ``VRTD_RET_BAD_CONN`` / ``vrtd::Error`` with "connection" message.
