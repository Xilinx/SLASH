# rp1_hbm_stress

Hand-authored 69-node RP1 graph that dispatches 64 `hbm_bandwidth` HLS
instances in two 32-kernel waves (the firmware's exact `RP1_MAX_INFLIGHT`
cap) inside an N-iteration loop, then verifies completion via a sentinel
`SIGNAL` plus a `SCALAR_READ` of kernel_0's `out_acc` register. The
example uses libslash for raw BAR4 access and the shared
[`slash/uapi/rp1_protocol.h`](../../driver/libslash/include/slash/uapi/rp1_protocol.h)
ABI; no vrtbin, no VRT discovery.

It exists to stress-test the dispatch + barrier + LOOP/RERUN path of the
RP1 firmware end-to-end with the maximum kernel concurrency the
firmware supports.

## What the graph does

```
                ┌────────── body, N iterations ──────────┐
init ─> LOOP ─> 32x wave A ─> 32x wave B ─> RERUN ─> (back to LOOP)
           │                                            │
           └─────── exit ─────> sentinel ─> SCALAR_READ ┘
```

Each iteration:

- `Wave A` is 32 `KERNEL_DISPATCH` nodes (kernels 0..31). Their await
  mask is `bucket 2 / 0x00000000` — always met inside the body, since
  the LOOP clears bucket 2 at the start of every non-exit iteration.
  Each kernel sets its own bit in bucket 2.
- `Wave B` is the next 32 `KERNEL_DISPATCH` nodes (kernels 32..63).
  Their await is `bucket 2 / 0xFFFFFFFF` — fully gated on all 32
  wave-A completions. Each kernel sets its own bit in bucket 3.
- `RERUN` awaits `bucket 3 / 0xFFFFFFFF` and pokes the LOOP node back to
  `PENDING`, which re-fires LOOP on the next scan pass for the next
  iteration's exit-condition check.

The RP1 scanner dispatches all 32 ready KERNEL_DISPATCH nodes in a
single `activate_nodes()` pass (one ~50 ns AXI-Lite dispatch each →
~1.6 µs to fill the in-flight table). They then run truly concurrently
on the fabric, with `check_inflight()` reading each kernel's `ap_done`
bit on subsequent passes.

After `--iters` iterations LOOP exits, sentinel `SIGNAL` writes
`0xC0FFEE01` into slot 1, and `SCALAR_READ` captures kernel_0's
`out_acc` into slot 2.

## Bucket plan

| Bucket | Bits used | Cleared by LOOP? | Purpose |
|--------|-----------|-------------------|---------|
| 0 | bit 0 = init, bit 1 = LOOP-exit, bit 2 = sentinel, bit 3 = SCALAR_READ | no | lifecycle gates outside the loop body |
| 2 | bits 0..31 (one per wave-A kernel) | yes (cleared every non-exit LOOP iteration) | wave-A completion fan-in |
| 3 | bits 0..31 (one per wave-B kernel) | yes | wave-B completion fan-in |
| 4 | bit 0 (RERUN done) | no | accumulates harmlessly, nobody awaits it |

