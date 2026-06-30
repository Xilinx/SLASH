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

Before RP1 can function as a command processor, the block design needs two additions:

### 1. RPU -> User Region AXI-Lite Path

**Problem:** R5's `M_AXI_LPD` currently only routes to `gcq_m2r` and `axi_smbus_rpu` via `rpu_sc`. Kernel AXI-Lite slaves live in the user region at `0x0202_0000_0000+`, unreachable from the R5.

**Solution:** Add a third master port to `rpu_sc` (NUM_MI=3) and connect `M02_AXI` through an AXI-to-NoC bridge into `S_AXILITE_INI` (the NoC ingress port for the user kernel region). This gives RP1 direct AXI-Lite access to all kernel control/argument registers.

**Address mapping:** Map the user region into the R5's address space. The R5 has a 32-bit virtual address space but the Versal NoC handles address translation. We assign kernel registers to a window within the R5's addressable range (e.g., `0xA000_0000 - 0xA080_0000`, 8MB, mirroring the 0x0202 space).

**Files to modify:**
- `linker/resources/base/scripts/top.tcl` -- rpu_sc config, new M02_AXI port, address assignment
- `linker/src/emit/hw/tcl_gen.py` -- dynamic address assignment for kernel instances visible to R5

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
0x0002  SIGNAL           -- Write a value to a signal array slot.
0x0010  KERNEL_DISPATCH  -- Set args + start a kernel on the FPGA.
0x0011  SCALAR_WRITE     -- Write immediate values to kernel AXI-Lite registers.
0x0012  SCALAR_READ      -- Read kernel register -> signal array slot.
0x0020  DMA_COPY         -- Memory transfer (DDR-DDR phase 1, DDR-HBM phase 2).
0x0021  DMA_FILL         -- Fill a memory region with a pattern.
0x0030  PDI_LOAD         -- Trigger a partial PDI reload from DDR via the PMC.
0x0040  LOOP    -- Clear body state + buckets for next loop iteration.
0x0041  COND    -- Evaluate condition, set then_bucket or else_bucket barriers.
0x0042  RERUN            -- Clear DONE state of a target node back to PENDING.
0x00FF  HALT             -- Stop graph processing (supplemental, for early exits).
```

### Packet Payloads

#### KERNEL_DISPATCH (0x0010)

```
0x10    4B    kernel_base_addr    -- AXI-Lite base address (R5 address space)
0x14    4B    arg_buffer_offset   -- Offset into DDR arg buffer for staged arguments
0x18    2B    arg_count           -- Number of (reg_offset, value) argument pairs
0x1A    2B    ctrl_flags          -- Bit 0: auto-restart
0x1C    4B    timeout_cycles      -- Watchdog timeout (0 = default 10M cycles)
0x20    4B    expected_image_id   -- Image this kernel needs; 0 = no guard
0x24    24B   reserved
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
with reserved gaps); a 64-bit argument is two consecutive pairs. Then RP1
writes 0x01 to `kernel_base_addr + 0x00` (ap_start).

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
0x18    36B   reserved
```

Reads a kernel register and stores the value in the signal array. This bridges hardware register space to the signal array, enabling LOOP exit conditions and COND decisions based on kernel-computed values.

#### SIGNAL (0x0002)

```
0x10    4B    target_slot         -- Signal array slot index (0-255)
0x14    4B    value               -- Value to write
0x18    2B    operation           -- 0=SET, 1=ADD, 2=OR, 3=AND
0x1A    2B    reserved
0x1C    32B   reserved
```

Writes to `signal_array[target_slot].value`. Completes immediately (DONE).

#### DMA_COPY (0x0020)

```
0x10    8B    src_addr            -- 64-bit source physical address
0x18    8B    dst_addr            -- 64-bit destination physical address
0x20    4B    length              -- Transfer size in bytes
0x24    2B    src_type            -- 0=DDR, 1=HBM, 2=HOST
0x26    2B    dst_type            -- 0=DDR, 1=HBM, 2=HOST
0x28    16B   reserved
```

Phase 1: DDR-DDR only (R5 software memcpy). HOST/HBM deferred to Phase 2.

#### DMA_FILL (0x0021)

```
0x10    8B    dst_addr            -- 64-bit destination physical address
0x18    4B    length              -- Fill size in bytes
0x1C    4B    pattern             -- 32-bit fill pattern
0x20    2B    dst_type            -- 0=DDR, 1=HBM
0x22    2B    reserved
0x24    28B   reserved
```

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
0x29    15B   reserved
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
0x17    37B   reserved
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
[`driver/libslash/include/slash/uapi/rp1_protocol.h`](../../../driver/libslash/include/slash/uapi/rp1_protocol.h).
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
0x3000_0000        4KB       Control Block
0x3000_1000        256KB     Node Array -- up to 4096 x 64-byte nodes
0x3004_1000        64KB      Completion Queue (CQ) -- 4096 x 16-byte entries
0x3005_1000        1MB       Argument Buffer -- pre-staged kernel arguments
0x3015_1000        4KB       Signal Array -- 256 x 16-byte value slots
0x3015_2000        4KB       Debug/Status Area
0x3015_3000        ...       Free for future use within the BAR window
```

