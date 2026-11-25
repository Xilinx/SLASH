# RP1 HSA Command Processor Architecture

## Status and source of truth

This document describes the implemented **protocol-v6** RP1 command processor.
RP1 is Cortex-R5 core 1 on the AMD Alveo V80. It executes a host-built graph
from shared DDR and publishes one rich, sequence-tagged result for the whole
graph.

The canonical wire ABI is
[`driver/libslash/include/slash/uapi/rp1_protocol.h`](../../../../../driver/libslash/include/slash/uapi/rp1_protocol.h).
The firmware package contains a staged copy at
[`include/slash/uapi/rp1_protocol.h`](include/slash/uapi/rp1_protocol.h).
Update the canonical header first, then synchronize and check the packaged copy:

```bash
python3 scripts/stage-rp1-protocol-header.py
python3 scripts/stage-rp1-protocol-header.py --check
```

The header is freestanding and shared by firmware, VRT, SMI, and tests.
Compile-time assertions fix every structure size and critical offset.

Protocol v6 makes these deliberate choices:

- There is one graph in flight per RP1.
- Dependency scheduling is private to RP1 and uses BTCM barriers.
- The active node prefix is snapshotted into BTCM before packet validation or
  execution; DDR node packets are never read or written on the execution path.
- There is **no per-node completion queue**.
- Every node error is fail-fast and stops new activation.
- Firmware publishes one 64-byte `rp1_graph_result_t` per accepted graph.
- Optional tracing carries per-node history and profiling detail.
- A failed graph is not transactional; its outputs must be treated as invalid.
- `graph_done_seq` is the final release point for the committed result.

## Why RP1 exists

The legacy VRT path drives each kernel synchronously from the host. Argument and
control writes cross PCIe, and the host polls for completion before launching
the next kernel. RP1 is on-die and can access the user-region AXI-Lite network
without PCIe round trips. The host stages a complete graph once; RP1 then
launches kernels, moves local DDR data, evaluates control flow, and performs
partial reconfiguration without host intervention.

The execution model has four important guarantees:

1. **Explicit parallelism.** Every barrier-ready node is eligible in the next
   scanner pass. Independent kernels can be in flight together.
2. **Static graph storage.** Firmware takes one immutable BTCM node snapshot;
   shared arguments and other graph data remain firmware-owned from the
   `graph_seq` doorbell until exact-sequence completion.
3. **Fail-fast errors.** The first validation, dispatch, timeout, image, or PDI
   error stops all later activation and starts terminal quiescence.
4. **Non-transactional effects.** Kernels, DMA, scalar writes, signals, and PDI
   loads completed before an error are not rolled back.

Kernels connected by AXI streams must still tolerate backpressure. RP1 starts
all barrier-ready endpoints, but it cannot repair a graph whose dependency
barriers make a stream consumer unreachable.

## Hardware paths

### RPU to the user region

The R5 uses `M_AXI_LPD` through `rpu_sc` and the NoC to reach user-region
AXI-Lite slaves. The base design maps the PCIe-visible user range
`0x0202_0000_0000` into the R5's 32-bit address space at `0x8800_0000`.
For a kernel address from `system_map.xml`:

```text
r5_addr = xml_addr - 0x0202_0000_0000 + 0x8800_0000
```

The linker does not yet emit a separate R5 address table. `FpgaVbinSpec`
performs this conversion at runtime; lower-level callers may provide an
explicit `FpgaKernelLocationLookup`.

### RPU to memory

The implemented phase-1 data path reaches DDR:

- `DMA_COPY` is an R5 software word copy between DDR addresses.
- `DMA_FILL` is an R5 software word fill in DDR.
- Host transfers still use QDMA before or after graph execution.
- Direct HBM and host-memory DMA from RP1 are not implemented.

The packet ABI reserves high address words and memory-type fields for later
hardware-DMA phases. Current firmware uses the low 32-bit addresses. Lengths
must be multiples of four; phase-1 firmware processes `length / 4` words.

### Partial reconfiguration

