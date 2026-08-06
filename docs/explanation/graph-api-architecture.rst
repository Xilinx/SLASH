..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2026 Advanced Micro Devices, Inc

######################
Graph API Architecture
######################

The Graph API is a typed, hardware-style authoring and execution interface
for applications naturally described as dataflow with structured control.
``Graph`` and ``RegionBuilder`` provide named struct-literal connections;
``GraphRegion`` and ``IOMap`` provide the lower-level netlist surface.
Compiler stage IR and backend programs remain implementation details.

Compilation Pipeline
====================

.. code-block:: text

   Graph / GraphRegion netlist authoring
              |
              v
        AuthoredGraph snapshot
              |
              v
      validate authored graph
              |
              v
   Resolved -> Placed -> Routed -> Scheduled
              |
              v
      direct executable assembler
          /        |        \
        CPU       RP1       HIP
      program   packets    program
              |
              v
          Execution

Authored validation checks ownership, scope, named port bindings, producer
uniqueness, and control completeness. It collects a complete region before
checking consumers, so forward references are legal and textual authoring
order is not execution order. Resolution assigns strong ``NodeId``,
``RegionId``, and ``ValueId`` identities, checks topology, and makes in-place
value versions and control boundaries explicit. Placement selects devices and
materializes typed value replicas. Routing selects transfer mechanisms using
source and destination locations. Scheduling emits queue-local steps,
dependencies, and logical rendezvous.

Direct Backend Lowering
=======================

Every ``IDevice`` lowers one scheduled ``QueueProgram`` directly through
``lowerQueue``. There is no public or compatibility graph between scheduling
and backend lowering.

The executable assembler owns resource leases, runtime state, bridge actions,
device pins, and graph I/O metadata. The resulting ``Execution`` exposes
token-keyed writes and reads plus launch and wait operations.

Cross-Device Transfers
======================

Transfer capabilities are derived from registered devices and bridge
factories. Routing may select a direct bridge, a host bounce, or a
host-mediated same-device memory-region copy. The scheduled graph expresses
producer, action, and consumer steps with logical rendezvous; physical
resources are assigned only while assembling executables.

FPGA Control
============

FPGA queues lower scheduled operations directly into RP1 packet images.
Kernel argument names and order are backend ABI metadata. Graph dependencies
use typed compiler identities.

Control entirely owned by the FPGA can execute autonomously. Control spanning
CPU and FPGA queues uses an authority/follower protocol with logical value,
decision, and acknowledgement rendezvous. Resource leasing maps those logical
events to physical RP1 slots.

Failure Model
=============

Compilation returns structured diagnostics for invalid scope, topology, port
binding, placement, routing, control, image safety, and resource requirements.
Runtime validation is limited to dynamic execution values such as symbolic
buffer sizes and supplied byte counts.
