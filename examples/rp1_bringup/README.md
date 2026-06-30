# rp1_bringup

> **Deprecated for the `diamond` stage.** The VRT-graph port at
> [`../rp1_bringup_vrt/`](../rp1_bringup_vrt/) reproduces the four-kernel
> diamond DAG on top of `vrt::graph::Graph` + `vrt::graph::FpgaDevice`
> and is the canonical phase-1 regression test. Prefer it over the
> `diamond` subcommand of this tool. The `dump`, `signal`, and `kernel`
> subcommands are still useful as lower-level diagnostics and are kept
> in tree. The whole tool will be deleted once `rp1_bringup_vrt` has
> been validated on silicon.

Temporary scaffolding for the first on-hardware tests of the RP1 graph
processor. The `signal` / `kernel` subcommands remain the smallest
possible way to validate firmware liveness and the AXI-Lite path
without any VRT runtime in the picture.

The tool submits tiny graphs to the RP1 firmware over BAR4 (the same
BAR the existing `07_rp1_memcheck` uses) and polls `graph_done_seq` for
completion.

## Prerequisites

1. **Bitstream.** A PDI that maps the user-region AXI-Lite aperture
   (`0x0202_0000_0000 + 128 MB` from the host side) into R5 space at
   `0x8800_0000 + 128 MB` via the RPU → NoC path. The hardware/linker
   team's drop satisfies this.
2. **Firmware.** Build `rp1.elf` with `RP1_POLLING_BRINGUP=ON`. The GCQ
   doorbell (`irq_sq`) is not yet wired, so until it is, RP1 must
   spin-poll `graph_seq` instead of `wfi`-ing:
   ```bash
   cd linker/resources/aved/rp1
   cmake -S . -B build-bringup -DRP1_POLLING_BRINGUP=ON
   cmake --build build-bringup
   # produces build-bringup/rp1.elf
   ```
3. **Load `rp1.elf` onto R5-1 via xsdb** (same pattern as
   `rp1_memtest.elf`). After load, the firmware should sit in
   `RP1_STATE_READY` with `heartbeat` advancing.

## Build

```bash
cd examples/rp1_bringup
cmake -B build -S . -DSLASH_USE_REPO=ON
cmake --build build
```

## Run

Three subcommands, all targeting BAR4. Run them in order.

### 1. `dump` — sanity-check firmware liveness

```bash
./build/rp1_bringup dump /dev/slash_ctl0
```

Reads the RP1 control block, prints all fields, samples `heartbeat`
twice 500 ms apart. Expect:
- `magic == 0x53515231` ("SQR1")
- `rp1_state == 1 (READY)`
- heartbeat advancing

If `magic` is `0` or `0xFFFFFFFF`, the firmware never wrote the control
block. If `heartbeat` is stuck, RP1 is hung (often the `wfi` problem —
double-check `RP1_POLLING_BRINGUP` was set).

### 2. `signal` — Stage 0: no kernels

```bash
./build/rp1_bringup signal /dev/slash_ctl0
```

Submits a one-node `SIGNAL` graph that writes `0xDEADBEEF` into signal
slot 0. Pass iff slot 0 reads back `0xDEADBEEF`. This validates:

- the firmware boots end-to-end on real silicon
- BAR4 → RP1 shared DDR mapping is correct
- the flat scanner can process at least one immediate-completion node
- `graph_done_seq` advances and is visible to the host

Stage 0 deliberately does **not** exercise the new AXI-Lite path. If
this fails, the issue is in shared-DDR plumbing or the firmware itself,
not the new aperture.

### 3. `kernel` — Stage 1: one KERNEL_DISPATCH

```bash
./build/rp1_bringup kernel /dev/slash_ctl0 <r5_addr_hex> [arg0_hex ...]
```

Submits  `SIGNAL → KERNEL_DISPATCH → SIGNAL`. Pass iff both signal
slots read back their expected markers (`0xBEEFBEEF` pre, `0xCAFEBABE`
post). This validates:

- everything Stage 0 validated, plus
- RP1 can write to a kernel's AXI-Lite registers through the new
  LPD → NoC → user-region path