`PDI_LOAD` sends the Versal load-PDI request to the PMC through the R5_1-owned
IPI. Hardware builds generate `rp1_platform_config.h` from the R5_1 standalone
BSP. It defines the source agent, target mask, request/response buffers,
trigger register, observation register, and a non-zero platform identity.
QEMU uses an explicit fixture.

The host must order a PDI node after every kernel using the region being
reconfigured. RP1 tracks in-flight kernels, but it does not infer a safe
reconfiguration boundary from physical connectivity.

A PDI timeout can leave the IPI observation bit asserted while the PMC still
owns the request. Firmware therefore marks the image `UNKNOWN`, publishes
`RP1_RESULT_RECOVERY_REQUIRED`, and enters reset-only `ERROR`. The host must
quarantine graph storage and recover the card before another submission.

## Shared DDR layout

The host-visible RP1 aperture is 64 MiB at RP1 physical address
`0x3000_0000`. The default layout is:

```text
RP1 address       Size       Purpose
----------------  ---------  --------------------------------------------
0x3000_0000       4 KiB      Control block
0x3000_1000       256 KiB    Reserved node region; v6 uses at most 32 KiB
0x3004_1000       64 KiB     Reserved legacy-v4 gap; unused by v6
0x3005_1000       1 MiB      Packed kernel argument records
0x3015_1000       4 KiB      256 host-visible signal slots
0x3015_2000       up to 64K  Optional 4096-entry (64 KiB) trace ring
```

The corresponding `RP1_DEFAULT_*_OFFSET` values are conventions. Host code may
choose other aligned ranges inside the aperture and program the control block,
but firmware validates all shared ranges before converting them to pointers.
The legacy gap is intentionally not reclaimed in v6, so argument, signal, and
trace offsets remain stable.

## Protocol-v6 control block

`rp1_ctrl_t` occupies exactly 4 KiB. Ownership is per field:

```text
Offset  Field                         Writer       Meaning
------  ----------------------------  -----------  -----------------------------
0x00    magic                         RP1          Boot contract commit "SQR1"
0x04    version                       RP1          Must be 6
0x08    node_count                    Host         Submitted packet count
0x0c    _reserved_cq_size             Host         Must be zero
0x10    node_base_lo/hi               Host         Node-array address
0x18    _reserved_cq_base_lo/hi       Host         Must be zero
0x20    graph_seq                     Host         Submission doorbell
0x24    graph_done_seq                RP1          Final completed sequence
0x28    _reserved_cq_write_idx        RP1          Must remain zero
0x2c    _reserved_cq_read_idx         RP1          Must remain zero
0x30    rp1_state                     RP1          INIT/READY/RUNNING/ERROR/HALTED
0x34    rp1_error_code                RP1          Live first-error diagnostic
0x38    rp1_current_node              RP1          Last activated node
0x3c    heartbeat                     RP1          Liveness counter
0x40    arg_buf_base_lo/hi            Host         Argument-buffer address
0x48    sig_array_base_lo/hi          Host         Signal-array address
0x50    trace_enable                  Host         Non-zero enables tracing
0x54    trace_base_lo/hi              Host         Trace-ring address
0x5c    trace_size                    Host         Power-of-two entry capacity
0x60    trace_write_idx               RP1          Per-graph trace producer count
0x64    capabilities                  RP1          Implemented RP1_CAP_* mask
0x68    pdi_ipi_platform_id           RP1          Generated platform identity
0x6c    terminal_error_node           RP1          Live first failing node
0x70    terminal_error_detail         RP1          Live primary detail
0x74    terminal_error_aux            RP1          Live auxiliary detail
0x78    reserved alignment            —            Zero/reserved
0x80    result                        RP1          Committed 64-byte graph result
0xc0    reserved                      —            Rest of the 4 KiB block
```

The host must zero all v6 reserved words. Non-zero legacy words fail
configuration validation with `RP1_ERR_INVALID_CONFIG` and
`RP1_CONFIG_RESERVED_CQ`.

Firmware advertises the exact required behavior with
`RP1_REQUIRED_CAPABILITIES`:

- generated platform/IPI configuration;
- PMU-cycle timeouts;
- structured PDI responses;
- first-error-wins terminal diagnostics;
- BTCM-staged trace events; and
- the sequence-tagged graph result.

VRT and SMI reject missing capability bits, a version other than 6, or a zero
platform identity.

## Graph result ABI

`rp1_graph_result_t` is a fixed 64-byte record embedded at control offset
`0x80`. It is rewritten for every accepted sequence.

```text
Offset  Field                  Meaning
------  ---------------------  ----------------------------------------------
0x00    magic                  Commit marker "RSLT", written last
0x04    graph_seq              Exact accepted sequence
0x08    outcome                NONE, SUCCESS, FAILED, or HALTED
0x0c    flags                  RP1_RESULT_* bit mask
0x10    error_code             First terminal RP1_ERR_* code
0x14    terminal_node          Failing/HALT node, or UINT32_MAX
0x18    terminal_opcode        Opcode, or UINT32_MAX
0x1c    error_detail           Error-specific primary value
0x20    error_aux              Error-specific auxiliary value
0x24    active_image_id        Final known image id, otherwise zero
0x28    image_state            NONE, KNOWN, or UNKNOWN
0x2c    completed_operations   Successful executions, including repeats
0x30    graph_elapsed_ticks    Graph start through GRAPH_DONE
0x34    publish_elapsed_ticks  Through trace drain/result preparation
0x38    trace_write_idx        Final trace producer count
0x3c    quiescence             Packed finite-done/timeout/infinite counts
```

### Outcomes

- `NONE` exists only before a result is committed.
- `SUCCESS` means the scanner reached natural completion without a firmware
  error. It does not imply transaction rollback or that every packet ran;
  inspect flags and the VRT lifecycle sentinel.
- `FAILED` carries a non-zero first error and places firmware in reset-only
  `ERROR`.
- `HALTED` identifies an explicit `RP1_OP_HALT`, carries error code zero, and
  places firmware in reset-only `HALTED`.

`FAILED` and `HALTED` are determinate graph results. A host-side timeout before
exact-sequence completion is different: firmware may still own the graph, so
VRT poisons the submitter and requires device recovery.

### Result flags

- `RP1_RESULT_RECOVERY_REQUIRED`: finite work timed out during quiescence or
  infinite work could not be stopped. Card reset/recovery is mandatory.
- `RP1_RESULT_EFFECTS_MAY_BE_PARTIAL`: a non-success outcome occurred after at
  least one node began activation.
- `RP1_RESULT_INFINITE_WORK_REMAINS`: an infinite kernel remains or was found
  during terminal quiescence.
- `RP1_RESULT_TRACE_ENABLED`: this graph recorded trace events.
- `RP1_RESULT_TRACE_OVERFLOW`: the producer count exceeded ring capacity and
  older entries were overwritten.
- `RP1_RESULT_UNREACHED_NODES`: at least one packet remained pending,
  dispatched, or waiting at terminal classification.

Failure invalidates application outputs even when
`EFFECTS_MAY_BE_PARTIAL` is clear. The flag distinguishes pre-activation
validation rejection from a graph that may already have changed hardware or
memory; it is not a transaction guarantee.

### Quiescence counts

`quiescence` packs three 8-bit protocol counts into one word:

- bits 0-7: finite kernels that completed while terminal quiescence waited;
- bits 8-15: finite kernels that reached their deadline during quiescence;
- bits 16-23: infinite kernels that could not be quiesced.

The first error record never changes during quiescence. Secondary inability to
stop work is represented by counts and `RECOVERY_REQUIRED`.

### Timing

The Cortex-R5 PMU runs with the divide-by-64 bit enabled. One protocol tick is
exactly 64 R5 core cycles. Unsigned subtraction is valid across one 32-bit PMU
wrap.

`graph_elapsed_ticks` starts before shared-store validation and ends after
`GRAPH_DONE` is staged. It includes validation, scanner work, kernels, local
DMA, PDI waits, and any periodic trace flushes encountered during execution.
It excludes the final partial trace drain.

