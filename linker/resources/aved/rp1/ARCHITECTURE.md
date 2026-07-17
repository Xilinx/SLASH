# RP1 HSA Command Processor Architecture

## Context

The SLASH platform currently uses a host-driven dispatch model: the VRT runtime writes kernel arguments and control registers over PCIe BAR MMIO, one kernel at a time, synchronously polling for completion. Every register write is a PCIe round-trip (~1-2us). For a kernel launch with 8 arguments, that's ~18 PCIe transactions (8 arg writes + control write + polling reads).

RP1 (ARM Cortex-R5 core 1) sits **on-die** with single-digit-nanosecond access to all AXI peripherals. By making RP1 a command processor, we eliminate PCIe latency from the critical dispatch path. The host submits computation graphs to shared DDR, and RP1 executes them autonomously -- kernel dispatch, DMA orchestration, loops, and conditionals -- with no host intervention until the entire graph completes.

### Execution Model Guarantees

**Single user region.** All kernels referenced by a graph must be resident in the FPGA fabric simultaneously. The graph operates on a static hardware layout -- there is no dynamic partial reconfiguration between nodes. The host programs the bitstream once, then submits graphs against that fixed set of kernels.

**Explicit parallelism.** If a node's barrier dependencies are satisfied, RP1 **will** dispatch it. This is a hard guarantee, not best-effort. It matters because kernels may communicate via AXI-Stream interfaces while co-executing. A stream producer kernel started before its consumer can block on backpressure, and the guarantee ensures the consumer will be started -- preventing deadlock. Kernels connected by streams must be designed for backpressure tolerance, but they can rely on RP1 dispatching both ends once barriers are met.

**Explicit reprogram points (`PDI_LOAD`, opcode `0x0030`).** A graph can include a `PDI_LOAD` node that asks the PMC to partial-reconfigure the fabric from a host-staged DDR PDI; see Section A. The host MUST gate the node behind barriers that drain every kernel resident in the to-be-reconfigured region -- RP1 does not validate the drain. RP1 does, however, track the image last installed by `PDI_LOAD` (its `image_id`) and rejects a `KERNEL_DISPATCH` whose non-zero `expected_image_id` does not match, so a stale dispatch fails fast instead of hanging on an absent kernel. Updating the R5 kernel-base table to reach kernels that only exist in the new design is the graph creator's responsibility (it can be done by `SCALAR_WRITE` nodes or by re-submitting a fresh graph against the new layout).

**Future: graph regions.** For very large graphs (thousands of nodes), the host may partition the graph into regions with guaranteed non-overlapping execution. This also provides a natural boundary for partial reconfiguration. The flat scanner scales to current graph sizes; regions are the path for scaling further.

---

## Hardware Prerequisites

### 1. RPU -> User Region AXI-Lite Path -- wired in the base block design

**Problem:** R5's `M_AXI_LPD` needs a path to the kernel AXI-Lite slaves in
the user region, which live at `0x0202_0000_0000+` on the PCIe-visible NoC
address map.

**Solution (landed):** `rpu_sc` (the RPU-side smartconnect) has a third
master port (`NUM_MI {3}`, `M02_AXI`) wired through the NoC into the user
kernel region, and the NoC ingress `S_AXILITE_INI` carries a
`REMAPS {M04_INI {{0x8800_0000 0x202_0000_0000 0x8000000}}}` entry that
mirrors the PCIe-visible `0x0202_0000_0000` user-region window into the R5's
32-bit address space at `0x8800_0000` (128MB). This is what the
`r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000` formula (used in
Section H) converts between. See
`linker/slashkit/resources/base/scripts/top.tcl` (`rpu_sc`, `M02_AXI`,
`REMAPS`).

**Remaining gap:** the linker does not yet emit an R5-address table
alongside `system_map.xml`. `linker/slashkit/emit/hw/tcl_gen.py` assigns
kernel instances into the NoC address space (`S_AXILITE_INI` at
`0x0202_0000_0000`) for the PCIe/host view only; there is no separate
R5-visible address emission step. Until that lands, `FpgaKernelLocation`
addresses are supplied by the caller (see Section H) -- either hand-derived
with the formula above, or (for `FpgaVbinSpec`-backed devices) computed
from the vbin's `system_map` at runtime.

### 2. RPU -> HBM/DDR Data Path (DMA)

**Problem:** R5's `LPD_AXI_NOC_0` only reaches DDR4 (0x0-0x8000_0000). HBM is not addressable. There is no CDMA or hardware DMA engine accessible to the R5.

**Solution (phased):**
- **Phase 1 (SW DMA):** R5 does software memcpy via AXI for DDR-DDR transfers and uses kernel-mediated transfers for HBM. For host-device DMA, the host still performs QDMA transfers before/after submitting the graph. RP1 commands reference pre-staged buffers.
- **Phase 2 (HW DMA):** Add an AXI CDMA IP connected to R5 via M_AXI_LPD, with master ports reaching both DDR and HBM via the NoC. R5 programs CDMA descriptors, CDMA does the heavy lifting.
- **Phase 3 (Full):** Add HBM slave ports to `LPD_AXI_NOC_0` so R5 can issue HBM reads/writes directly (for small transfers and scatter-gather).

**For this architecture, we design for Phase 1 with hooks for Phase 2.**

---

## A. Command Packet Format

### Fixed 64-byte nodes

Every graph node is a 64-byte (16-word) packet, naturally aligned. 16-byte header + 48-byte payload.

```
Offset  Size  Field
------  ----  -----
0x00    2B    opcode              (uint16_t)
0x02    2B    flags               (uint16_t) -- universal flags, shared across opcodes
0x04    4B    barrier_await_mask  (uint32_t) -- which barriers in await_bucket must be set
0x08    4B    barrier_set_mask    (uint32_t) -- which barriers in set_bucket to raise on completion
0x0C    1B    barrier_await_bucket (uint8_t) -- which of 32 buckets to check (0-31)
0x0D    1B    barrier_set_bucket   (uint8_t) -- which of 32 buckets to write (0-31)
0x0E    2B    status              (uint16_t) -- written by RP1: 0=PENDING, 1=DISPATCHED, 2=DONE, 0xFF=ERROR
0x10    48B   payload             (varies)
```

### Barrier System

The barrier system is **flat and global**: one `completed_barriers[32]` array (32 buckets x 32 bits = 1024 barriers), stored in BTCM (128 bytes).

**Scheduling check (per node):**
```
if (completed_barriers[node.await_bucket] & node.await_mask) == node.await_mask:
    // all dependencies met -- execute this node
```

**On node completion:**
```
completed_barriers[node.set_bucket] |= node.set_mask
```

This is one indexed load + one AND + one CMP from BTCM. Single-cycle access.

**AND logic:** A node with multiple bits set in `await_mask` waits for ALL of them. Each bit is set by a different predecessor.

**OR logic:** Multiple nodes can `set` the **same bit** in the same bucket. The first to complete sets it. A downstream node awaiting that bit unblocks as soon as any one predecessor fires.

**Cross-bucket bridging:** A NOP node with `await_bucket=A, set_bucket=B` gathers signals from bucket A and publishes into bucket B. NOP nodes execute immediately (just set their barriers), making them free bridge/reduction nodes.

**AND-of-ORs:** Combine both patterns. A node awaits multiple bits, each of which can be set by any of several producers via NOP bridges.

### Flags (Universal)

Flags are shared across all opcodes. Opcode-specific configuration goes in the payload.

```
Bit 0: HALT_ON_ERROR  -- abort graph processing if this node fails
Bit 1: SILENT         -- suppress CQ entry for this node
Bit 2: INFINITE       -- (KERNEL_DISPATCH only) node immediately DONE, kernel monitored for errors
Bit 3-15: reserved
```

**INFINITE flag:** When set on a KERNEL_DISPATCH, the node transitions to DONE immediately after launching the kernel, and its barriers are set. The kernel is still added to the inflight list for error monitoring, but it does not block graph completion. If the kernel's `ap_done` fires unexpectedly, it is silently dropped from the inflight list. This is designed for stream producers and other long-running kernels that should not prevent the graph from halting. Consumers can backpressure them and they will stall naturally when the graph completes.

### Opcodes

```
0x0000  NOP              -- Immediately DONE. Use as barrier bridge/reduction node.
0x0001  WAIT             -- Park until a signal slot satisfies a condition.
0x0002  SIGNAL           -- Write a value to a signal array slot.
0x0010  KERNEL_DISPATCH  -- Set args + start a kernel on the FPGA.
0x0011  SCALAR_WRITE     -- Write immediate values to kernel AXI-Lite registers.
0x0012  SCALAR_READ      -- Read kernel register -> signal array slot.
0x0013  SCALAR_COPY      -- Copy a signal slot's value into a kernel AXI-Lite register.
0x0020  DMA_COPY         -- Memory transfer (DDR-DDR phase 1, DDR-HBM phase 2).
0x0021  DMA_FILL         -- Fill a memory region with a pattern.
0x0030  PDI_LOAD         -- Trigger a partial PDI reload from DDR via the PMC.
0x0040  LOOP    -- Clear body state + buckets for next loop iteration.
0x0041  COND    -- Evaluate condition, set then_bucket or else_bucket barriers.
0x0042  RERUN            -- Clear DONE state of a target node back to PENDING.
0x00FF  HALT             -- Stop graph processing (supplemental, for early exits).
```

