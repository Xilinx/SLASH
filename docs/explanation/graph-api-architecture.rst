..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

######################
Graph API Architecture
######################

The Graph API is a higher-level VRT interface for applications that are better
described as a graph of work than as a sequence of manual kernel launches. It
lets an application declare typed buffers and scalars, place kernels on devices,
and express loops, conditionals, and explicit ordering constraints. Compilation
then lowers that authored graph into per-device execution plans.

For a worked introduction, see :doc:`/tutorials/user/graph-api`. For the API
surface, see :doc:`/reference/vrt-api/graph` and
:doc:`/reference/vrt-api/graph-devices`.

Compilation Pipeline
====================

A ``vrt::graph::Graph`` is authoring state. Calling ``compile()`` produces a
``CompiledGraph`` snapshot that can run independently of later edits to the
original graph.

.. code-block:: text

   User code
      |
      v
   Graph / GraphRegion
      authored kernels, tokens, loops, conditionals, reprogram ops
      |
      v
   GraphCompiler
      validates scopes, types, dependencies, devices, and bridge coverage
      |
      v
   DGraph per device
      compiled nodes assigned to one IDevice, plus child DGraphs for regions
      |
      v
   CompiledGraph
      owns device plans, scalar state, and pinned bridge instances
      |
      v
   run() / launch() + wait()

The authored graph is structured. The root graph is a ``GraphRegion``; each
loop body and conditional branch is a child ``GraphRegion`` with explicit
boundary mappings for values that cross scope boundaries. The compiler validates
that every consumed token has a producer, that types match the declared kernel
ports, that explicit ``after`` dependencies refer to valid operations, and that
cross-device data movement can be routed.

Device Abstraction
==================

Every compute target implements ``IDevice``. A device has:

- a device type, such as ``DeviceType::CPU`` or ``DeviceType::FPGA``;
- a unique id, such as ``cpu`` or ``fpga:0``;
- a ``compilePlan(DGraph)`` method that lowers its per-device subgraph into an
  executable ``IDevicePlan``.

Kernel placement is resolved by device id during graph compilation. The device
runtime is therefore responsible only for its own ordered node list; the graph
compiler is responsible for proving the whole graph is well-formed and for
inserting bridge operations wherever device boundaries are crossed.

CPU Device Invariant
====================

A graph may host at most one CPU-typed device. ``Graph::withDefaults()`` creates
the canonical CPU device under the id ``cpu`` and registers the production
bridge factories available in the current build.

The single CPU device has two architectural roles:

- It is the fallback executor for structured control flow that cannot run
  autonomously on a device queue.
- It is the bounce hub for cross-device transfers when no direct bridge exists
  for a device-type pair.

This invariant keeps routing deterministic. If a transfer cannot use a direct
bridge from source to destination, the compiler can try a two-hop route through
the one CPU device instead of choosing among multiple host devices.

Cross-Device Bridges
====================

Bridges are registered by ordered device-type pairs. A bridge factory produces
one concrete bridge instance for a concrete source and destination device pair.
For each cross-device dependency, the bridge returns a producer-side closure,
a consumer-side readiness probe, a consumer-side action, and an opaque shared
operation object.

The compiler splices these closures into the relevant per-device ``DGraph`` objects:

.. code-block:: text

   producer device DGraph              consumer device DGraph
   ----------------------              ----------------------
   kernel producing token
   bridge producer action  -------->   bridge consumer readiness/action
                                      kernel consuming token

If a direct bridge exists for ``src -> dst``, the compiler emits one bridge
leg. If not, it asks ``BridgeRouter`` for a CPU-bounce route:

.. code-block:: text

   source device  ->  cpu  ->  destination device

The same bridge mechanism handles buffer transfers, scalar transfers, and pure
ordering barriers introduced by cross-device ``after`` dependencies.

Same-Device Memory Routing
==========================

Some devices have multiple independently addressed local memory regions. On the
V80, an FPGA kernel port connected to HBM0 cannot use an address allocated from
HBM1. For ordinary buffer dependencies inside one FPGA device, the graph
compiler treats a region mismatch as an explicit same-device copy rather than a
cross-device bridge:

.. code-block:: text

   producer kernel (HBM0)
   device-copy node: HBM0 -> HBM1 (host/QDMA fallback)
   consumer kernel (HBM1)

The copy is visible in compiled ``DGraph`` rendering and currently uses a
host/QDMA fallback: the source device buffer is synchronized to host memory,
copied, and synchronized back to the destination HBM region. This keeps the
operation correct before RP1 has an HBM-capable DMA path. It also makes the
cost visible instead of hiding it behind a silent address mismatch.

Cross-bank in-place buffers and loop-carried buffers are intentionally still
rejected. Those flows require per-iteration versioning or ping-pong buffers,
which would change the autonomous FPGA loop contract.

FPGA Control
============

The FPGA graph backend lowers a per-device ``DGraph`` into RP1 operations. A
kernel dispatch becomes an RP1 kernel-dispatch packet, and an explicit
``addReprogram`` node becomes an RP1 ``PDI_LOAD`` operation for the selected
image. The vbin/PDI metadata path is described in
:ref:`Graph API and RP1 <graph-api-and-rp1>`.

Structured control flow has two execution modes:

All-FPGA control
   If an entire loop or conditional body is assigned to the FPGA and satisfies
   the backend's constraints, it can run autonomously on the FPGA queue. The
   compiler lowers it to RP1 loop or conditional control operations plus the
   child graph packets they execute.

Split CPU/FPGA control
   If a control operation spans CPU and FPGA work, the compiler splits the
   control operation into per-queue participants. The CPU side acts as the
   authority for the control decision, while the FPGA side follows through
   signal/wait rendezvous slots visible through the FPGA BAR window.

This split lets independent CPU and FPGA work proceed concurrently while still
preserving graph-level ordering at loop iterations, branch boundaries, and
cross-device token transfers.

Failure Model
=============

Graph validation is intentionally front-loaded into ``compile()``. Typical
compile-time failures include missing devices, duplicate port bindings, type
mismatches, cycles, invalid ``after`` dependencies, missing bridge factories,
and FPGA dispatches that are not gated behind a reprogram of the active image.

Runtime validation still exists for values only known at execution time: for
example, ``CompiledGraph::launch()`` requires all symbolic size scalars to be
set, and ``CompiledGraph::write()`` / ``read()`` validate byte counts against
resolved buffer sizes.