`publish_elapsed_ticks` is the final result payload word. It includes the final
trace drain and result preparation, but not the later commit-magic, state, and
`graph_done_seq` stores.

## Node packets

Every node is a naturally four-byte-aligned 32-byte `rp1_node_t`:

```text
Offset  Size  Field
------  ----  ----------------------------------------------------------
0x00    2     packed control
0x02    1     barrier_await_bucket
0x03    1     barrier_set_bucket
0x04    4     barrier_await_mask
0x08    4     barrier_set_mask
0x0c    20    opcode-specific payload union
```

The control word has four nibbles:

```text
Bits   Meaning
-----  ------------------------------------------------------------
0-3    Dense opcode
4-7    Flags
8-11   Status: PENDING=0, DISPATCHED=1, DONE=2, WAITING=3, ERROR=4
12-15  Reserved
```

The shared header provides C/C++-safe mask/shift helpers; the ABI uses no
compiler bitfields. Flag bit 0 is
`RP1_FLAG_INFINITE` for `KERNEL_DISPATCH`: the node becomes complete
immediately after launch and does not block natural graph completion. RP1 still
tracks the kernel while the graph runs so an observed `ap_done` can remove it.
Flag bits 1-3 are reserved. There is no per-node error policy flag; all errors
terminate the graph. Packet validation rejects non-zero reserved control or
flag bits and rejects `INFINITE` on every opcode except `KERNEL_DISPATCH`.

After validating `node_count` and the complete DDR source range, firmware
executes a barrier and copies the active prefix word-by-word into the
authoritative `rp1_node_t g_nodes[1024]` array in BTCM. It then resets each
packed BTCM status to `PENDING`. Packet validation, LOOP/COND/RERUN re-arming,
wait polling, quiescence, and error reporting use only that snapshot. Firmware
does not reread or write any DDR node word during execution.

Defined opcodes are:

```text
0   NOP
1   WAIT
2   SIGNAL
3   KERNEL_DISPATCH
4   SCALAR_WRITE
5   SCALAR_READ
6   SCALAR_COPY
7   DMA_COPY
8   DMA_FILL
9   PDI_LOAD
10  LOOP
11  COND
12  RERUN
13  HALT
```

Unknown opcodes fail whole-graph validation with `RP1_ERR_INVALID_NODE`; they
are never executed as no-ops.

Payload layouts are naturally aligned within the 20-byte union:

```text
Opcode             Payload fields in byte order
-----------------  -----------------------------------------------------------
KERNEL_DISPATCH    base u32, arg offset u32, arg count u16, ctrl flags u8,
                   reserved u8, timeout u32, expected image u32
SCALAR_WRITE       two {address u32, value u32} pairs, reserved[4]
SCALAR_READ        source u32, target slot u8, reserved[3]
SCALAR_COPY        destination u32, source slot u8, reserved[3]
SIGNAL             value u32, target slot u8, operation u8, reserved[2]
WAIT               condition value u32, signal u8, operation u8, reserved[2]
DMA_COPY           source lo/hi u32, destination lo/hi u32, length/types u32
DMA_FILL           destination lo/hi u32, length u32, pattern u32,
                   destination type u8, reserved[3]
PDI_LOAD           address lo/hi u32, timeout u32, image u32, reserved[4]
LOOP               body start/end u16, max u32, value u32, signal/op/clear
                   start/clear end/loop id u8, reserved[3]
COND               value u32, done mask u32, body start/end u16,
                   signal/op/clear start/clear end/done bucket u8, reserved[3]
RERUN              target u16, flags u8, loop id u8, reserved[16]
```

`DMA_COPY.length_types` uses bits 0-27 for byte length, bits 28-29 for source
type, and bits 30-31 for destination type. Shared helpers pack, read, and
replace each field without bitfields.

### Kernel dispatch

The 20-byte payload carries:

- R5-visible AXI-Lite base address;
- byte offset into the shared argument buffer;
- count of `rp1_kernel_arg_t` register-offset/value pairs;
- control flags;
- PMU-tick timeout, with zero selecting the generated default; and
- expected image id, with zero disabling the image guard.

