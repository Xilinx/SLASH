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
 * rp1_bringup_vrt — the diamond stage of `examples/rp1_bringup`, but
 * built on top of `vrt::graph::Graph` and `vrt::graph::FpgaDevice`
 * instead of hand-laid `rp1_node_t` packets.  This is the canonical
 * regression test for the phase-1 VRT -> RP1 integration on real
 * silicon.
 *
 * Topology:
 *
 *     A → {B, C} → D  (+ auto-generated sentinel SIGNAL → slot 0)
 *
 * Pass iff:
 *   - signal slot 0 reads back 0xD1A1D0DD after the compiled graph run returns;
 *   - exactly 5 CQ entries land (4 kernel dispatches + 1 sentinel signal).
 *
 * Usage:
 *
 *     ./rp1_bringup_vrt [--socket /run/vrtd.sock] [--bdf 0000:65:00.0]
 *
 * Defaults match the production vrtd socket and pick device index 0.
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <slash/uapi/rp1_protocol.h>

#include <vrtd/bar.hpp>
#include <vrtd/bar_file.hpp>
#include <vrtd/device.hpp>
#include <vrtd/session.hpp>

#include <vrt/graph/device/fpga/rp1_bar_window.hpp>
#include <vrt/graph/device/fpga_device.hpp>
#include <vrt/graph/graph.hpp>
#include <vrt/graph/node/io_map.hpp>
#include <vrt/graph/node/io_type_map.hpp>
#include <vrt/graph/node/kernel_descriptor.hpp>

using vrt::graph::FpgaDevice;
using vrt::graph::FpgaKernelLocation;
using vrt::graph::Graph;
using vrt::graph::IOMap;
using vrt::graph::IOTypeMap;
using vrt::graph::KernelDescriptor;
using vrt::graph::fpga::Rp1BarWindow;

// ---------------------------------------------------------------------------
// Diamond test config.
//
// The four kernels are the four instances created by config.cfg's
// `nk=bringup_kernel:4` line.  The linker places them at 64 KiB-aligned
// addresses in alphabetical instance order starting at host
// 0x0202_0000_0000 (see linker/src/emit/hw/tcl_gen.py + addr_ctx.py).
// On the R5 side they show up at the addresses below; verify against
// `system_map.xml` next to your built vbin if you fork the kernel
// signature or instance count.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kKernelA_R5 = 0x88000000u;  // bringup_kernel_0
constexpr std::uint32_t kKernelB_R5 = 0x88010000u;  // bringup_kernel_1
constexpr std::uint32_t kKernelC_R5 = 0x88020000u;  // bringup_kernel_2
constexpr std::uint32_t kKernelD_R5 = 0x88030000u;  // bringup_kernel_3

constexpr std::uint32_t kSentinelSlot  = 0u;
constexpr std::uint32_t kSentinelMagic = 0xD1A1D0DDu;
constexpr int           kPollTimeoutMs = 3000;

// Shared arg list, three 32-bit zeros — matches DIAMOND_ARGS in the C tool.
// All four kernels share the same AXI-Lite signature (e.g. four
// `00_axilite/increment(size=0, in*)` instances).
constexpr std::uint32_t kSharedArg0 = 0u;
constexpr std::uint64_t kSharedAddr = 0ull;

namespace {

struct Cli {
    std::string socket = "/run/vrtd.sock";
    int         device_index = 0;
    std::string bdf;          // optional; if non-empty, takes precedence
};

Cli parseArgs(int argc, char** argv) {
    Cli c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag) -> std::string {
            if (++i >= argc) {
                throw std::runtime_error(std::string("missing argument to ") + flag);
            }
            return argv[i];
        };
        if (a == "--socket")              c.socket = need("--socket");
        else if (a == "--bdf")            c.bdf    = need("--bdf");
        else if (a == "--device-index")   c.device_index = std::stoi(need("--device-index"));
        else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: rp1_bringup_vrt [--socket PATH] [--bdf BDF | --device-index N]\n"
                << "\n"
                << "Submits the canonical diamond DAG (A -> {B,C} -> D + sentinel)\n"
                << "via vrt::graph::Graph + FpgaDevice.  Pass iff slot " << kSentinelSlot
                << " reads back 0x" << std::hex << kSentinelMagic << std::dec
                << " and 5 CQ entries are emitted.\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown arg: " + a);
        }
    }
    return c;
}

vrtd::Device openDevice(vrtd::Session& sess, const Cli& cli) {
    if (!cli.bdf.empty()) {
        return sess.getDeviceByBdf(cli.bdf);
    }
    return sess.getDevice(cli.device_index);
}

}  // namespace

