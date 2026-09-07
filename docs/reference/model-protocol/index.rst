..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

#######################
FPGA Model ZMQ Protocol
#######################

When a VRT application targets a software-emulation (``vpp_emu``) or RTL-simulation
(``vpp_sim``) platform instead of hardware, the VRT runtime does not drive a PCIe
device. Instead it speaks a ZeroMQ request/reply protocol to a model process that
plays the role of the card. This reference specifies that wire protocol: the
transport, the framing, the two distinct command dialects, and every command verb
with its request schema, payload, and reply.

The protocol has two peers:

- **Client** — the VRT runtime (``ZmqServer``, ``vrt/src/utils/zmq_server.cpp``).
  It issues requests.
- **Model** — the generated emulation or simulation executable that ships inside
  the VBIN. It services requests. The two model implementations are
  ``vpp_emu`` (``linker/slashkit/resources/templates/sw_emu_tb.cpp``) and
  ``vpp_sim`` (``linker/slashkit/resources/sim/sim.cpp``).

.. important::

   ``vpp_emu`` and ``vpp_sim`` speak **disjoint dialects** of this protocol over
   the same transport. ``vpp_sim`` is purely **address-keyed**; ``vpp_emu`` is
   **named and command-oriented**. A peer must know which model it is talking to —
   the protocol carries no platform discriminator in the framing. See
   `The two dialects`_.

Transport and Framing
=====================

Transport
---------

- **Library:** ZeroMQ.
- **Pattern:** ``REQ`` (client) ↔ ``REP`` (model). Strict lock-step: every request
  receives exactly one reply, and at most one request is in flight at any time.
  Callers must serialize all model I/O for a given model process behind a single
  request/reply cycle.
- **Endpoint:** the model binds the endpoint; the client connects to it. The
  endpoint is passed to the model as its sole command-line argument (``argv[1]``).
  Both a ZMQ ``tcp://`` endpoint (historically ``tcp://*:5555``) and a ZMQ
  ``ipc://`` endpoint (an ``AF_UNIX`` socket path) are valid. One endpoint serves
  one model process.

Request framing
---------------

A request is **one or two ZMQ frames**:

- **Frame 0** — a JSON object: the command. It is encoded one of two ways and a
  parser must accept **both**:

  - compact, single-line (``Json::writeString``) — used by most commands;
  - pretty-printed, multi-line (``Json::Value::toStyledString``) — used by the
    buffer- and stream-populate commands.

  The models parse frame 0 with a tolerant ``Json::Reader``.

- **Frame 1** — a raw binary payload, present **only** for ``populate`` and
  ``stream_in``. It is sent with the ``ZMQ_SNDMORE`` flag on frame 0 and carries
  the bytes to be written into device/buffer memory.

Reply framing
-------------

A reply is always a **single frame**. There is no envelope or type tag: the caller
knows from the command it sent which of the following reply shapes to expect.

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Reply shape
     - Meaning
   * - Literal ASCII ``"OK"``
     - Success for control commands, ``populate``, and ``stream_in``.
   * - Literal ASCII ``"ERR"``
     - Failure reported by the model. The client treats **any** reply other than
       ``"OK"`` (where ``"OK"`` is expected) as an error and raises.
   * - JSON value
     - A scalar number, an array of byte integers (a buffer read-back), or an
       error object ``{"error": ...}``.
   * - Raw binary
     - Returned only by ``stream_out``; the bytes are returned directly with no
       JSON wrapper.

The two dialects
================

The same transport carries two non-overlapping command sets, selected by which
model is running.

``vpp_sim`` — address-keyed
---------------------------

``vpp_sim`` drives real AXI-Lite / AXI-MM finite-state machines by **address**. It
implements only:

- ``populate{addr, size}`` — buffer write (H2C),
- ``fetch buffer{addr, size}`` — buffer read (C2H),
- ``fetch scalar{addr}`` — register/scalar read,
- ``reg{addr, val}`` — register write,
- global ``start`` and ``exit``.

There is **no kernel name anywhere** in the sim dialect. Every access is a raw
address into the device address space.

``vpp_emu`` — named and command-oriented
----------------------------------------

``vpp_emu`` is driven by **named, fully-formed commands**. It implements:

- ``populate{name, size}`` / ``fetch buffer{name}`` — buffer transfer by name,
- ``read_register{function, offset}`` / ``fetch scalar{function, arg}`` —
  register and scalar reads routed by kernel function,
