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
// 01_edge_detection — illumination-normalized edge detection on vrt::graph.
// ===========================================================================
//
// Companion example for the Graph API tutorial's Algorithm 1. A single
// vrt::graph::Graph runs three steps over a 1-D signal:
//
//   Step A (edges_kernel):     edges[i]  = |input[i+1] - input[i]|
//   Step B (level_kernel):     level     = max(1, sum(input) / n)
//   Step C (normalize_kernel): output[i] = edges[i] * K / level
//
// Steps A and B each read only the graph-level input and can run
// concurrently; step C waits for both to finish before normalizing. All
// three kernels live in a single vbin image, so one reprogram node gates
// every FPGA dispatch.
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
using std::uint64_t;

using vrt::graph::Graph;
using vrt::graph::GraphBuffer;
using vrt::graph::GraphScalar;

namespace {

struct Cli {
    std::string socket = "/run/vrtd.sock";
    std::string bdf;
    std::string vbin;
    int32_t k = 16;
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
        << "  --k N           normalization gain K (default: 16)\n"
        << "  --elements N    int32 elements, at least 2 (default: 16)\n"
        << "  --help, -h      show this help\n";
}

Cli parseArgs(int argc, char** argv) {
    Cli cli;
    const auto binDir = executableDir(argv[0]);
    cli.vbin = (binDir / "edge_detection_hw.vbin").string();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string("missing argument to ") + flag);
            return argv[i];
        };
        if (arg == "--socket") cli.socket = need("--socket");
        else if (arg == "--bdf") cli.bdf = need("--bdf");
        else if (arg == "--vbin") cli.vbin = need("--vbin");
        else if (arg == "--k") cli.k = std::stoi(need("--k"));
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
    if (cli.elementCount < 2) {
        throw std::runtime_error("--elements must be at least 2");
    }
    return cli;
}

std::vector<int32_t> generateInput(uint32_t n) {
    std::vector<int32_t> input(n);
    for (uint32_t i = 0; i < n; ++i) {
        input[i] = static_cast<int32_t>((i * 37) % 251) - 64;
    }
    return input;
}

// Host-side reference for the same computation the graph runs.
std::vector<int32_t> expectedOutput(const std::vector<int32_t>& input, int32_t K) {
    const size_t n = input.size();

    // Step A: local derivative. edges[i] = |input[i+1] - input[i]|, last = 0.
    std::vector<int32_t> edges(n, 0);
    for (size_t i = 0; i + 1 < n; ++i) {
        edges[i] = std::abs(input[i + 1] - input[i]);
    }

    // Step B: global brightness. level = max(1, sum(input) / n).
    int64_t sum = 0;
    for (int32_t v : input) sum += v;
    int32_t level = static_cast<int32_t>(sum / static_cast<int64_t>(n));
    level = (level > 1) ? level : 1;

    // Step C: normalize the edges by brightness. output[i] = edges[i] * K / level.
    std::vector<int32_t> output(n, 0);
    for (size_t i = 0; i < n; ++i) {
        output[i] = static_cast<int32_t>(
            static_cast<int64_t>(edges[i]) * K / level);
    }
    return output;
}

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    // 1. Device bring-up -- one call, one image containing all three kernels.
    Graph graph = Graph::withDefaults();

    auto fpga = graph.addFpga({
        .bdf         = cli.bdf,
        .socket      = cli.socket,
        .images      = {{"image", cli.vbin}},
        .waitTimeout = 30s,
    });
    auto image = fpga.image("image");

    // 2. FPGA kernel handles.
    auto fpgaEdges = image.kernel("edges_kernel_0")
                         .scalarIn<uint64_t>("n")
                         .in<int32_t>("in")
                         .out<int32_t>("edges");

    auto fpgaLevel = image.kernel("level_kernel_0")
                         .scalarIn<uint64_t>("n")
                         .in<int32_t>("in")
                         .scalarOut<int32_t>("level");

    auto fpgaNorm = image.kernel("normalize_kernel_0")
                        .scalarIn<uint64_t>("n")
                        .scalarIn<int32_t>("K")
                        .scalarIn<int32_t>("level")
                        .inout<int32_t>("edges");

    // 3. Scalars and buffers which are inputs to the graph as a whole.
    GraphScalar n = graph.scalarInput<uint64_t>("n");
    GraphScalar K = graph.scalarInput<int32_t>("K");
    GraphBuffer input = graph.input<int32_t>("input", n);

    // 4. Scalars and buffers which are outputs to the graph as a whole.
    GraphBuffer output = graph.output<int32_t>("output", n);

    // 5. Kernel calls and temporaries. All three kernels share one image, so
    //    one reprogram gates every dispatch below.
    auto r = graph.addReprogram({.image = image});

    GraphBuffer edges = graph.buffer<int32_t>("edges", n);
    GraphScalar level = graph.scalar<int32_t>("level");

    // Step A and Step B both read `input` directly and can run concurrently.
    graph.addKernelCall({
        .kernel       = fpgaEdges,
        .inputScalars = {{"n", n}},
        .inputs       = {{"in", input}},
        .outputs      = {{"edges", edges}},
        .after        = {r},
    });

    graph.addKernelCall({
        .kernel        = fpgaLevel,
        .inputScalars  = {{"n", n}},
        .inputs        = {{"in", input}},
        .outputScalars = {{"level", level}},
        .after         = {r},
    });

    // Step C consumes `edges` (Step A) and `level` (Step B), so it implicitly
    // waits for both.
    graph.addKernelCall({
        .kernel       = fpgaNorm,
        .inputScalars = {{"n", n}, {"K", K}, {"level", level}},
        .inouts       = {{"edges", edges, output}},
        .after        = {r},
    });

    // 6. Compile, bind inputs, run, read back.
    const std::vector<int32_t> inputData = generateInput(cli.elementCount);

    std::cout << "[01_edge_detection] compiling graph for " << cli.elementCount
              << " element(s), K=" << cli.k << std::endl;
    auto exec = graph.compile();
    exec.writeScalar(n, static_cast<uint64_t>(cli.elementCount));
    exec.writeScalar(K, cli.k);
    exec.write(input, inputData);

    std::cout << "[01_edge_detection] running graph..." << std::endl;
    exec.run();
    std::cout << "[01_edge_detection] graph run complete; checking output..." << std::endl;

    std::vector<int32_t> result(cli.elementCount, 0);
    exec.read(output, result);
    const auto expected = expectedOutput(inputData, cli.k);

    std::cout << "[01_edge_detection] output:";
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

    std::cout << "PASS: illumination-normalized edge detection matches the host reference."
              << std::endl;
    return 0;
} catch (const std::exception& e) {
    std::cerr << "01_edge_detection: " << e.what() << std::endl;
    return 1;
}
