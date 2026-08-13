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
// 02_sharpen_loop — iterative sharpening with adaptive gain on vrt::graph.
// ===========================================================================
//
// Companion example for the Graph API tutorial's Algorithm 2, exercising the
// two remaining structured-authoring building blocks: loops and
// conditionals.
//
//   - The loop: repeatedly apply a discrete Laplacian sharpening step to the
//     signal, `--iterations` times, each iteration reading the previous
//     iteration's result (loop-carried state). The sharpening step
//     (`sharpen_kernel`) runs on the FPGA; every iteration gates its
//     dispatch behind a reprogram of the loop's one image.
//   - In parallel with the loop: `cpu_level` computes the average brightness
//     of the *original* signal on the CPU. Because it never touches the
//     loop's carried state, it runs for the whole duration of the FPGA loop.
//   - The conditional: once both the loop and the brightness computation are
//     done, `cpu_passthrough` or `cpu_boost` applies one of two gains
//     depending on whether the signal is bright or dark.
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
using std::int32_t;
using std::size_t;
using std::uint32_t;
using std::uint64_t;

using vrt::graph::CpuKernel;
using vrt::graph::Graph;
using vrt::graph::GraphBuffer;
using vrt::graph::GraphScalar;
using vrt::graph::IOTypeMap;

namespace {

struct Cli {
    std::string socket = "/run/vrtd.sock";
    std::string bdf;
    std::string vbin;
    uint32_t iterations = 4;
    int32_t alpha = 1;
    int32_t threshold = 50;
    int32_t boost = 2;
    uint32_t elementCount = 16;
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
        << "  --socket PATH   vrtd socket (default: /run/vrtd.sock)\n"
        << "  --vbin PATH     image vbin (default: next to executable)\n"
        << "  --iterations N  loop iterations (default: 4)\n"
        << "  --alpha N       sharpening strength (default: 1)\n"
        << "  --threshold N   brightness threshold (default: 50)\n"
        << "  --boost N       dark-branch gain (default: 2)\n"
        << "  --elements N    int32 elements (default: 16)\n"
        << "  --help, -h      show this help\n";
}

Cli parseArgs(int argc, char** argv) {
    Cli cli;
    const auto binDir = executableDir(argv[0]);
    cli.vbin = (binDir / "sharpen_loop_hw.vbin").string();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string("missing argument to ") + flag);
            return argv[i];
        };
        if (arg == "--socket") cli.socket = need("--socket");
        else if (arg == "--bdf") cli.bdf = need("--bdf");
        else if (arg == "--vbin") cli.vbin = need("--vbin");
        else if (arg == "--iterations") cli.iterations = static_cast<uint32_t>(
            std::stoul(need("--iterations")));
        else if (arg == "--alpha") cli.alpha = std::stoi(need("--alpha"));
        else if (arg == "--threshold") cli.threshold = std::stoi(need("--threshold"));
        else if (arg == "--boost") cli.boost = std::stoi(need("--boost"));
        else if (arg == "--elements") cli.elementCount = static_cast<uint32_t>(
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

std::vector<int32_t> generateInput(uint32_t n) {
    std::vector<int32_t> input(n);
    for (uint32_t i = 0; i < n; ++i) {
        const int32_t base  = 40 + static_cast<int32_t>(i % 7) * 6;
        const int32_t spike = (i % 5 == 0) ? 15 : 0;
        input[i] = base + spike;
    }
    return input;
}

// Host-side reference for the same pipeline the graph runs.
std::vector<int32_t> expectedOutput(const std::vector<int32_t>& input,
                                    uint32_t K, int32_t alpha,
                                    int32_t threshold, int32_t boost) {
    const size_t n = input.size();

    // Loop: K iterations of discrete Laplacian sharpening (edge-replicated boundaries).
    std::vector<int32_t> s = input;
    for (uint32_t iter = 0; iter < K; ++iter) {
        std::vector<int32_t> next(n);
        for (size_t i = 0; i < n; ++i) {
            int32_t left  = (i == 0)     ? s[i] : s[i - 1];
            int32_t right = (i + 1 == n) ? s[i] : s[i + 1];
            int32_t lap   = 2 * s[i] - left - right;
            next[i] = s[i] + alpha * lap;
        }
        s = std::move(next);
    }

    // In parallel with the loop: brightness of the ORIGINAL input.
    int64_t sum = 0;
    for (int32_t v : input) sum += v;
    int32_t level = static_cast<int32_t>(sum / static_cast<int64_t>(n));
    level = (level > 1) ? level : 1;

    // Conditional: adaptive gain based on brightness.
    std::vector<int32_t> output(n);
    if (level >= threshold) {
        for (size_t i = 0; i < n; ++i) output[i] = s[i];         // bright: pass through
    } else {
        for (size_t i = 0; i < n; ++i) output[i] = s[i] * boost; // dark: boost
    }
    return output;
}

// ---------------------------------------------------------------------------
// CPU kernels: the loop body's sharpening step moves to FPGA; the brightness
// reduction and both conditional branches stay on the CPU.
// ---------------------------------------------------------------------------

// Identical in shape to Algorithm 1's Kernel B: it only ever reads the
// graph's original input, never the loop's carried state.
class CpuLevel : public CpuKernel {
   public:
    CpuLevel() : CpuKernel("cpu_level") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}.in<int32_t>("input").scalarOut<int32_t>("level");
    }

    void run(Args& args) override {
        auto  input = args.in<int32_t>("input");
        auto& level = args.scalarOut<int32_t>("level");

        int64_t sum = 0;
        for (size_t i = 0; i < input.size(); ++i) sum += input[i];
        level = static_cast<int32_t>(sum / static_cast<int64_t>(input.size()));
        level = (level > 1) ? level : 1;
    }
};