- the kernel asserts `ap_done`
- `check_inflight()` finalises the dispatched node and unblocks
  downstream

`<r5_addr_hex>` is the kernel's address **in R5 space**, computed from
the host-view `<BaseAddress>` in `system_map.xml`:

```
r5_addr = xml_addr - 0x0202_0000_0000 + 0x8800_0000
```

For example, if `system_map.xml` lists the kernel at `0x0202_0001_0000`,
the R5 address is `0x8801_0000`.

Optional `[arg0_hex arg1_hex ...]` are written to the kernel's AXI-Lite
arg registers at offsets `+0x10, +0x14, +0x18, ...`. For
`examples/00_axilite/increment(size, in)` you typically want `size=0`
(so the loop is a no-op and `ap_done` fires immediately) plus the two
words of an unused `in` pointer, e.g.:

```bash
./build/rp1_bringup kernel /dev/slash_ctl0 0x88010000 0 0 0
```

If you'd rather pre-stage the args yourself over the user-region BAR
before invoking this tool, leave the args off — `arg_count` will be 0
and the firmware will only emit `ap_start`.

### 4. `diamond` — Stage 2: four kernels, A → {B, C} → D

```bash
./build/rp1_bringup diamond /dev/slash_ctl0
```

Builds and submits

```
        A
       / \
      B   C        all four KERNEL_DISPATCH
       \ /
        D
        |
      SIGNAL       sentinel: writes 0xD1A1D0DD into slot 0
```

Pass iff the sentinel slot reads back `0xD1A1D0DD` and exactly 5 CQ
entries were written (4 kernels + 1 signal). Validates, beyond what
the single-kernel test does:

- barrier AND — D waits for both B and C
- parallel dispatch — B and C are in flight together at some point
- the scanner chains multiple in-flight kernels through `check_inflight`

The four R5 addresses + shared args are **hardcoded** at the top of
`rp1_bringup.c`:

```c
#define DIAMOND_KERNEL_A_R5   0x88010000u
#define DIAMOND_KERNEL_B_R5   0x88020000u
#define DIAMOND_KERNEL_C_R5   0x88030000u
#define DIAMOND_KERNEL_D_R5   0x88040000u
static const uint32_t DIAMOND_ARGS[] = { 0u, 0u, 0u };
```

Edit those to match your bitstream, rebuild, re-run. All four kernels
share the same `DIAMOND_ARGS` list, so this assumes four instances of a
kernel with the same AXI-Lite signature (e.g. four `00_axilite/increment`
instances with `size=0`). If your diamond needs heterogeneous args,
duplicate the dispatch loop inside `cmd_diamond`.

On failure the tool prints `cq_delta` (how many CQ entries the diamond
produced) — this tells you exactly how far the graph got:

| `cq_delta` | Where it stalled |
|---|---|
| 0 | A never started — re-check `dump`, and that DIAMOND_KERNEL_A_R5 responds |
| 1 | A completed, B/C never returned ap_done — DIAMOND_KERNEL_B_R5 or _C_R5 is wrong |
| 2 | A + one of {B,C} completed, the other stalled |
| 3 | A + B + C done, D stalled — DIAMOND_KERNEL_D_R5 is wrong |
| 4 | All four kernels completed, but the trailing SIGNAL didn't fire — barrier wiring or DDR write visibility bug |
| 5 | PASS (you shouldn't see this in the failure path) |

## Diagnostics

On failure (or timeout) the tool prints the full control block. Useful
fields:

- `rp1_state` — `RUNNING` means stuck mid-graph; `ERROR` means the
  firmware bailed; `READY` means the graph is "done" but our marker
  signal didn't fire.
- `rp1_error_code` — `1` = inflight list full, `2` = kernel timeout.
- `rp1_current_node` — the last node the scanner activated.
- `cq_write_idx` — number of CQ entries written.

If `cq_write_idx` increased but the trailing SIGNAL didn't fire,
suspect the new AXI-Lite path: the kernel was dispatched but its
`ap_done` never came back through `check_inflight`.
