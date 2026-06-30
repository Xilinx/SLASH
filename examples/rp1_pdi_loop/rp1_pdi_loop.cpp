/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * rp1_pdi_loop -- end-to-end demonstration of RP1's PDI_LOAD opcode.
 *
 * Workflow on real silicon (this branch builds the artefacts but does
 * not run them; see README.md for the pending hardware bring-up):
 *
 *   1. Open libvrt against the V80, programming the user region with
 *      vbin A's partial PDI ("kernel A loaded" initial state).
 *   2. Extract both vbins' user-region partial PDIs to in-memory byte
 *      vectors via vrt::Vrtbin::getPdiPath().  These are exactly the
 *      *_partial.pdi files emitted by Vivado's `write_device_image
 *      -cell top_i/slash` (see linker/resources/base/scripts/
 *      slash_project_build.tcl).
 *   3. Allocate two vrt::Buffer<uint8_t>(DDR) chunks, memcpy each PDI
 *      in, and sync(HOST_TO_DEVICE).  QDMA H2C copies the bytes into
 *      FPGA DDR; getPhysAddr() returns the 64-bit DDR physical address
 *      RP1 will hand to the PMC.
 *   4. Open BAR4 via vrtd; construct an Rp1BarWindow + Rp1Submitter to
 *      manage the host-shared DDR aperture (control block, node array,
 *      arg/CQ/signal regions).
 *   5. Hand-pack a 10-node RP1 graph that loops `PDI_LOAD(A) ->
 *      KERNEL_DISPATCH @ R5 0x88000000 -> SCALAR_READ +0x10 -> slot 0
 *      -> PDI_LOAD(B) -> KERNEL_DISPATCH -> SCALAR_READ -> slot 1 ->
 *      RERUN` for `--iterations` body cycles.  Bucket 0 carries the
 *      init -> LOOP -> sentinel edges; bucket 1 carries body bits and
 *      is cleared each iteration via the LOOP's bucket_clear range.
 *   6. submitAndWait, then read the three signal slots and the CQ
 *      delta to confirm both kernel variants were dispatched, that
 *      slot 0 was last written by kernel A and slot 1 by kernel B,
 *      and that exactly the expected number of CQ entries fired.
 *
 * This file authors the graph by hand because phase-1
 * vrt::graph::FpgaDevice does not yet emit `PDI_LOAD` / `LOOP` /
 * `RERUN` nodes (see vrt/src/graph/device/fpga/fpga_device.cpp).  The
 * authoring style mirrors examples/rp1_bringup/rp1_bringup.c's
 * `cmd_diamond`.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <slash/uapi/rp1_protocol.h>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/vrtbin.hpp>

#include <vrtd/bar.hpp>
#include <vrtd/bar_file.hpp>
#include <vrtd/device.hpp>

#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga/rp1_submitter.hpp>

using vrt::graph::fpga::Rp1BarWindow;
using vrt::graph::fpga::Rp1GraphImage;
using vrt::graph::fpga::Rp1Submitter;
using vrt::graph::fpga::Rp1TimeoutError;

// ---------------------------------------------------------------------------
// Test config -- edit if you fork the example.
// ---------------------------------------------------------------------------

/// R5 address of the single user-region kernel.  Both partial PDIs
/// instantiate one kernel called `pdi_kernel`, which the linker places
/// at host `0x0202_0000_0000` -> R5 `0x8800_0000` (the first user-
/// region slot, 64 KiB-aligned; see linker/src/emit/hw/user_region/
/// addr_ctx.py and linker/src/emit/hw/tcl_gen.py).
constexpr std::uint32_t kKernelR5 = 0x8800'0000u;

/// AXI-Lite offset of the `out` output scalar register inside the
/// kernel's slave.  HLS conventionally lays scalar I/O ports out
/// starting at +0x10 after the `ap_ctrl` control set (+0x00..+0x0F).
/// If the kernel signature changes, confirm against the generated
/// `pdi_kernel_hw.h` / `xpdi_kernel_hw.h` driver header and update.
constexpr std::uint32_t kKernelOutOffset = 0x10u;

/// Magic written by `hls_a/pdi_kernel.cpp`.
constexpr std::uint32_t kMagicA = 0xAAAA'AAAAu;
/// Magic written by `hls_b/pdi_kernel.cpp`.
constexpr std::uint32_t kMagicB = 0xBBBB'BBBBu;

/// Sentinel signal slots.  Slots 0/1 capture kernel A/B output; slot 2
/// is the "graph done" sentinel; slot 31 holds the LOOP's exit-condition
/// sentinel value (we want condition_op=EQ vs 0xFFFFFFFF to never match
/// so the loop exits via `max_iterations` instead).
constexpr std::uint32_t kSlotA           = 0u;
constexpr std::uint32_t kSlotB           = 1u;
constexpr std::uint32_t kSlotGraphDone   = 2u;
constexpr std::uint32_t kSlotLoopSentry  = 31u;
constexpr std::uint32_t kGraphDoneMagic  = 0xD1A1'D0DDu;

/// Default loop body iterations.  Each iteration performs two partial
/// reconfigurations (A then B), two kernel dispatches, two scalar
/// reads, and a RERUN.  CQ delta = 1 (init SIGNAL) + iterations * 7
/// (body) + 1 (LOOP exit) + 1 (sentinel SIGNAL).
constexpr std::uint32_t kDefaultIterations = 4u;

/// Submission timeout.  Generous because each iteration includes two
/// PDI reloads, which take milliseconds even on real silicon.
constexpr std::chrono::milliseconds kSubmitTimeout{30'000};

namespace {

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct Cli {
    std::string bdf;
    std::string vbin_a = "rp1_pdi_loop_a_hw.vbin";
    std::string vbin_b = "rp1_pdi_loop_b_hw.vbin";
    bool        program    = true;
    std::uint32_t iterations = kDefaultIterations;
};

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " --bdf <PCI_BDF> [options]\n"
        << "\n"
        << "Demonstrates RP1 PDI_LOAD by alternating two partial PDIs in a\n"
        << "loop on the same R5 kernel address (0x" << std::hex << kKernelR5
        << std::dec << ").\n"
        << "\n"
        << "Required:\n"
        << "  --bdf <PCI_BDF>          Target V80 PCI BDF (e.g. 0000:65:00.0).\n"
        << "\n"
        << "Optional:\n"
        << "  --vbin-a <path>          Path to vbin variant A "
        << "(default: rp1_pdi_loop_a_hw.vbin in cwd)\n"
        << "  --vbin-b <path>          Path to vbin variant B "
        << "(default: rp1_pdi_loop_b_hw.vbin in cwd)\n"
        << "  --no-program             Skip programming the user region with vbin A\n"
        << "                           (assume the board is already in the right state).\n"
        << "  --iterations N           Loop body iterations (default "
        << kDefaultIterations << ").  Each iteration performs two\n"
        << "                           partial reconfigurations.\n"
        << "  --help, -h               Show this help.\n";
}

Cli parseArgs(int argc, char** argv) {
    Cli c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) {
                throw std::runtime_error(std::string("missing argument to ") + flag);
            }
            return argv[i];
        };
        if      (a == "--bdf")          c.bdf        = need("--bdf");
        else if (a == "--vbin-a")       c.vbin_a     = need("--vbin-a");
        else if (a == "--vbin-b")       c.vbin_b     = need("--vbin-b");
        else if (a == "--no-program")   c.program    = false;
        else if (a == "--iterations")   c.iterations = static_cast<std::uint32_t>(
                                                          std::stoul(need("--iterations")));
        else if (a == "--help" || a == "-h") { printUsage(argv[0]); std::exit(0); }
        else throw std::runtime_error("unknown arg: " + a);
    }
    if (c.bdf.empty()) {
        printUsage(argv[0]);
        throw std::runtime_error("--bdf is required");
    }
    if (c.iterations == 0u) {
        throw std::runtime_error("--iterations must be >= 1");
    }
    return c;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        throw std::runtime_error("cannot open PDI: " + path);
    }
    const auto end = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!f.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("short read on PDI: " + path);
    }
    return bytes;
}

/**
 * @brief Allocate a DDR buffer sized to the PDI byte count, copy the
 *        bytes in, and sync to the device.  Returns the buffer (move-
 *        only; caller keeps it alive for the lifetime of the loop) and
 *        the 64-bit DDR physical address RP1 will hand to the PMC.
 */
struct StagedPdi {
    vrt::Buffer<std::uint8_t> buffer;
    std::uint64_t             phys_addr;
};

StagedPdi stagePdi(vrt::Device& device, const std::vector<std::uint8_t>& bytes,
                   const char* label) {
    vrt::Buffer<std::uint8_t> buf(device, bytes.size(), vrt::MemoryRangeType::DDR);
    std::memcpy(buf.get(), bytes.data(), bytes.size());
    buf.sync(vrt::SyncType::HOST_TO_DEVICE);
    const std::uint64_t pa = buf.getPhysAddr();
    std::cout << "[rp1_pdi_loop] staged " << label << " partial PDI: "
              << bytes.size() << " bytes @ DDR 0x" << std::hex << pa
              << std::dec << std::endl;
    return StagedPdi{std::move(buf), pa};
}

// ---------------------------------------------------------------------------
// Hand-packed node builders.
// ---------------------------------------------------------------------------

void setHeader(rp1_node_t& n, std::uint16_t opcode,
               std::uint8_t aw_b, std::uint32_t aw_m,
               std::uint8_t st_b, std::uint32_t st_m,
               std::uint16_t flags = 0u) {
    std::memset(&n, 0, sizeof(n));
    n.opcode               = opcode;
    n.flags                = flags;
    n.barrier_await_mask   = aw_m;
    n.barrier_set_mask     = st_m;
    n.barrier_await_bucket = aw_b;
    n.barrier_set_bucket   = st_b;
    n.status               = RP1_NODE_PENDING;
}

rp1_node_t makeSignal(std::uint32_t slot, std::uint32_t value, std::uint16_t op,
                      std::uint8_t aw_b, std::uint32_t aw_m,
                      std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_SIGNAL, aw_b, aw_m, st_b, st_m);
    n.payload.signal.target_slot = slot;
    n.payload.signal.value       = value;
    n.payload.signal.operation   = op;
    return n;
}

rp1_node_t makeLoop(std::uint32_t body_start, std::uint32_t body_end,
                    std::uint32_t max_iterations,
                    std::uint32_t condition_signal, std::uint16_t condition_op,
                    std::uint32_t condition_value,
                    std::uint8_t bucket_clear_start, std::uint8_t bucket_clear_end,
                    std::uint8_t loop_id,
                    std::uint8_t aw_b, std::uint32_t aw_m,
                    std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_LOOP, aw_b, aw_m, st_b, st_m);
    n.payload.loop.body_start         = body_start;
    n.payload.loop.body_end           = body_end;
    n.payload.loop.max_iterations     = max_iterations;
    n.payload.loop.condition_signal   = condition_signal;
    n.payload.loop.condition_value    = condition_value;
    n.payload.loop.condition_op       = condition_op;
    n.payload.loop.bucket_clear_start = bucket_clear_start;
    n.payload.loop.bucket_clear_end   = bucket_clear_end;
    n.payload.loop.loop_id            = loop_id;
    return n;
}

rp1_node_t makePdiLoad(std::uint64_t phys_addr,
                       std::uint8_t aw_b, std::uint32_t aw_m,
                       std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_PDI_LOAD, aw_b, aw_m, st_b, st_m);
    n.payload.pdi_load.pdi_addr_lo    = static_cast<std::uint32_t>(phys_addr & 0xFFFFFFFFull);
    n.payload.pdi_load.pdi_addr_hi    = static_cast<std::uint32_t>(phys_addr >> 32);
    n.payload.pdi_load.timeout_cycles = 0u;
    return n;
}

rp1_node_t makeKernelDispatch(std::uint32_t kernel_r5,
                              std::uint8_t aw_b, std::uint32_t aw_m,
                              std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_KERNEL_DISPATCH, aw_b, aw_m, st_b, st_m);
    n.payload.kernel_dispatch.kernel_base_addr  = kernel_r5;
    n.payload.kernel_dispatch.arg_buffer_offset = 0u;
    n.payload.kernel_dispatch.arg_count         = 0u;
    n.payload.kernel_dispatch.ctrl_flags        = 0u;
    n.payload.kernel_dispatch.timeout_cycles    = 0u;
    return n;
}

rp1_node_t makeScalarRead(std::uint32_t source_addr, std::uint32_t target_slot,
                          std::uint8_t aw_b, std::uint32_t aw_m,
                          std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_SCALAR_READ, aw_b, aw_m, st_b, st_m);
    n.payload.scalar_read.source_addr = source_addr;
    n.payload.scalar_read.target_slot = target_slot;
    return n;
}

rp1_node_t makeRerun(std::uint32_t target_node,
                     std::uint8_t aw_b, std::uint32_t aw_m,
                     std::uint8_t st_b, std::uint32_t st_m) {
    rp1_node_t n;
    setHeader(n, RP1_OP_RERUN, aw_b, aw_m, st_b, st_m);
    n.payload.rerun.target_node = target_node;
    n.payload.rerun.rerun_flags = 0u;
    n.payload.rerun.loop_id     = 0u;
    return n;
}

// ---------------------------------------------------------------------------
// Build the 10-node loop graph.
//
// Node 0  SIGNAL slot 31 := 0          await=(0,0x00)  set=(0,0x01)
// Node 1  LOOP body=[2..8] max_iter=N  await=(0,0x01)  set-on-exit=(0,0x02)
// Node 2  PDI_LOAD(physA)              await=(1,0x00)  set=(1,0x01)
// Node 3  KERNEL_DISPATCH @ kKernelR5  await=(1,0x01)  set=(1,0x02)
// Node 4  SCALAR_READ +0x10 -> slot 0  await=(1,0x02)  set=(1,0x04)
// Node 5  PDI_LOAD(physB)              await=(1,0x04)  set=(1,0x08)
// Node 6  KERNEL_DISPATCH @ kKernelR5  await=(1,0x08)  set=(1,0x10)
// Node 7  SCALAR_READ +0x10 -> slot 1  await=(1,0x10)  set=(1,0x20)
// Node 8  RERUN target=1               await=(1,0x20)  set=(1,0x40)
// Node 9  SIGNAL slot 2 := kGraphDoneMagic  await=(0,0x02)  set=(0,0x04)
//
// Bucket 0 carries the init->LOOP and LOOP->sentinel edges (must not be
// cleared by the LOOP).  Bucket 1 carries the body bits and is cleared
// each iteration by the LOOP (bucket_clear=[1..1]).  Body includes the
// RERUN at node 8 so its DONE state is reset each iteration along with
// the rest of the body (see linker/resources/aved/rp1/src/rp1_loop.c
// lines 289-316 for the LOOP semantics that this layout relies on).
// ---------------------------------------------------------------------------

constexpr std::size_t kNodeCount  = 10u;
constexpr std::uint32_t kLoopNode = 1u;

std::vector<rp1_node_t> buildGraph(std::uint64_t physA, std::uint64_t physB,
                                   std::uint32_t iterations) {
    std::vector<rp1_node_t> nodes(kNodeCount);

    // Node 0: zero slot 31 so the loop's never-match exit condition stays
    // strictly never-match for the whole run.
    nodes[0] = makeSignal(kSlotLoopSentry, 0u, RP1_SIGOP_SET,
                          /* await */ 0, 0x00,
                          /* set   */ 0, 0x01);

    // Node 1: the loop header.  body=[2..8] (inclusive) so the RERUN is
    // reset to PENDING along with the rest of the body each iteration.
    nodes[1] = makeLoop(/* body */ 2u, 8u,
                        /* max_iterations */ iterations,
                        /* condition_signal */ kSlotLoopSentry,
                        /* condition_op    */ static_cast<std::uint16_t>(RP1_COP_EQ),
                        /* condition_value */ 0xFFFFFFFFu,
                        /* bucket_clear    */ 1u, 1u,
                        /* loop_id         */ 0u,
                        /* await */ 0, 0x01,
                        /* set-on-exit */ 0, 0x02);

    // Body: PDI_LOAD A -> KD -> SR -> PDI_LOAD B -> KD -> SR -> RERUN.
    nodes[2] = makePdiLoad(physA, /* await */ 1, 0x00, /* set */ 1, 0x01);
    nodes[3] = makeKernelDispatch(kKernelR5,
                                  /* await */ 1, 0x01, /* set */ 1, 0x02);
    nodes[4] = makeScalarRead(kKernelR5 + kKernelOutOffset, kSlotA,
                              /* await */ 1, 0x02, /* set */ 1, 0x04);
    nodes[5] = makePdiLoad(physB, /* await */ 1, 0x04, /* set */ 1, 0x08);
    nodes[6] = makeKernelDispatch(kKernelR5,
                                  /* await */ 1, 0x08, /* set */ 1, 0x10);
    nodes[7] = makeScalarRead(kKernelR5 + kKernelOutOffset, kSlotB,
                              /* await */ 1, 0x10, /* set */ 1, 0x20);
    nodes[8] = makeRerun(/* target */ kLoopNode,
                         /* await */ 1, 0x20, /* set */ 1, 0x40);

    // Node 9: sentinel fires after LOOP exits.
    nodes[9] = makeSignal(kSlotGraphDone, kGraphDoneMagic, RP1_SIGOP_SET,
                          /* await */ 0, 0x02,
                          /* set   */ 0, 0x04);

    return nodes;
}

const char* stateName(std::uint32_t s) {
    switch (s) {
    case RP1_STATE_INIT:    return "INIT";
    case RP1_STATE_READY:   return "READY";
    case RP1_STATE_RUNNING: return "RUNNING";
    case RP1_STATE_ERROR:   return "ERROR";
    case RP1_STATE_HALTED:  return "HALTED";
    default:                return "?";
    }
}

void dumpCtrl(Rp1BarWindow& win) {
    rp1_ctrl_t c{};
    win.readCtrl(c);
    std::cerr << "  rp1_state        = " << c.rp1_state
              << " (" << stateName(c.rp1_state) << ")\n"
              << "  rp1_error_code   = " << c.rp1_error_code << "\n"
              << "  rp1_current_node = " << c.rp1_current_node << "\n"
              << "  cq_write_idx     = " << c.cq_write_idx << "\n"
              << "  graph_seq        = " << c.graph_seq << "\n"
              << "  graph_done_seq   = " << c.graph_done_seq << "\n"
              << "  heartbeat        = " << c.heartbeat << std::endl;
}

}  // namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    // 1. Extract both vbins' partial PDIs to in-memory byte buffers
    //    BEFORE constructing vrt::Device.  Vrtbin uses a BDF-keyed
    //    cache directory and the second construction clobbers the
    //    first's extracted files (see vrt/src/vrtbin.cpp::Vrtbin and
    //    tempExtractPath wiring), so we must read each into memory
    //    immediately.
    std::cout << "[rp1_pdi_loop] extracting partial PDI from "
              << cli.vbin_a << " ..." << std::endl;
    std::vector<std::uint8_t> bytes_a;
    {
        vrt::Vrtbin vb(cli.vbin_a, cli.bdf);
        bytes_a = readFileBytes(vb.getPdiPath());
    }
    std::cout << "[rp1_pdi_loop] extracting partial PDI from "
              << cli.vbin_b << " ..." << std::endl;
    std::vector<std::uint8_t> bytes_b;
    {
        vrt::Vrtbin vb(cli.vbin_b, cli.bdf);
        bytes_b = readFileBytes(vb.getPdiPath());
    }
    std::cout << "[rp1_pdi_loop] PDI sizes: A=" << bytes_a.size()
              << " B=" << bytes_b.size() << " bytes" << std::endl;

    // 2. Open libvrt and (optionally) program the user region with vbin A.
    //    With --no-program the user is expected to have already loaded
    //    the right partial onto the board (e.g. via v80-smi program).
    std::cout << "[rp1_pdi_loop] opening device " << cli.bdf
              << " (program=" << (cli.program ? "yes" : "no") << ") ..."
              << std::endl;
    vrt::Device device(cli.bdf, cli.vbin_a, cli.program);

    // 3. Stage both partial PDIs in DDR via QDMA H2C.
    StagedPdi staged_a = stagePdi(device, bytes_a, "A");
    StagedPdi staged_b = stagePdi(device, bytes_b, "B");

    // 4. Open BAR4 and wrap it in an Rp1BarWindow + Rp1Submitter.
    vrtd::Device&  vrtd_dev = device.getHandle()->getVrtdDevice();
    vrtd::Bar      bar4     = vrtd_dev.getBar(4);
    vrtd::BarFile  bar_file = bar4.openBarFile();
    Rp1BarWindow   window(std::move(bar_file));
    Rp1Submitter   submitter(window);

    // 5. Verify firmware liveness before we commit a multi-second graph.
    //    Rp1Submitter::submitAndWait() also calls ensureReady() but the
    //    explicit call gives the user a clearer error if rp1.elf isn't
    //    loaded onto R5-1 yet.
    std::cout << "[rp1_pdi_loop] waiting for RP1 firmware to be READY..."
              << std::endl;
    try {
        submitter.ensureReady();
    } catch (const Rp1TimeoutError& e) {
        std::cerr << "[rp1_pdi_loop] firmware not ready: " << e.what() << "\n"
                  << "  -- make sure rp1.elf has been loaded onto R5-1 via xsdb\n"
                  << "     (see linker/resources/aved/rp1/README -- not yet checked in)\n"
                  << "  -- inspect /dev/slash_ctl0 with `rp1_bringup dump` for context\n";
        dumpCtrl(window);
        return 2;
    }

    // 6. Build the graph image and submit.
    Rp1GraphImage image;
    image.nodes = buildGraph(staged_a.phys_addr, staged_b.phys_addr,
                             cli.iterations);
    image.clear_signal_slots = { kSlotA, kSlotB, kSlotGraphDone, kSlotLoopSentry };
    // arg_buf intentionally empty: every KERNEL_DISPATCH has arg_count=0.

    std::cout << "[rp1_pdi_loop] submitting "
              << kNodeCount << "-node graph (iterations="
              << cli.iterations << ", physA=0x" << std::hex
              << staged_a.phys_addr << ", physB=0x" << staged_b.phys_addr
              << std::dec << "), polling..." << std::endl;

    try {
        submitter.submitAndWait(image, kSubmitTimeout);
    } catch (const Rp1TimeoutError& e) {
        std::cerr << "[rp1_pdi_loop] TIMEOUT: " << e.what() << std::endl;
        dumpCtrl(window);
        return 1;
    } catch (const std::runtime_error& e) {
        std::cerr << "[rp1_pdi_loop] ERROR: " << e.what() << std::endl;
        dumpCtrl(window);
        return 1;
    }

    // 7. Verify slots and CQ delta.
    rp1_signal_slot_t s_a{}, s_b{}, s_done{};
    window.readSignal(kSlotA,         s_a);
    window.readSignal(kSlotB,         s_b);
    window.readSignal(kSlotGraphDone, s_done);

    const std::uint32_t cq_start = submitter.lastCqStart();
    const std::uint32_t cq_end   = window.readCqWriteIdx();
    const std::uint32_t cq_delta = cq_end - cq_start;

    // CQ accounting (see rp1_loop.c::case RP1_OP_LOOP at lines 289-316):
    //   init SIGNAL                                                       1
    //   body iterations * (PDI+KD+SR + PDI+KD+SR + RERUN = 7)             N*7
    //   LOOP exit firing (writes CQ on exit only)                          1
    //   sentinel SIGNAL                                                    1
    const std::uint32_t expected_cq = 1u + cli.iterations * 7u + 1u + 1u;

    std::cout << "[rp1_pdi_loop] slot[" << kSlotA << "] (kernel A) = 0x"
              << std::hex << s_a.value << "  (expect 0x" << kMagicA << ")\n"
              << "[rp1_pdi_loop] slot[" << kSlotB << "] (kernel B) = 0x"
              << s_b.value << "  (expect 0x" << kMagicB << ")\n"
              << "[rp1_pdi_loop] slot[" << kSlotGraphDone
              << "] (sentinel)  = 0x" << s_done.value
              << "  (expect 0x" << kGraphDoneMagic << ")\n"
              << "[rp1_pdi_loop] cq_delta = " << std::dec << cq_delta
              << "  (expect " << expected_cq << ")" << std::endl;

    bool ok = true;
    if (s_a.value    != kMagicA)         { ok = false; std::cerr << "FAIL: slot A magic\n"; }
    if (s_b.value    != kMagicB)         { ok = false; std::cerr << "FAIL: slot B magic\n"; }
    if (s_done.value != kGraphDoneMagic) { ok = false; std::cerr << "FAIL: sentinel\n"; }
    if (cq_delta     != expected_cq)     { ok = false; std::cerr << "FAIL: cq_delta\n"; }

    if (!ok) {
        dumpCtrl(window);
        return 1;
    }

    std::cout << "PASS: PDI_LOAD loop ran "
              << cli.iterations << " body iterations ("
              << (cli.iterations * 2u) << " partial reconfigurations) cleanly."
              << std::endl;
    return 0;
} catch (const std::exception& e) {
    std::cerr << "rp1_pdi_loop: " << e.what() << std::endl;
    return 1;
}