On a CU's first launch after firmware startup or a PDI attempt, RP1 reads the
HLS control register to clear stale clear-on-read `ap_done`, followed by
`dmb sy`. Completion polling performs that clear for later launches, so RP1
caches the clean CU base and skips the redundant read and barrier. It then
writes each non-contiguous argument register, uses `dmb st` for store-to-store
ordering, and writes `ap_start`. Dispatches to the same CU serialize even when
graph barriers are independent. Finite kernels remain `DISPATCHED` until
`ap_done`. A timeout is fatal; the timed-out tracker remains available to
terminal quiescence.

Before touching a CU, a non-zero `expected_image_id` must match a `KNOWN`
active image. Mismatch fails fast, so stale code cannot launch against an
absent hardware design.

### Immediate operations

- `NOP` has no side effect beyond completion and barrier publication.
- `SIGNAL` applies SET, ADD, OR, or AND to one signal slot and records the
  writing node.
- `SCALAR_WRITE` performs up to two address/value writes, stopping at address
  zero.
- `SCALAR_READ` reads one AXI-Lite register into a signal slot.
- `SCALAR_COPY` writes one signal-slot value to an AXI-Lite register.
- `DMA_COPY` copies phase-1 DDR words.
- `DMA_FILL` fills phase-1 DDR words with one 32-bit pattern.

These operations finish in the activation pass. Side effects complete before
the node's barrier-set mask becomes visible to later packets.

### PDI load

The payload supplies a 64-bit staged-DDR PDI address, timeout, and image id.
RP1 writes the four-word IPI request, orders it before the trigger with
`dmb st`, completes the trigger store with `dsb st`, waits for the PMC
observation bit to clear, then orders the response reads with `dmb sy`. Every
attempt invalidates cached CU-clean state because the fabric may have changed
even when PLM reports rejection or timeout.

On success:

- the node completes;
- `active_image_id` becomes the requested id;
- image state is `KNOWN` for a non-zero id or `NONE` for zero; and
- downstream barriers may run.

On timeout or PMC rejection:

- activation stops immediately;
- image id becomes zero and image state becomes `UNKNOWN`;
- the result records `RP1_ERR_PDI_TIMEOUT` or `RP1_ERR_PDI_FAILED`; and
- the firmware enters reset-only `ERROR` after quiescence/publication.

A later error after a successful PDI does not erase that success. The final
result still reports the installed `KNOWN` image, allowing VRT to reconcile
host image state before it surfaces graph failure.

### WAIT and signals

The signal array has 256 fixed 16-byte slots:

```text
Offset  Field
------  ----------------------------------------------------
0x00    value
0x04    reserved
0x08    last_writer_node
0x0c    flags
```

Signals carry values; they do not schedule dependencies. A `WAIT` first becomes
eligible through barriers, then compares a signal using EQ, NE, LT, GE,
AND-nonzero, or AND-zero. If false, it moves to BTCM `WAITING` state and is
rechecked every pass. RP1 does not sleep because a host BAR write does not yet
wake the R5.

There is no firmware-wide graph deadline. A permanently false `WAIT` remains
in flight until the host's `submitAndWait()` timeout. That timeout is
indeterminate and poisons the VRT device; adding a firmware graph deadline is a
separate feature.

### LOOP, COND, and RERUN

`LOOP` increments its `loop_id` counter, then exits when its condition is true
or the current implementation's counter exceeds `max_iterations`. On continue,
it clears the configured barrier buckets and resets a contiguous body range to
`PENDING`; it deliberately withholds its output barrier. A body-end `RERUN`
resets the loop node so the next iteration can evaluate.

`COND` evaluates one signal. A true condition publishes `done_mask`; a false
condition clears a configured bucket/body range so that branch can run. The
COND node itself always completes and publishes its ordinary set mask.

`RERUN` resets one target node to `PENDING`. Its optional
`RP1_RERUN_CLEAR_STATE` flag also resets one loop counter.

