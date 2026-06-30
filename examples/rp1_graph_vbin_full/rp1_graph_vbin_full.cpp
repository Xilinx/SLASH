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

// ===========================================================================
// rp1_graph_vbin_full — full CPU+FPGA work-graph demo on the vrt::graph API.
// ===========================================================================
//
// One application source drives a heterogeneous graph across two FPGA images
// and several CPU kernels:
//
//   - graph.addFpga() folds QDMA PDI staging, vbin/image loading, the vrtd
//     session + BAR4 window, the RP1 readiness preflight, and FpgaDevice
//     construction into one call; named image handles know their own PDI path.
//   - The user region starts with NO active image, so every FPGA dispatch is
//     gated behind an explicit reprogram (PDI_LOAD) of its image. compile()
//     proves this from the `.after` edges and derives the old-image drain when
//     one reprogram chains to the prior one (kernels are never named in a drain
//     edge).
//   - Each node is one designated-initializer struct literal; buffers/scalars
//     are first-class single-assignment tokens; data dependencies are inferred
//     from token use and `.after` carries only image-safety ordering.
//   - A fixed-count loop carries `state` across iterations; an in-place
//     (inout) kernel bumps every 10th element; a post-loop conditional branches
//     on a scalar written by a CPU kernel.
//
// Algorithm (applied per element i):
//   x = i
//   x = x + 10                          # cpu_preprocess
//   repeat `iterations`:                # loop, carrying x
//       x = x + 1                       # cpu_stage
//       reprogram(imageA)               # PDI_LOAD A
//       x = x + 1                       # fpgaA (image A: out = in + 1)
//       if i % 10 == 0: x = x + 1       # cpu_sparse (in place)
//       reprogram(imageB)               # PDI_LOAD B
//       x = x * 2                       # fpgaB (image B: out = in * 2)
//       x = x - 4                       # cpu_finalize
//   parity = post[0] & 1                # cpu_parity (writes a scalar)
//   if parity == 0: out[i] = post[i] + 100   # cpu_report
//   else:           out[i] = post[i] + 200   # cpu_report_odd
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vrt/graph/device/cpu_device.hpp>
#include <vrt/graph/graph.hpp>

using namespace std::chrono_literals;
using vrt::graph::CpuKernel;
using vrt::graph::Graph;
using vrt::graph::GraphBuffer;
using vrt::graph::GraphScalar;
using vrt::graph::IOTypeMap;

namespace {

struct Cli {
    std::string socket = "/run/vrtd.sock";
    std::string bdf;
    std::string vbinA;
    std::string vbinB;
    std::uint32_t iterations = 2;
    std::uint32_t elementCount = 16;
};

std::filesystem::path executableDir(const char* argv0) {
    std::filesystem::path p(argv0);
    if (p.has_parent_path()) return p.parent_path();
    return std::filesystem::current_path();
}

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " --bdf <PCI_BDF> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --socket PATH       vrtd socket (default: /run/vrtd.sock)\n"
        << "  --vbin-a PATH       image A vbin (default: next to executable)\n"
        << "  --vbin-b PATH       image B vbin (default: next to executable)\n"
        << "  --iterations N      loop iterations (default: 2)\n"
        << "  --elements N        int32 elements (default: 16)\n"
        << "  --help, -h          show this help\n";
}

