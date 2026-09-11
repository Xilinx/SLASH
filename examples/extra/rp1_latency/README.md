# RP1 latency microbenchmarks

This standalone hardware benchmark compares the RP1 command processor with the
legacy VRT host-driven path on the same V80 and the same no-op HLS kernel. It
targets latency and dispatch scaling, not application throughput.

## What it measures

- Setup without programming:
  - `vrt.setup.no_program`
  - `rp1.setup.add_fpga`
- User-region programming:
  - `vrt.program.design_write_reset` calls the vrtd design writer directly. It
    includes vrtd's PF2 remove/rescan and BAR reopen, but not the legacy
    `Device` constructor's fixed one-second post-program sleep.
  - `rp1.program.first_with_staging` includes the first QDMA PDI staging plus
    `PDI_LOAD`. Vbin extraction and reading the PDI into the graph image spec
    already happened in `rp1.setup.add_fpga`.
  - `rp1.program.cached_pdi` reuses the staged PDI and measures `PDI_LOAD`.
- One no-op kernel:
  - Legacy VRT `Kernel::start()`, `Kernel::wait()`, and their total.
  - RP1 backend `launch()`, `wait()`, and their total.
  - Raw `Rp1Submitter::submitAndWait()` host round trip. The projected backend
    image includes its lifecycle `SIGNAL` sentinel, so this is a tiny graph
    round trip rather than a bare one-packet doorbell.
  - `rp1.result.kernel.graph_elapsed` is the firmware's uninstrumented
    `Rp1GraphResult::graphElapsedTicks` for that graph.
  - Instrumented RP1 graph-to-dispatch, launch-to-done, and graph duration in
    protocol PMU ticks.
- Sequential batches of 1, 10, and 100 launches by default:
  - Legacy VRT performs `start()`/`wait()` for each launch.
  - `vrt.batch.<N>.start_to_next_start` measures adjacent returned
    `Kernel::start()` calls with the host monotonic clock.
  - `vrt.batch.<N>.done_to_next_start` measures a returned `Kernel::wait()`
    to the following returned `Kernel::start()`.
  - RP1 submits one dependency chain and executes it without host intervention.
  - `rp1.result.batch.<N>.graph_elapsed` reports the graph-result timing without
    enabling tracing.
  - `rp1.trace.batch.10.launch_to_next_launch` measures all nine adjacent
    `KERNEL_LAUNCH(i)` to `KERNEL_LAUNCH(i+1)` intervals directly.
  - `rp1.trace.batch.10.done_to_next_launch` summarizes all nine adjacent
    `KERNEL_DONE(i)` to `KERNEL_LAUNCH(i+1)` handoffs per traced submission.
  - The corresponding `*_excluding_flush` rows subtract any bracketed
    `TRACE_FLUSH_START` to `TRACE_FLUSH_END` interval, while `trace_flush`
    reports the blocking flush itself.
- Transfers:
  - Legacy VRT host-to-DDR and DDR-to-host QDMA sync.
  - RP1 phase-1 DDR-to-DDR software `DMA_COPY`, reported both as host round-trip
    time and graph-result elapsed ticks. Protocol v6 packs its byte count into
    28 bits; this benchmark's 8 MiB scratch ranges impose the smaller limit.
- Differential RP1 memory access tracing:
  - Chains of 128 immediate nodes execute in one scanner pass, keeping every
    `NODE_ACTIVATE` interval below the BTCM trace-page flush threshold.
  - `nop_gap`, `dma_fill_gap`, and `dma_copy_gap` are raw adjacent activation
    intervals in divided PMU ticks.
  - `dma_fill_minus_nop` estimates DDR write completion cost.
  - `dma_copy_minus_nop` estimates combined DDR read/write cost.
  - `ddr_read_copy_minus_fill` estimates the additional DDR read cost.
  - Differential rows aggregate the whole chain before dividing and report R5
    core cycles, recovering sub-80-nanosecond mean resolution.
  - Four-byte `scalar.write_*` and `scalar.read_*` rows characterize the actual
    scalar opcodes; SCALAR_READ includes publication to its DDR signal slot.

The transfer metrics are intentionally not presented as a speedup ratio. The
current RP1 firmware only implements local DDR-to-DDR software copies; it does
not implement the host/HBM DMA path. VRT buffer sync crosses PCIe, so these are
different transfer domains.

VRT transfer sizes are logical API byte counts. libvrtd rounds sub-page
requests to a 4 KiB DMA page; a sub-page host-to-device sync also performs a
bounce-buffer read/modify/write. The default 4-byte and 64-byte rows therefore
characterize small-transfer API latency, not literal 4-byte or 64-byte PCIe
transactions.

## Build

Host-only build on a machine without Vivado:

```bash
cmake -S examples/extra/rp1_latency -B examples/extra/rp1_latency/build \
  -G Ninja -DVRT_USE_REPO=ON -DBUILD_VBIN=OFF
cmake --build examples/extra/rp1_latency/build
ctest --test-dir examples/extra/rp1_latency/build --output-on-failure
```

