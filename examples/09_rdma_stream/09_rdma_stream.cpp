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

// RDMA throughput between the V80's user region and a peer over a range of
// message sizes: rdma_gen feeds the RDMA service's stream (rdma.tx), so the
// V80 sends without card memory (its WRITEs, the peer's READs), and
// rdma_check takes what it receives (rdma.rx: the peer's WRITEs, its READs).
// Both kernels make and check the address pattern, the 64-bit word at peer
// address a being (seed << 47) | a[46:0], so every transfer is checked by
// the end that receives it. The peer is set up over the sideband of
// docs/rdma-sideband.md and runs its half of the tests on request.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/rdma.hpp>

namespace {

// register maps of the kernels (HLS, s_axilite bundle control)
namespace gen_reg {
constexpr uint32_t SEED = 0x10, BASE = 0x18, MSG_BYTES = 0x24, TOTAL_BYTES = 0x2c, EPOCH = 0x38,
                   SENT = 0x40;
}
namespace check_reg {
constexpr uint32_t SEED = 0x10, BASE = 0x18, EPOCH = 0x24, BYTES = 0x2c, ERRORS = 0x44,
                   FIRST_ERROR = 0x5c, MSGS = 0x74;
}

// Both RDMA stacks post a message as requests of at most one PMTU, each an RDMA
// operation of its own on the wire (WRITE ONLY, READ REQUEST), and rdma.rx ends
// each operation it receives with tlast: rdma_check counts one per PMTU.
constexpr uint32_t PMTU = 4096;
uint32_t rxMessages(uint32_t count, uint32_t size) {
    return count * ((size + PMTU - 1) / PMTU);
}

struct Options {
    std::string bdf;
    std::string vrtbin;
    std::string ip = "192.168.0.1";
    std::string peerIp;
    std::string peerHost;          // empty: server
    uint16_t port = vrt::Rdma::SIDEBAND_PORT;
    uint32_t count = 2000;
    uint32_t minSize = 4096;
    uint32_t maxSize = 1u << 20;
    uint64_t window = 1ull << 30;
    bool program = true;
};

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " <BDF> <vrtbin file> [options]\n"
        << "  --ip <addr>          RoCE IPv4 address of the V80 (default 192.168.0.1)\n"
        << "  --peer-ip <addr>     the peer's RoCE address, checked when it connects\n"
        << "  --connect <host>     connect to the peer's sideband (default: listen for it)\n"
        << "  --port <n>           sideband TCP port (default 18515)\n"
        << "  --size <bytes>       one message size only\n"
        << "  --min <bytes>        smallest message size of the sweep (default 4k)\n"
        << "  --max <bytes>        largest message size of the sweep, doubling (default 1M)\n"
        << "  --count <n>          messages per size and test (default 2000)\n"
        << "  --window <bytes>     bytes per test at most, which caps the count of large\n"
        << "                       messages (default 1G)\n"
        << "  --no-program         use the image already on the card\n"
        << "Sizes are multiples of 128 bytes.\n";
}

uint64_t parseBytes(const std::string& v) {
    size_t n = 0;
    uint64_t x = std::stoull(v, &n, 0);
    std::string unit = v.substr(n);
    if (unit == "k" || unit == "K") {
        x <<= 10;
    } else if (unit == "m" || unit == "M") {
        x <<= 20;
    } else if (unit == "g" || unit == "G") {
        x <<= 30;
    } else if (!unit.empty()) {
        throw std::invalid_argument("bad size '" + v + "'");
    }
    return x;
}