Unrecognized opcodes are not rejected: the scanner's `default` case treats
them as a no-op immediate completion (marked `DONE`, own `barrier_set_mask`
applied, `RP1_CQ_OK` written) rather than raising `rp1_state = ERROR`. See
Section G.

### Packet Payloads

#### KERNEL_DISPATCH (0x0010)

```
0x10    4B    kernel_base_addr    -- AXI-Lite base address (R5 address space)
0x14    4B    arg_buffer_offset   -- Offset into DDR arg buffer for staged arguments
0x18    2B    arg_count           -- Number of (reg_offset, value) argument pairs
0x1A    2B    ctrl_flags          -- Bit 0: auto-restart
0x1C    4B    timeout_cycles      -- Watchdog timeout (0 = default 10M cycles)
0x20    4B    expected_image_id   -- Image this kernel needs; 0 = no guard
0x24    28B   reserved
```

**Expected-image guard.** When `expected_image_id` is non-zero, RP1 compares it
against `g_active_image_id` -- the image id recorded by the most recent
successful `PDI_LOAD` -- before launching. On mismatch the node fails fast:
status `ERROR`, `rp1_error_code = RP1_ERR_IMAGE_MISMATCH (4)`, a `RP1_CQ_ERROR`
CQ entry whose `error_detail` carries the active image id, and (when
`HALT_ON_ERROR` is set) the scanner aborts. This is belt-and-braces behind the
host compiler's static image-safety proof, so a stale dispatch fails instead of
poking an absent kernel and hanging. `expected_image_id = 0` disables the check
(no-image kernels and the mock/lookup host path).

The host pre-stages kernel arguments in the argument buffer as an array of
`rp1_kernel_arg_t` `(reg_offset, value)` pairs (protocol v2). RP1 reads
`arg_count` pairs from `arg_buffer_offset` and writes each `value` to
`kernel_base_addr + reg_offset`. This honours the non-contiguous register
layout real HLS `s_axilite` maps produce (e.g. `n@0x10`, `in@0x1c`, `out@0x28`
with reserved gaps); a 64-bit argument is two consecutive pairs. Before
writing arguments, RP1 first reads `kernel_base_addr + 0x00` once to clear
any stale, sticky `ap_done` left over from a *previous* dispatch of the same
kernel (HLS `ap_ctrl_hs` status bits are clear-on-read); it then writes the
arguments and writes 0x01 to `kernel_base_addr + 0x00` (ap_start).

All kernel dispatches are non-blocking from the scanner's perspective. The scanner launches the kernel, marks the node DISPATCHED (or DONE if INFINITE), and continues scanning. When `ap_done` fires (detected by `check_inflight_kernels()`), the node transitions to DONE and its barriers are set.

#### SCALAR_WRITE (0x0011)

```
0x10    48B   writes[6]           -- Array of (addr, value) pairs
              Each pair: 4B addr + 4B value = 8 bytes
              Stop at first addr == 0
```

Batches up to 6 register writes. Completes immediately (DONE).

#### SCALAR_READ (0x0012)

```
0x10    4B    source_addr         -- AXI-Lite address to read
0x14    4B    target_slot         -- Signal array slot to store value (0-255)
0x18    40B   reserved
```

Reads a kernel register and stores the value in the signal array. This bridges hardware register space to the signal array, enabling LOOP exit conditions and COND decisions based on kernel-computed values.

#### SCALAR_COPY (0x0013)

```
0x10    4B    source_slot         -- Signal array slot index to read
0x14    4B    dest_addr           -- AXI-Lite address to write
0x18    40B   reserved
```

The inverse of SCALAR_READ: writes `signal_array[source_slot].value` to
`dest_addr`. Completes immediately (DONE). Used to feed a loop-carried
scalar held in a host-visible signal slot into a body kernel's `s_axilite`
input register each iteration, so the carried value can flow through a
kernel argument rather than a DDR buffer.

#### SIGNAL (0x0002)

```
0x10    4B    target_slot         -- Signal array slot index (0-255)
0x14    4B    value               -- Value to write
0x18    2B    operation           -- 0=SET, 1=ADD, 2=OR, 3=AND
0x1A    2B    reserved
0x1C    36B   reserved
```

Writes to `signal_array[target_slot].value`. Completes immediately (DONE).

#### WAIT (0x0001)

```
0x10    4B    condition_signal    -- Signal array slot to poll
0x14    4B    condition_value     -- Comparison value
0x18    2B    condition_op        -- 0=EQ, 1=NE, 2=LT, 3=GE, 4=AND_NZ, 5=AND_Z
0x1A    2B    reserved
0x1C    36B   reserved
```

The cross-queue rendezvous primitive. Unlike a barrier (BTCM, private to one
graph execution), the signal array is host-visible DDR, so a WAIT can gate on
a producer outside this graph -- a peer device's RP1 graph, or the host
writing over the BAR. When a WAIT node's barriers are met but its condition
does not yet hold, the node status becomes `RP1_NODE_WAITING` (not
`PENDING`/`DISPATCHED`); `check_waits()` re-evaluates every `WAITING` node
each scan pass and completes it (sets `DONE`, raises `barrier_set_mask`) as
soon as `compare(signal_array[condition_signal].value, condition_op,
condition_value)` holds. While any node is `WAITING` the scanner does not
`wfi()` -- a host BAR write does not raise an R5 wake event, so the core must
busy-poll to observe it promptly. This supersedes the older `LOOP`+`RERUN`
polling idiom for semaphores; see the `AWAIT_SEMAPHORE` pattern in Section F,
which now lowers to a single WAIT node instead of two.

#### DMA_COPY (0x0020)

```
0x10    4B    src_addr_lo         -- Source physical address, low 32 bits
0x14    4B    src_addr_hi         -- Source physical address, high 32 bits
0x18    4B    dst_addr_lo         -- Destination physical address, low 32 bits
0x1C    4B    dst_addr_hi         -- Destination physical address, high 32 bits
0x20    4B    length              -- Transfer size in bytes
0x24    2B    src_type            -- 0=DDR, 1=HBM, 2=HOST
0x26    2B    dst_type            -- 0=DDR, 1=HBM, 2=HOST
0x28    24B   reserved
```

Phase 1 implementation: DDR-DDR only, R5 software word-copy using only the
`_lo` halves (32-bit addressing). HOST/HBM and 64-bit addressing via the
`_hi` halves are deferred to Phase 2.

#### DMA_FILL (0x0021)

```
0x10    4B    dst_addr_lo         -- Destination physical address, low 32 bits
0x14    4B    dst_addr_hi         -- Destination physical address, high 32 bits
0x18    4B    length              -- Fill size in bytes
0x1C    4B    pattern             -- 32-bit fill pattern
0x20    2B    dst_type            -- 0=DDR, 1=HBM
0x22    2B    reserved
0x24    28B   reserved
```

Phase 1 implementation uses only `dst_addr_lo` (32-bit addressing).

#### PDI_LOAD (0x0030)

```
0x10    4B    pdi_addr_lo         -- DDR physical address of partial PDI (low 32)
0x14    4B    pdi_addr_hi         -- DDR physical address of partial PDI (high 32)
0x18    4B    timeout_cycles      -- Poll budget for IPI ACK (0 = default 10M)
0x1C    4B    image_id            -- Image this PDI installs; recorded as active
0x20    32B   reserved
```

On success RP1 records `image_id` in `g_active_image_id`, which the
`KERNEL_DISPATCH` expected-image guard checks. This state reflects physical
reconfiguration and therefore **persists across graph submissions** (it is not
cleared by the per-graph BTCM reset); only another `PDI_LOAD` changes it, and it
starts at 0 (no image) at firmware boot. `image_id = 0` records "no image".

Triggers a partial PDI reconfiguration by asking the PMC (PLM) to load
the PDI staged at `(pdi_addr_hi << 32) | pdi_addr_lo` in DDR.  RP1 writes
the canonical XLoader command block into the PMC scratch area at
`0xFF3F0A40-0xFF3F0A4C`, pokes IPI channel 3 (`0xFF360000 <- 0x02`), and
then blocks reading the IPI observation register (`0xFF360004`) until it
clears or the timeout budget is exhausted.

The IPI ACK does **not** guarantee that the fabric has finished
reconfiguring -- the PMC continues loading the PDI asynchronously after
the ACK.  The graph creator is responsible for sequencing any node that
depends on the new fabric content (e.g., a downstream `KERNEL_DISPATCH`
or `SCALAR_WRITE` against a kernel that only exists in the new design).
A simple way to insert that delay is an `AWAIT_SEMAPHORE` pattern with
the host writing the "PR complete" signal once it has verified the new
design via its own out-of-band path.

The host MUST also drain any in-flight kernels that live in the
to-be-reconfigured region by gating the `PDI_LOAD` node behind their
barriers.  The "single user region" guarantee makes this trivial in the
common case: every kernel in the graph is in the active region, so the
`PDI_LOAD` simply awaits the graph's join barrier.

