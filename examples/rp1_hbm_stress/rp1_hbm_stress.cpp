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
 * rp1_hbm_stress -- hand-author a 69-node RP1 graph that dispatches 64
 * hbm_bandwidth instances in two waves of 32 (the firmware's exact
 * RP1_MAX_INFLIGHT cap) inside an N-iteration loop and verifies
 * completion via a sentinel SIGNAL plus a SCALAR_READ of kernel_0's
 * out_acc register.
 *
 *      ┌──────── loop body, N iterations ────────┐
 * init ─> LOOP ─> 32x wave A ─> 32x wave B ─> RERUN ─> (loop)
 *           │
 *           └──exit──> sentinel SIGNAL ─> SCALAR_READ
 *
 * Bucket plan (all kernel completion bits live in scratch buckets so the
 * LOOP can clear them per iteration):
 *
 *   Bucket 0 (lifecycle, never cleared):
 *     bit 0  -- init done
 *     bit 1  -- LOOP exit
 *     bit 2  -- sentinel done
 *     bit 3  -- SCALAR_READ done
 *   Bucket 2 (wave-A completion, cleared per iter by LOOP):
 *     bits 0..31 -- one per wave-A kernel
 *   Bucket 3 (wave-B completion, cleared per iter by LOOP):
 *     bits 0..31 -- one per wave-B kernel
 *   Bucket 4 (RERUN done -- accumulates harmlessly, nobody awaits it)
 *
 * Hand-authoring (rather than VRT's FpgaDevice) lets us use all 32 bits
 * of bucket 2/3 instead of paying the 31-kernel-per-graph cap that
 * FpgaDevice phase-1 enforces (bit 31 reserved for a sentinel).
 *
 * Pre-reqs (same as examples/rp1_bringup and examples/rp1_pipeline):
 *   - Bitstream with PF2 BAR4 mapping the RP1 64 MiB DDR aperture at
 *     host offset 64 MiB.
 *   - 64 hbm_bandwidth instances at R5 0x88000000 + n*0x10000.
 *   - rp1.elf loaded onto R5-1 with -DRP1_POLLING_BRINGUP=ON.
 *   - HBM mapped contiguously for 32 GiB starting at the wired-in
 *     base address (kHbmBase).
 *
 * Usage:
 *   rp1_hbm_stress /dev/slash_ctl0 [--wr 0|1] [--iters N] [--timeout-ms MS]
 *
 * The HBM base address and CQ size are not exposed: the HBM aperture is
 * wired into the bitstream at 0x40_0000_0000 (any other value faults),
 * and the CQ size is mechanically derived from --iters.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <slash/ctldev.h>
#include <slash/uapi/rp1_protocol.h>
#include <slash/uapi/slash_interface.h>
}

// ---------------------------------------------------------------------------
// Bring-up constants. These match every other RP1 example in this tree
// (examples/rp1_bringup, examples/rp1_pipeline, examples/rp1_bringup_gpu).
// Override at the source level if your bitstream differs.
// ---------------------------------------------------------------------------