- ``stream_in{name}`` / ``stream_out{name, size}`` — streaming buffer transfer,
- ``call{function, args}`` / ``start{function, args}`` — synchronous /
  asynchronous kernel launch,
- ``wait{function}`` — join an asynchronous launch,
- ``exit``.

``vpp_emu`` has **no** ``reg{addr, val}`` handler and **no** address-keyed write
path. Register reads are served from a per-kernel shadow register file seeded from
the VBIN's ``emu_manifest.json``. The only way to make a kernel execute is a
complete, correctly typed ``call`` or ``start``; the model validates argument count
and kind and rejects malformed launches.

Command Reference
=================

Notation: **F0** is the frame-0 JSON object, **F1** is the optional frame-1 binary
payload. The "Model" column names which dialect implements the verb.

.. list-table::
   :header-rows: 1
   :widths: 16 34 8 26 16

   * - Command
     - F0 fields
     - F1
     - Reply
     - Model
   * - ``start`` (global)
     - ``{command:"start"}``
     - –
     - ``"OK"``
     - sim
   * - ``populate`` (named)
     - ``{command:"populate", name, size}``
     - bytes
     - ``"OK"``
     - emu
   * - ``populate`` (addr)
     - ``{command:"populate", addr, size}``
     - bytes
     - ``"OK"``
     - sim
   * - ``fetch`` buffer (named)
     - ``{command:"fetch", type:"buffer", name}``
     - –
     - JSON array of byte ints
     - emu
   * - ``fetch`` buffer (addr)
     - ``{command:"fetch", type:"buffer", addr, size}``
     - –
     - JSON array of byte ints
     - sim
   * - ``fetch`` scalar (named)
     - ``{command:"fetch", type:"scalar", function, arg [, offset]}``
     - –
     - JSON uint, or ``{error}``
     - emu
   * - ``fetch`` scalar (addr)
     - ``{command:"fetch", type:"scalar", addr}``
     - –
     - JSON uint
     - sim
   * - ``read_register``
     - ``{command:"read_register", function, offset}``
     - –
     - JSON uint, or ``{error, function, offset}``
     - emu
   * - ``reg``
     - ``{command:"reg", addr, val}``
     - –
     - ``"OK"``
     - sim
   * - ``stream_in``
     - ``{command:"stream_in", name}``
     - bytes
     - ``"OK"``
     - emu
   * - ``stream_out``
     - ``{command:"stream_out", name, size}``
     - –
     - **raw bytes**
     - emu
   * - ``call``
     - ``{command:"call", function, args:{...}}``
     - –
     - ``"OK"`` / ``"ERR"``
     - emu
   * - ``start`` (kernel)
     - ``{command:"start", function, args:{...}}``
     - –
     - ``"OK"`` / ``"ERR"``
     - emu
   * - ``wait``
     - ``{command:"wait", function}``
     - –
     - ``"OK"`` / ``"ERR"``
     - emu
   * - ``exit``
     - ``{command:"exit"}``
     - –
     - ``"OK"``
     - both

Control commands
----------------

``start`` (global)
   Issued by the sim runtime at initialization to start the global simulation
   clock/driver. No arguments. Reply ``"OK"``.

``exit``
   Tears down the model. The model replies ``"OK"`` and then terminates its worker
   loop. Sent by the runtime at teardown.

Buffer transfers
----------------

``populate``
   Writes host bytes into device/buffer memory (host-to-device). Frame 1 carries
   the raw payload; ``size`` is its byte length.

   - **sim:** ``{command:"populate", addr, size}`` — ``addr`` is the device-side
     physical address.
   - **emu:** ``{command:"populate", name, size}`` — ``name`` is the buffer name
     (see `Naming conventions`_).

   Reply ``"OK"``.

``fetch`` (buffer)
   Reads device/buffer memory back to the host (device-to-host). The reply is a
   JSON **array of byte-sized integers**.

   - **sim:** ``{command:"fetch", type:"buffer", addr, size}``.
   - **emu:** ``{command:"fetch", type:"buffer", name}``.

Register and scalar access
--------------------------

``reg`` (sim only)
   ``{command:"reg", addr, val}`` — writes ``val`` to the AXI-Lite register at
   ``addr``. Reply ``"OK"``. ``vpp_sim`` performs all register writes as 32-bit
   AXI-Lite accesses.