int main(int argc, char** argv) try {
    const Cli cli = parseArgs(argc, argv);

    vrtd::Session session(cli.socket.c_str());
    vrtd::Device  dev  = openDevice(session, cli);
    std::cout << "[rp1_bringup_vrt] connected to vrtd @ " << cli.socket
              << ", device=" << dev.getName() << " (" << dev.getBdf() << ")"
              << std::endl;

    vrtd::Bar bar4  = dev.getBar(4);
    vrtd::BarFile bf = bar4.openBarFile();

    auto window = std::make_shared<Rp1BarWindow>(std::move(bf));

    // KernelDescriptor::name must match the bitstream instance name (per
    // the convention in vrt/include/vrt/graph/node/kernel_descriptor.hpp:
    // "For FPGA kernels this must match the name used in the system_map /
    // bitstream").  The four entries below are the four instances
    // produced by config.cfg's `nk=bringup_kernel:4`.  The host -> R5
    // address conversion is `r5_addr = xml_addr - 0x0202'0000'0000 +
    // 0x8800'0000`.
    const std::unordered_map<std::string, std::uint32_t> kernelMap{
        {"bringup_kernel_0", kKernelA_R5},
        {"bringup_kernel_1", kKernelB_R5},
        {"bringup_kernel_2", kKernelC_R5},
        {"bringup_kernel_3", kKernelD_R5},
    };
    auto lookup = [&](const std::string& name) -> FpgaKernelLocation {
        auto it = kernelMap.find(name);
        if (it == kernelMap.end()) {
            throw std::runtime_error("rp1_bringup_vrt: unmapped kernel '" + name + "'");
        }
        return FpgaKernelLocation{it->second, /*timeout_cycles=*/0u};
    };

    auto fpga = std::make_shared<FpgaDevice>("fpga:0", window, lookup);
    fpga->setSentinelSlot(kSentinelSlot);
    fpga->setSentinelValue(kSentinelMagic);
    fpga->setWaitTimeout(std::chrono::milliseconds(kPollTimeoutMs));

    // Three shared scalar args — split into a uint32 `size` and a uint64
    // `in_ptr`.  The compiler bakes constants directly into the arg
    // buffer; no global scalars in flight.
    IOTypeMap iot;
    iot.inputScalars.push_back({"size", vrt::graph::ScalarType::U32});
    iot.inputScalars.push_back({"in_ptr", vrt::graph::ScalarType::U64});

    Graph g = Graph::withDefaults();
    g.registerDevice(fpga);
    GraphScalar sharedSize = g.scalarInput<std::uint32_t>("shared_size");
    GraphScalar sharedAddr = g.scalarInput<std::uint64_t>("shared_addr");

    auto bindArgs = [&] {
        IOMap io;
        io.bindInputScalar("size", sharedSize);
        io.bindInputScalar("in_ptr", sharedAddr);
        return io;
    };
    const std::string idA = g.addNode(KernelDescriptor{"bringup_kernel_0",
                                                       vrt::graph::DeviceType::FPGA,
                                                       std::nullopt, iot},
                                       bindArgs(), "fpga:0");
    const std::string idB = g.addNode(KernelDescriptor{"bringup_kernel_1",
                                                       vrt::graph::DeviceType::FPGA,
                                                       std::nullopt, iot},
                                       bindArgs(), "fpga:0", {idA});
    const std::string idC = g.addNode(KernelDescriptor{"bringup_kernel_2",
                                                       vrt::graph::DeviceType::FPGA,
                                                       std::nullopt, iot},
                                       bindArgs(), "fpga:0", {idA});
    g.addNode(KernelDescriptor{"bringup_kernel_3", vrt::graph::DeviceType::FPGA,
                               std::nullopt, iot},
              bindArgs(), "fpga:0", {idB, idC});

    auto exec = g.compile();
    exec.writeScalar(sharedSize, kSharedArg0);
    exec.writeScalar(sharedAddr, kSharedAddr);
    std::cout << "[rp1_bringup_vrt] compiled diamond ("
              << "A=0x" << std::hex << kKernelA_R5
              << " B=0x" << kKernelB_R5
              << " C=0x" << kKernelC_R5
              << " D=0x" << kKernelD_R5 << std::dec << "), submitting..."
              << std::endl;

    const std::uint32_t prior_cq = fpga->submitter()->lastCqStart();
    exec.launch();
    exec.wait();
    const std::uint32_t post_cq = fpga->window()->readCqWriteIdx();
    const std::uint32_t cq_delta = post_cq - prior_cq;

    rp1_signal_slot_t slot{};
    fpga->window()->readSignal(kSentinelSlot, slot);

    bool pass = true;
    if (slot.value != kSentinelMagic) {
        std::cerr << "FAIL: sentinel slot " << kSentinelSlot
                  << " = 0x" << std::hex << slot.value
                  << ", expected 0x" << kSentinelMagic << std::dec
                  << " (cq_delta=" << cq_delta
                  << " - check which kernel stalled)" << std::endl;
        pass = false;
    }
    if (cq_delta != 5u) {
        std::cerr << "FAIL: cq_delta=" << cq_delta
                  << ", expected 5 (4 kernels + sentinel signal)" << std::endl;
        pass = false;
    }
    if (!pass) {
        rp1_ctrl_t ctrl{};
        fpga->window()->readCtrl(ctrl);
        std::cerr << "  rp1_state        = " << ctrl.rp1_state << "\n"
                  << "  rp1_error_code   = " << ctrl.rp1_error_code << "\n"
                  << "  rp1_current_node = " << ctrl.rp1_current_node << "\n"
                  << "  cq_write_idx     = " << ctrl.cq_write_idx << "\n"
                  << "  heartbeat        = " << ctrl.heartbeat << std::endl;
        return 1;
    }

    std::cout << "PASS: slot[" << kSentinelSlot << "]=0x"
              << std::hex << slot.value << std::dec
              << " cq_delta=" << cq_delta
              << " state=" << fpga->window()->readState() << std::endl;
    return 0;
} catch (const std::exception& e) {
    std::cerr << "rp1_bringup_vrt: " << e.what() << std::endl;
    return 1;
}