Cli parseArgs(int argc, char** argv) {
    Cli cli;
    const auto binDir = executableDir(argv[0]);
    cli.vbinA = (binDir / "rp1_graph_vbin_full_a_hw.vbin").string();
    cli.vbinB = (binDir / "rp1_graph_vbin_full_b_hw.vbin").string();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string("missing argument to ") + flag);
            return argv[i];
        };
        if (arg == "--socket") cli.socket = need("--socket");
        else if (arg == "--bdf") cli.bdf = need("--bdf");
        else if (arg == "--vbin-a") cli.vbinA = need("--vbin-a");
        else if (arg == "--vbin-b") cli.vbinB = need("--vbin-b");
        else if (arg == "--iterations") cli.iterations = static_cast<std::uint32_t>(
            std::stoul(need("--iterations")));
        else if (arg == "--elements") cli.elementCount = static_cast<std::uint32_t>(
            std::stoul(need("--elements")));
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (cli.bdf.empty()) {
        usage(argv[0]);
        throw std::runtime_error("--bdf is required");
    }
    if (cli.iterations == 0 || cli.elementCount == 0) {
        throw std::runtime_error("--iterations and --elements must be non-zero");
    }
    return cli;
}

// Host-side reference for the same pipeline the graph runs.
std::vector<std::int32_t> expectedOutput(std::uint32_t elementCount, std::uint32_t iterations) {
    std::vector<std::int32_t> post(elementCount);
    for (std::uint32_t i = 0; i < elementCount; ++i) {
        std::int32_t v = static_cast<std::int32_t>(i);
        v = v + 10;  // cpu_preprocess
        for (std::uint32_t iter = 0; iter < iterations; ++iter) {
            v = v + 1;                       // cpu_stage
            v = v + 1;                       // image A FPGA kernel
            if (i % 10 == 0) v = v + 1;      // cpu_sparse (in place, every 10th element)
            v = v * 2;                       // image B FPGA kernel
            v = v - 4;                       // cpu_finalize
        }
        post[i] = v;  // loop-carried result
    }

    const std::int32_t bias = (post[0] & 1) == 0 ? 100 : 200;
    std::vector<std::int32_t> out(elementCount);
    for (std::uint32_t i = 0; i < elementCount; ++i) {
        out[i] = post[i] + bias;
    }
    return out;
}

// ---------------------------------------------------------------------------
// CPU kernels: each declares its typed I/O once via ioTypeMap() and receives
// ready-made typed spans in run() by port name.
// ---------------------------------------------------------------------------

class CpuPreprocess : public CpuKernel {
   public:
    CpuPreprocess() : CpuKernel("cpu_preprocess") {}
    
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }

    void run(Args& args) override {
        auto in = args.in<int32_t>("in");
        auto out = args.out<int32_t>("out");
        
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] + 10;
        }
    }
};

class CpuStage : public CpuKernel {
   public:
    CpuStage() : CpuKernel("cpu_stage") {}
    
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    
    void run(Args& args) override {
        auto in = args.in<int32_t>("in");
        auto out = args.out<int32_t>("out");
        
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] + 1;
        }
    }
};

// In-place: bumps every 10th element. inout<T>() declares one read-write port;
// run() mutates the span over the consumed token's memory (no copy).
class CpuSparse : public CpuKernel {
   public:
    CpuSparse() : CpuKernel("cpu_sparse") {}
    IOTypeMap ioTypeMap() const override { return IOTypeMap{}.inout<int32_t>("data"); }
    void run(Args& args) override {
        auto data = args.inout<int32_t>("data");

        for (std::size_t i = 0; i < data.size(); i += 10) {
            data[i] += 1;
        }
    }
};

class CpuFinalize : public CpuKernel {
   public:
    CpuFinalize() : CpuKernel("cpu_finalize") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }
    void run(Args& args) override {
        auto in = args.in<int32_t>("in");
        auto out = args.out<int32_t>("out");

        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] - 4;
        }
    }
};

// Writes a scalar the post-loop conditional branches on.
class CpuParity : public CpuKernel {
   public:
    CpuParity() : CpuKernel("cpu_parity") {}
    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").scalarOut<uint64_t>("parity");
    }
    void run(Args& args) override {
        auto in = args.in<int32_t>("in");

        args.setScalar("parity", static_cast<std::uint64_t>(in[0] & 1));
    }
};

class CpuReport : public CpuKernel {
   public:
    CpuReport() : CpuKernel("cpu_report") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }

    void run(Args& args) override {
        auto in = args.in<int32_t>("in");
        auto out = args.out<int32_t>("out");

        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] + 100;
        }
    }
};