``fetch`` (scalar)
   Reads a scalar / register value, replied as a JSON unsigned integer.

   - **sim:** ``{command:"fetch", type:"scalar", addr}``.
   - **emu:** ``{command:"fetch", type:"scalar", function, arg [, offset]}`` —
     routed by kernel ``function`` and functional-argument ``arg``; ``offset`` is
     optional. May reply ``{error: ...}`` if the function/arg cannot be resolved.

``read_register`` (emu only)
   ``{command:"read_register", function, offset}`` — reads a kernel control/status
   register from the kernel's shadow register file. Reply is a JSON unsigned
   integer, or ``{error, function, offset}`` if the function or offset is unknown.
   Used for example to poll the ``ap_done`` bit at control register offset ``0``.

Streaming (emu only)
--------------------

``stream_in``
   ``{command:"stream_in", name}`` + frame 1 bytes — pushes a payload into the
   named input streaming buffer (host-to-device). Reply ``"OK"``.

``stream_out``
   ``{command:"stream_out", name, size}`` — pulls ``size`` bytes from the named
   output streaming buffer (device-to-host). The reply is **raw binary** (the bytes
   directly), not JSON.

Kernel launch (emu only)
------------------------

``call`` / ``start``
   Launch a kernel function. ``call`` is synchronous (the reply is sent only after
   the kernel completes); ``start`` is asynchronous (the reply acknowledges launch,
   and the caller later joins with ``wait`` or by polling ``read_register`` for
   ``ap_done``). Both take the same schema::

       {command:"call",  function:"<instance>", args:{...}}
       {command:"start", function:"<instance>", args:{...}}

   Reply ``"OK"`` on success or ``"ERR"`` on validation failure (wrong argument
   count or wrong argument kind).

``wait``
   ``{command:"wait", function}`` — blocks until the named asynchronously-started
   kernel completes. Reply ``"OK"`` / ``"ERR"``.

Argument encoding
~~~~~~~~~~~~~~~~~

The ``args`` object of ``call`` / ``start`` maps each functional argument by index
to a typed entry. The key is ``arg<idx>`` where ``idx`` is the **functional-argument
index** from ``system_map.xml`` (not a register offset):

.. code-block:: json

   {
     "arg0": { "type": "buffer", "name": "<decimal physical address>" },
     "arg1": { "type": "scalar", "value": 42 }
   }

- **buffer argument** — ``{type:"buffer", name:"<addr>"}`` where ``name`` is the
  decimal string of the buffer's physical address.
- **scalar argument** — ``{type:"scalar", value:<u64>}``.

The model validates that the supplied arguments match the kernel's declared
functional arguments in count and kind.

Naming conventions
==================

In the ``vpp_emu`` dialect, buffers and streams are referred to by name rather than
by address.

- **Buffer name** — a buffer's name is the **decimal string** of its physical
  address (e.g. address ``0x10000`` → name ``"65536"``). Because the runtime uses
  the device address itself as the name, no separate name table is required: any
  device address can be formatted as a decimal string to name its buffer.
- **Stream names** — input streams are named ``streamingBuffer_<qid>`` and output
  streams ``outputStreamingBuffer_<qid>``, where ``<qid>`` is the streaming queue
  index.

VBIN metadata
=============

A model executable ships inside the VBIN together with the metadata needed to map
between the two dialects:

- ``system_map.xml`` — present in **every** VBIN. Describes each ``<Kernel>``:
  instance ``<Name>``, ``<BaseAddress>``, ``<Range>``, the register ``offset``
  list, and ``<functional_args>`` (per-arg ``idx``, ``offset``, ``range``, read/
  write flags, and ``port``), plus port-to-memory connections.
- ``emu_manifest.json`` — present in **EMU** VBINs. Describes per-kernel
  ``call_args`` (argument kinds), ``registers``, and ``fetch.scalar`` routes
  (function/arg → register offset and value source), plus the manifest schema
  version and autostart/callable/shutdown policy. This is the data the EMU model
  uses to seed its shadow register file and validate launches.
- ``vpp_emu`` / ``vpp_sim`` executable — one per platform.

The VBIN does **not** carry an allocator address map: the runtime assigns device
addresses and uses them directly as both the sim transfer address and the emu
buffer name.