// Bright branch: the signal is already well-lit, so no gain adjustment is needed.
// Both branches of a conditional must produce every declared output port, so
// this kernel exists purely to satisfy that contract.
class CpuPassthrough : public CpuKernel {
   public:
    CpuPassthrough() : CpuKernel("cpu_passthrough") {}
    IOTypeMap ioTypeMap() const override { return IOTypeMap{}.inout<int32_t>("data"); }
    void run(Args&) override {}
};

// Dark branch: scale the signal up by a fixed boost factor.
class CpuBoost : public CpuKernel {
   public:
    CpuBoost() : CpuKernel("cpu_boost") {}

    IOTypeMap ioTypeMap() const override {
        return IOTypeMap{}
            .in<int32_t>("in")
            .scalarIn<int32_t>("boost")
            .out<int32_t>("out");
    }

    void run(Args& args) override {
        auto in    = args.in<int32_t>("in");
        auto boost = args.scalarIn<int32_t>("boost");
        auto out   = args.out<int32_t>("out");
        for (size_t i = 0; i < in.size(); ++i) out[i] = in[i] * boost;
    }
};

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    // 1. Device bring-up.
    Graph graph = Graph::withDefaults();

    auto fpga = graph.addFpga({
        .bdf         = cli.bdf,
        .socket      = cli.socket,
        .images      = {{"image", cli.vbin}},
        .waitTimeout = 30s,
    });
    auto image = fpga.image("image");

    // 2. Kernels -- the loop body's sharpening step runs on FPGA; the rest
    //    stay on the CPU.
    auto fpgaSharpen = image.kernel("sharpen_kernel_0")
                           .scalarIn<uint64_t>("n")
                           .scalarIn<int32_t>("alpha")
                           .in<int32_t>("in")
                           .out<int32_t>("out");

    auto level       = graph.cpu().add<CpuLevel>();
    auto passthrough = graph.cpu().add<CpuPassthrough>();
    auto boost       = graph.cpu().add<CpuBoost>();

    // 3. Scalars and buffers which are inputs to the graph as a whole.
    GraphScalar n         = graph.scalarInput<uint64_t>("n");
    GraphBuffer input     = graph.input<int32_t>("input", n);
    GraphScalar K         = graph.scalarInput<uint32_t>("K");
    GraphScalar alpha     = graph.scalarInput<int32_t>("alpha");
    GraphScalar threshold = graph.scalarInput<int32_t>("threshold");
    GraphScalar boostBy   = graph.scalarInput<int32_t>("boost");

    // 4. Scalars and buffers which are outputs to the graph as a whole.
    GraphBuffer output = graph.output<int32_t>("output", n);

    // 5. Kernel calls and temporaries.

    // The loop dispatches to the FPGA every iteration. As with any FPGA
    // dispatch, it must be gated behind a reprogram of its image -- and
    // because the dispatch lives inside the loop body, the reprogram must be
    // authored there too; a reprogram at the graph's top level would not
    // gate a kernel nested inside a loop.
    GraphBuffer sharpened = graph.buffer<int32_t>("sharpened", n);
    {
        auto loop = graph.addLoop({
            .count   = K,
            .inputs  = {{"state", input}},
            .outputs = {{"state", sharpened}},
        });

        auto r = loop.addReprogram({.image = image});

        loop.addKernelCall({
            .kernel       = fpgaSharpen,
            .inputScalars = {{"n", n}, {"alpha", alpha}},
            .inputs       = {{"in", loop.input("state")}},
            .outputs      = {{"out", loop.output("state")}},
            .after        = {r},
        });
    }

    // `level` still reads the original `input`, so it keeps running on the
    // CPU for the whole time the FPGA loop is iterating.
    GraphScalar brightness = graph.scalar<int32_t>("brightness");
    graph.addKernelCall({
        .kernel        = level,
        .inputs        = {{"input", input}},
        .outputScalars = {{"level", brightness}},
    });

    // The conditional and both branches stay on the CPU.
    auto [thenBranch, elseBranch] = graph.addConditional({
        .condition = (brightness >= threshold),
        .inputs    = {{"signal", sharpened}},
        .outputs   = {{"signal", output}},
    });

    thenBranch.addKernelCall({
        .kernel = passthrough,
        .inouts = {{"data", thenBranch.input("signal"), thenBranch.output("signal")}},
    });

    elseBranch.addKernelCall({
        .kernel       = boost,
        .inputScalars = {{"boost", boostBy}},
        .inputs       = {{"in", elseBranch.input("signal")}},
        .outputs      = {{"out", elseBranch.output("signal")}},
    });

    // 6. Compile, bind inputs, run, read back.
    const std::vector<int32_t> inputData = generateInput(cli.elementCount);

    std::cout << "[02_sharpen_loop] compiling graph with " << cli.iterations
              << " loop iteration(s), " << cli.elementCount << " element(s)" << std::endl;
    auto exec = graph.compile();
    exec.writeScalar(n, static_cast<uint64_t>(cli.elementCount));
    exec.write(input, inputData);
    exec.writeScalar(K, cli.iterations);
    exec.writeScalar(alpha, cli.alpha);
    exec.writeScalar(threshold, cli.threshold);
    exec.writeScalar(boostBy, cli.boost);

    std::cout << "[02_sharpen_loop] running graph..." << std::endl;
    exec.run();
    std::cout << "[02_sharpen_loop] graph run complete; checking output..." << std::endl;

    std::vector<int32_t> result(cli.elementCount, 0);
    exec.read(output, result);
    const auto expected = expectedOutput(inputData, cli.iterations, cli.alpha,
                                         cli.threshold, cli.boost);

    std::cout << "[02_sharpen_loop] output:";
    for (size_t i = 0; i < std::min<size_t>(result.size(), 8); ++i) {
        std::cout << ' ' << result[i];
    }
    if (result.size() > 8) std::cout << " ...";
    std::cout << std::endl;

    if (result != expected) {
        std::cerr << "FAIL: output mismatch\nexpected:";
        for (size_t i = 0; i < std::min<size_t>(expected.size(), 8); ++i) {
            std::cerr << ' ' << expected[i];
        }
        if (expected.size() > 8) std::cerr << " ...";
        std::cerr << std::endl;
        return 1;
    }

    std::cout << "PASS: iterative sharpening with adaptive gain matches the host reference."
              << std::endl;
    return 0;
} catch (const std::exception& e) {
    std::cerr << "02_sharpen_loop: " << e.what() << std::endl;
    return 1;
}