On timeout RP1 marks the node `ERROR`, sets
`rp1_error_code = RP1_ERR_PDI_TIMEOUT (3)`, and emits a `RP1_CQ_TIMEOUT` CQ
entry.  If `HALT_ON_ERROR` is set, the graph aborts; otherwise the node's
`barrier_set_mask` is still raised so downstream nodes can run.

#### LOOP (0x0040)

```
0x10    4B    body_start          -- First node index of loop body
0x14    4B    body_end            -- Last node index (inclusive)
0x18    4B    max_iterations      -- Hard cap (0 = condition-only)
0x1C    4B    condition_signal    -- Signal array slot to check for exit
0x20    4B    condition_value     -- Exit when signal matches this value
0x24    2B    condition_op        -- 0=EQ, 1=NE, 2=LT, 3=GE, 4=AND_NZ, 5=AND_Z
0x26    1B    bucket_clear_start  -- First bucket to clear each iteration
0x27    1B    bucket_clear_end    -- Last bucket to clear (inclusive)
0x28    1B    loop_id             -- Index into in-flight loops array (assigned by graph creator)
0x29    23B   reserved
```

When LOOP fires (barriers met), it:
1. Increments `loop_iterations[loop_id]`
2. Checks exit condition: `signal_array[condition_signal].value` against `condition_value`, or `loop_iterations[loop_id] > max_iterations`
3. If exiting: marks itself DONE, sets barriers. Loop is over.
4. If continuing: clears `completed_barriers[bucket_clear_start..bucket_clear_end]`, resets node statuses in `[body_start..body_end]` to PENDING, marks itself DONE, sets barriers. Body nodes become runnable and are picked up by the flat scanner.

The LOOP node is non-blocking. It does not call a nested scheduler. It just clears state and lets the flat scanner do its job. The loop body runs naturally as part of the same scan pass.

A RERUN node at the end of the loop body re-triggers the LOOP by clearing its DONE state back to PENDING. On the next scan, the loop node fires again, checks the condition, and either continues or exits.

#### COND (0x0041)

```
0x10    4B    condition_signal    -- Signal array slot to evaluate
0x14    4B    condition_value     -- Comparison value
0x18    2B    condition_op        -- 0=EQ, 1=NE, 2=LT, 3=GE, 4=AND_NZ, 5=AND_Z
0x1A    1B    bucket_clear_start  -- First bucket to clear on continue
0x1B    1B    bucket_clear_end    -- Last bucket to clear (inclusive)
0x1C    4B    body_start          -- First node to reset on continue
0x20    4B    body_end            -- Last node to reset (inclusive)
0x24    1B    done_bucket         -- Bucket to set on done
0x25    3B    reserved
0x28    4B    done_mask           -- Barrier mask to set on done
0x2C    20B   reserved
```

COND evaluates `signal_array[condition_signal].value` against `condition_value`. The node always becomes DONE immediately and its own `barrier_set_mask` is always applied.

- **Continue (condition not met):** clears `completed_barriers[bucket_clear_start..bucket_clear_end]`, resets node statuses in `[body_start..body_end]` to PENDING. Body nodes become runnable and are picked up by the flat scanner.
- **Done (condition met):** sets `completed_barriers[done_bucket] |= done_mask`. No body clearing.

This is the general control flow primitive. Combined with RERUN:
- **While-loop:** COND checks exit condition. Continue = re-run body. Done = set done barriers, unblock downstream. RERUN at end of body resets COND to PENDING.
- **If/else:** Two CONDs with complementary conditions, each with its own body range. Or a single COND (continue = then-branch body) with done_bucket enabling else-branch nodes.

#### RERUN (0x0042)

```
0x10    4B    target_node         -- Node index to reset from DONE to PENDING
0x14    2B    rerun_flags         -- Bit 0: CLEAR_STATE (reset loop iteration counter)
0x16    1B    loop_id             -- Loop ID to clear (if CLEAR_STATE set)
0x17    41B   reserved
```

RERUN does one thing: clears the DONE state of `target_node` back to PENDING. On the next scan pass, the target node's barriers will be re-evaluated and it will fire again if dependencies are met.

**Loop end pattern:** A RERUN at the end of a loop body targets the LOOP node. When the RERUN fires (all body nodes done), it resets the loop node to PENDING. The loop node fires again, increments iteration count, checks exit condition, and either clears the body for another iteration or exits.

- `CLEAR_STATE` flag: resets `loop_iterations[loop_id]` to zero. Used when entering a loop fresh (e.g., the first time, or when re-entering from an outer loop).
- Without `CLEAR_STATE`: the iteration counter is preserved. This is the normal loop-end case.

#### HALT (0x00FF)

No payload. Immediately stops graph processing. Supplemental -- normal graph completion is detected automatically when no further progress is possible (see Section D). HALT is for early exits or creative graph patterns.

---

## B. Graph Submission Protocol

### Shared UAPI

The on-wire layout described below (control block, node packets, signal
slots, CQ entries, opcodes, payload structs) is defined once in the
shared header
[`driver/libslash/include/slash/uapi/rp1_protocol.h`](../../../../driver/libslash/include/slash/uapi/rp1_protocol.h).
Both the RP1 firmware (Cortex-R5 baremetal) and host code (libslash, SMI,
VRT FpgaDevice) include this header, and `_Static_assert` checks at the
bottom of the file enforce all sizes and critical offsets at compile
time on both sides. Any change to the protocol must land in that header.

### Memory Layout

The host-visible BAR window is a 64MB aperture at `0x3000_0000`. All
host/RP1 shared control, queue, argument, and signal structures must live in
that window. DDR below `0x3000_0000` is available for RP1-private storage.

```
DDR Address        Size      Purpose
---------------    ----      -------
0x3000_0000        4KB       Control Block           (RP1_CTRL_BAR_OFFSET)
0x3000_1000        256KB     Node Array -- up to 4096 x 64-byte nodes    (RP1_DEFAULT_NODE_ARRAY_OFFSET)
0x3004_1000        64KB      Completion Queue (CQ) -- 4096 x 16-byte entries  (RP1_DEFAULT_CQ_OFFSET)
0x3005_1000        1MB       Argument Buffer -- pre-staged kernel arguments   (RP1_DEFAULT_ARG_BUF_OFFSET)
0x3015_1000        4KB       Signal Array -- 256 x 16-byte value slots        (RP1_DEFAULT_SIG_ARRAY_OFFSET)
0x3015_2000        ...       Optional trace ring -- 16-byte event entries     (RP1_DEFAULT_TRACE_OFFSET)
```

The offsets above are the `RP1_DEFAULT_*_OFFSET` constants in
`rp1_protocol.h` and what `Rp1Submitter::ensureReady()` programs into the
control block on first use. They are a convention, not hard protocol --
firmware reads whatever base addresses the host writes into the control
block -- so a host is free to lay the sub-regions out differently as long as
it programs the corresponding `*_base_lo/_hi` fields consistently.

### Control Block (0x3000_0000, 4KB)

```
Offset  Size  Field              Writer  Reader
------  ----  -----              ------  ------
0x00    4B    magic              RP1     Host    -- 0x53515231 ("SQR1")
0x04    4B    version            RP1     Host    -- Protocol version (RP1_PROTOCOL_VERSION = 3)
0x08    4B    node_count         Host    RP1     -- Number of nodes in this graph
0x0C    4B    cq_size            Host    RP1     -- Number of CQ entries (power of 2)
0x10    4B    node_base_lo       Host    RP1     -- Node array base address (low 32)
0x14    4B    node_base_hi       Host    RP1     -- Node array base address (high 32)
0x18    4B    cq_base_lo         Host    RP1     -- CQ base address (low 32)
0x1C    4B    cq_base_hi         Host    RP1     -- CQ base address (high 32)
0x20    4B    graph_seq          Host    RP1     -- Graph sequence number (host increments)
0x24    4B    graph_done_seq     RP1     Host    -- Last completed graph sequence
0x28    4B    cq_write_idx       RP1     Host    -- Next CQ write position
0x2C    4B    cq_read_idx        Host    RP1     -- Last consumed CQ position
0x30    4B    rp1_state          RP1     Host    -- 0=INIT, 1=READY, 2=RUNNING, 3=ERROR, 4=HALTED
0x34    4B    rp1_error_code     RP1     Host    -- Last error code
0x38    4B    rp1_current_node   RP1     Host    -- Current node being processed (debug)
0x3C    4B    heartbeat          RP1     Host    -- Incrementing counter (liveness)
0x40    4B    arg_buf_base_lo    Host    RP1     -- Argument buffer base address
0x44    4B    arg_buf_base_hi    Host    RP1
0x48    4B    sig_array_base_lo  Host    RP1     -- Signal array base address
0x4C    4B    sig_array_base_hi  Host    RP1
0x50    4B    trace_enable       Host    RP1     -- Non-zero enables trace queue writes
0x54    4B    trace_base_lo      Host    RP1     -- Trace queue base address
0x58    4B    trace_base_hi      Host    RP1
0x5C    4B    trace_size         Host    RP1     -- Trace entries (power of 2 recommended)
0x60    4B    trace_write_idx    RP1     Host    -- Next trace write position
0x64    ...   reserved
```

### Submission Protocol

The current implementation (both `rp1_run.c` on the firmware side and
`Rp1Submitter` on the host side) is **polling-based on both ends**; the GCQ
doorbell/IRQ path described in earlier drafts of this document
(`irq_sq`/`irq_cq`) is not wired yet -- see "Not Yet Implemented" below.