### Control Block (0x3000_0000, 4KB)

```
Offset  Size  Field              Writer  Reader
------  ----  -----              ------  ------
0x00    4B    magic              RP1     Host    -- 0x53515231 ("SQR1")
0x04    4B    version            RP1     Host    -- Protocol version
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
0x50    ...   reserved
```

### Submission Protocol

1. **Host writes** graph nodes to the node array
2. **Host writes** kernel arguments to the argument buffer
3. **Host clears** signal array slots used by this graph
4. **Host writes** `node_count` and increments `graph_seq`
5. **Host writes** GCQ `S00_AXI/0x000` (doorbell) -> triggers `irq_sq`
6. **RP1 receives** `irq_sq`, detects `graph_seq > graph_done_seq`
7. **RP1 clears** `completed_barriers[32]`, all node statuses, loop iteration counters
8. **RP1 processes** the graph (Section D)
9. **RP1 writes** CQ entries for completed/failed nodes
10. **RP1 sets** `graph_done_seq = graph_seq`
11. **RP1 writes** GCQ `S01_AXI/0x000` (doorbell) -> triggers `irq_cq`
12. **Host receives** `irq_cq`, reads CQ, submits next graph

### Completion Queue Entry (16 bytes)

```
Offset  Size  Field
------  ----  -----
0x00    4B    node_index          -- Which node this completes
0x04    4B    status              -- 0=OK, 1=ERROR, 2=TIMEOUT
0x08    4B    error_detail        -- Command-specific error code
0x0C    4B    timestamp           -- R5 cycle counter at completion
```

Nodes with the SILENT flag set do not generate CQ entries.

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

```
uint32_t completed_barriers[32];     // 128 bytes in BTCM, flat, global
uint8_t  node_status[MAX_NODES];     // 1 byte per node in BTCM
uint32_t loop_iterations[MAX_LOOPS]; // iteration counter per loop ID

struct inflight_kernel {
    uint32_t base_addr;
    uint32_t node_index;
    uint32_t set_bucket;
    uint32_t set_mask;
    uint32_t timeout_remaining;
    bool     infinite;               // INFINITE flag -- don't block halt
};
inflight_kernel inflight[32];

rp1_main():
    init_control_block()
    configure_gcq_interrupts()
    rp1_state = READY

    while (true):
        if graph_seq == graph_done_seq:
            WFI()
            continue

        // New graph submitted
        memset(completed_barriers, 0, sizeof(completed_barriers))
        memset(node_status, PENDING, node_count)
        memset(loop_iterations, 0, sizeof(loop_iterations))
        rp1_state = RUNNING

        run_graph()

        graph_done_seq = graph_seq
        DSB()
        ring_cq_doorbell()
        rp1_state = READY


run_graph():
    while (true):
        check_inflight_kernels()
        made_progress = false

        for i in [0 .. node_count):
            if node_status[i] != PENDING:
                continue

            pkt = read_packet(i)

            if (completed_barriers[pkt.await_bucket] & pkt.await_mask) != pkt.await_mask:
                continue    // deps not met

            match pkt.opcode:
                KERNEL_DISPATCH:
                    if inflight_count == 32:
                        report_error(i, ERR_INFLIGHT_FULL)
                        return
                    launch_kernel(pkt)
                    if pkt.flags & INFINITE:
                        node_status[i] = DONE
                        completed_barriers[pkt.set_bucket] |= pkt.set_mask
                        write_cq_entry(i, OK)
                    else:
                        node_status[i] = DISPATCHED
                    add_to_inflight(pkt, i)
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

                HALT:
                    write_cq_entry(i, OK)
                    return

                default:  // NOP, SIGNAL, SCALAR_*, DMA_*
                    execute_immediate(pkt)
                    node_status[i] = DONE
                    completed_barriers[pkt.set_bucket] |= pkt.set_mask
                    write_cq_entry(i, OK)
                    made_progress = true

        // Halt condition: no DISPATCHED nodes, no progress made
        if !made_progress:
            check_inflight_kernels()
            has_dispatched = any(node_status[i] == DISPATCHED for i in 0..node_count)
            if !has_dispatched:
                return  // graph complete (or deadlocked -- we trust the graph creator)
            // else: kernels still running, wait for completion
            WFI()

        update_heartbeat()


check_inflight_kernels():
    for each inflight kernel k:
        if AXI_READ(k.base_addr + 0x00) & 0x2:     // ap_done
            if k.infinite:
                // INFINITE kernel finished unexpectedly -- silently drop
                remove k from inflight list
            else:
                node_status[k.node_index] = DONE
                completed_barriers[k.set_bucket] |= k.set_mask
                write_cq_entry(k.node_index, OK)
                remove k from inflight list
```