namespace {

constexpr int      kBarNumber       = 4;
constexpr uint64_t kBarCtrlOffset   = 64ULL * 1024ULL * 1024ULL;

// 64 instances of hbm_bandwidth at AXI-Lite offsets 0, 0x10000, ..., 0x3F0000.
// R5-side they sit at 0x88000000 + n*0x10000 (standard
// r5_addr = xml_addr - 0x202_0000_0000 + 0x88000000 translation).
constexpr uint32_t kKernelCount    = 64u;
constexpr uint32_t kWaveSize       = 32u;
constexpr uint32_t kKernelBaseR5   = 0x88000000u;
constexpr uint32_t kKernelStrideR5 = 0x00010000u;

// AXI-Lite register layout of hbm_bandwidth, from the Vitis HLS header:
//   0x10 : hbm_ptr[31:0]
//   0x14 : hbm_ptr[63:32]
//   0x18 : reserved (skipped by HLS, writing 0 is harmless)
//   0x1c : wr (1 = read+XOR mode, 0 = write i->hbm_ptr[i])
//   0x24 : out_acc (read-only, populated when wr==1)
// The firmware writes kernel_base + 0x10 + i*4 for i in [0, arg_count),
// matching this layout exactly with arg_count=4.
constexpr uint32_t kKernelOutAcc   = 0x24u;
constexpr uint32_t kKernelArgWords = 4u;

constexpr uint32_t kSentinelMagic  = 0xC0FFEE01u;

// Signal slots
constexpr uint32_t kSlotLoopCond = 0u;  // LOOP condition slot -- always 0
constexpr uint32_t kSlotSentinel = 1u;  // sentinel slot
constexpr uint32_t kSlotOutAcc   = 2u;  // kernel_0 out_acc readback

// Barrier buckets
constexpr uint8_t  kBucketLifecycle = 0u;
constexpr uint8_t  kBucketWaveA     = 2u;
constexpr uint8_t  kBucketWaveB     = 3u;
constexpr uint8_t  kBucketRerun     = 4u;

// Lifecycle bits in bucket 0
constexpr uint32_t kBitInit     = 1u << 0;
constexpr uint32_t kBitLoopExit = 1u << 1;
constexpr uint32_t kBitSentinel = 1u << 2;
constexpr uint32_t kBitScalarRd = 1u << 3;

// Node indices (kept explicit so the bucket plan is auditable)
constexpr uint32_t kNodeInit       = 0u;
constexpr uint32_t kNodeLoop       = 1u;
constexpr uint32_t kNodeWaveABase  = 2u;
constexpr uint32_t kNodeWaveBBase  = 34u;
constexpr uint32_t kNodeRerun      = 66u;
constexpr uint32_t kNodeSentinel   = 67u;
constexpr uint32_t kNodeScalarRead = 68u;
constexpr uint32_t kNodeCount      = 69u;

// CQ counting: 1 init + N*(32+32+1) body + 1 LOOP-exit + 1 sentinel + 1 SCALAR_READ.
constexpr uint32_t kCqPerIter = kWaveSize + kWaveSize + 1u;  // 65
constexpr uint32_t kCqFixed   = 1u + 1u + 1u + 1u;            // init + exit + sentinel + scalar_read

// Defaults
// HBM base address is HARD-WIRED in the bitstream: each kernel's m_axi_gmem0
// must address HBM starting at 0x40_0000_0000 + n * 512 MiB.  This is a
// constexpr, not a CLI flag -- any other value faults or stalls on real
// hardware; edit here if you re-wire the bitstream's HBM aperture.
constexpr uint64_t kHbmBase            = 0x4000000000ULL;  // 0x40_0000_0000
constexpr uint64_t kHbmSliceSize       = 0x20000000ULL;    // 512 MiB per kernel
constexpr uint32_t kDefaultIters       = 5u;
constexpr uint32_t kDefaultWr          = 1u;
constexpr int      kDefaultTimeoutMs   = 30000;
constexpr int      kPollIntervalMs     = 1;

}  // namespace

// ---------------------------------------------------------------------------
// CLI parsing
// ---------------------------------------------------------------------------

struct Cli {
    std::string ctlDev;
    uint32_t    wr        = kDefaultWr;
    uint32_t    iters     = kDefaultIters;
    int         timeoutMs = kDefaultTimeoutMs;
};

/* Smallest power of two >= v.  Used to mechanically derive cq_size
 * from the expected CQ entry count (iters * 65 + 4). */
static uint32_t nextPow2(uint32_t v) {
    uint32_t p = 1u;
    while (p < v) p <<= 1;
    return p;
}

static void printUsage(const char* argv0) {
    std::cerr <<
        "Usage: " << argv0 << " <slash_ctl_dev>\n"
        "  [--wr 0|1]          hbm_bandwidth mode for all 64 kernels (default 1 = read+XOR).\n"
        "  [--iters N]         outer loop iterations (default 5).\n"
        "  [--timeout-ms MS]   submission timeout in ms (default 30000).\n"
        "\n"
        "HBM base address is wired into the bitstream at 0x40_0000_0000 (kHbmBase\n"
        "in the source). CQ size is derived from --iters automatically.\n"
        "\n"
        "Example: " << argv0 << " /dev/slash_ctl0 --wr 1 --iters 5\n";
}