`HALT` completes its own operation, records its node/opcode, stops activation,
quiesces tracked kernels, and publishes `HALTED`. It is an explicit non-success
result, not a firmware error code.

## Barrier scheduler

RP1 keeps 32 buckets of 32 bits in BTCM: 1024 dependency signals total. A node
is ready when:

```text
(barriers[await_bucket] & barrier_await_mask) == barrier_await_mask
```

Successful completion applies:

```text
barriers[set_bucket] |= barrier_set_mask
```

One node can await up to 32 bits from one bucket. Multiple producers may set
the same bit to express OR. A `NOP` can gather bits from one bucket and publish
a reduction bit in another bucket. VRT allocates bucket ranges per reset
domain and inserts bridge/reduction nodes as needed.

The scanner performs these phases every pass:

1. Scan `PENDING` nodes and activate all barrier-ready packets.
2. Poll in-flight kernels for completion or timeout.
3. Re-evaluate parked waits.
4. Repeat while work made progress or a kernel/wait can still progress.

Natural completion occurs when a pass makes no progress and no node is
`DISPATCHED` or `WAITING`. Permanently blocked `PENDING` nodes do not keep the
scanner alive. The result marks `UNREACHED_NODES`; VRT's trailing lifecycle
`SIGNAL` sentinel provides an independent success check that all intended graph
leaves joined.

`completed_operations` counts successful executions, not unique packets.
Repeated LOOP, COND, RERUN, and body executions each contribute.

## Validation and fail-fast errors

Validation has two layers before activation:

1. Shared configuration proves non-zero/bounded counts, zero reserved words,
   aligned in-window bases, and a valid optional trace ring.
2. Packet validation scans the whole graph for barrier buckets, signal slots,
   conditions, kernel argument ranges, loop/body ranges, targets, and defined
   opcodes.

A failure in either layer executes no graph packet. Runtime errors stop
activation at the first failing node. The first error wins even if quiescence
later discovers additional stuck work.

Implemented base error codes are:

```text
Code  Symbol                    Primary detail / auxiliary detail
----  ------------------------  --------------------------------------------
1     RP1_ERR_INFLIGHT_FULL     current count / maximum count
2     RP1_ERR_KERNEL_TIMEOUT    kernel base / timeout ticks
3     RP1_ERR_PDI_TIMEOUT       zero / timeout ticks
4     RP1_ERR_IMAGE_MISMATCH    expected image / active image
5     RP1_ERR_PDI_FAILED        PMC status / PMC detail
6     RP1_ERR_INVALID_CONFIG    RP1_CONFIG_* selector / offending value
7     RP1_ERR_INVALID_NODE      RP1_NODE_BAD_* selector / offending value
```

### Terminal quiescence

After an error or HALT, RP1 never activates another packet. It inspects only
already tracked kernels:

- a finite kernel that asserts `ap_done` is counted as finite-done;
- a finite kernel that reaches its original deadline is counted as
  finite-timeout and requires recovery; and
- an infinite kernel is counted as infinite and requires recovery.

Quiescence does not release dependency barriers because graph execution has
already ended. It exists to classify whether buffers and the fabric can be
trusted for reuse, not to continue useful graph work.

`ERROR` and `HALTED` are reset-only states. The outer loop continues updating
heartbeat but rejects later `graph_seq` values. Current host recovery is a card
reset/hotplug sequence; there is no protocol soft-reset path in VRT.

## Submission and publication ordering

Both host and firmware currently poll shared DDR. There is no wired interrupt
handoff.

### Firmware boot publication

Firmware publishes its fixed contract in this order:

1. Clear control magic and enter `INIT`.
2. Reset both `graph_seq` and `graph_done_seq` to the same zero idle baseline,
   then write version, required capabilities, platform id, zero result, and
   `READY`.
3. Execute a full barrier.
4. Write `RP1_CTRL_MAGIC`.
5. Execute another full barrier.

Visible `SQR1` magic therefore commits the boot contract. Firmware may write
the host-owned doorbell only while magic is invalid; clearing it before
publication prevents a graph left in DDR by an earlier firmware instance from
replaying after reload. Hosts must wait for visible magic before incrementing
the new baseline.