### Halt Condition

The graph completes when **no further progress is possible**:
- No `made_progress` in the last scan pass (no PENDING node had its barriers met)
- No nodes in DISPATCHED state (no kernels still running that could unblock others)

INFINITE kernels are in DONE state from the moment they're dispatched, so they never block halt. Their entries in the inflight list are for error monitoring only.

All remaining PENDING nodes have permanently unsatisfied barriers (e.g., the unchosen branch of a COND). This is correct -- those nodes were never meant to run.

HALT opcode is supplemental. It provides an explicit early exit for graphs that want to terminate before natural completion.

### Kernel Dispatch Sequence

When the scanner executes KERNEL_DISPATCH:

```
1. args = (rp1_kernel_arg_t *) &arg_buffer[arg_buffer_offset]
2. For i in 0..arg_count-1:
     AXI_WRITE(kernel_base_addr + args[i].reg_offset, args[i].value)
3. DSB()                                    // Ensure all args written
4. AXI_WRITE(kernel_base_addr + 0x00, 0x01) // ap_start
5. Add to inflight table, continue scanning
```

Steps 1-4 are AXI-Lite writes from R5 to PL, each taking ~10ns. A kernel with 8 arguments launches in **~100ns instead of ~10us (100x faster than PCIe)**.

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

**Nodes:** 2 (LOOP + RERUN)

```
Node K:   LOOP   await=<upstream>/mask  set=<downstream>/mask
                 body_start=K+1, body_end=K+1, max_iterations=0
                 condition_signal=S, condition_op=<op>, condition_value=V
                 bucket_clear_start=B, bucket_clear_end=B, loop_id=L

Node K+1: RERUN  await=B/0x00  set=B/0x01
                 target_node=K, rerun_flags=0
```

**Mechanism:**

1. LOOP fires when upstream barriers are met. Reads `signal_array[S].value`.
2. If condition not met: clears bucket B, resets node K+1 to PENDING, marks self DONE (does not set downstream barriers).
3. RERUN fires immediately (await=0x00, no dependencies). Resets LOOP to PENDING.
4. Next scan pass: LOOP fires again, re-reads the signal. Repeat.
5. When condition is met: LOOP marks itself DONE, sets downstream barriers. RERUN stays DONE from the previous iteration and does not fire. Downstream proceeds.

**Signal reads are atomic** -- aligned 32-bit loads on the R5 are single AXI transactions. No special synchronization is needed beyond the `volatile` qualifier on signal slots.

**Cost:** One scan pass per poll (~200-300ns). For signals written by other nodes within the same graph, the semaphore resolves within 1-2 scan passes. For host-written signals, latency depends on when the host performs the DDR write.

**Example -- wait for host flag:**

```
Node 10: SIGNAL slot=5, value=0, op=SET   await=0/0x01  set=0/0x02   (init slot)
Node 11: LOOP  await=0/0x02  set=0/0x04                              (AWAIT_SEMAPHORE)
               body_start=12, body_end=12, max_iterations=0
               condition_signal=5, condition_op=NE, condition_value=0
               bucket_clear_start=8, bucket_clear_end=8, loop_id=0
Node 12: RERUN target=11  await=8/0x00  set=8/0x01
Node 13: KERNEL_DISPATCH  await=0/0x04  set=0/0x08                   (runs after host sets signal[5] != 0)
```

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

**Interrupt-driven targets (host, GPU):** Use a second write slot for the doorbell so the target wakes without polling. SCALAR_WRITE supports up to 4 writes per node:

```
Node K: SCALAR_WRITE  await=<upstream>/mask  set=<downstream>/mask
                      target_addr   = <signal slot>       value   = V
                      target_addr_2 = <doorbell register>  value_2 = <token>
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

### Inflight Limit
If the scanner would dispatch a 33rd kernel, it reports ERR_INFLIGHT_FULL and aborts the graph.

### Graph Error
If RP1 encounters an invalid opcode or malformed packet:
1. Sets `rp1_state = ERROR`
2. Writes error detail to control block
3. Halts processing

### Watchdog
RP1 increments `heartbeat` every N cycles. Host checks liveness periodically. If heartbeat stalls, host resets R5 via PMC registers.

### Recovery
Host resets by asserting soft reset on GCQ (`RESET_INTERRUPT_CTRL[31]`) and waiting for `rp1_state = READY`.

---

## H. Integration Points

### VRT Runtime (Phase 1 — landed)

The VRT side of the integration is built as three layered components in
`vrt/{include,src}/vrt/graph/device/` (under namespace
`vrt::graph` for the public surface and `vrt::graph::fpga` for the
internal plumbing):

| Layer | Header | Role |
|-------|--------|------|
| `Rp1BarWindow` | `device/fpga/rp1_bar_window.hpp` | Owns the `vrtd::BarFile` for BAR4. Each method brackets exactly one BAR access through `BarFile::getPtr<T>(Direction, offset)` so the dma-buf `SYNC_START` / `SYNC_END` contract is honoured. |
| `Rp1Submitter` | `device/fpga/rp1_submitter.hpp` | Programs the control block on first use (`ensureReady`), stages a fully-realised `Rp1GraphImage`, bumps `graph_seq`, polls `graph_done_seq`. Knows nothing about graphs or kernels. |
| `FpgaDevice : IDevice` | `device/fpga_device.hpp` | Lowers a `vrt::graph::DGraph` into an `Rp1GraphImage`. Walks the topologically-ordered `CompiledKernelNode`s, allocates one barrier bit per kernel in bucket 0 (bit 31 reserved for the sentinel), packs scalar args from each `IOMap` into the argument buffer as `(reg_offset, value)` pairs (using each kernel's `system_map` register offsets), and appends a trailing `RP1_OP_SIGNAL` whose `await_mask` is the OR of every leaf kernel's set-bit. |

Authoring stays in `vrt::graph::Graph` — there is no separate
`GraphBuilder` type. The user calls `Graph::withDefaults()`,
`registerDevice(std::make_shared<FpgaDevice>(...))`, then
`addNode(KernelDescriptor{name, DeviceType::FPGA, ...}, IOMap{...},
"fpga:0", afterNodes)`. Kernel names are resolved to R5 AXI-Lite base
addresses through a user-supplied `FpgaKernelLocationLookup`. The
canonical demonstration is `examples/rp1_bringup_vrt`, which is the
direct VRT-graph port of the bringup C tool's `diamond` stage.

Limitations of phase 1 (each will be addressed by a follow-up phase):

- Only `CompiledKernelNode`s are honoured. Any `CompiledBridgeOpNode`,
  `CompiledBoundaryNode`, `CompiledLoopNode`, or `CompiledConditionalNode`
  causes `FpgaDevice::compilePlan()` to throw with a descriptive
  diagnostic. Cross-device buffer transfers, structured control flow,
  and graph-region boundaries are deferred.
- Up to 31 kernels per graph (bucket-0 bits 0..30, bit 31 = sentinel).
- Argument scalars must be `GraphScalar::constant(...)` from a phase-1
  Graph; the compiler currently rejects global-variable scalar bindings
  on non-CPU kernels at the front end. FpgaDevice does implement
  deferred resolution and that path is exercised by direct DGraph
  construction in `fpga_device_test`, ready for when the compiler
  relaxes the restriction.
- Buffer data movement is the user's problem: the existing
  `CpuFpgaBridge` (`vrt/src/graph/crossdevice/cpu_fpga_bridge.cpp`)
  still has FPGA-side TODOs. Phase 1 graphs operate on buffers the
  user has pre-staged outside the graph (e.g. via libvrtdpp QDMA or
  the existing host-side `vrt::Buffer`).

### Driver Changes

Phase 1 reuses the existing daemon path: vrtd hands out a BAR fd via
`VRTD_REQ_GET_BAR_FD`, the libvrtdpp `vrtd::BarFile` wraps it, and
`Rp1BarWindow` builds typed accessors on top. No new wire opcodes.

Multi-tenant serialisation of `graph_seq` and an `irq_cq`-backed
eventfd for completion (replacing the current poll loop) are tracked
as future phases.

### Linker Changes
- `project_gen.py`: emit kernel base address table for R5 address space
- `tcl_gen.py`: generate RPU -> user region AXI path, address assignments
- System map must include R5-visible kernel addresses alongside PCIe-visible ones

For phase 1 the R5 addresses are hardcoded in the example (see
`examples/rp1_bringup_vrt/rp1_bringup_vrt.cpp`); a future phase will
auto-generate the `FpgaKernelLocationLookup` from `system_map.xml`
using the documented formula
`r5_addr = xml_addr - 0x0202'0000'0000 + 0x8800'0000`.

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