static Cli parseArgs(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        std::exit(1);
    }
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        printUsage(argv[0]);
        std::exit(0);
    }
    Cli c;
    c.ctlDev = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) {
                throw std::runtime_error(std::string("missing argument to ") + flag);
            }
            return argv[i];
        };
        if      (a == "--wr")          c.wr        = static_cast<uint32_t>(std::stoul(need("--wr")));
        else if (a == "--iters")       c.iters     = static_cast<uint32_t>(std::stoul(need("--iters")));
        else if (a == "--timeout-ms")  c.timeoutMs = std::stoi(need("--timeout-ms"));
        else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown arg: " + a);
        }
    }
    if (c.wr > 1u)         throw std::runtime_error("--wr must be 0 or 1");
    if (c.iters == 0u)     throw std::runtime_error("--iters must be >= 1");
    return c;
}

// ---------------------------------------------------------------------------
// Raw BAR4 access through libslash.  Mirrors the BarMap helper from
// examples/rp1_pipeline/rp1_pipeline.cpp -- bracket the whole submission
// in a single start_write/end_write so the dma-buf sync contract is met.
// ---------------------------------------------------------------------------

struct BarMap {
    slash_ctldev*         dev  = nullptr;
    slash_ioctl_bar_info* info = nullptr;
    slash_bar_file*       file = nullptr;
    volatile uint8_t*     base = nullptr;

    explicit BarMap(const std::string& slashCtlPath) {
        dev = slash_ctldev_open(slashCtlPath.c_str());
        if (!dev) {
            throw std::runtime_error("slash_ctldev_open(" + slashCtlPath + ") failed");
        }
        info = slash_bar_info_read(dev, kBarNumber);
        if (!info || !info->usable) {
            cleanup();
            throw std::runtime_error(
                "BAR" + std::to_string(kBarNumber) + " not usable (check `dmesg | grep slash`)");
        }
        file = slash_bar_file_open(dev, kBarNumber, O_CLOEXEC);
        if (!file) {
            cleanup();
            throw std::runtime_error(
                "slash_bar_file_open(BAR" + std::to_string(kBarNumber) + ") failed");
        }
        if (slash_bar_file_start_write(file) != 0) {
            cleanup();
            throw std::runtime_error("slash_bar_file_start_write failed");
        }
        base = static_cast<volatile uint8_t*>(file->map);
    }

    ~BarMap() { cleanup(); }
    BarMap(const BarMap&) = delete;
    BarMap& operator=(const BarMap&) = delete;

    void cleanup() {
        if (file) { slash_bar_file_end_write(file); slash_bar_file_close(file); file = nullptr; }
        if (info) { slash_bar_info_free(info); info = nullptr; }
        if (dev)  { slash_ctldev_close(dev);   dev = nullptr; }
        base = nullptr;
    }

    /* Translate an R5 absolute address (in the BAR-visible DDR window)
     * into a pointer in our local BAR mapping. */
    template <typename T>
    volatile T* at(uint64_t r5_addr) {
        const uint64_t bar_off = kBarCtrlOffset + (r5_addr - RP1_CTRL_PHYS_ADDR);
        return reinterpret_cast<volatile T*>(base + bar_off);
    }
};

// ---------------------------------------------------------------------------
// Node + ctrl helpers
// ---------------------------------------------------------------------------

static void setHeader(volatile rp1_node_t* n, uint16_t opcode,
                      uint8_t aw_b, uint32_t aw_m,
                      uint8_t st_b, uint32_t st_m) {
    n->opcode               = opcode;
    n->flags                = 0;
    n->barrier_await_mask   = aw_m;
    n->barrier_set_mask     = st_m;
    n->barrier_await_bucket = aw_b;
    n->barrier_set_bucket   = st_b;
    n->status               = RP1_NODE_PENDING;
}

static void programCtrl(volatile rp1_ctrl_t* c, uint32_t nodeCount, uint32_t cqSize) {
    c->node_count        = nodeCount;
    c->cq_size           = cqSize;
    c->node_base_lo      = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    c->node_base_hi      = 0;
    c->cq_base_lo        = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_CQ_OFFSET);
    c->cq_base_hi        = 0;
    c->arg_buf_base_lo   = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    c->arg_buf_base_hi   = 0;
    c->sig_array_base_lo = static_cast<uint32_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);
    c->sig_array_base_hi = 0;
}

/* Volatile-aware byte zero. */
static void barZero(volatile void* dst, size_t bytes) {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(dst);
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}

