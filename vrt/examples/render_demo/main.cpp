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
 */

/**
 * @file render_demo/main.cpp
 * @brief Demo: build a multi-device pipeline with CPU/mock-CPU control flow,
 *        run it, and write the rendered Graph + per-device DGraphs as Graphviz
 *        `.dot` files. Visualise the produced files with e.g. `dot`, `xdot`,
 *        or any Graphviz-compatible viewer.
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vrt/graph/core/graph_buffer.hpp>
#include <vrt/graph/core/types.hpp>
#include <vrt/graph/crossdevice/bridge.hpp>
#include <vrt/graph/crossdevice/bridge_op.hpp>
#include <vrt/graph/crossdevice/semaphore_pool.hpp>
#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/device/dgraph.hpp>
#include <vrt/graph/device/device.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>
#include <vrt/graph/node/compiled_node.hpp>
#include <vrt/graph/render/dot.hpp>

using namespace vrt::graph;

// ============================================================================
// MockCpuDevice — same executor as CpuDevice; only the type tag differs so the
// graph compiler treats it as a separate device class for routing purposes.
// ============================================================================

class MockCpuDevice : public CpuDevice {
   public:
    using CpuDevice::CpuDevice;
    DeviceType type() const override { return DeviceType::MOCK_CPU; }
};

// ============================================================================
// Bridge between any pair of cpu-like devices (CpuDevice + MockCpuDevice).
// Returns a BridgeStepPair so the compiler can splice it into the DGraphs.
// ============================================================================

namespace {

struct DemoBridgeOp : IBridgeOp {
    SemaphorePool*       pool;
    SemaphoreHandle      sem;
    std::vector<uint8_t> staging;

    std::string label() const override { return "demo_xfer"; }
};

BridgeStepPair makeCpuLikeTransfer(SemaphorePool&     pool,
                                    IDevice&            src,
                                    IDevice&            dst,
                                    const GraphBuffer&  buffer) {
    auto op  = std::make_shared<DemoBridgeOp>();
    op->pool = &pool;
    op->sem  = pool.allocate();

    const std::string n = scopedBufferKey(buffer.scopeId(), buffer.name());
    auto* sc = dynamic_cast<CpuDevice*>(&src);
    auto* sm = dynamic_cast<MockCpuDevice*>(&src);
    auto* dc = dynamic_cast<CpuDevice*>(&dst);
    auto* dm = dynamic_cast<MockCpuDevice*>(&dst);

    auto producerClosure = [op, sc, sm, n] {
        size_t sz = sc ? sc->bufferSize(n) : sm ? sm->bufferSize(n) : 0;
        op->staging.resize(sz);
        if (sz) {
            if      (sc) sc->getOutputBuffer(n, op->staging.data(), sz);
            else if (sm) sm->getOutputBuffer(n, op->staging.data(), sz);
        }
        op->pool->signal(op->sem);
    };
    auto tryReady = [op]() { return op->pool->tryAwait(op->sem); };
    auto consumerAction = [op, dc, dm, n] {
        if      (dc) dc->setInputBuffer(n, op->staging.data(), op->staging.size());
        else if (dm) dm->setInputBuffer(n, op->staging.data(), op->staging.size());
    };

    return BridgeStepPair{op,
                          std::move(producerClosure),
                          std::move(tryReady),
                          std::move(consumerAction)};
}

struct DemoBarrierOp : IBridgeOp {
    SemaphorePool*  pool;
    SemaphoreHandle sem;
    std::string     label() const override { return "barrier"; }
};

BridgeStepPair makeCpuLikeBarrier(SemaphorePool& pool) {
    auto op  = std::make_shared<DemoBarrierOp>();
    op->pool = &pool;
    op->sem  = pool.allocate();
    auto producer = [op]() { op->pool->signal(op->sem); };
    auto tryReady = [op]() { return op->pool->tryAwait(op->sem); };
    auto consumer = []() {};
    return BridgeStepPair{op,
                          std::move(producer),
                          std::move(tryReady),
                          std::move(consumer)};
}

}  // namespace

class CpuMockBridge : public IBridge {
   public:
    CpuMockBridge(IDevice& /*src*/, IDevice& /*dst*/) {}
    BridgeStepPair makeTransfer(IDevice& s, IDevice& d, const GraphBuffer& b,
                                 uint64_t, const std::string&, const std::string&) override {
        return makeCpuLikeTransfer(pool_, s, d, b);
    }
    BridgeStepPair makeBarrier(IDevice&, IDevice&,
                                const std::string&, const std::string&) override {
        return makeCpuLikeBarrier(pool_);
    }

   private:
    SemaphorePool pool_;
};

