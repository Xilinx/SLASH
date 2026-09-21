..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

##################
System Emulation
##################

*System emulation* runs a software model of an Alveo V80 accelerator inside a
long-running, centrally configured daemon, so that SLASH applications can be
developed and tested without any FPGA hardware. Unlike *FPGA emulation* and
*FPGA simulation* — where VRT hosts a model of your kernels *inside your
application process* (see :doc:`/explanation/platform-modes`) — a
system-emulated accelerator lives in the **system emulation daemon** and
persists independently of any program that uses it, exactly as a physical card
persists across the programs that open it.

This page has two parts. `For Users`_ explains what system emulation offers and
what to expect from it. `For Developers`_ explains where it sits in the SLASH
stack and how the daemon is built. It assumes you have already built a small
SLASH application (see :doc:`/tutorials/user/getting-started`).

.. note::

   **Three "emulation" concepts, kept distinct.**

   * **FPGA emulation** and **FPGA simulation** model the *programmable logic*
     — your kernels — in software (see :doc:`/explanation/platform-modes`).
   * **System emulation** models the *whole accelerator and its driver* in
     software, in a persistent daemon.

   They are orthogonal and compose: a system-emulated accelerator is driven by
   a *model process*, and that model process is itself an FPGA emulation or
   simulation of your kernels.

*****
Usage
*****

Why System Emulation?
=====================

FPGA emulation and simulation already let you run without a board. But because
VRT builds the model *into your application process*, the model is born and dies
with the single program that created it, and only that program can talk to it.
That is fine for answering "is my kernel logic correct?" — and nothing more.

System emulation is different in a way that matters for testing whole systems:
**the accelerator does not belong to your process.** It lives in the daemon and
outlives any individual program, just as a real card does. That unlocks usage
patterns FPGA emulation and simulation cannot express:

- **Multiple processes, one accelerator.** Several programs can attach to the
  same emulated accelerator at once and coordinate their access, exactly as they
  would share a physical board.
- **One-shot tools.** A short-lived CLI command can configure the accelerator,
  start a computation, and exit — while the accelerator keeps running. A second
  command, launched later, can fetch the results. The state lives in the
  accelerator, not in any one process.
- **Fidelity to production.** Because the accelerator is managed centrally
  rather than by the application, the application you test is the application you
  deploy — there is no emulation-specific plumbing compiled into it.

The bigger promise
------------------

The long-term goal this design is built toward is emulating **entire clusters
of FPGAs on a single host.** Many emulated accelerators can run under one
daemon, and — because the daemon is configured centrally — the *network
topology* connecting them can live in the daemon's configuration rather than in
the applications.

This mirrors real hardware: on a physical cluster the wiring is external, and
your application needs no network-configuration tooling to use it. System
emulation aims to preserve that property, so that a distributed application can
be exercised against a *virtual* cluster without changing a line of its
networking code. (Virtual networking is not implemented yet; see
`Current Status`_.)

Current Status
==============

System emulation is still being integrated into the SLASH stack. As of today:

- The **system emulation daemon** is complete: it hosts simulated accelerators,
  exposes the driver's socket interface, and drives FPGA-simulation models.
- **libslash** already speaks to the daemon transparently — it detects whether a
  device is a real character device or an emulated socket and dispatches
  accordingly (see `For Developers`_).
- **vrtd and VRT do not yet select the emulated path automatically.** They still
  assume real hardware, so you cannot yet point an unmodified VRT application at
  an emulated accelerator.

In practice, the daemon is currently exercised directly through libslash rather
than through the full VRT/vrtd stack. Teaching vrtd to discover emulated
accelerators and VRT to select the socket transport — so that an ordinary
application and its vrtbin run against an emulated accelerator unchanged — is the
next integration step. The rest of this section describes the behaviour to
expect once that step lands.

What to Expect
==============

System emulation is a **functional** model, not a performance or cycle-accurate
one. Once integrated, it will let you — on any Linux host, with no board —

- discover accelerators, allocate buffers, and DMA data host↔device;
- launch kernels over AXI4-Lite, set arguments, and poll for completion;
- read and write registers through BAR MMIO and set the kernel clock;
- reconfigure an accelerator by loading a different vrtbin, and drive the
  hotplug / reset sequences around it;