static const char* stateStr(uint32_t s) {
    switch (s) {
        case RP1_STATE_INIT:    return "INIT";
        case RP1_STATE_READY:   return "READY";
        case RP1_STATE_RUNNING: return "RUNNING";
        case RP1_STATE_ERROR:   return "ERROR";
        case RP1_STATE_HALTED:  return "HALTED";
        default:                return "?";
    }
}

// ---------------------------------------------------------------------------
// Stall-point diagnostics.  Maps (cq_delta, rp1_current_node) into where in
// the graph the firmware got stuck.  rp1_current_node is updated whenever
// the scanner processes a node (activate_nodes in rp1_loop.c), so it
// distinguishes "wave A never dispatched" from "wave A dispatched, but no
// ap_done yet" -- which look identical from cq_delta alone since
// KERNEL_DISPATCH only writes a CQ entry on ap_done, not on dispatch.
// ---------------------------------------------------------------------------

static std::string stallPoint(uint32_t cqDelta, uint32_t iters, uint32_t currentNode) {
    const uint32_t expected = iters * kCqPerIter + kCqFixed;
    if (cqDelta == expected) return "complete (no stall)";
    if (cqDelta == 0) {
        return "init SIGNAL never fired (firmware not picking up graph_seq?)";
    }

    const uint32_t body = cqDelta - 1;
    const uint32_t fullIters = body / kCqPerIter;
    const uint32_t rem       = body % kCqPerIter;

    const bool inWaveA   = (currentNode >= kNodeWaveABase && currentNode < kNodeWaveBBase);
    const bool inWaveB   = (currentNode >= kNodeWaveBBase && currentNode <= kNodeWaveBBase + kWaveSize - 1);
    const bool atOrBeforeLoop = (currentNode <= kNodeLoop);

    auto describeKernels = [&](const char* waveName, uint32_t done, bool dispatched) {
        std::ostringstream s;
        s << " " << waveName << ": " << done << "/" << kWaveSize << " kernels reported ap_done, "
          << (kWaveSize - done) << (dispatched
              ? " still in flight (no ap_done received -- check kernel R5 addresses + HBM access)"
              : " not yet dispatched (LOOP barrier or scanner issue?)");
        return s.str();
    };

    std::ostringstream oss;
    if (fullIters < iters) {
        oss << "in iter " << (fullIters + 1);
        if (rem < kWaveSize) {
            // Either wave-A hasn't been dispatched yet, or it has been but
            // some kernels haven't completed.  rp1_current_node disambiguates.
            const bool waveADispatched = inWaveA || inWaveB
                                          || (rem > 0)   // any wave-A CQ entry means dispatch happened
                                          || (currentNode > kNodeWaveBBase + kWaveSize - 1);
            oss << describeKernels("wave A", rem, waveADispatched);
            if (atOrBeforeLoop && rem == 0) {
                oss << " (current node = " << currentNode << ", expected scanner past LOOP)";
            } else if (currentNode != 0) {
                oss << " [rp1_current_node = " << currentNode << "]";
            }
        } else if (rem < 2u * kWaveSize) {
            const uint32_t bDone = rem - kWaveSize;
            const bool waveBDispatched = inWaveB
                                          || (bDone > 0)
                                          || (currentNode > kNodeWaveBBase + kWaveSize - 1);
            oss << describeKernels("wave B", bDone, waveBDispatched);
            if (currentNode != 0) {
                oss << " [rp1_current_node = " << currentNode << "]";
            }
        } else {
            // rem == 2*kWaveSize == 64 -- both waves done, RERUN didn't fire
            oss << " RERUN never fired (impossible unless barrier wiring is wrong)";
        }
    } else {
        // fullIters >= iters -- loop body finished
        if (cqDelta == iters * kCqPerIter + 1u) {
            oss << "all " << iters << " iters done, LOOP-exit never fired";
        } else if (cqDelta == iters * kCqPerIter + 2u) {
            oss << "LOOP exited, sentinel SIGNAL never fired";
        } else if (cqDelta == iters * kCqPerIter + 3u) {
            oss << "sentinel done, SCALAR_READ on kernel_0 never fired";
        } else {
            oss << "cq_delta=" << cqDelta << " in unexpected range (expect " << expected << ")";
        }
    }
    return oss.str();
}