Options parse(int argc, char* argv[]) {
    if (argc < 3) {
        usage(argv[0]);
        std::exit(1);
    }
    Options o;
    o.bdf = argv[1];
    o.vrtbin = argv[2];
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "--ip") {
            o.ip = value();
        } else if (a == "--peer-ip") {
            o.peerIp = value();
        } else if (a == "--connect") {
            o.peerHost = value();
        } else if (a == "--port") {
            o.port = static_cast<uint16_t>(std::stoul(value()));
        } else if (a == "--size") {
            o.minSize = o.maxSize = static_cast<uint32_t>(parseBytes(value()));
        } else if (a == "--min") {
            o.minSize = static_cast<uint32_t>(parseBytes(value()));
        } else if (a == "--max") {
            o.maxSize = static_cast<uint32_t>(parseBytes(value()));
        } else if (a == "--count") {
            o.count = static_cast<uint32_t>(std::stoul(value()));
        } else if (a == "--window") {
            o.window = parseBytes(value());
        } else if (a == "--no-program") {
            o.program = false;
        } else {
            usage(argv[0]);
            std::exit(1);
        }
    }
    if (o.count == 0 || o.minSize == 0 || o.minSize % 128 || o.maxSize < o.minSize ||
        o.window < o.maxSize) {
        std::cerr << "--count must be positive, sizes positive multiples of 128 with --min <= "
                     "--max <= --window\n";
        std::exit(1);
    }
    return o;
}

uint64_t nsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                t0)
        .count();
}

double gbps(uint64_t bytes, uint64_t ns) { return ns ? 8.0 * bytes / ns : 0.0; }

// 64-bit kernel registers are two words, not latched: HI, LO, HI until the
// two HI reads agree
uint64_t read64(vrt::Kernel& k, uint32_t off) {
    for (;;) {
        uint32_t hi = k.read(off + 4);
        uint32_t lo = k.read(off);
        if (k.read(off + 4) == hi) {
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }
    }
}

void write64(vrt::Kernel& k, uint32_t off, uint64_t v) {
    k.write(off, static_cast<uint32_t>(v));
    k.write(off + 4, static_cast<uint32_t>(v >> 32));
}