Hardware vbin build on the Vivado/Vitis build server:

```bash
cmake -S examples/extra/rp1_latency -B examples/extra/rp1_latency/build-hw \
  -G Ninja -DVRT_USE_REPO=ON
cmake --build examples/extra/rp1_latency/build-hw --target rp1_latency
cmake --build examples/extra/rp1_latency/build-hw --target latency_hls
cmake --build examples/extra/rp1_latency/build-hw --target rp1_latency_hw
```

The hardware artifact is `rp1_latency_hw.vbin`.

## Run

The FPGA host must run matching protocol-v6 RP1 firmware and vrtd. No other RP1
submitter may use the card concurrently.

```bash
timeout --foreground 120s ./rp1_latency \
  --bdf 0000:65:00.0 \
  --vbin ./rp1_latency_hw.vbin
```

Useful overrides:

```bash
timeout --foreground 300s ./rp1_latency \
  --bdf 0000:65:00.0 \
  --vbin ./rp1_latency_hw.vbin \
  --iterations 200 \
  --warmup 20 \
  --program-iterations 5 \
  --transfer-iterations 50 \
  --trace-iterations 20 \
  --batch-sizes 1,10,100 \
  --transfer-sizes 4,64,4096,1048576 \
  --memory-sizes 4,64,256,4096 \
  --memory-chain 128 \
  --csv > latency.csv
```

Pass the generated R5 clock with `--r5-hz HZ` to add estimated-nanosecond rows
for firmware timestamps. One protocol PMU tick is exactly 64 R5 core cycles.
The benchmark always retains the raw tick rows.

The external timeout is a safety boundary for legacy `Kernel::wait()`, which
busy-polls without an internal timeout. An RP1 submission has its own 30-second
timeout, but that timeout poisons the submitter and requires card recovery.

## Reading the results

Each row reports count, minimum, p50, p95, p99, maximum, and mean. Host times
use `std::chrono::steady_clock` and nanoseconds.

VRT buffer construction happens outside the transfer timers. The transfer rows
therefore exclude daemon allocation, first-superblock provisioning, QDMA queue
creation, registration, and mmap costs; they measure repeated `Buffer::sync()`.

`rp1.backend.launch_thread` measures the backend's asynchronous worker launch;
the RP1 doorbell is written by that worker. It is not the hardware dispatch
timestamp. Use `rp1.trace.kernel.dispatch` for the firmware-side dispatch
interval.

Raw RP1 host round trips currently include the submitter's one-millisecond
polling cadence. Fast graphs can therefore appear quantized near one
millisecond even when firmware trace intervals are much smaller. Trace runs are
reported separately because recording timestamps still adds firmware work.
Every raw submission measures the complete `submitAndWait()` call, including
firmware-contract preflight, BAR staging, polling, result reads, and protocol
consistency validation. The benchmark then checks outcome, flags, active image,
completed operation count, timing, trace state, and quiescence after the stop
timestamp. Those benchmark-specific checks do not inflate the host-latency
rows, but submitter validation does.

`rp1.result.*.graph_elapsed` and
`rp1.transfer.*.graph_elapsed` come from
`Rp1GraphResult::graphElapsedTicks`. This interval starts when firmware accepts
the graph and ends immediately after it emits `GRAPH_DONE`; it excludes final
trace draining and result publication. The associated raw kernel/batch samples
run with tracing disabled, while the separate `rp1.trace.*` rows intentionally
measure instrumented executions.

Normal events are staged in a 4 KiB BTCM page. When that page fills, firmware
blocks to copy it to the shared DDR ring and brackets the copy with
`TRACE_FLUSH_START/END`. The raw handoff metric includes such a flush when it
falls between a completion and its successor; the adjusted metric removes it.
Both metrics still include timestamp capture, dependency scheduling, node
activation, and the next kernel's no-argument launch. At the V80 R5's 800 MHz
clock, five microseconds is 62.5 protocol PMU ticks; pass `--r5-hz 800000000`
to emit estimated-nanosecond rows.

The RP1 transfer graph-result timing includes graph validation, scanner
dispatch, the software copy, and graph completion. It is not a copy-only
hardware timer. Programming rows also have intentionally different boundaries,
as described above, and should not be interpreted as a pure PDI-loader speedup
ratio.

The memory-trace benchmark uses two private ranges near the end of RP1's
64 MiB shared window. DMA_FILL and DMA_COPY use identical dependency chains,
destinations, trace boundaries, and `dsb st` completion semantics. Their
difference is therefore a practical latency estimate, not a physical DDR
controller counter. Repeated addresses characterize the hot shared-window path
used by RP1 telemetry and arguments; they do not model random-address DRAM.

For less noisy host measurements, reserve the machine, pin the process to one
CPU, use a fixed CPU frequency policy, and repeat the complete run. Programming
samples modify the user region repeatedly; do not run applications on the same
card at the same time.