class MockMockBridge : public IBridge {
   public:
    MockMockBridge(IDevice& /*src*/, IDevice& /*dst*/) {}
    BridgeStepPair makeTransfer(IDevice& s, IDevice& d, const GraphBuffer& b,
                                 uint64_t, const std::string&, const std::string&) override {
        return makeCpuLikeTransfer(pool_, s, d, b);
    }
    BridgeStepPair makeBarrier(IDevice&, IDevice&,
                                const std::string&, const std::string&) override {
        return makeCpuLikeBarrier(pool_);
    }

   private:
    SemaphorePool pool_;
};

// ============================================================================
// Kernel functions
// ============================================================================

// 1-in, 1-out: byte-copy
static void copyKernel(const CpuKernelArgs& a) {
    const auto& in  = a.buffer("in");
    const auto& out = a.buffer("out");
    std::memcpy(out.data, in.data, std::min(in.sizeBytes, out.sizeBytes));
}

static void stageKernel(const CpuKernelArgs& a) {
    const auto& in  = a.buffer("in");
    const auto& out = a.buffer("stage");
    std::memcpy(out.data, in.data, std::min(in.sizeBytes, out.sizeBytes));
}

// 2-in, 1-out: XOR-merge (just to exercise multi-input wiring)
static void mergeKernel(const CpuKernelArgs& a) {
    const auto& a_buf = a.buffer("in_a");
    const auto& b_buf = a.buffer("in_b");
    const auto& out   = a.buffer("out");
    auto* ap = static_cast<const uint8_t*>(a_buf.data);
    auto* bp = static_cast<const uint8_t*>(b_buf.data);
    auto* op = static_cast<uint8_t*>(out.data);
    size_t n = std::min({a_buf.sizeBytes, b_buf.sizeBytes, out.sizeBytes});
    for (size_t i = 0; i < n; ++i) op[i] = ap[i] ^ bp[i];
}

class DemoCpuKernel : public CpuKernel {
   public:
    using Fn = std::function<void(const CpuKernelArgs&)>;

    DemoCpuKernel(std::string name, Fn fn, IOTypeMap ioType)
        : CpuKernel(std::move(name)), fn_(std::move(fn)), ioType_(std::move(ioType)) {}

    IOTypeMap ioTypeMap() const override { return ioType_; }
    void run(Args& args) override { fn_(args); }

   private:
    Fn          fn_;
    IOTypeMap   ioType_;
};

// ============================================================================
// Helpers to build kernel descriptors with fixed I/O signatures
// ============================================================================

static IOTypeMap io1in1out() {
    IOTypeMap io;
    io.inputs.push_back({"in",  BufferType::U8});
    io.outputs.push_back({"out", BufferType::U8});
    return io;
}

static IOTypeMap io1in1stage() {
    IOTypeMap io;
    io.inputs.push_back({"in",    BufferType::U8});
    io.outputs.push_back({"stage", BufferType::U8});
    return io;
}

static IOTypeMap io2in1out() {
    IOTypeMap io;
    io.inputs.push_back({"in_a", BufferType::U8});
    io.inputs.push_back({"in_b", BufferType::U8});
    io.outputs.push_back({"out", BufferType::U8});
    return io;
}

static IOTypeMap io1out() {
    IOTypeMap io;
    io.outputs.push_back({"out", BufferType::U8});
    return io;
}