### Host submission

`Rp1Submitter`:

1. Requires compatible magic/version/capabilities/platform and `READY`.
2. Writes argument records.
3. Clears graph-owned signal slots.
4. Writes all node packets.
5. Writes node count and trace configuration.
6. Executes a host fence.
7. Writes `graph_seq = previous + 1`.
8. Executes another host fence.

Only the sequence store transfers ownership. Submission equality, not numeric
ordering, handles `uint32_t` wrap.

### Firmware result commit

After observing a new sequence, firmware snapshots the sequence and node count,
then:

1. Clears `result.magic`, barriers, resets per-graph BTCM/error state, and
   enters `RUNNING`.
2. Validates every shared range, executes a barrier, and copies the active DDR
   node words once into BTCM.
3. Validates the BTCM packets and executes or classifies the graph.
4. Emits `GRAPH_DONE` and captures `graph_elapsed_ticks`.
5. Flushes the final partial trace page.
6. Fills every result payload word, ending with
   `publish_elapsed_ticks`.
7. Barriers, writes `RP1_GRAPH_RESULT_MAGIC`, and barriers.
8. Writes terminal `READY`, `ERROR`, or `HALTED`, and barriers.
9. Writes the exact accepted sequence to `graph_done_seq`, and barriers.

The host must not use visible result magic or terminal state as an early
completion signal. It polls until `graph_done_seq == wanted`, fences, then
copies and validates result magic, result sequence, outcome/state agreement,
image-state consistency, and all discriminants.

## Optional trace ring

`rp1_trace_entry_t` is 16 bytes:

```text
Offset  Field       Meaning
------  ----------  --------------------------------------------
0x00    timestamp   PMU ticks since graph start
0x04    event       rp1_trace_event_t
0x06    node_index  Packet index, or 0xffff for graph events
0x08    aux0        Event-specific detail
0x0c    aux1        Event-specific detail
```

Events cover graph start/done, activation, kernel launch/done/timeout, loops,
conditions, wait park/wake, PDI response, image mismatch, and trace flush
boundaries.

Normal events are staged in one 4 KiB BTCM page (256 entries). When a normal
event leaves one slot:

1. `TRACE_FLUSH_START` occupies the final slot.
2. Firmware synchronously copies the full page into the DDR ring.
3. The producer cursor is published only after all entries are visible.
4. `TRACE_FLUSH_END` becomes the first entry in the new BTCM page.

The marker interval measures blocking DDR flush cost. The final partial page is
copied after `GRAPH_DONE` without another marker pair, avoiding recursive
flushes. `trace_write_idx` starts at zero for each graph and is monotonic within
that graph. Ring addressing wraps and overwrites old entries; the result flag
reports overflow.

The latency benchmark preserves both physical handoff time and a
flush-adjusted handoff:

- `KERNEL_DONE(i)` to `KERNEL_LAUNCH(i+1)`;
- the same interval minus any fully bracketed flush; and
- each `TRACE_FLUSH_START` to `TRACE_FLUSH_END` duration.

## Host integration

### Rp1Submitter

`vrt::graph::fpga::Rp1Submitter` is the mechanical v6 adapter. It stages an
`Rp1GraphImage`, writes the sequence doorbell, waits by exact equality, and
returns a host-owned `Rp1GraphResult`.

The typed result exposes:

- `Rp1GraphOutcome`;
- optional `Rp1TerminalError`;
- `Rp1ImageState` and active image id;
- flags and completed operation count;
- graph/publication ticks and trace cursor; and
- decoded `Rp1Quiescence`.

`FAILED` and `HALTED` return normally from `submitAndWait()` so callers can
reconcile state. Invalid host images, corrupt publication, transport errors,
and host timeouts throw. A post-doorbell timeout permanently poisons the
submitter because staged storage may still be firmware-owned.

### FpgaDevicePlan

The FPGA graph backend adds a trailing `SIGNAL` sentinel that depends on all
intended leaves. After a result:

1. Reconcile active image state, even for failure.
2. Optionally print the result when `VRT_RP1_RESULT` is set.
3. Drain/print trace when `VRT_RP1_TRACE` is set.
4. Surface non-success with the complete terminal record.
5. On success, require the lifecycle sentinel value.

The one-line diagnostic is suitable for acceptance automation:

```text
[rp1-result] seq=N outcome=SUCCESS(1) flags=0x... image=KNOWN(1):I completed=C graph_ticks=G publish_ticks=P trace_write_idx=T quiescence=D/T/I
```

### Image-aware lowering

`FpgaVbinSpec` assigns stable 1-based numeric ids in image-name order.
`PDI_LOAD` carries the installed id; every guarded kernel packet carries its
expected id. `FpgaDevice` reconciles the result back to the named image.

High-level `Graph::addFpga()` handles vbin parsing, QDMA PDI staging, the vrtd
session, BAR mapping, readiness checks, and device construction. Lower-level
mock/test construction may use a kernel-address lookup and image id zero.

### SMI and acceptance

`v80-smi debug rp1-dump` prints the v6 control contract and all graph-result
fields without mutating the device. `rp1-ping` validates one untraced SIGNAL
result; `rp1-trace-ping` validates the same result with tracing and prints the
trace ring.

The graph hardware acceptance script enables `VRT_RP1_RESULT` and
`VRT_RP1_TRACE`, validates each result's outcome/flags/image/timing/quiescence,
retains PDI trace-count checks, and compares the last diagnostic sequence with
the following read-only SMI dump.

## Limits and deferred work

Current protocol limits:

```text
Resource                         Limit
-------------------------------  -------------------------------------
Packets per graph                1024
Barrier bits                     1024 (32 buckets x 32)
Direct fan-in per packet         32 bits from one bucket
In-flight kernels                32
Signal slots                     256
Loop counters                    64
Trace ring                       4096 entries
Argument staging                 Default 1 MiB shared range
Concurrent submitters            1 exclusive owner
```

Not yet implemented:

- firmware graph deadline for permanently parked waits;
- interrupt-driven host/RP1 completion;
- safe multi-client graph serialization;
- protocol soft reset/recovery;
- hardware DMA or direct HBM/host access;
- linker-emitted R5 address tables; and
- automatic proof that a PDI reconfiguration boundary drained the region.

If multiple outstanding graphs are added later, completion needs a
graph-granularity transport. Protocol v6 deliberately avoids reintroducing
per-node publication into the dispatch hot path.

## Verification

The migration is covered at several layers:

- static ABI size/offset assertions in the shared header;
- RP1 unit and QEMU graph tests for the exact 1024-node maximum, immutable BTCM
  snapshots, success, fatal errors, HALT, sequence wrap, PDI image state,
  partial effects, quiescence, trace finalization, and result publication;
- VRT mock-BAR tests for result validation, state/outcome agreement, image
  reconciliation, sentinel checks, timeout poisoning, and diagnostics;
- SMI compilation plus the read-only/result-aware probes;
- the local fake graph-hardware acceptance harness; and
- RP1 latency host tests for result validation and BTCM flush accounting.

Typical local checks from the repository root are:

```bash
./scripts/build-and-test-rp1-qemu.sh

cmake -S vrt -B vrt/build -G Ninja \
  -DVRT_INCLUDE_VRTD=1 -DVRTD_INCLUDE_LIBSLASH=1 -DVRT_BUILD_TESTS=1
cmake --build vrt/build --target unit_tests
ctest --test-dir vrt/build/tests --output-on-failure

cmake -S examples/extra/rp1_latency -B examples/extra/rp1_latency/build \
  -G Ninja -DVRT_USE_REPO=ON -DBUILD_VBIN=OFF
cmake --build examples/extra/rp1_latency/build
ctest --test-dir examples/extra/rp1_latency/build --output-on-failure

./scripts/test-graph-hardware-local.sh
```

Matched firmware, VRT, SMI, graph examples, and vbins are required for V80
hardware acceptance. Protocol-v4 firmware is intentionally incompatible.