class CpuReportOdd : public CpuKernel {
   public:
    CpuReportOdd() : CpuKernel("cpu_report_odd") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("in").out<int32_t>("out");
    }

    void run(Args& args) override {
        auto in = args.in<int32_t>("in");
        auto out = args.out<int32_t>("out");
        
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] + 200;
        }
    }
};

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    // 1. Device bring-up -- one call.
    Graph graph = Graph::withDefaults();

    auto fpga = graph.addFpga({
        .bdf         = cli.bdf,
        .socket      = cli.socket,
        .images      = {{"imageA", cli.vbinA}, {"imageB", cli.vbinB}},
        .waitTimeout = 30s,
    });

    auto imageA = fpga.image("imageA");
    auto imageB = fpga.image("imageB");

    // 2. Kernels -- signature and behaviour declared once, yielding a handle.
    auto preprocess   = graph.cpu().add<CpuPreprocess>();
    auto stage        = graph.cpu().add<CpuStage>();
    auto sparse       = graph.cpu().add<CpuSparse>();
    auto finalize     = graph.cpu().add<CpuFinalize>();
    auto parityKernel = graph.cpu().add<CpuParity>();
    auto report       = graph.cpu().add<CpuReport>();      // even branch (+100)
    auto reportOdd    = graph.cpu().add<CpuReportOdd>();   // else branch (+200)

    auto fpgaA = imageA.kernel("graph_kernel_0")
                     .scalarIn<uint64_t>("n")
                     .in<int32_t>("in")
                     .out<int32_t>("out");
    auto fpgaB = imageB.kernel("graph_kernel_0")
                     .scalarIn<uint64_t>("n")
                     .in<int32_t>("in")
                     .out<int32_t>("out");

    // 3. Author the graph.
    GraphScalar elements = graph.scalarInput<std::uint64_t>("elements");
    GraphBuffer raw = graph.input<int32_t>("raw", elements);
    GraphScalar elementCount = graph.scalarInput<uint64_t>("elementCount");
    GraphScalar loopIterations = graph.scalarInput<std::uint32_t>("loopIterations");

    GraphBuffer pre = graph.buffer<int32_t>("pre", elements);
    graph.addKernelCall({
        .kernel  = preprocess,
        .inputs  = {{"in", raw}},
        .outputs = {{"out", pre}},
    });

    GraphBuffer post = graph.buffer<int32_t>("post", elements);
    {
        auto loop = graph.addLoop({
            .count   = loopIterations,
            .inputs  = {{"state", pre}},
            .outputs = {{"state", post}},
        });

        GraphBuffer s = loop.input("state");

        GraphBuffer staged = loop.buffer<int32_t>("staged", elements);
        loop.addKernelCall({
            .kernel  = stage,
            .inputs  = {{"in", s}},
            .outputs = {{"out", staged}},
        });

        // First reprogram of the iteration. On the first iteration the region
        // is empty; on later iterations the loop boundary serialises iterations
        // so image B has already drained.
        auto rA = loop.addReprogram({.image = imageA});

        // `.after = {rA}` orders this dispatch after the reprogram and binds it
        // to image A; compile() rejects the graph if fpgaA's image != rA's.
        GraphBuffer afterA = loop.buffer<int32_t>("afterA", elements);
        loop.addKernelCall({
            .kernel       = fpgaA,
            .inputScalars = {{"n", elementCount}},
            .inputs       = {{"in", staged}},
            .outputs      = {{"out", afterA}},
            .after        = {rA},
        });

        // cpu_sparse mutates afterA in place, producing `bumped`; afterA is dead
        // afterward.
        GraphBuffer bumped = loop.buffer<int32_t>("bumped", elements);
        loop.addKernelCall({
            .kernel = sparse,
            .inouts = {{"data", afterA, bumped}},
        });

        // Chain to the prior reprogram: compile() expands `.after = {rA}` to also
        // wait on every kernel gated behind rA (fpgaA), draining image A.
        auto rB = loop.addReprogram({
            .image = imageB,
            .after = {rA},
        });

        GraphBuffer afterB = loop.buffer<int32_t>("afterB", elements);
        loop.addKernelCall({
            .kernel       = fpgaB,
            .inputScalars = {{"n", elementCount}},
            .inputs       = {{"in", bumped}},
            .outputs      = {{"out", afterB}},
            .after        = {rB},
        });

        loop.addKernelCall({
            .kernel  = finalize,
            .inputs  = {{"in", afterB}},
            .outputs = {{"out", loop.output("state")}},
        });
    }

    // cpu_parity reduces the loop result to one scalar; compile() infers the
    // conditional's dependency on this node through the `parity` scalar.
    GraphScalar parity = graph.scalar<uint64_t>("parity");
    graph.addKernelCall({
        .kernel        = parityKernel,
        .inputs        = {{"in", post}},
        .outputScalars = {{"parity", parity}},
    });

    GraphBuffer out = graph.buffer<int32_t>("out", elements);
    {
        auto [thenBranch, elseBranch] = graph.addConditional({
            .condition = (parity == 0),       // even -> then (+100), odd -> else (+200)
            .inputs    = {{"x", post}},
            .outputs   = {{"y", out}},
        });

        thenBranch.addKernelCall({
            .kernel  = report,
            .inputs  = {{"in", thenBranch.input("x")}},
            .outputs = {{"out", thenBranch.output("y")}},
        });

        elseBranch.addKernelCall({
            .kernel  = reportOdd,
            .inputs  = {{"in", elseBranch.input("x")}},
            .outputs = {{"out", elseBranch.output("y")}},
        });
    }

    // 4. Compile, bind dispatch inputs, run, read back -- all keyed by token.
    std::vector<std::int32_t> input(cli.elementCount);
    for (std::uint32_t i = 0; i < cli.elementCount; ++i) {
        input[i] = static_cast<std::int32_t>(i);
    }

    std::cout << "[rp1_graph_vbin_full] compiling graph with "
              << cli.iterations << " loop iteration(s), "
              << cli.elementCount << " element(s)" << std::endl;
    auto exec = graph.compile();
    exec.setScalar(elements, static_cast<std::uint64_t>(cli.elementCount));
    exec.setScalar(elementCount, static_cast<std::uint64_t>(cli.elementCount));
    exec.setScalar(loopIterations, cli.iterations);
    exec.write(raw, input);

    std::cout << "[rp1_graph_vbin_full] running graph..." << std::endl;
    exec.run();
    std::cout << "[rp1_graph_vbin_full] graph run complete; checking output..." << std::endl;

    std::vector<std::int32_t> output(cli.elementCount, 0);
    exec.read(out, output);
    const auto expected = expectedOutput(cli.elementCount, cli.iterations);

    std::cout << "[rp1_graph_vbin_full] output:";
    for (std::size_t i = 0; i < std::min<std::size_t>(output.size(), 8); ++i) {
        std::cout << ' ' << output[i];
    }
    if (output.size() > 8) std::cout << " ...";
    std::cout << std::endl;

    if (output != expected) {
        std::cerr << "FAIL: output mismatch\nexpected:";
        for (std::size_t i = 0; i < std::min<std::size_t>(expected.size(), 8); ++i) {
            std::cerr << ' ' << expected[i];
        }
        if (expected.size() > 8) std::cerr << " ...";
        std::cerr << std::endl;
        return 1;
    }

    std::cout << "PASS: CPU + FPGA graph with two vbins and explicit reprogram nodes completed."
              << std::endl;
    return 0;
} catch (const std::exception& e) {
    std::cerr << "rp1_graph_vbin_full: " << e.what() << std::endl;
    return 1;
}