static KernelDescriptor kd(std::string name, DeviceType t, IOTypeMap io = io1in1out()) {
    return KernelDescriptor{std::move(name), t, std::nullopt, std::move(io)};
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path outDir = (argc > 1) ? fs::path(argv[1]) : fs::path("render_demo_out");

    auto cpu     = std::make_shared<CpuDevice>("cpu");
    auto mock_a  = std::make_shared<MockCpuDevice>("mock_a");
    auto mock_b  = std::make_shared<MockCpuDevice>("mock_b");

    // --- Register kernels (all use copyKernel except merge) ---

    auto makeKernel = [](std::string n, DemoCpuKernel::Fn fn, IOTypeMap io) {
        return std::make_shared<DemoCpuKernel>(std::move(n), std::move(fn), std::move(io));
    };
    auto regCpu = [&](std::string n, DemoCpuKernel::Fn fn, IOTypeMap io = io1in1out()) {
        cpu->registerKernel(makeKernel(std::move(n), std::move(fn), std::move(io)));
    };
    auto regA = [&](std::string n, DemoCpuKernel::Fn fn, IOTypeMap io = io1in1out()) {
        mock_a->registerKernel(makeKernel(std::move(n), std::move(fn), std::move(io)));
    };
    auto regB = [&](std::string n, DemoCpuKernel::Fn fn, IOTypeMap io = io1in1out()) {
        mock_b->registerKernel(makeKernel(std::move(n), std::move(fn), std::move(io)));
    };

    regCpu("ingest",     copyKernel);
    regCpu("normalize",  copyKernel);
    regCpu("enhanceA",   copyKernel);
    regCpu("postA",      copyKernel);
    regCpu("postB",      copyKernel);
    regCpu("merge",      mergeKernel, io2in1out());
    regCpu("encode",     copyKernel);
    regCpu("finalize",   copyKernel);
    regCpu("loop_prepare", stageKernel, io1in1stage());
    regCpu("condition_then_copy", copyKernel);
    regCpu("condition_else_prepare", stageKernel, io1in1stage());
    regCpu("control_sink", copyKernel);

    regA  ("filterA1",   copyKernel);
    regA  ("filterA2",   copyKernel);
    regA  ("sharpenA",   copyKernel);
    regA  ("featuresB",  copyKernel);  // bounces from mock_b to mock_a
    regA  ("loop_refine_remote", copyKernel);

    regB  ("filterB1",   copyKernel);
    regB  ("detectB",    copyKernel);
    regB  ("denoiseA",   copyKernel);  // bounces from cpu to mock_b
    regB  ("classifyB",  copyKernel);
    regB  ("condition_else_remote", copyKernel);

    // --- Build the graph ---

    Graph g;
    g.registerDevice(cpu);
    g.registerDevice(mock_a);
    g.registerDevice(mock_b);
    g.registerBridgeFactory(DeviceType::CPU, DeviceType::MOCK_CPU,
        [](IDevice& s, IDevice& d){ return std::make_shared<CpuMockBridge>(s, d); });
    g.registerBridgeFactory(DeviceType::MOCK_CPU, DeviceType::CPU,
        [](IDevice& s, IDevice& d){ return std::make_shared<CpuMockBridge>(s, d); });
    g.registerBridgeFactory(DeviceType::MOCK_CPU, DeviceType::MOCK_CPU,
        [](IDevice& s, IDevice& d){ return std::make_shared<MockMockBridge>(s, d); });

    GraphBuffer raw = g.inputBuffer(BufferType::U8, "raw");
    GraphScalar renderBranchFlag = g.globalScalar(ScalarType::I32, "render_branch_flag");

    GraphBuffer bIngest, bNorm,
                bA1, bA2, bA3, bEnhA, bDenA, bPostA,
                bB1, bDetB, bFeatB, bClsB, bPostB,
                bMerged, bEnc, bFinal, bLoop, bConditional, bControlFinal;

    auto add1 = [&](std::string name, DeviceType dt, const std::string& did,
                    const GraphBuffer& in, GraphBuffer& out,
                    std::vector<std::string> after = {}) {
        IOMap m; m.bindInput("in", in).bindOutput("out", BufferType::U8, out);
        return g.addNode(kd(std::move(name), dt), std::move(m), did, std::move(after));
    };

    auto add2 = [&](std::string name, DeviceType dt, const std::string& did,
                    const GraphBuffer& inA, const GraphBuffer& inB, GraphBuffer& out) {
        IOMap m;
        m.bindInput("in_a", inA)
         .bindInput("in_b", inB)
         .bindOutput("out", BufferType::U8, out);
        return g.addNode(kd(std::move(name), dt, io2in1out()), std::move(m), did);
    };

    // 1–2: cpu ingestion
    /*nIngest =*/ add1("ingest",    DeviceType::CPU,      "cpu",    raw,     bIngest);
    /*nNorm   =*/ add1("normalize", DeviceType::CPU,      "cpu",    bIngest, bNorm);

    // Branch A: cpu → mock_a (×3) → cpu → mock_b → cpu
    /*nA1 =*/ add1("filterA1", DeviceType::MOCK_CPU, "mock_a", bNorm, bA1);
    /*nA2 =*/ add1("filterA2", DeviceType::MOCK_CPU, "mock_a", bA1,   bA2);
    /*nA3 =*/ add1("sharpenA", DeviceType::MOCK_CPU, "mock_a", bA2,   bA3);
    /*nEh =*/ add1("enhanceA", DeviceType::CPU,      "cpu",    bA3,   bEnhA);
    /*nDn =*/ add1("denoiseA", DeviceType::MOCK_CPU, "mock_b", bEnhA, bDenA);
    auto nPostA = add1("postA",    DeviceType::CPU,      "cpu",    bDenA, bPostA);

    // Branch B: cpu → mock_b → mock_b → mock_a → mock_b → cpu
    /*nB1 =*/ add1("filterB1",  DeviceType::MOCK_CPU, "mock_b", bNorm,  bB1);
    /*nDt =*/ add1("detectB",   DeviceType::MOCK_CPU, "mock_b", bB1,    bDetB);
    /*nFe =*/ add1("featuresB", DeviceType::MOCK_CPU, "mock_a", bDetB,  bFeatB);
    /*nCl =*/ add1("classifyB", DeviceType::MOCK_CPU, "mock_b", bFeatB, bClsB);
    auto nPostB = add1("postB",     DeviceType::CPU,      "cpu",    bClsB,  bPostB);

    // Merge + tail (cpu only); merge depends on both branches
    /*nMg =*/ add2("merge",    DeviceType::CPU, "cpu", bPostA, bPostB, bMerged);
    /*nEn =*/ add1("encode",   DeviceType::CPU, "cpu", bMerged, bEnc);
    auto nFinalize = add1("finalize", DeviceType::CPU, "cpu", bEnc, bFinal,
                          /*after=*/{nPostA, nPostB});

    // Control-flow coverage for the rendered demo: a fixed loop imports the
    // finalized CPU buffer, runs CPU -> mock-CPU work inside the body, then
    // materializes the declared loop output back at CPU placement.
    auto loopBody = g.rootRegion().createChild();
    GraphBuffer loopInput = loopBody->inputBuffer(BufferType::U8, "loop_input");
    std::string loopStart = loopBody->importFromParent(
        std::vector<BufferBoundaryMapping>{{bFinal, loopInput}});

    IOMap loopPrepareIo;
    GraphBuffer loopCpuStage;
    loopPrepareIo.bindInput("in", loopInput)
                 .bindOutput("stage", BufferType::U8, loopCpuStage,
                                   loopBody->scopeId());
    loopBody->addKernel(kd("loop_prepare", DeviceType::CPU, io1in1stage()),
                        std::move(loopPrepareIo), "cpu", {loopStart});

    IOMap loopRemoteIo;
    GraphBuffer loopRemoteOutput;
    loopRemoteIo.bindInput("in", loopCpuStage)
                .bindOutput("out", BufferType::U8, loopRemoteOutput,
                                  loopBody->scopeId());
    loopBody->addKernel(kd("loop_refine_remote", DeviceType::MOCK_CPU),
                        std::move(loopRemoteIo), "mock_a");

    GraphScalar renderLoopCount = g.scalarInput<int32_t>("render_loop_count");
    LoopSpec loopSpec;
    loopSpec.ioType = io1out();
    loopSpec.ioMap.bindOutput("out", BufferType::U8, bLoop,
                                    g.rootRegion().scopeId());
    loopSpec.tripCount = LoopTripCount::scalar(renderLoopCount);
    loopSpec.body = loopBody;
    loopSpec.outputPlacement.buffers["out"] = "cpu";
    loopSpec.afterOps = {nFinalize};
    std::string nLoop = g.addLoop(std::move(loopSpec));

    // The conditional has a CPU-only selected branch and a CPU -> mock-CPU else
    // branch so both authored branches and remote-output placement bridges show
    // up in the DOT tree.
    auto thenRegion = g.rootRegion().createChild();
    GraphBuffer thenInput = thenRegion->inputBuffer(BufferType::U8, "then_input");
    std::string thenStart = thenRegion->importFromParent(
        std::vector<BufferBoundaryMapping>{{bLoop, thenInput}});
    IOMap thenIo;
    GraphBuffer thenOutput;
    thenIo.bindInput("in", thenInput)
          .bindOutput("out", BufferType::U8, thenOutput,
                            thenRegion->scopeId());
    thenRegion->addKernel(kd("condition_then_copy", DeviceType::CPU),
                          std::move(thenIo), "cpu", {thenStart});

    auto elseRegion = g.rootRegion().createChild();
    GraphBuffer elseInput = elseRegion->inputBuffer(BufferType::U8, "else_input");
    std::string elseStart = elseRegion->importFromParent(
        std::vector<BufferBoundaryMapping>{{bLoop, elseInput}});
    IOMap elsePrepareIo;
    GraphBuffer elseCpuStage;
    elsePrepareIo.bindInput("in", elseInput)
                 .bindOutput("stage", BufferType::U8, elseCpuStage,
                                   elseRegion->scopeId());
    elseRegion->addKernel(kd("condition_else_prepare", DeviceType::CPU, io1in1stage()),
                          std::move(elsePrepareIo), "cpu", {elseStart});
    IOMap elseRemoteIo;
    GraphBuffer elseOutput;
    elseRemoteIo.bindInput("in", elseCpuStage)
                .bindOutput("out", BufferType::U8, elseOutput,
                                  elseRegion->scopeId());
    elseRegion->addKernel(kd("condition_else_remote", DeviceType::MOCK_CPU),
                          std::move(elseRemoteIo), "mock_b");

    Condition renderCondition = Condition::compare(
        CompareOp::EQ,
        ConditionOperand::scalar(ScalarType::I32, renderBranchFlag.varName(),
                                 renderBranchFlag.scopeId()),
        ConditionOperand::constant<int32_t>(1));
        ConditionalSpec conditionalSpec;
        conditionalSpec.ioType = io1out();
        conditionalSpec.ioMap.bindOutput("out", BufferType::U8, bConditional,
                                       g.rootRegion().scopeId());
        conditionalSpec.condition = std::move(renderCondition);
        conditionalSpec.thenRegion = thenRegion;
        conditionalSpec.elseRegion = elseRegion;
        conditionalSpec.outputPlacement.buffers["out"] = "cpu";
        conditionalSpec.afterOps = {nLoop};
        std::string nConditional = g.addConditional(std::move(conditionalSpec));

        add1("control_sink", DeviceType::CPU, "cpu", bConditional, bControlFinal,
            {nConditional});

    // --- Run the pipeline (so the renderer can show the populated DGraphs) ---

    std::vector<uint8_t> data(64, 0xAA);
    auto exec = g.compile();
    exec.write(raw, data);
    exec.setScalar(renderBranchFlag, 1);
    exec.setScalar(renderLoopCount, 2);
    exec.run();

    // --- Verify the executed pipeline ---
    //
    // Pipeline shape with the configured input (raw = 0xAA x 64) and
    // render_branch_flag = 1:
    //   ingest .. normalize .. {branchA, branchB} -> merge (XOR) -> encode -> finalize
    //     branchA and branchB run the same copyKernel chain, so both feed merge
    //     with 0xAA x 64; XOR yields 0x00 x 64. The loop body, then-branch, and
    //     control_sink are all memcpy, so bControlFinal must be 0x00 x 64.

    constexpr uint8_t kExpectedByte = 0x00;
    std::vector<uint8_t> result(data.size(), 0xFF);
    cpu->getOutputBuffer(bControlFinal.name(), result.data(), result.size());
    const bool ok = std::all_of(result.begin(), result.end(),
                                [](uint8_t b) { return b == kExpectedByte; });

    std::cout << "verification: bControlFinal == 0x"
              << std::hex << static_cast<int>(kExpectedByte) << std::dec
              << " x " << result.size()
              << " -> " << (ok ? "OK" : "FAILED") << "\n";
    if (!ok) {
        auto firstMismatch = std::find_if(
            result.begin(), result.end(),
            [](uint8_t b) { return b != kExpectedByte; });
        std::cerr << "  first mismatch at byte " << (firstMismatch - result.begin())
                  << " = 0x" << std::hex << static_cast<int>(*firstMismatch)
                  << std::dec << "\n";
        return 2;
    }

    // --- Render: write full Graph + every per-device DGraph as .dot files ---

    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        std::cerr << "render_demo: failed to create output dir '" << outDir
                  << "': " << ec.message() << "\n";
        return 1;
    }

    const fs::path graphPath = outDir / "graph.dot";
    render::writeToDotFile(g, graphPath.string());
    std::cout << "wrote " << graphPath << "\n";

    auto sanitizeStem = [](std::string stem) {
        for (char& c : stem) {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!keep) c = '_';
        }
        return stem;
    };

    auto childRoleName = [](DGraphChildRole role) {
        switch (role) {
            case DGraphChildRole::LoopBody:        return std::string{"loop_body"};
            case DGraphChildRole::ConditionalThen: return std::string{"then"};
            case DGraphChildRole::ConditionalElse: return std::string{"else"};
        }
        return std::string{"child"};
    };

    std::vector<fs::path> dotFiles{graphPath};
    std::function<void(const DGraph&, std::string)> writeDGraphTree =
        [&](const DGraph& dg, std::string stem) {
        const fs::path p = outDir / (sanitizeStem(std::move(stem)) + ".dot");
        render::writeToDotFile(dg, p.string());
        std::cout << "wrote " << p << "\n";
        dotFiles.push_back(p);

        for (const auto& child : dg.childDGraphs) {
            for (size_t i = 0; i < child.dgraphs.size(); ++i) {
                if (!child.dgraphs[i]) continue;
                writeDGraphTree(
                    *child.dgraphs[i],
                    p.stem().string() + "_" + child.parentNodeId + "_" +
                        childRoleName(child.role) + "_" + std::to_string(i) + "_" +
                        child.dgraphs[i]->deviceId);
            }
        }
    };

    for (const auto& dg : exec.dgraphs()) {
        writeDGraphTree(dg, "dgraph_" + dg.deviceId);
    }

    // If `dot` (Graphviz) is on PATH, also render PNGs alongside the .dot files.
    const bool hasDot = (std::system("command -v dot >/dev/null 2>&1") == 0);
    if (hasDot) {
        for (const auto& dotPath : dotFiles) {
            const fs::path pngPath = dotPath.string() + ".png";
            const std::string cmd  = "dot -Tpng " + dotPath.string() +
                                     " -o " + pngPath.string();
            const int rc = std::system(cmd.c_str());
            if (rc == 0) std::cout << "wrote " << pngPath << "\n";
            else         std::cerr << "dot failed (rc=" << rc << ") for " << dotPath << "\n";
        }
    } else {
        std::cout << "\n[dot not found on PATH — skipping PNG rendering]\n"
                  << "Visualise manually with e.g.:\n"
                  << "  dot -Tpng " << graphPath << " -o " << graphPath.string() << ".png\n"
                  << "  xdot " << graphPath << "\n";
    }
    return 0;
}