1. **Host writes** graph nodes to the node array
2. **Host writes** kernel arguments to the argument buffer
3. **Host clears** signal array slots used by this graph (`clearSignalSlots()`,
   or the `submitAndWait()` image's `clear_signal_slots`)
4. **Host records** the current `cq_write_idx` and `graph_done_seq` (so it can
   later compute this graph's CQ delta), memory-fences, then writes
   `node_count` and increments `graph_seq`, memory-fences again
5. **RP1**, spinning in `rp1_run()`'s outer loop (`wfi()` between polls when
   not built with `RP1_POLLING_BRINGUP`/`QEMU_SEMIHOSTING`), notices
   `graph_seq != graph_done_seq`
6. **RP1 calls** `rp1_store_init()` -- resolves DDR pointers from the control
   block and resets per-graph BTCM state (`g_barriers[32]`, all node
   statuses, `g_loop_iters[]`, the inflight table); sets `rp1_state = RUNNING`
7. **RP1 processes** the graph via `rp1_loop()` (Section D)
8. **RP1 writes** CQ entries for completed/failed nodes as it goes (skipped
   for `SILENT` nodes), and if `trace_enable != 0`, writes trace-ring events
   for graph, scheduling, wait, control-flow, PDI, and kernel lifecycle points
9. **RP1 sets** `graph_done_seq = graph_seq` and `rp1_state` to `READY`
   (normal completion), `ERROR` (`HALT_ON_ERROR` abort or inflight-full), or
   `HALTED` (`HALT` opcode)
10. **Host polls** `graph_done_seq` at ~1ms cadence (`kDefaultSubmitTimeout`
    default 3000ms) until it catches up to the `graph_seq` it wrote, then
    calls `drainCq()` to read the CQ entries in `[recorded cq_write_idx,
    current cq_write_idx)` and submits the next graph

### Not Yet Implemented

- **GCQ doorbell / IRQ handoff.** Ringing `S01_AXI` to raise `irq_cq` and
  waking the host without polling is a `TODO` left in `rp1_run.c`; the
  symmetric host->RP1 `irq_sq` doorbell and a host-side `eventfd` consumer
  for `irq_cq` are not implemented either. Today both sides poll a DDR word.
- **Host trace consumer.** The optional trace ring ABI exists and the firmware
  can write it, but `Rp1Submitter`/SMI do not yet drain or decode it.

### Completion Queue Entry (16 bytes)

```
Offset  Size  Field
------  ----  -----
0x00    4B    node_index          -- Which node this completes
0x04    4B    status              -- 0=OK, 1=ERROR, 2=TIMEOUT
0x08    4B    error_detail        -- Command-specific error code
0x0C    4B    timestamp           -- 64-cycle PMU ticks since graph start
```

Nodes with the SILENT flag set do not generate CQ entries.

`timestamp` is written by the firmware as `PMCCNTR - g_graph_start_cycles`.
The R5 PMU divider is left enabled, so the field is in 64-cycle ticks. It is
monotonically meaningful within one graph execution; unsigned subtraction makes
the per-graph delta robust across the 32-bit counter wrap.

### Optional Trace Queue Entry (16 bytes)

When `trace_enable != 0`, RP1 writes trace events into the ring described by
`trace_base_lo/hi`, `trace_size`, and `trace_write_idx`. The ring uses the same
monotonic-index convention as the CQ:

```
Offset  Size  Field
------  ----  -----
0x00    4B    timestamp           -- 64-cycle PMU ticks since graph start
0x04    2B    event               -- rp1_trace_event_t
0x06    2B    node_index          -- Node index, or 0xFFFF for graph events
0x08    4B    aux0                -- Event-specific detail
0x0C    4B    aux1                -- Event-specific detail
```

Current firmware events:

```
RP1_TRACE_GRAPH_START
RP1_TRACE_NODE_ACTIVATE
RP1_TRACE_KERNEL_LAUNCH
RP1_TRACE_KERNEL_DONE
RP1_TRACE_KERNEL_TIMEOUT
RP1_TRACE_LOOP_ITER
RP1_TRACE_COND_EVAL
RP1_TRACE_WAIT_PARK
RP1_TRACE_WAIT_WAKE
RP1_TRACE_PDI_LOAD
RP1_TRACE_IMAGE_MISMATCH
RP1_TRACE_GRAPH_DONE
```

The firmware resets `trace_write_idx` at each graph submission. This keeps the
first trace entry for a submission at index 0; a cumulative cross-graph trace
can be added later when a host-side drainer exists.

### Memory Ordering

R5 Cortex-R5 is weakly ordered. All writes to shared DDR must be followed by `DSB SY`. The `graph_seq` / `graph_done_seq` fields are the synchronization points. The host must complete all writes before updating `graph_seq`. RP1 must complete all side-effects before updating `graph_done_seq`.

---

## C. Signal Array

256 value-carrying slots in DDR, each 16 bytes:

```
Offset  Size  Field
------  ----  -----
0x00    4B    value               -- Current 32-bit value
0x04    4B    reserved
0x08    4B    last_writer_node    -- Which node last wrote this slot
0x0C    4B    flags               -- Bit 0: host-visible (triggers CQ entry on change)
```

The signal array is **entirely separate from the barrier system**. It carries **values** for control flow:

- **LOOP** reads a slot value to decide whether to continue iterating
- **COND** reads a slot value to decide which branch to take
- **SCALAR_READ** writes kernel register values into slots
- **SIGNAL** writes explicit values (initialization, aggregation)

The signal array is never consulted for dependency scheduling. Barriers handle that.

| System | Storage | Size | Purpose |
|--------|---------|------|---------|
| Barriers (`completed_barriers[32]`) | BTCM | 128B | Dependency scheduling (binary: done/not-done) |
| Signal array | DDR | 4KB | Value-carrying communication (for loop/cond decisions) |

---

## D. Command Processing Engine

### Flat Graph Scanner

The engine is a single flat loop that scans all nodes every pass. There are no nested scheduler calls. LOOP and COND are non-blocking -- they modify barrier/node state and return immediately. Loop bodies and conditional branches execute naturally as part of the same flat scan.

This maps onto two source files: `rp1_run.c` (the outer graph_seq poll loop,
`rp1_main` below) and `rp1_loop.c` (the flat scanner itself, `run_graph`
below -- named `rp1_loop()`/`activate_nodes()`/`check_inflight()`/
`check_waits()` in the real source).

```
uint32_t completed_barriers[32];     // g_barriers -- 128 bytes in BTCM, flat, global
uint8_t  node_status[MAX_NODES];     // g_node_status -- 1 byte per node in BTCM
uint32_t loop_iterations[MAX_LOOPS]; // g_loop_iters -- iteration counter per loop ID

struct inflight_kernel {
    uint32_t base_addr;
    uint32_t node_index;
    uint32_t set_bucket;
    uint32_t set_mask;
    uint32_t timeout_remaining;
    uint8_t  infinite;                // INFINITE flag -- don't block halt
    uint8_t  settle_polls;            // reserved for future stale-ap_done tolerance
};
inflight_kernel inflight[32];         // g_inflight, 24 bytes/entry

rp1_main():                            // rp1_run()
    magic          = RP1_CTRL_MAGIC
    version        = RP1_PROTOCOL_VERSION
    rp1_state      = READY
    graph_done_seq = 0

    while (true):
        if graph_seq == graph_done_seq:
            heartbeat++
            WFI()                       // skipped under RP1_POLLING_BRINGUP/QEMU
            continue

        // New graph submitted
        rp1_store_init()                 // resolve DDR pointers, zero all BTCM state
        rp1_state = RUNNING
        rp1_error_code = 0

        result = run_graph()

        graph_done_seq = graph_seq
        rp1_state = (result == ERR) ? ERROR : (result == HALT) ? HALTED : READY
        // TODO: ring GCQ CQ doorbell once the block design wires it


run_graph():                            // rp1_loop()
    while (true):
        activated  = activate_nodes()    // one flat scan pass, PENDING nodes only
        if activated < 0: return ERR     // inflight-full / HALT_ON_ERROR abort
        if activated == HALT: return HALT

        inflight_progress = check_inflight_kernels()
        if inflight_progress < 0: return ERR   // HALT_ON_ERROR timeout abort

        wait_progress = check_waits()    // re-poll every RP1_NODE_WAITING node
        made_progress = activated || inflight_progress || wait_progress

        // Halt condition: no DISPATCHED/WAITING nodes, no progress made
        if !made_progress:
            has_dispatched = any(node_status[i] == DISPATCHED for i in 0..node_count)
            has_waiting    = any(node_status[i] == WAITING    for i in 0..node_count)
            if !has_dispatched && !has_waiting:
                return DONE  // graph complete (or deadlocked -- we trust the graph creator)
            // A pending WAIT must busy-poll: a host BAR write does not raise
            // an R5 wake event, so WFI() could oversleep it. Only idle the
            // core when the sole outstanding work is in-flight kernels.
            if !has_waiting:
                WFI()

        heartbeat++


activate_nodes():                       // one flat scan pass over PENDING nodes
    made_progress = false

    for i in [0 .. node_count):
        if node_status[i] != PENDING:
            continue

        pkt = read_packet(i)

        if (completed_barriers[pkt.await_bucket] & pkt.await_mask) != pkt.await_mask:
            continue    // deps not met

        match pkt.opcode:
            KERNEL_DISPATCH:
                // Expected-image guard (fails fast instead of poking an
                // absent kernel) -- see the KERNEL_DISPATCH payload section.
                if pkt.expected_image_id != 0 && pkt.expected_image_id != g_active_image_id:
                    node_status[i] = ERROR
                    report_error(i, ERR_IMAGE_MISMATCH, detail=g_active_image_id)
                    if pkt.flags & HALT_ON_ERROR: return ERR
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask  // non-fatal
                    made_progress = true
                    break
                if inflight_count >= 32:
                    report_error(i, ERR_INFLIGHT_FULL)
                    return ERR
                launch_kernel(pkt)          // clears stale ap_done, writes args, pulses ap_start
                if pkt.flags & INFINITE:
                    node_status[i] = DONE
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask
                    write_cq_entry(i, OK)
                else:
                    node_status[i] = DISPATCHED
                add_to_inflight(pkt, i)
                made_progress = true

            PDI_LOAD:
                ok = rp1_pdi_load(pkt.pdi_addr_lo, pkt.pdi_addr_hi, pkt.timeout_cycles)
                if ok:
                    g_active_image_id = pkt.image_id   // persists across graphs
                    node_status[i] = DONE
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask
                    write_cq_entry(i, OK)
                else:
                    node_status[i] = ERROR
                    report_error(i, ERR_PDI_TIMEOUT)
                    if pkt.flags & HALT_ON_ERROR: return ERR
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask  // non-fatal
                made_progress = true

            LOOP:
                loop_iterations[pkt.loop_id]++
                // Check exit condition
                val = signal_array[pkt.condition_signal].value
                if (pkt.max_iterations > 0 && loop_iterations[pkt.loop_id] > pkt.max_iterations)
                   || compare(val, pkt.condition_op, pkt.condition_value):
                    // Loop done -- mark DONE, set barriers, do NOT clear body
                    node_status[i] = DONE
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask
                    write_cq_entry(i, OK)
                else:
                    // Continue looping -- clear body state, mark self DONE
                    for b in [pkt.bucket_clear_start .. pkt.bucket_clear_end]:
                        completed_barriers[b] = 0
                    for n in [pkt.body_start .. pkt.body_end]:
                        node_status[n] = PENDING
                    node_status[i] = DONE
                    // Do NOT set barrier_set -- body must complete + RERUN must fire first
                made_progress = true

            COND:
                val = signal_array[pkt.condition_signal].value
                if compare(val, pkt.condition_op, pkt.condition_value):
                    // Done -- set done barriers
                    completed_barriers[pkt.done_bucket] |= pkt.done_mask
                else:
                    // Continue -- clear body state
                    for b in [pkt.bucket_clear_start .. pkt.bucket_clear_end]:
                        completed_barriers[b] = 0
                    for n in [pkt.body_start .. pkt.body_end]:
                        node_status[n] = PENDING
                node_status[i] = DONE
                completed_barriers[pkt.set_bucket] |= pkt.set_mask
                write_cq_entry(i, OK)
                made_progress = true

            RERUN:
                node_status[pkt.target_node] = PENDING
                if pkt.rerun_flags & CLEAR_STATE:
                    loop_iterations[pkt.loop_id] = 0
                node_status[i] = DONE
                completed_barriers[pkt.set_bucket] |= pkt.set_mask
                write_cq_entry(i, OK)
                made_progress = true

            WAIT:
                val = signal_array[pkt.condition_signal].value
                if compare(val, pkt.condition_op, pkt.condition_value):
                    node_status[i] = DONE
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask
                    write_cq_entry(i, OK)
                    made_progress = true
                else:
                    node_status[i] = WAITING   // parked; check_waits() re-polls it

            HALT:
                node_status[i] = DONE
                write_cq_entry(i, OK)
                return HALT

            default:  // NOP, SIGNAL, SCALAR_WRITE, SCALAR_READ, SCALAR_COPY, DMA_COPY, DMA_FILL
                execute_immediate(pkt)         // unrecognized opcodes: no-op here too
                node_status[i] = DONE
                completed_barriers[pkt.set_bucket] |= pkt.set_mask
                write_cq_entry(i, OK)
                made_progress = true

    return made_progress


check_inflight_kernels():                // check_inflight() -- also handles per-kernel timeout
    made_progress = false
    for each inflight kernel k (iterated with in-place removal):
        if AXI_READ(k.base_addr + 0x00) & 0x2:     // ap_done
            if k.infinite:
                // INFINITE kernel finished unexpectedly -- silently drop
                remove k from inflight list
            else:
                node_status[k.node_index] = DONE
                completed_barriers[k.set_bucket] |= k.set_mask
                write_cq_entry(k.node_index, OK)
                remove k from inflight list
            made_progress = true
        else:
            k.timeout_remaining--          // decremented once per scan pass, not per cycle
            if k.timeout_remaining == 0:
                node_status[k.node_index] = ERROR
                report_error(k.node_index, ERR_KERNEL_TIMEOUT)
                if node_flags(k.node_index) & HALT_ON_ERROR:
                    remove k from inflight list
                    return ERR
                completed_barriers[k.set_bucket] |= k.set_mask  // non-fatal
                remove k from inflight list
                made_progress = true
    return made_progress


check_waits():                            // re-polls every RP1_NODE_WAITING node
    made_progress = false
    for i in [0 .. node_count):
        if node_status[i] != WAITING:
            continue
        pkt = read_packet(i)
        if compare(signal_array[pkt.condition_signal].value, pkt.condition_op, pkt.condition_value):
            node_status[i] = DONE
            completed_barriers[pkt.set_bucket] |= pkt.set_mask
            write_cq_entry(i, OK)
            made_progress = true
    return made_progress
```

### Halt Condition

The graph completes when **no further progress is possible**:
- No `made_progress` in the last scan pass (no PENDING node had its barriers met, no inflight kernel finished/timed out, no parked WAIT resolved)
- No nodes in DISPATCHED state (no kernels still running that could unblock others)
- No nodes in WAITING state (no WAIT still gated on a signal a peer/host may yet raise)

INFINITE kernels are in DONE state from the moment they're dispatched, so they never block halt. Their entries in the inflight list are for error monitoring only.

If the only outstanding work is a WAITING node, the scanner busy-polls
instead of `wfi()`-ing: a host write to the signal array over the BAR does
not raise an R5 wake event, so idling the core could miss it indefinitely.
`wfi()` is only attempted when the sole remaining work is in-flight kernels,
and only outside `QEMU_SEMIHOSTING`/`RP1_POLLING_BRINGUP` builds.

All remaining PENDING nodes have permanently unsatisfied barriers (e.g., the unchosen branch of a COND). This is correct -- those nodes were never meant to run.

HALT opcode is supplemental. It provides an explicit early exit for graphs that want to terminate before natural completion.

### Kernel Dispatch Sequence

When the scanner executes KERNEL_DISPATCH:

```
1. AXI_READ(kernel_base_addr + 0x00)        // clear stale, sticky ap_done from a prior run
2. DSB()
3. args = (rp1_kernel_arg_t *) &arg_buffer[arg_buffer_offset]
4. For i in 0..arg_count-1:
     AXI_WRITE(kernel_base_addr + args[i].reg_offset, args[i].value)
5. DSB()                                    // Ensure all args written
6. AXI_WRITE(kernel_base_addr + 0x00, 0x01) // ap_start
7. Add to inflight table, continue scanning
```

Steps 3-6 are AXI-Lite writes from R5 to PL, each taking ~10ns. A kernel with 8 arguments launches in **~100ns instead of ~10us (100x faster than PCIe)**.

### Node Layout

Each scope's nodes must be contiguous in the node array: top-level nodes in one block, each loop body in its own block, each conditional branch in its own block. The flat scanner scans all `node_count` nodes every pass, but nodes outside the active scope have unsatisfied barriers and are skipped cheaply (one BTCM read + one AND+CMP).

At any point during execution, multiple disjoint blocks of the node array can be active simultaneously. For example, two independent LOOPs in a diamond pattern both have their body nodes in PENDING state -- the scanner dispatches from both bodies in the same pass. This is how concurrent loops work without nested scheduler calls.

### Arbitrary Graph Support

| Constraint | Limit | Notes |
|------------|-------|-------|
| Total nodes per graph | 4096 | 256KB node array |
| Barrier signals | 1024 (32 buckets x 32) | 128 bytes in BTCM |
| Barriers per node dependency | 32 (one bucket) | Cross-bucket via NOP bridges |
| In-flight kernels | 32 | Error if exceeded; internal implementation limit |
| Signal array slots | 256 | For value-carrying control flow |
| Fan-in per node | 32 direct | Wider fan-in via NOP reduction nodes |
| Fan-out per node | Unlimited | Multiple nodes can await the same barrier bit |

**OR logic:** Multiple nodes set the same barrier bit. First to complete unblocks the waiter.

**AND logic:** One node awaits multiple bits. All must be set before it proceeds.

**Cross-bucket:** NOP node with `await_bucket=A, set_bucket=B` bridges regions. Executes instantly.

**Fan-in > 32:** Host inserts NOP reduction nodes. 64-wide fan-in = 2 NOPs (each gathering 32), feeding a final node that awaits both NOP outputs.

---

## E. Examples

### Diamond DAG

```
      A
     / \
    B   C
    |   |
    D   E
     \ /
      F
```

All nodes in bucket 0:

```
Node 0: KERNEL_DISPATCH A  await=0/0x00  set=0/0x01   (sets bit 0)
Node 1: KERNEL_DISPATCH B  await=0/0x01  set=0/0x02   (needs bit 0, sets bit 1)
Node 2: KERNEL_DISPATCH C  await=0/0x01  set=0/0x04   (needs bit 0, sets bit 2)
Node 3: KERNEL_DISPATCH D  await=0/0x02  set=0/0x08   (needs bit 1, sets bit 3)
Node 4: KERNEL_DISPATCH E  await=0/0x04  set=0/0x10   (needs bit 2, sets bit 4)
Node 5: KERNEL_DISPATCH F  await=0/0x18  set=0/0x20   (needs bits 3+4, sets bit 5)
```

E dispatches as soon as C finishes, regardless of B.

### OR Pattern (Race)

```
algo_A -.
         >- use_winner
algo_B -'
```

```
Node 0: KERNEL_DISPATCH algo_A  await=0/0x00  set=1/0x01   (sets bucket 1 bit 0)
Node 1: KERNEL_DISPATCH algo_B  await=0/0x00  set=1/0x01   (sets bucket 1 bit 0 -- same!)
Node 2: KERNEL_DISPATCH winner  await=1/0x01  set=1/0x02   (fires when EITHER A or B done)
```

### Cross-Bucket Bridge

```
Region A (bucket 0): nodes 0-15, produce bits 0-15
Region B (bucket 1): nodes 20-35, need all of region A done
```

```
Node 16: NOP  await=0/0xFFFF  set=1/0x01   (gather all 16 from bucket 0, bridge to bucket 1)
Node 20: ...  await=1/0x01    ...           (depends on the bridge)
```

### Iterative Convergence

```
        init
         |
    +- LOOP ----------------------+
    |      A                      |
    |     / \                     |
    |    B   C   (parallel)       |
    |     \ /                     |
    |      D                      |
    |      |                      |
    |   read_err -> signal[10]    |
    |   RERUN -> loop node        |
    +-----------------------------+
         |
      finalize
```

Outer graph: bucket 0. Loop body: buckets 4-5.

```
-- outer graph (bucket 0) --
Node 0: KERNEL_DISPATCH init     await=0/0x00  set=0/0x01
Node 1: LOOP            await=0/0x01  set=0/0x02
        body_start=3, body_end=8, max_iterations=1000
        condition_signal=10, condition_op=LT, condition_value=threshold
        bucket_clear_start=4, bucket_clear_end=5, loop_id=0
Node 2: KERNEL_DISPATCH finalize await=0/0x02  set=0/0x04

-- loop body (nodes 3-8, uses buckets 4-5) --
Node 3: KERNEL_DISPATCH A        await=4/0x00  set=4/0x01
Node 4: KERNEL_DISPATCH B        await=4/0x01  set=4/0x02
Node 5: KERNEL_DISPATCH C        await=4/0x01  set=4/0x04
Node 6: KERNEL_DISPATCH D        await=4/0x06  set=4/0x08   (needs B+C)
Node 7: SCALAR_READ err -> [10]  await=4/0x08  set=4/0x10   (needs D)
Node 8: RERUN target=1           await=4/0x10  set=4/0x20   (needs read, reruns loop node)
```

Flow: init completes -> LOOP fires, clears buckets 4-5, resets nodes 3-8 to PENDING -> body runs with full parallelism (B,C concurrent) -> SCALAR_READ captures error -> RERUN resets loop node to PENDING -> loop node fires again, checks condition -> continues or exits -> finalize runs after exit.

### Conditional Execution (If/Else)

```
    compute -> flag
       |
    COND (flag == 1?)
    /          \
  fast_path   slow_path
    \          /
      merge
```

Two CONDs with complementary conditions, each gating its own branch body:

```
-- outer graph (bucket 0) --
Node 0: KERNEL_DISPATCH compute  await=0/0x00  set=0/0x01
Node 1: SCALAR_READ flag->[20]   await=0/0x01  set=0/0x02
Node 2: COND (flag!=1 -> continue=run fast)  await=0/0x02  set=0/0x04
        condition_signal=20, condition_op=EQ, condition_value=1
        body_start=5, body_end=5, bucket_clear_start=4, bucket_clear_end=4
        done_bucket=0, done_mask=0x00
Node 3: COND (flag==1 -> continue=run slow)  await=0/0x02  set=0/0x08
        condition_signal=20, condition_op=NE, condition_value=1
        body_start=6, body_end=6, bucket_clear_start=5, bucket_clear_end=5
        done_bucket=0, done_mask=0x00
Node 4: KERNEL_DISPATCH merge    await=0/0x40  set=0/0x80

-- then branch (bucket 4) --
Node 5: KERNEL_DISPATCH fast     await=4/0x00  set=0/0x40   (sets merge dep via OR)

-- else branch (bucket 5) --
Node 6: KERNEL_DISPATCH slow     await=5/0x00  set=0/0x40   (same output bit -- OR logic)
```

Both CONDs evaluate. One's condition is "not met" (continue) so it resets its body to PENDING. The other's is "met" (done) so it does nothing. Only one branch runs. Both branches set the same merge barrier (0/0x40), so merge unblocks regardless of which ran.

### Concurrent Loops (Diamond)

```
      init
     /    \
   LOOP_A  LOOP_B    (independent, concurrent)
     \    /
      join
```

```
Node 0:  KERNEL_DISPATCH init    await=0/0x00  set=0/0x01
Node 1:  LOOP A         await=0/0x01  set=0/0x02  (body=3-5, buckets 4-5, loop_id=0)
Node 2:  LOOP B         await=0/0x01  set=0/0x04  (body=6-8, buckets 6-7, loop_id=1)
Node 9:  KERNEL_DISPATCH join    await=0/0x06  set=0/0x08  (needs both loops done)

-- loop A body (nodes 3-5, buckets 4-5) --
Node 3:  KERNEL_DISPATCH ...     await=4/0x00  set=4/0x01
Node 4:  SCALAR_READ -> [10]     await=4/0x01  set=4/0x02
Node 5:  RERUN target=1          await=4/0x02  set=4/0x04

-- loop B body (nodes 6-8, buckets 6-7) --
Node 6:  KERNEL_DISPATCH ...     await=6/0x00  set=6/0x01
Node 7:  SCALAR_READ -> [11]     await=6/0x01  set=6/0x02
Node 8:  RERUN target=2          await=6/0x02  set=6/0x04
```

Both loop bodies are active simultaneously. The flat scanner dispatches nodes from both bodies in the same pass. Kernels from loop A and loop B can be in-flight concurrently on the FPGA. Each loop has its own bucket range and loop ID -- no interference.

---

## F. Patterns

Compound node sequences that implement higher-level primitives from the base opcodes. These are analogous to pseudo-instructions in assembly -- the graph builder emits them as a fixed template, and the scanner executes them with no special case.

### AWAIT_SEMAPHORE

Wait until a signal slot satisfies a condition before allowing downstream nodes to proceed. The signal can be written by a SIGNAL node, a SCALAR_READ from a kernel register, or directly by the host into DDR.

**Nodes:** 1 (native `WAIT`, opcode `0x0001`)

```
Node K: WAIT  await=<upstream>/mask  set=<downstream>/mask
              condition_signal=S, condition_op=<op>, condition_value=V
```

**Mechanism:**

1. WAIT fires when upstream barriers are met. Reads `signal_array[S].value`.
2. If the condition holds immediately: marks itself DONE, sets downstream barriers. Downstream proceeds in the same scan pass.
3. If not: transitions to `RP1_NODE_WAITING` (not `PENDING` -- it will not be re-evaluated as a fresh dispatch candidate). `check_waits()` re-polls every `WAITING` node each scan pass, independent of barrier state, until the condition holds.

**Signal reads are atomic** -- aligned 32-bit loads on the R5 are single AXI transactions. No special synchronization is needed beyond the `volatile` qualifier on signal slots.

**Cost:** One scan pass per poll (~200-300ns). For signals written by other nodes within the same graph, the semaphore resolves within 1-2 scan passes. For host-written signals, latency depends on when the host performs the DDR write. While any node is `WAITING` the scanner busy-polls instead of `wfi()`-ing (see Section D), so this latency does not grow with core idling.

**Example -- wait for host flag:**

```
Node 10: SIGNAL slot=5, value=0, op=SET   await=0/0x01  set=0/0x02   (init slot)
Node 11: WAIT  await=0/0x02  set=0/0x04                              (AWAIT_SEMAPHORE)
               condition_signal=5, condition_op=NE, condition_value=0
Node 12: KERNEL_DISPATCH  await=0/0x04  set=0/0x08                   (runs after host sets signal[5] != 0)
```

**Legacy pattern (pre-WAIT):** before opcode `0x0001` was added, the same
effect was built from `LOOP` + `RERUN`: a `LOOP` node whose body is a single
`RERUN` that immediately re-triggers it, spinning until its condition holds
without ever clearing real work. This still works (LOOP/RERUN semantics are
unchanged) but the single-node `WAIT` form above is preferred for new graphs
-- it's cheaper (no body-clear/RERUN bookkeeping) and self-documenting.

### PUSH_SEMAPHORE

Signal a remote device that a local operation has completed. The remote device may be another SLASH board, a ROCm GPU, or any PCIe peer with a memory-mapped doorbell or signal slot. The graph creator resolves the target address at graph build time based on the device topology and P2P mappings.

**Nodes:** 1 (SCALAR_WRITE)

```
Node K: SCALAR_WRITE  await=<upstream>/mask  set=<downstream>/mask
                      target_addr=A, value=V
```

`A` is a P2P-mapped address in the R5's address space that routes through the NoC and PCIe outbound window to the remote device's memory. The graph creator computes it from the remote device's BAR address and the target offset (e.g., a signal array slot, a doorbell register, or a GPU completion flag).

**Requires:** RPU → PCIe outbound AXI path in the block design (not yet wired).

**Examples of remote targets:**

| Remote device | Target address `A` | Value `V` |
|---------------|-------------------|-----------|
| SLASH board | P2P window + signal array offset + slot × 16 | Semaphore value |
| ROCm GPU | P2P window + device doorbell offset | Completion token |
| Host memory | P2P window + host-pinned buffer offset | Status word |

**Interrupt-driven targets (host, GPU):** Use a second write pair for the doorbell so the target wakes without polling. SCALAR_WRITE supports up to `RP1_SCALAR_WRITE_MAX` (6) `(addr, value)` pairs per node, stopping at the first `addr == 0`:

```
Node K: SCALAR_WRITE  await=<upstream>/mask  set=<downstream>/mask
                      writes[0] = (addr=<signal slot>,      value=V)
                      writes[1] = (addr=<doorbell register>, value=<token>)
```

| Target | Signal address | Doorbell address | Effect |
|--------|---------------|-----------------|--------|
| Host (GCQ) | DDR signal slot (BAR-mapped) | `0x80010000` (GCQ S01_AXI CQ tail) | `irq_cq` fires, host driver wakes |
| Host (MSI-X) | Host-pinned memory via P2P | MSI-X table entry address | MSI-X vector fires |
| ROCm GPU | GPU VRAM via P2P | GPU doorbell BAR | GPU signal/interrupt |
| Remote SLASH | Remote DDR signal slot via P2P | not needed — remote RP1 polls | |

**Combined with AWAIT_SEMAPHORE for cross-device synchronization:**

```
Board A graph:                          Board B graph:
  KERNEL_DISPATCH (compute)               AWAIT_SEMAPHORE signal[5] != 0
  PUSH_SEMAPHORE → Board B signal[5]=1   KERNEL_DISPATCH (consume)
```

Board A computes, then pushes a semaphore to Board B's signal array. Board B spins until the signal arrives, then proceeds. Both graphs are submitted independently. No host involvement on the critical path.

---

## G. Error Handling

### Error codes (`rp1_ctrl_t.rp1_error_code`)

| Value | Symbol             | Meaning                                          |
|-------|--------------------|--------------------------------------------------|
| 0     | (none)             | No error since the last graph reset.             |
| 1     | `ERR_INFLIGHT_FULL`| Scanner tried to dispatch a 33rd in-flight kernel. |
| 2     | `ERR_KERNEL_TIMEOUT` | A kernel did not assert `ap_done` within `timeout_cycles`. |
| 3     | `ERR_PDI_TIMEOUT`  | The PMC did not ACK a `PDI_LOAD` IPI within `timeout_cycles`. |
| 4     | `ERR_IMAGE_MISMATCH` | A `KERNEL_DISPATCH`'s non-zero `expected_image_id` did not match the image last installed by `PDI_LOAD` (see Section A). |

### Kernel Timeout
If a kernel doesn't assert `ap_done` within `timeout_cycles`, RP1:
1. Marks the node ERROR
2. Writes a CQ entry with status=TIMEOUT
3. Sets `rp1_error_code = 2` in the control block
4. If HALT_ON_ERROR: stops graph processing. Otherwise: applies `barrier_set_mask` and continues.

### PDI Load Timeout
If the PMC does not clear the IPI observation register within
`timeout_cycles` of a `PDI_LOAD` node firing, RP1 follows the same
recipe with `rp1_error_code = 3` and a `RP1_CQ_TIMEOUT` CQ entry.  See
the `PDI_LOAD` payload description in Section A for the full sequence.

### Image Mismatch
If a `KERNEL_DISPATCH`'s `expected_image_id` is non-zero and does not match
`g_active_image_id` (the image last installed by `PDI_LOAD`; 0 = none),
RP1:
1. Marks the node ERROR
2. Writes a `RP1_CQ_ERROR` CQ entry with `error_detail` set to the active
   image id
3. Sets `rp1_error_code = 4` in the control block
4. If HALT_ON_ERROR: stops graph processing. Otherwise: applies `barrier_set_mask` and continues.

### Inflight Limit
If the scanner would dispatch a 33rd kernel, it reports ERR_INFLIGHT_FULL, sets `rp1_state = ERROR`, and aborts the graph unconditionally (this one is not gated behind `HALT_ON_ERROR` -- there is no slot to track the kernel in, so there is nothing safe to continue with).

### Unrecognized Opcode
RP1 does **not** treat an opcode outside the table in Section A as a graph
error. The scanner's `default` case (which also handles `NOP`) executes it
as a no-op immediate completion: the node is marked `DONE`, its
`barrier_set_mask` is applied, and an `RP1_CQ_OK` entry is written. There is
currently no distinct "malformed packet" detection -- a bad opcode silently
becomes a no-op rather than an error. Graph correctness for opcode values
is the host compiler's responsibility.

### Watchdog
RP1 increments `heartbeat` every scan-loop iteration (including idle polling
of `graph_seq`), so it is a liveness counter, not a fixed-period timer.
**Not yet implemented on the host side:** `Rp1Submitter` does not currently
poll `heartbeat` independently of `graph_done_seq`; a stalled RP1 during a
submission is only detected via the `submitAndWait()` timeout
(`Rp1TimeoutError`), not a dedicated heartbeat check.

### Recovery
Design intent: host resets by asserting soft reset on GCQ
(`RESET_INTERRUPT_CTRL[31]`) and waiting for `rp1_state = READY`. **Not yet
implemented:** there is no reset path wired in the current host stack
(`Rp1Submitter`/`FpgaDevice`); today the only recourse after `Rp1TimeoutError`
or `rp1_state = ERROR` is a full SBR/hotplug reset of the card via
`v80-smi reset` or the driver's hotplug ioctls.

---

## H. Integration Points

### VRT Runtime

The VRT side of the integration is built as layered components in
`vrt/{include,src}/graph/device/` (under namespace
`vrt::graph` for the public surface and `vrt::graph::fpga` for the
internal plumbing):

| Layer | Header | Role |
|-------|--------|------|
| `Rp1BarWindow` | `device/fpga/rp1_bar_window.hpp` | Owns the `vrtd::BarFile` for BAR4. Each method brackets exactly one BAR access through `BarFile::getPtr<T>(Direction, offset)` so the dma-buf `SYNC_START` / `SYNC_END` contract is honoured. |
| `Rp1Submitter` | `device/fpga/rp1_submitter.hpp` | Programs the control block on first use (`ensureReady`, checks `RP1_PROTOCOL_VERSION`), stages a fully-realised `Rp1GraphImage`, clears its `clear_signal_slots`, bumps `graph_seq`, polls `graph_done_seq`; `drainCq()` reads back this submission's CQ entries. Knows nothing about graphs or kernels. |
| `FpgaVbinSpec` | `device/fpga/vbin_spec.hpp` | Resolves an image's `system_map` (register offsets, m_axi memory regions, R5 base addresses) and assigns each named image a stable 1-based numeric id for the `expected_image_id` guard. Constructing `FpgaDevice` with a `FpgaVbinSpec` (instead of a raw `FpgaKernelLocationLookup`) is what lets `Graph::addFpga()` resolve everything below without the caller hardcoding addresses. |
| `control_lowering` helpers | `device/fpga/control_lowering.hpp` | `isRp1EvaluableCondition` / `mapRp1Condition` decide whether a host `Condition` can become a native RP1 `compare(signal, op, value)`; `SignalSlotAllocator` / `LoopIdAllocator` hand out signal slots / loop ids; `lowerBarrierEvents()` allocates barrier bits per **reset domain** (not just a single flat bucket 0). |
| `FpgaDevice : IDevice` | `device/fpga_device.hpp` | Lowers a `vrt::graph::DGraph` into an `Rp1GraphImage` (see below). |

Authoring stays in `vrt::graph::Graph` — there is no separate
`GraphBuilder` type. The high-level entry point is
`Graph::addFpga(const FpgaSpec&)` (`vrt/include/vrt/graph/authoring/fpga.hpp`),
which takes a BDF, a vrtd socket, and a list of named `FpgaImageSource`s
(vbin paths), and folds QDMA PDI staging, the vrtd session + BAR4 window,
the RP1 readiness preflight, and `FpgaDevice` construction into one call. It
returns an `FpgaHandle`; `FpgaHandle::image(name)` yields a named
`FpgaImageHandle` used both for kernel handles and, via its `ImageRef`
conversion, for reprogram nodes. The user region starts with no active
image, so every FPGA dispatch must be gated (via `.after`/reprogram
ordering) behind loading its image first. The lower-level constructor,
`FpgaDevice(id, window, FpgaKernelLocationLookup lookup, ...)`, is still
available for tests and mock/no-vbin setups: it resolves kernel names to R5
AXI-Lite base addresses via a user-supplied lookup function instead of a
vbin's `system_map`. The canonical demonstration is
`examples/graph/00_multi_image_pipeline` (`multi_image_pipeline.cpp`), which
uses the high-level `addFpga()` path with two named images (no hardcoded R5
addresses in the example itself); it exercises a CPU+FPGA loop with an
in-loop `PDI_LOAD` reprogram between the two images and a post-loop
CPU-driven conditional.

`FpgaDevice::compilePlan()` now lowers, beyond plain kernel dispatch:

- **`CompiledKernelNode` -> `RP1_OP_KERNEL_DISPATCH`.** Arguments come from
  `IOMap` scalar bindings packed in `IOTypeMap::inputScalars` order;
  constants are baked in at compile time, global-variable bindings are
  resolved at `launch()` time via the per-graph scalar map (this deferred
  path is exercised directly, not just as an unused fallback -- see
  `fpga_device_test.cpp`'s `GlobalScalarOnFpgaKernelUsesDeferredLaunchValue`
  and `DeferredScalarsResolvedAtLaunch`).
- **`CompiledReprogramNode` -> `RP1_OP_PDI_LOAD`.** The PDI is staged into
  DDR via QDMA (`setPdiStagingDevice()` / `stagePdiFile()`/`stagePdiBytes()`)
  and the resulting physical address + the image's numeric id
  (`imageNumericId()`) go into the packet; downstream `KERNEL_DISPATCH`
  nodes in that image get `expected_image_id` set so a stale dispatch fails
  fast instead of hanging (Section A).
- **`CompiledLoopNode` / `CompiledConditionalNode` -> `RP1_OP_LOOP` /
  `RP1_OP_COND` + `RP1_OP_RERUN`**, when the whole loop body / branch lowers
  to the FPGA and its predicate is RP1-evaluable
  (`isRp1EvaluableCondition`) — otherwise a split Authority/Follower
  rendezvous is generated so a peer device (e.g. CPU) can drive the control
  decision and the FPGA side follows via `WAIT`/`SIGNAL`.
- **`CompiledSignalNode` / `CompiledWaitNode` -> `RP1_OP_SIGNAL` /
  `RP1_OP_WAIT`**, for cross-queue rendezvous over host-visible signal
  slots.
- **`CompiledBridgeOpNode` -> data movement** via the registered bridge
  (see `CpuFpgaBridge` below).
- Barrier bits are allocated **per reset domain** via `lowerBarrierEvents()`
  (not simply "bucket 0"); each domain gets its own bucket range, and bit 31
  of the top-level lifecycle bucket is still reserved for the trailing
  sentinel `RP1_OP_SIGNAL` (`kDefaultSentinelSlot` / `kDefaultSentinelValue`)
  that fires once every leaf node in that domain completes.

Buffer arguments are no longer purely "the user's problem": `FpgaDevice`
packs 64-bit DDR addresses for `IOTypeMap::inputs`/`outputs`/RW buffer pairs
following the scalars, and `ensureBufferByKey()` allocates each buffer
either in real device memory (HBM/DDR, when the kernel's `m_axi` region is
known via the vbin spec and a staging device is configured) or, as a
mock/test fallback, in the BAR-window arena past the signal array
(`kBufferArenaStart`). `CpuFpgaBridge`
(`vrt/src/graph/crossdevice/cpu_fpga_bridge.cpp`) implements both buffer and
scalar transfer between CPU and FPGA queues via a semaphore pool; it has no
outstanding FPGA-side TODOs.

Current limitations (each throws a descriptive diagnostic from
`compilePlan()` rather than silently misbehaving):

- Top-level `CompiledBoundaryNode`s are not yet supported (boundaries are
  only handled inside loop/conditional child DGraphs).
- A loop body that mixes CPU and FPGA kernels, or an FPGA loop with no FPGA
  body nodes, is not yet supported.
- Control-flow outputs published to a device other than the one that
  produced them are not yet executable.
- Up to 31 "leaf" barrier bits per reset-domain bucket (bit 31 reserved);
  large graphs span multiple buckets via the reset-domain lowering rather
  than being capped at a single flat 31-kernel graph.
- Wider-than-32-bit FPGA output scalars are rejected during plan compilation
  until the protocol grows a multi-slot read.

### Driver Changes

The current implementation reuses the existing daemon path unchanged: vrtd
hands out a BAR fd via `VRTD_REQ_GET_BAR_FD`, the libvrtdpp `vrtd::BarFile`
wraps it, and `Rp1BarWindow` builds typed accessors on top. No new wire
opcodes.

Multi-tenant serialisation of `graph_seq` and an `irq_cq`-backed eventfd for
completion (replacing the current poll loop -- see Section B, "Not Yet
Implemented") remain future phases; `Rp1Submitter` documents itself as
unsafe for concurrent submitters against the same `Rp1BarWindow`.

### Linker Changes
- The RPU -> user-region AXI-Lite hardware path (`rpu_sc` M02_AXI, NoC
  `REMAPS` to `0x8800_0000`) is already wired in the base block design --
  see "Hardware Prerequisites" above. What's still missing:
- `tcl_gen.py` / `system_map.xml` do not yet emit an R5-visible address
  table alongside the PCIe-visible one; `FpgaVbinSpec` currently derives R5
  addresses from the PCIe `system_map` via the
  `r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000` formula at runtime
  rather than reading a precomputed table.

---

## I. Performance Analysis

### Dispatch Latency

| Operation | Host-Driven (PCIe) | RP1 Graph Processor |
|-----------|--------------------|-----------------------|
| 1 register write | ~1us | ~10ns |
| Kernel launch (8 args) | ~10us | ~100ns |
| 10 kernels back-to-back | ~120us | ~1us |
| 100-iter convergence loop | ~12ms + 100 host RTTs | ~12ms + 0 host RTTs |

### Throughput

- Node array: 4096 nodes x 64B = 256KB (fits in a single QDMA transfer)
- Dispatch overhead per node: ~200-300ns (DDR read + AXI-Lite writes)
- Throughput limited by kernel execution time, not dispatch

### BTCM Budget

| Item | Size |
|------|------|
| `completed_barriers[32]` | 128B |
| `node_status[4096]` | 4KB |
| `loop_iterations[64]` | 256B |
| `inflight[32]` | 768B |
| Stack | 4KB |
| Code variables | ~1KB |
| **Total** | **~10.1KB of 64KB** |

---

## J. Verification Plan

1. **QEMU test:** Write test nodes to emulated DDR. Verify barrier propagation, node execution order, CQ generation.
2. **Hardware bringup:** Verify GCQ interrupt delivery. Verify R5 DDR access.
3. **Diamond DAG:** A->{B,C}->{D,E}->F. Verify B/C dispatch concurrently.
4. **OR pattern:** Two nodes set same barrier bit. Verify consumer unblocks on first completion.
5. **Cross-bucket bridge:** NOP gathers from bucket 0, sets into bucket 1. Verify downstream unblock.
6. **LOOP + RERUN:** Loop with SCALAR_READ exit condition. Verify iteration count, bucket clearing, intra-iteration parallelism, RERUN re-triggering loop node.
7. **COND:** Conditional with both branches. Verify only one executes, merge unblocks via OR.
8. **Concurrent loops:** Diamond with two independent LOOPs. Verify both bodies active simultaneously, kernels interleaved.
9. **INFINITE kernel:** Stream producer with INFINITE flag. Verify graph completes while kernel runs, kernel silently dropped from inflight on ap_done.
10. **Fan-in reduction:** 64-wide fan-in via 2 NOP reduction nodes. Verify correct join.
11. **Error/timeout:** Invalid kernel address. Verify timeout, HALT_ON_ERROR, inflight limit error.
12. **WAIT (cross-queue rendezvous):** WAIT node parked on a signal slot the host/a peer writes after a delay. Verify status transitions to `WAITING`, `check_waits()` resolves it without a `wfi()` stall, downstream unblocks.
13. **PDI_LOAD + expected_image_id:** Reprogram between two images; verify `KERNEL_DISPATCH` nodes with a matching `expected_image_id` succeed and a stale/mismatched one fails fast with `RP1_ERR_IMAGE_MISMATCH` instead of hanging. Covered end-to-end today by `examples/graph/00_multi_image_pipeline` and `fpga_device_test.cpp`.

This plan predates the current implementation; items 1, 3-11 are exercised
today by `rp1_graph_test.c` (QEMU) and `vrt/tests/fpga_device_test.cpp` /
`graph_authoring_test.cpp` (host-side compilation + mock RP1). Item 2
(hardware bringup) is exercised by `v80-smi debug rp1-ping` and the
`smi/src/debug/rp1_probe.{hpp,cpp}` probe.