static void dumpCtrl(volatile rp1_ctrl_t* c) {
    std::cerr << std::hex << std::setfill('0')
              << "  magic            = 0x" << std::setw(8) << c->magic
              << " (" << (c->magic == RP1_CTRL_MAGIC ? "SQR1" : "BAD") << ")\n"
              << std::dec << std::setfill(' ')
              << "  version          = " << c->version << "\n"
              << "  node_count       = " << c->node_count << "\n"
              << "  cq_size          = " << c->cq_size << "\n"
              << "  graph_seq        = " << c->graph_seq << "\n"
              << "  graph_done_seq   = " << c->graph_done_seq << "\n"
              << "  cq_write_idx     = " << c->cq_write_idx << "\n"
              << "  rp1_state        = " << c->rp1_state
              << " (" << stateStr(c->rp1_state) << ")\n"
              << "  rp1_error_code   = " << c->rp1_error_code << "\n"
              << "  rp1_current_node = " << c->rp1_current_node << "\n"
              << "  heartbeat        = " << c->heartbeat << "\n";
}

// ---------------------------------------------------------------------------
// Graph construction
// ---------------------------------------------------------------------------

static void stageArgs(volatile uint32_t* argbuf, uint32_t wr) {
    // Each kernel gets 4 contiguous arg words starting at byte offset
    // kKernelArgWords*4 * kernelIdx (matches the arg_buffer_offset we
    // bake into each KERNEL_DISPATCH).  No padding between slabs -- 4
    // words is already 16-byte aligned.
    for (uint32_t n = 0; n < kKernelCount; ++n) {
        const uint64_t ptr = kHbmBase + static_cast<uint64_t>(n) * kHbmSliceSize;
        const uint32_t base = n * kKernelArgWords;
        argbuf[base + 0] = static_cast<uint32_t>(ptr & 0xFFFFFFFFu);  // -> +0x10 hbm_ptr_lo
        argbuf[base + 1] = static_cast<uint32_t>(ptr >> 32);          // -> +0x14 hbm_ptr_hi
        argbuf[base + 2] = 0u;                                         // -> +0x18 reserved
        argbuf[base + 3] = wr;                                         // -> +0x1c wr
    }
}