Hand-authoring (rather than going through VRT's `FpgaDevice`) lets us
use all 32 bits of buckets 2/3 instead of paying the
[31-kernel-per-graph cap](../../vrt/src/graph/device/fpga_device.cpp)
that `FpgaDevice` enforces (it reserves bit 31 for an
auto-emitted sentinel).

## Concurrency

- Up to **32 kernels in flight simultaneously**, set by `RP1_MAX_INFLIGHT`
  in [`driver/libslash/include/slash/uapi/rp1_protocol.h`](../../driver/libslash/include/slash/uapi/rp1_protocol.h).
  Attempting to dispatch a 33rd raises `ERR_INFLIGHT_FULL` and halts
  the graph. This example dispatches exactly 32 per wave.
- Wave B is fully gated on wave-A completion via
  `await = 2 / 0xFFFFFFFF`, so the in-flight table is empty again
  before wave B starts. Same across iterations.
- A single `hbm_bandwidth` kernel does `LENGTH = 16 Mi × 32 B = 512 MiB`
  of HBM traffic. With 32 kernels sharing the HBM ports, expect tens to
  hundreds of ms per wave, dominated by HBM contention.

## Prerequisites

Same as [`examples/rp1_bringup`](../rp1_bringup/),
[`examples/rp1_pipeline`](../rp1_pipeline/), and
[`examples/rp1_bringup_gpu`](../rp1_bringup_gpu/):

1. **Bitstream.** PF2 BAR4 maps the RP1 64 MiB DDR aperture at host
   BAR4 offset 64 MiB (the same convention every other RP1 example
   uses).
2. **Kernel placement.** 64 instances of `hbm_bandwidth` placed at R5
   addresses `0x88000000 + n * 0x10000` for `n in [0, 64)` (i.e.
   host-view `0x202_0000_0000 + n * 0x10000`, following the standard
   `r5_addr = xml_addr - 0x202_0000_0000 + 0x88000000` translation).
   If your bitstream uses a different base or stride, edit
   `kKernelBaseR5` / `kKernelStrideR5` near the top of
   `rp1_hbm_stress.cpp`.
3. **HBM.** Each kernel gets a 512 MiB slice at
   `kHbmBase + n * 0x2000_0000` (64 × 512 MiB = 32 GiB total).
   `kHbmBase = 0x40_0000_0000` (`0x4000000000`) is the address the
   bitstream is **wired** to expose HBM at; the 64 slices span
   `0x40_0000_0000 .. 0x48_0000_0000`. This is a `constexpr` in
   `rp1_hbm_stress.cpp`, not a CLI flag — every other value will fault
   or stall on this bitstream; edit the constant only if you re-wire
   the HBM aperture.
4. **Firmware.** Build `rp1.elf` with `-DRP1_POLLING_BRINGUP=ON` and
   load it onto R5-1 via xsdb (the GCQ doorbell isn't wired yet, so RP1
   spin-polls `graph_seq` instead of `wfi`'ing). Confirm with
   `examples/rp1_bringup dump /dev/slash_ctl0` before running this tool:
   `magic = "SQR1"`, `rp1_state = READY`, `heartbeat` advancing.

## Build

```bash
cd examples/rp1_hbm_stress
cmake -B build -S . -DSLASH_USE_REPO=ON
cmake --build build
# produces: build/rp1_hbm_stress
```

## Run

```bash
sudo ./build/rp1_hbm_stress /dev/slash_ctl0
```

Full CLI:

```
rp1_hbm_stress <slash_ctl_dev>
  [--wr 0|1]         default 1 (read+XOR mode; out_acc gets populated)
  [--iters N]        default 5
  [--timeout-ms MS]  default 30000
```

`kHbmBase` is a `constexpr` in `rp1_hbm_stress.cpp` (wired in by the
bitstream). `cq_size` is mechanically derived as the smallest power of
two that fits `iters * 65 + 4` CQ entries.

Expected output:

```
rp1_hbm_stress: ctl=/dev/slash_ctl0 hbm_base=0x4000000000 wr=1 iters=5 cq_size=512 timeout_ms=30000
[rp1_hbm_stress] graph layout: 69 nodes, expecting 329 CQ entries (1 init + 5x65 + 1 loop-exit + 1 sentinel + 1 scalar-read)
[rp1_hbm_stress] kernels at R5 0x88000000..0x883f0000 stride 0x10000; HBM slices 0x4000000000..0x4800000000
[rp1_hbm_stress] submitting graph seq=1, polling...
[rp1_hbm_stress] done in <us> us (avg <ms> ms per iter)
  cq_delta             = 329 (expect 329)
  signal[1] (sentinel)         = 0xc0ffee01 (expect 0xc0ffee01)
  signal[2] (kernel_0 out_acc) = 0x........
  rp1_state            = 1 (READY)
PASS
```

## Diagnostics: `cq_delta` to stall point

If the graph stalls or times out, `cq_delta` (the number of CQ entries
this submission produced) tells you exactly where in the graph the
firmware got stuck. Let `K = (cq_delta - 1) / 65` (the number of fully
completed iterations) and `r = (cq_delta - 1) % 65`.

| `cq_delta` | Meaning |
|---|---|
| 0 | init SIGNAL never fired — firmware didn't pick up the new `graph_seq`. Re-check `rp1_bringup dump` for `heartbeat` advancing. |
| 1 | init done; iter 1 wave A never dispatched — bucket 2 / barrier wiring problem (impossible unless source code edited). |
| `1 + r` where `0 < r < 32` | iter `K+1` wave A: `r` of 32 kernels reported `ap_done`, the other `32 - r` are stuck. Check those kernels' AXI-Lite base addresses respond, and probe their `ap_idle` / `ap_done` bits via `lspci` / direct BAR access. |
| `1 + 32` | iter `K+1` wave A done, wave B not started — extremely unlikely (wave B's await mask is `2/0xFFFFFFFF`, met by definition). |
| `1 + r` where `32 < r < 64` | iter `K+1` wave B: `r - 32` of 32 kernels done, the rest stuck (same diagnosis as wave A). |
| `1 + 64` | iter `K+1` both waves done, RERUN never fired — barrier wiring on node 66 is broken (impossible unless source code edited). |
| `1 + iters*65` | all `iters` body iterations done, LOOP didn't exit — `max_iterations` is wrong or `loop_iters[0]` got clobbered. |
| `1 + iters*65 + 1` | LOOP exited (cq has the exit entry); sentinel never fired — barrier wiring on node 67. |
| `1 + iters*65 + 2` | sentinel done; SCALAR_READ on kernel_0 never fired — barrier wiring on node 68. |
| `1 + iters*65 + 3` = `iters*65 + 4` | PASS (== expected) |

The tool prints the human-readable form of this lookup automatically:

```
TIMEOUT after 30000 ms
  graph_done_seq=0 (want 1)
  cq_delta=33 (expect 329)
  stall:   in iter 1 wave A: 32/32 kernels done, 0 stuck (check those kernel base addresses + ap_done)
  magic            = 0x53515231 (SQR1)
  ...
```

## Notes

- The kernel address translation is `r5_addr = xml_addr - 0x202_0000_0000 + 0x88000000`.
  If your bitstream's `system_map.xml` places the 64 instances at host
  base addresses other than `0x202_0000_0000 + n * 0x10000`, edit
  `kKernelBaseR5` and `kKernelStrideR5` near the top of
  `rp1_hbm_stress.cpp` and rebuild.
- With `--wr 0` the kernels are in write mode; `out_acc` is **not**
  populated, so `signal[2]` will read 0 (or whatever stale value was in
  HBM's `out_acc` register). Use `--wr 1` if you want a meaningful
  read-back; first running with `--wr 0` then `--wr 1` initialises the
  slices to a known pattern before the read so the XOR is reproducible.
- The 32 in-flight cap is *exact*. Bumping wave size above 32 will trip
  `ERR_INFLIGHT_FULL` (firmware error code 1). The scanner code for
  this check lives in
  [`linker/resources/aved/rp1/src/rp1_loop.c`](../../linker/resources/aved/rp1/src/rp1_loop.c)
  (`activate_nodes` → `RP1_OP_KERNEL_DISPATCH`).