- run several accelerators at once, and share each one across cooperating
  processes.

What Not to Expect (for now)
============================

- **No timing fidelity.** Latency, bandwidth, and cycle counts are meaningless.
  Use FPGA simulation for cycle-level insight into the logic, and real hardware
  for performance numbers.
- **Approximate register semantics.** Registers are backed by polled shared
  memory rather than a live bus, so the daemon models only the ``ap_start`` /
  ``ap_done`` control handshake and cannot observe the *order* or *timing* of
  individual accesses. Clear-on-read registers are not auto-cleared, write-only
  registers are not write-protected, and clear-on-handshake side effects are not
  instantaneous. Most compute kernels do not depend on these subtleties.
- **FPGA-simulation models only, for now.** The daemon drives ``vpp_sim``
  (simulation) model processes; driving ``vpp_emu`` (emulation) models is
  planned but not yet available.
- **Device memory is not persistent across reconfiguration.** HBM/DDR contents
  are owned by the model process, so reprogramming the accelerator starts from
  empty memory. On hardware, memory survives a logic reconfiguration.

**************
Implementation
**************

The SLASH kernel driver exposes each device as a character device driven by
``ioctl`` calls (see :doc:`/explanation/pcie-topology`). System emulation
replaces that boundary — and nothing above it:

.. code-block:: text

   Hardware path                         System-emulation path
   ─────────────                         ─────────────────────
   User Application                      User Application
        │                                     │
       VRT                                   VRT          ┐  transport
        │                                     │           │  selection
      vrtd                                  vrtd          ┘  not yet ported
        │                                     │
    libslash                              libslash
        │  ioctl()                            │  send/recv datagram
   ┌────┴─────┐                          ┌────┴──────────────┐
   │  slash   │  (kernel module)         │  system emulation │  (user-space
   │  driver  │                          │      daemon       │   daemon)
   └────┬─────┘                          └────┬──────────────┘
        │ PCIe                                │ ZeroMQ
   ┌────┴─────┐                          ┌────┴──────────────┐
   │   V80    │                          │   model process   │  (vpp_sim /
   │ hardware │                          │ (FPGA in software)│   vpp_emu)
   └──────────┘                          └───────────────────┘

The substitution is twofold:

- **Sockets instead of character devices.** For every device the driver would
  expose, the daemon creates an ``AF_UNIX`` / ``SOCK_SEQPACKET`` socket with the
  *same name* under a runtime directory (default ``/run/slash_sysemu``).
- **Datagrams instead of ioctls.** Each ``ioctl`` becomes a request/response
  datagram pair. Where an ioctl would return or accept a file descriptor (a BAR
  mapping, a DMA buffer, a queue-pair I/O channel), the descriptor travels as
  ``SCM_RIGHTS`` ancillary data and is referenced by index in the message body.

The dispatch seam lives in **libslash**: before an operation it checks whether
the underlying file is a character device or a socket, and either issues an
``ioctl`` or exchanges datagrams. Everything above libslash is transport-
agnostic — which is why the remaining integration work is for vrtd and VRT to
*choose* the emulated device paths, not to change how they talk to libslash.

Anatomy of the Daemon
=====================

One daemon hosts many accelerators, each keyed by its **board BDF** — the PCI
address without the function suffix, e.g. ``0000:61:00`` (see
:doc:`/explanation/pcie-topology`). An accelerator is assembled from:

The model process
-----------------

A separate process that models the FPGA — the ``vpp_sim`` or ``vpp_emu``
executable packaged inside a vrtbin (see :doc:`/explanation/vrtbin-format`). The
daemon talks to it over a ZeroMQ request/response protocol (see
:doc:`/reference/model-protocol/index`) to populate and fetch device memory and
to drive kernel execution. If the model process dies, the daemon treats the
accelerator as lost — just as a real card would vanish if its PCIe link dropped.

The emulated Physical Functions
-------------------------------

Two per accelerator, mirroring the board's PFs:

.. list-table::
   :header-rows: 1
   :widths: 25 15 60

   * - Socket
     - Emulates
     - Role
   * - ``slash_ctl<N>``
     - PF2
     - Device identity, BAR enumeration, and register access (BAR MMIO).
   * - ``slash_qdma_ctl<N>``
     - PF1
     - Queue-pair management and host↔device DMA transfers.

Hotplug control
---------------

A single, daemon-wide ``slash_hotplug`` socket handles lifecycle operations for
all accelerators. It is the emulated equivalent of the driver's hotplug control device.

BARs
----

Where the real driver hands out a mapping onto physical device memory, the
daemon hands out shared-memory regions that stand in for the board's BARs — a
128 MiB user region for kernel registers, the service-layer, and the static region.
Access is bracketed by locks so the daemon and the client see a
consistent view. This memory-backed, poll-based design is what keeps the daemon
simple, and is the source of the approximate register semantics noted under
`What Not to Expect`_.

Model control workers
---------------------

For each compute kernel in the design, the daemon runs a worker that watches the
kernel's control register in the user-region BAR. When it sees ``ap_start`` it
gathers the kernel's arguments, forwards them to the model process, waits for
``ap_done``, and writes the results back into the BAR — reproducing the
AXI4-Lite ``ap_ctrl`` handshake the client polls on. A separate worker services
the clock wizard so that frequency-setting calls succeed.

QDMA transfer sessions
----------------------

Each DMA transfer is serviced by a per-session worker that moves data between
the client's buffers and the model process (for HBM/DDR), or appends the client's
data to the staged vrtbin (for reconfiguration writes).

Lifecycle and Reconfiguration
=============================

Because the daemon models the whole board, it also models the board's *life
cycle*. An accelerator moves through a small set of states:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - State
     - Meaning
   * - **Inactive**
     - A known configuration, but no model process or PFs running.
   * - **Active**
     - Model process, kernel workers, and all PFs up — ready for use.
   * - **Partial**
     - Model process running, but one or more PFs have been removed.

Transitions are driven through the ``slash_hotplug`` socket with the same
operations the real driver offers — ``RESCAN``, ``REMOVE``, ``HOTPLUG``, and
``TOGGLE_SBR`` — serialised under a single lock, so the reset-and-reprogram
sequences that VRT and ``v80-smi`` perform work unchanged. ``TOGGLE_SBR`` even
blocks for about a second to imitate PCIe link retraining.

Reconfiguration mirrors how a real board is reprogrammed. To load a new design,
a vrtbin is written to a *staging* area over QDMA (to a dedicated reconfiguration
address), then the control function is removed and rescanned. On rescan the
daemon launches the model contained in the staged vrtbin; if it starts
successfully it becomes the accelerator's active design, otherwise the previous
design keeps running. A **default** vrtbin — a model that supports memory and
register round-trips but contains no kernels — ships with the daemon and is used
to bootstrap a fresh accelerator.

Future Directions
=================

The current release is a deliberately scoped first step. The design leaves room
for several capabilities expected to arrive later:

- **Full VRT/vrtd integration** — device discovery and transport selection onto
  the daemon, so unmodified applications target emulated accelerators (the gap
  in `Current Status`_).
- **Virtual networks of accelerators** — wiring emulated boards together to
  model multi-accelerator and distributed topologies. This is the north-star
  motivation for the whole approach.
- **A push-based BAR interface** — replacing today's polling model with
  read/write syscalls per register access, which removes the register-semantics
  approximations above and improves efficiency.
- **FPGA-emulation model support** — driving ``vpp_emu`` models once they can
  report kernel completion asynchronously.
- **Persistent device memory** — preserving HBM/DDR contents across
  reconfigurations.
- **Stronger model-process isolation** — sandboxing the model executable, which
  is effectively untrusted user code.

See Also
========

- :doc:`/explanation/platform-modes` — FPGA emulation and simulation, and how
  they differ from system emulation.
- :doc:`/explanation/pcie-topology` — the PFs, BDFs, and hotplug operations that
  system emulation mirrors.
- :doc:`/explanation/architecture` — the full SLASH software stack.
- :doc:`/explanation/vrtbin-format` — what a vrtbin contains, including the
  model executable.
- :doc:`/reference/model-protocol/index` — the ZeroMQ protocol between the
  daemon and a model process.