static void buildGraph(volatile rp1_node_t* nodes, uint32_t iters) {
    // -------- Node 0: init SIGNAL (resets the loop condition slot) ----------
    // Writes slot 0 = 0.  LOOP uses slot 0 with op=NE,val=0 as its
    // condition signal; since slot 0 stays 0, the condition never fires
    // and only max_iterations terminates the loop.  The init also sets
    // bucket 0 bit 0 to gate the LOOP.
    setHeader(&nodes[kNodeInit], RP1_OP_SIGNAL,
              /* await */ kBucketLifecycle, 0x0,
              /* set   */ kBucketLifecycle, kBitInit);
    nodes[kNodeInit].payload.signal.target_slot = kSlotLoopCond;
    nodes[kNodeInit].payload.signal.value       = 0u;
    nodes[kNodeInit].payload.signal.operation   = RP1_SIGOP_SET;

    // -------- Node 1: LOOP (5 iterations over the body [2..66]) -------------
    setHeader(&nodes[kNodeLoop], RP1_OP_LOOP,
              /* await */ kBucketLifecycle, kBitInit,
              /* set   */ kBucketLifecycle, kBitLoopExit);
    auto& lp = nodes[kNodeLoop].payload.loop;
    lp.body_start         = kNodeWaveABase;
    lp.body_end           = kNodeRerun;       // inclusive: covers 2..66
    lp.max_iterations     = iters;
    lp.condition_signal   = kSlotLoopCond;
    lp.condition_op       = RP1_COP_NE;       // exit on slot != 0; slot stays 0 -> never met
    lp.condition_value    = 0u;
    lp.bucket_clear_start = kBucketWaveA;
    lp.bucket_clear_end   = kBucketWaveB;     // inclusive: clears buckets 2 and 3
    lp.loop_id            = 0u;

    // -------- Nodes 2..33: 32x KERNEL_DISPATCH (wave A, kernels 0..31) ------
    // await = 2/0x0 -- always met inside the body (bucket 2 is cleared
    // by LOOP at the start of every iteration).  set = 2/(1<<n).
    for (uint32_t n = 0; n < kWaveSize; ++n) {
        const uint32_t nodeIdx   = kNodeWaveABase + n;
        const uint32_t kernelIdx = n;
        setHeader(&nodes[nodeIdx], RP1_OP_KERNEL_DISPATCH,
                  /* await */ kBucketWaveA, 0x0,
                  /* set   */ kBucketWaveA, (1u << n));
        auto& kd = nodes[nodeIdx].payload.kernel_dispatch;
        kd.kernel_base_addr  = kKernelBaseR5 + kernelIdx * kKernelStrideR5;
        kd.arg_buffer_offset = kernelIdx * kKernelArgWords * sizeof(uint32_t);
        kd.arg_count         = static_cast<uint16_t>(kKernelArgWords);
        kd.ctrl_flags        = 0;
        kd.timeout_cycles    = 0;  // default (10M scan-pass decrements)
    }

    // -------- Nodes 34..65: 32x KERNEL_DISPATCH (wave B, kernels 32..63) ----
    // await = 2/0xFFFFFFFF -- gated on all 32 wave-A kernels completing.
    // set   = 3/(1<<n).
    for (uint32_t n = 0; n < kWaveSize; ++n) {
        const uint32_t nodeIdx   = kNodeWaveBBase + n;
        const uint32_t kernelIdx = kWaveSize + n;
        setHeader(&nodes[nodeIdx], RP1_OP_KERNEL_DISPATCH,
                  /* await */ kBucketWaveA, 0xFFFFFFFFu,
                  /* set   */ kBucketWaveB, (1u << n));
        auto& kd = nodes[nodeIdx].payload.kernel_dispatch;
        kd.kernel_base_addr  = kKernelBaseR5 + kernelIdx * kKernelStrideR5;
        kd.arg_buffer_offset = kernelIdx * kKernelArgWords * sizeof(uint32_t);
        kd.arg_count         = static_cast<uint16_t>(kKernelArgWords);
        kd.ctrl_flags        = 0;
        kd.timeout_cycles    = 0;
    }

    // -------- Node 66: RERUN target=LOOP ------------------------------------
    // Fires once all wave-B kernels are done; resets the LOOP node to
    // PENDING so it fires again on the next scan and re-evaluates the
    // exit condition / iteration count.
    setHeader(&nodes[kNodeRerun], RP1_OP_RERUN,
              /* await */ kBucketWaveB, 0xFFFFFFFFu,
              /* set   */ kBucketRerun,  0x1);
    nodes[kNodeRerun].payload.rerun.target_node = kNodeLoop;
    nodes[kNodeRerun].payload.rerun.rerun_flags = 0;
    nodes[kNodeRerun].payload.rerun.loop_id     = 0;

    // -------- Node 67: sentinel SIGNAL (gated on LOOP-exit) -----------------
    setHeader(&nodes[kNodeSentinel], RP1_OP_SIGNAL,
              /* await */ kBucketLifecycle, kBitLoopExit,
              /* set   */ kBucketLifecycle, kBitSentinel);
    nodes[kNodeSentinel].payload.signal.target_slot = kSlotSentinel;
    nodes[kNodeSentinel].payload.signal.value       = kSentinelMagic;
    nodes[kNodeSentinel].payload.signal.operation   = RP1_SIGOP_SET;

    // -------- Node 68: SCALAR_READ kernel_0 out_acc -> slot[kSlotOutAcc] ----
    setHeader(&nodes[kNodeScalarRead], RP1_OP_SCALAR_READ,
              /* await */ kBucketLifecycle, kBitSentinel,
              /* set   */ kBucketLifecycle, kBitScalarRd);
    nodes[kNodeScalarRead].payload.scalar_read.source_addr = kKernelBaseR5 + kKernelOutAcc;
    nodes[kNodeScalarRead].payload.scalar_read.target_slot = kSlotOutAcc;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    const uint32_t expectedCq = cli.iters * kCqPerIter + kCqFixed;
    const uint32_t cqSize     = nextPow2(expectedCq);

    std::cout << "rp1_hbm_stress:"
              << " ctl=" << cli.ctlDev
              << " hbm_base=0x" << std::hex << kHbmBase << std::dec
              << " wr=" << cli.wr
              << " iters=" << cli.iters
              << " cq_size=" << cqSize
              << " timeout_ms=" << cli.timeoutMs
              << "\n";

    BarMap bar(cli.ctlDev);

    auto* ctrl   = bar.at<rp1_ctrl_t>(RP1_CTRL_PHYS_ADDR);
    auto* nodes  = bar.at<rp1_node_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_NODE_ARRAY_OFFSET);
    auto* argbuf = bar.at<uint32_t>  (RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_ARG_BUF_OFFSET);
    auto* sigs   = bar.at<rp1_signal_slot_t>(RP1_CTRL_PHYS_ADDR + RP1_DEFAULT_SIG_ARRAY_OFFSET);

    // Sanity-check firmware presence + READY state before we touch nodes.
    // If the firmware is RUNNING (mid-graph) or wedged, bumping graph_seq
    // here is a no-op and we'd waste the whole --timeout-ms polling for a
    // completion that will never come.
    if (ctrl->magic != RP1_CTRL_MAGIC) {
        std::cerr << "ERROR: ctrl->magic = 0x" << std::hex << ctrl->magic
                  << " != 0x" << RP1_CTRL_MAGIC << " (\"SQR1\")\n"
                  << "  RP1 firmware isn't running. Load rp1.elf onto R5-1 via xsdb\n"
                  << "  (build with -DRP1_POLLING_BRINGUP=ON) and re-check with\n"
                  << "  `rp1_bringup dump " << cli.ctlDev << "`.\n";
        return 1;
    }
    if (ctrl->graph_seq != ctrl->graph_done_seq
        || ctrl->rp1_state != RP1_STATE_READY) {
        std::cerr << "ERROR: firmware not READY for a new submission\n"
                  << "  rp1_state        = " << ctrl->rp1_state
                  << " (" << stateStr(ctrl->rp1_state) << ")\n"
                  << "  graph_seq        = " << ctrl->graph_seq << "\n"
                  << "  graph_done_seq   = " << ctrl->graph_done_seq << "\n"
                  << "  heartbeat        = " << ctrl->heartbeat << "\n"
                  << "  rp1_current_node = " << ctrl->rp1_current_node << "\n"
                  << "  Firmware is mid-processing or wedged (likely R5 stalled\n"
                  << "  inside an AXI access that never completed). Reload\n"
                  << "  rp1.elf onto R5-1 via xsdb to reset state, then retry.\n";
        return 1;
    }

    // Stage 64 kernels' worth of args.
    stageArgs(argbuf, cli.wr);

    // Zero the node array region we'll write.
    barZero(static_cast<volatile void*>(nodes), kNodeCount * sizeof(rp1_node_t));

    // Clear the signal slots we plan to write so leftovers from a prior
    // run can't masquerade as success.
    for (uint32_t s : {kSlotLoopCond, kSlotSentinel, kSlotOutAcc}) {
        sigs[s].value            = 0;
        sigs[s].last_writer_node = 0;
        sigs[s].flags            = 0;
    }

    buildGraph(nodes, cli.iters);
    programCtrl(ctrl, kNodeCount, cqSize);

    const uint32_t priorCq = ctrl->cq_write_idx;
    const uint32_t wantSeq = ctrl->graph_done_seq + 1u;

    std::cout << "[rp1_hbm_stress] graph layout: "
              << kNodeCount << " nodes, expecting " << expectedCq
              << " CQ entries (1 init + " << cli.iters << "x" << kCqPerIter
              << " + 1 loop-exit + 1 sentinel + 1 scalar-read)\n"
              << "[rp1_hbm_stress] kernels at R5 0x" << std::hex
              << kKernelBaseR5 << "..0x"
              << (kKernelBaseR5 + (kKernelCount - 1) * kKernelStrideR5)
              << " stride 0x" << kKernelStrideR5
              << "; HBM slices 0x" << kHbmBase << "..0x"
              << (kHbmBase + kKernelCount * kHbmSliceSize)
              << std::dec << "\n"
              << "[rp1_hbm_stress] submitting graph seq=" << wantSeq << ", polling...\n";

    __sync_synchronize();
    ctrl->graph_seq = wantSeq;
    __sync_synchronize();

    constexpr auto kHeartbeatStallWindow = std::chrono::milliseconds(500);

    const auto start = std::chrono::steady_clock::now();
    uint32_t   lastHb     = ctrl->heartbeat;
    auto       lastHbTick = start;

    while (ctrl->graph_done_seq < wantSeq) {
        const auto now = std::chrono::steady_clock::now();

        const uint32_t hb = ctrl->heartbeat;
        if (hb != lastHb) {
            lastHb = hb;
            lastHbTick = now;
        } else if (now - lastHbTick > kHeartbeatStallWindow) {
            const uint32_t cqDelta = ctrl->cq_write_idx - priorCq;
            const uint32_t curNode = ctrl->rp1_current_node;
            std::cerr << "STALLED: heartbeat=" << hb
                      << " has not advanced in 500 ms -- R5 is hung on an AXI\n"
                      << "         access (most likely an unmapped user-region read).\n"
                      << "         Reload rp1.elf onto R5-1 via xsdb to recover.\n"
                      << "  cq_delta=" << cqDelta << " (expect " << expectedCq << ")\n"
                      << "  stall:   " << stallPoint(cqDelta, cli.iters, curNode) << "\n";
            dumpCtrl(ctrl);
            return 1;
        }

        if (now - start > std::chrono::milliseconds(cli.timeoutMs)) {
            const uint32_t cqDelta = ctrl->cq_write_idx - priorCq;
            const uint32_t curNode = ctrl->rp1_current_node;
            std::cerr << "TIMEOUT after " << cli.timeoutMs << " ms\n"
                      << "  graph_done_seq=" << ctrl->graph_done_seq
                      << " (want " << wantSeq << ")\n"
                      << "  cq_delta=" << cqDelta << " (expect " << expectedCq << ")\n"
                      << "  stall:   " << stallPoint(cqDelta, cli.iters, curNode) << "\n";
            dumpCtrl(ctrl);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    __sync_synchronize();
    const uint32_t cqDelta        = ctrl->cq_write_idx - priorCq;
    const uint32_t observedSig    = sigs[kSlotSentinel].value;
    const uint32_t observedOutAcc = sigs[kSlotOutAcc].value;
    const uint32_t finalState     = ctrl->rp1_state;

    const double avgPerIterMs = static_cast<double>(elapsedUs)
                              / static_cast<double>(cli.iters) / 1000.0;

    std::cout << "[rp1_hbm_stress] done in " << elapsedUs << " us"
              << " (avg " << std::fixed << std::setprecision(3) << avgPerIterMs
              << " ms per iter)\n"
              << std::defaultfloat
              << "  cq_delta             = " << cqDelta
              << " (expect " << expectedCq << ")\n"
              << "  signal[" << kSlotSentinel << "] (sentinel)         = 0x"
              << std::hex << std::setw(8) << std::setfill('0') << observedSig << std::dec
              << " (expect 0x" << std::hex << kSentinelMagic << std::dec << ")\n"
              << "  signal[" << kSlotOutAcc << "] (kernel_0 out_acc)   = 0x"
              << std::hex << std::setw(8) << std::setfill('0') << observedOutAcc
              << std::dec << std::setfill(' ') << "\n"
              << "  rp1_state            = " << finalState
              << " (" << stateStr(finalState) << ")\n";

    bool fail = false;
    if (observedSig != kSentinelMagic) {
        std::cerr << "FAIL: sentinel slot " << kSlotSentinel << " = 0x"
                  << std::hex << observedSig << ", expected 0x" << kSentinelMagic
                  << std::dec << "\n";
        fail = true;
    }
    if (cqDelta != expectedCq) {
        std::cerr << "FAIL: cq_delta=" << cqDelta << ", expected " << expectedCq << "\n"
                  << "  stall: " << stallPoint(cqDelta, cli.iters, ctrl->rp1_current_node) << "\n";
        fail = true;
    }
    if (finalState != RP1_STATE_READY) {
        std::cerr << "FAIL: rp1_state=" << finalState
                  << " (" << stateStr(finalState) << "), expected READY\n";
        fail = true;
    }
    if (fail) {
        dumpCtrl(ctrl);
        return 1;
    }

    std::cout << "PASS\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "rp1_hbm_stress: " << e.what() << "\n";
    return 1;
}