// one test of one size
struct Result {
    uint64_t ns = 0;
    uint64_t retrans = 0;
    uint64_t errors = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
    Options opt = parse(argc, argv);
    std::vector<uint32_t> sizes;
    for (uint64_t size = opt.minSize; size <= opt.maxSize; size *= 2) {
        sizes.push_back(static_cast<uint32_t>(size));
    }
    try {
        vrt::Device device(opt.bdf, opt.vrtbin, opt.program);
        vrt::Rdma rdma = device.getRdmaDevice();
        vrt::Kernel gen = device.getKernel("rdma_gen_0");
        vrt::Kernel check = device.getKernel("rdma_check_0");

        rdma.setLocalIp(opt.ip);
        if (!opt.peerIp.empty()) {
            rdma.setRemoteIp(opt.peerIp);
        }
        if (!opt.peerHost.empty()) {
            rdma.setRole(vrt::Rdma::Role::Client);
            rdma.setRemoteHost(opt.peerHost);
        }
        rdma.setPort(opt.port);
        // our MR 0: the stream, both ways
        rdma.addMemory(vrt::Rdma::STREAM, opt.window);
        std::cout << "RDMA service build " << rdma.getBuild() << ", " << opt.ip << ", "
                  << (opt.peerHost.empty() ? "waiting for the peer on port "
                                           : "connecting to " + opt.peerHost + ":")
                  << opt.port << std::endl;
        rdma.connect();

        // the peer's MR 0 takes the data that reaches it, MR 1 (if any) sends
        // it; the peer has to make and check the address pattern
        const auto& peerMemory = rdma.remoteMemory();
        if (peerMemory.empty()) {
            throw std::runtime_error("the peer announced no memory");
        }
        const unsigned sinkMr = 0;
        const unsigned sourceMr = peerMemory.size() > 1 ? 1 : 0;
        const uint64_t sinkAddr = peerMemory[sinkMr].addr;
        const uint64_t sourceAddr = peerMemory[sourceMr].addr;
        const bool armSink = sinkMr != sourceMr;
        const uint32_t qp = rdma.qp(0);
        std::printf("connected to %s, peer memory 0x%llx (in) 0x%llx (out)\n",
                    rdma.remoteIp().c_str(), static_cast<unsigned long long>(sinkAddr),
                    static_cast<unsigned long long>(sourceAddr));

        std::mt19937 rng(std::random_device{}());
        auto newSeed = [&]() { return static_cast<uint32_t>(rng() & 0x1FFFF); };
        uint32_t epoch = rng() | 1;
        auto retransmissions = [&]() { return rdma.stats().stack[9]; };
        std::vector<std::string> notes;   // check failures, printed after the table

        // rdma_gen: bytes of the pattern for peer addresses base.. in messages
        // of size; stopped (total 0) while it is set up
        auto armGen = [&](uint32_t seed, uint64_t base, uint32_t size, uint64_t bytes) {
            write64(gen, gen_reg::TOTAL_BYTES, 0);
            gen.write(gen_reg::SEED, seed);
            write64(gen, gen_reg::BASE, base);
            gen.write(gen_reg::MSG_BYTES, size);
            gen.write(gen_reg::EPOCH, ++epoch);
            write64(gen, gen_reg::TOTAL_BYTES, bytes);
        };
        // rdma_check: starts over with the next beat it takes
        auto armCheck = [&](uint32_t seed, uint64_t base) {
            check.write(check_reg::SEED, seed);
            write64(check, check_reg::BASE, base);
            check.write(check_reg::EPOCH, ++epoch);
        };
        // wait until rdma_check has the bytes, then its errors
        auto checked = [&](const std::string& test, uint64_t bytes, uint32_t msgs) {
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            uint64_t got = 0;
            while ((got = read64(check, check_reg::BYTES)) < bytes &&
                   std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            uint64_t errors = read64(check, check_reg::ERRORS);
            uint32_t m = check.read(check_reg::MSGS);
            if (got != bytes || m != msgs) {
                notes.push_back(test + ": rdma.rx took " + std::to_string(got) + " bytes in " +
                                std::to_string(m) + " messages, expected " +
                                std::to_string(bytes) + " in " + std::to_string(msgs));
                errors += got < bytes ? (bytes - got) / 8 : 1;
            }
            if (errors) {
                notes.push_back(test + ": " + std::to_string(errors) +
                                " words wrong, first at byte " +
                                std::to_string(read64(check, check_reg::FIRST_ERROR)));
            }
            return errors;
        };
        auto peerChecked = [&](const std::string& test, uint64_t bytes, uint32_t seed) {
            auto r = rdma.peerCheck(sinkMr, 0, bytes, seed);
            if (r.errors) {
                notes.push_back(test + ": " + std::to_string(r.errors) +
                                " words wrong at the peer, first at byte " +
                                std::to_string(r.firstError));
            }
            return r.errors;
        };
        auto genSent = [&](const std::string& test, uint64_t bytes) {
            uint64_t sent = read64(gen, gen_reg::SENT);
            if (sent != bytes) {
                notes.push_back(test + ": rdma_gen sent " + std::to_string(sent) +
                                " bytes, expected " + std::to_string(bytes));
                return uint64_t{1};
            }
            return uint64_t{0};
        };
        // the V80 posts count messages from or to the stream and waits for them
        auto post = [&](bool read, uint32_t count, uint32_t size, uint64_t remote) {
            uint64_t bytes = static_cast<uint64_t>(count) * size;
            uint32_t retrans0 = retransmissions();
            auto t0 = std::chrono::steady_clock::now();
            for (uint32_t k = 0; k < count; k++) {
                uint64_t off = static_cast<uint64_t>(k) * size;
                if (read) {
                    rdma.readToStream(qp, remote + off, size);
                } else {
                    rdma.writeFromStream(qp, remote + off, size);
                }
            }
            if (!rdma.wait(std::chrono::seconds(10 + bytes / 1'000'000'000))) {
                throw std::runtime_error(std::string("V80 ") + (read ? "READs" : "WRITEs") +
                                         " of " + std::to_string(size) +
                                         " B did not complete");
            }
            Result r;
            r.ns = nsSince(t0);
            r.retrans = retransmissions() - retrans0;
            return r;
        };

        std::printf("\n%10s %7s | %-21s | %-21s | %7s %8s\n", "", "", "  V80 sends (Gb/s)",
                    "  V80 receives (Gb/s)", "", "");
        std::printf("%10s %7s | %10s %10s | %10s %10s | %7s %8s\n", "bytes", "count",
                    "V80 WRITE", "peer READ", "peer WRITE", "V80 READ", "retrans", "errors");
        bool pass = true;
        for (uint32_t size : sizes) {
            const uint32_t count =
                static_cast<uint32_t>(std::min<uint64_t>(opt.count, opt.window / size));
            const uint64_t bytes = static_cast<uint64_t>(count) * size;
            const std::string at = std::to_string(size) + " B ";
            std::array<Result, 4> r;

            // V80 WRITE: rdma.tx -> peer, checked by the peer
            uint32_t seed = newSeed();
            if (armSink) {
                rdma.peerFill(sinkMr, 0, bytes, seed);
            }
            armGen(seed, sinkAddr, size, bytes);
            r[0] = post(false, count, size, sinkAddr);
            r[0].errors = peerChecked(at + "V80 WRITE", bytes, seed) +
                          genSent(at + "V80 WRITE", bytes);

            // peer READ: rdma.tx -> peer, checked by the peer
            seed = newSeed();
            if (armSink) {
                rdma.peerFill(sinkMr, 0, bytes, seed);
            }
            armGen(seed, sinkAddr, size, bytes);
            auto run = rdma.peerRead(0, count, size, sinkAddr, vrt::Rdma::STREAM);
            r[1].ns = run.ns;
            r[1].retrans = run.retrans;
            r[1].errors = peerChecked(at + "peer READ", bytes, seed) +
                          genSent(at + "peer READ", bytes);

            // peer WRITE: peer -> rdma.rx, checked here
            seed = newSeed();
            rdma.peerFill(sourceMr, 0, bytes, seed);
            armCheck(seed, sourceAddr);
            run = rdma.peerWrite(0, count, size, sourceAddr, vrt::Rdma::STREAM);
            r[2].ns = run.ns;
            r[2].retrans = run.retrans;
            r[2].errors = checked(at + "peer WRITE", bytes, rxMessages(count, size));

            // V80 READ: peer -> rdma.rx, checked here
            seed = newSeed();
            rdma.peerFill(sourceMr, 0, bytes, seed);
            armCheck(seed, sourceAddr);
            r[3] = post(true, count, size, sourceAddr);
            r[3].errors = checked(at + "V80 READ", bytes, rxMessages(count, size));

            uint64_t retrans = 0, errors = 0;
            for (const auto& x : r) {
                retrans += x.retrans;
                errors += x.errors;
            }
            pass &= errors == 0;
            std::printf("%10u %7u | %10.2f %10.2f | %10.2f %10.2f | %7llu %8llu\n", size, count,
                        gbps(bytes, r[0].ns), gbps(bytes, r[1].ns), gbps(bytes, r[2].ns),
                        gbps(bytes, r[3].ns), static_cast<unsigned long long>(retrans),
                        static_cast<unsigned long long>(errors));
            std::fflush(stdout);
        }
        std::printf("V80 WRITE / READ: posted by the V80 host, one register write per message;\n"
                    "peer WRITE / READ: posted by the peer, timed by the peer.\n");
        for (const auto& n : notes) {
            std::printf("  %s\n", n.c_str());
        }

        auto s = rdma.stats();
        std::printf("V80:  psn_drops %u retrans %u rx_fcs_drops %u rx_room_drops %u\n", s.stack[8],
                    s.stack[9], s.rxFcsDrops, s.rxRoomDrops);
        std::printf("peer:");
        for (const auto& [key, value] : rdma.peerStats()) {
            std::printf(" %s %s", key.c_str(), value.c_str());
        }
        std::printf("\n%s\n", pass ? "PASS" : "FAIL");

        rdma.close();
        device.cleanup();
        return pass ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
