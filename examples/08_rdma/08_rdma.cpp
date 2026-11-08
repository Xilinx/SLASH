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

// RDMA throughput between the V80 and a peer over a range of message sizes,
// like perftest's -a: for each size, data sent by the V80 (its WRITEs, the
// peer's READs) and received by it (the peer's WRITEs, its READs), each
// checked by the end that receives the data. The peer is set up over the
// sideband of docs/rdma-sideband.md and runs its half of the tests on request.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/rdma.hpp>

namespace {

struct Options {
    std::string bdf;
    std::string vrtbin;
    std::string ip = "192.168.0.1";
    std::string peerIp;
    std::string peerHost;          // empty: server
    uint16_t port = vrt::Rdma::SIDEBAND_PORT;
    uint32_t count = 2000;
    uint32_t minSize = 128;
    uint32_t maxSize = 1u << 20;
    uint64_t window = 256ull << 20;
    bool program = true;
    std::string pattern = "auto";   // seq, addr or auto
};

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " <BDF> <vrtbin file> [options]\n"
        << "  --ip <addr>          RoCE IPv4 address of the V80 (default 192.168.0.1)\n"
        << "  --peer-ip <addr>     the peer's RoCE address, checked when it connects\n"
        << "  --connect <host>     connect to the peer's sideband (default: listen for it)\n"
        << "  --port <n>           sideband TCP port (default 18515)\n"
        << "  --size <bytes>       one message size only\n"
        << "  --min <bytes>        smallest message size of the sweep (default 128)\n"
        << "  --max <bytes>        largest message size of the sweep, doubling (default 1 MiB)\n"
        << "  --count <n>          messages per size and test (default 2000)\n"
        << "  --window <bytes>     memory each test spans at most, which caps the count\n"
        << "                       of large messages (default 256 MiB)\n"
        << "  --pattern <p>        seq: word k is seed + k (docs/rdma-sideband.md);\n"
        << "                       addr: the 64-bit word at peer address a is (seed << 47) | a[46:0]\n"
        << "                       (docs/rdma-sideband.md); default: addr when the peer\n"
        << "                       announces separate MRs in and out, else seq\n"
        << "  --no-program         use the image already on the card\n"
        << "Sizes are multiples of 8 bytes.\n";
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
        } else if (a == "--pattern") {
            o.pattern = value();
            if (o.pattern != "seq" && o.pattern != "addr" && o.pattern != "auto") {
                usage(argv[0]);
                std::exit(1);
            }
        } else if (a == "--no-program") {
            o.program = false;
        } else {
            usage(argv[0]);
            std::exit(1);
        }
    }
    if (o.count == 0 || o.minSize == 0 || o.minSize % 8 || o.maxSize < o.minSize ||
        o.window < o.maxSize) {
        std::cerr << "--count must be positive, sizes positive multiples of 8 with --min <= "
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

// the addr pattern: the 64-bit word at peer address a is (seed << 47) | the
// low 47 bits of a
uint64_t addrWord(uint32_t seed, uint64_t a) {
    return (static_cast<uint64_t>(seed) << 47) | (a & ((1ull << 47) - 1));
}

void fillAddr(uint32_t* p, size_t words, uint32_t seed, uint64_t base) {
    auto* q = reinterpret_cast<uint64_t*>(p);
    for (size_t i = 0; i < words / 2; i++) {
        q[i] = addrWord(seed, base + 8 * i);
    }
}

vrt::RdmaCheckResult checkAddr(const uint32_t* p, size_t words, uint32_t seed, uint64_t base) {
    vrt::RdmaCheckResult r;
    const auto* q = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < words / 2; i++) {
        uint64_t want = addrWord(seed, base + 8 * i);
        for (unsigned h = 0; h < 2; h++) {
            if (static_cast<uint32_t>(q[i] >> (32 * h)) != static_cast<uint32_t>(want >> (32 * h))) {
                if (r.errors++ == 0) {
                    r.firstError = static_cast<int64_t>(8 * i + 4 * h);
                }
            }
        }
    }
    return r;
}

// one test of one size
struct Result {
    uint64_t ns = 0;
    uint64_t retrans = 0;
    vrt::RdmaCheckResult check;
};

double gbps(uint64_t bytes, uint64_t ns) { return ns ? 8.0 * bytes / ns : 0.0; }

}  // namespace

int main(int argc, char* argv[]) {
    Options opt = parse(argc, argv);
    std::vector<uint32_t> sizes;
    for (uint64_t size = opt.minSize; size <= opt.maxSize; size *= 2) {
        sizes.push_back(static_cast<uint32_t>(size));
    }
    const uint64_t bufBytes =
        std::min<uint64_t>(opt.window, static_cast<uint64_t>(opt.count) * opt.maxSize);
    try {
        vrt::Device device(opt.bdf, opt.vrtbin, opt.program);
        vrt::Rdma rdma = device.getRdmaDevice();
        vrt::Buffer<uint32_t> buf(device, bufBytes / 4, vrt::MemoryRangeType::DDR);

        rdma.setLocalIp(opt.ip);
        if (!opt.peerIp.empty()) {
            rdma.setRemoteIp(opt.peerIp);
        }
        if (!opt.peerHost.empty()) {
            rdma.setRole(vrt::Rdma::Role::Client);
            rdma.setRemoteHost(opt.peerHost);
        }
        rdma.setPort(opt.port);
        rdma.addMemory(buf, bufBytes);    // our MR 0: buf, both in and out
        std::cout << "RDMA service build " << rdma.getBuild() << ", " << opt.ip << ", "
                  << (opt.peerHost.empty() ? "waiting for the peer on port "
                                           : "connecting to " + opt.peerHost + ":")
                  << opt.port << std::endl;
        rdma.connect();

        // the peer's MR 0 takes the data that reaches it; a peer that announces
        // a second MR sends its data from there (it checks and makes the data in
        // hardware, with the addr pattern)
        const auto& peerMemory = rdma.remoteMemory();
        if (peerMemory.empty()) {
            throw std::runtime_error("the peer announced no memory");
        }
        const unsigned sinkMr = 0;
        const unsigned sourceMr = peerMemory.size() > 1 ? 1 : 0;
        for (unsigned mr : {sinkMr, sourceMr}) {
            if (peerMemory[mr].len < bufBytes) {
                throw std::runtime_error("the peer's MR " + std::to_string(mr) +
                                         " is smaller than " + std::to_string(bufBytes) +
                                         " bytes");
            }
        }
        const uint64_t sinkAddr = peerMemory[sinkMr].addr;
        const uint64_t sourceAddr = peerMemory[sourceMr].addr;
        // a separate MR in checks the data as it arrives: FILL it with the
        // seed first (in plain memory that FILL would hide lost WRITEs)
        const bool armSink = sinkMr != sourceMr;
        const bool addrPattern = opt.pattern == "addr" || (opt.pattern == "auto" && armSink);
        const uint32_t qp = rdma.qp(0);
        std::printf("connected to %s, peer memory 0x%llx (in) 0x%llx (out), pattern %s\n",
                    rdma.remoteIp().c_str(), static_cast<unsigned long long>(sinkAddr),
                    static_cast<unsigned long long>(sourceAddr),
                    addrPattern ? "addr" : "seq");

        // 17-bit seeds, which the addr pattern keeps above bit 46
        std::mt19937 rng(std::random_device{}());
        auto newSeed = [&]() { return static_cast<uint32_t>(rng() & 0x1FFFF); };
        auto retransmissions = [&]() { return rdma.stats().stack[9]; };
        std::vector<std::string> notes;   // check failures, printed after the table

        // the data as the peer sees it at peer address base
        auto fill = [&](size_t words, uint32_t seed, uint64_t base) {
            if (addrPattern) {
                fillAddr(buf.get(), words, seed, base);
            } else {
                vrt::Rdma::fillPattern(buf.get(), words, seed);
            }
            buf.sync(vrt::SyncType::HOST_TO_DEVICE);
        };
        // what buf holds before the data arrives: wrong in every word
        auto clear = [&](size_t words, uint32_t seed) {
            vrt::Rdma::fillPattern(buf.get(), words, ~seed);
            buf.sync(vrt::SyncType::HOST_TO_DEVICE);
        };
        auto check = [&](const std::string& test, size_t words, uint32_t seed, uint64_t base) {
            buf.sync(vrt::SyncType::DEVICE_TO_HOST);
            auto r = addrPattern ? checkAddr(buf.get(), words, seed, base)
                                 : vrt::Rdma::checkPattern(buf.get(), words, seed);
            if (r.errors) {
                // the first wrong 64-bit words, received / expected
                size_t i0 = static_cast<size_t>(r.firstError) / 8;
                const auto* q = reinterpret_cast<const uint64_t*>(buf.get());
                for (size_t i = i0; i < i0 + 4 && i < words / 2; i++) {
                    uint64_t want = addrPattern
                                        ? addrWord(seed, base + 8 * i)
                                        : (static_cast<uint64_t>(seed + 2 * i + 1) << 32) |
                                              static_cast<uint32_t>(seed + 2 * i);
                    char line[128];
                    std::snprintf(line, sizeof(line),
                                  "%s: byte 0x%06zx got 0x%016llx expected 0x%016llx",
                                  test.c_str(), 8 * i, static_cast<unsigned long long>(q[i]),
                                  static_cast<unsigned long long>(want));
                    notes.push_back(line);
                }
            }
            return r;
        };
        auto peerChecked = [&](const std::string& test, uint64_t bytes, uint32_t seed) {
            auto r = rdma.peerCheck(sinkMr, 0, bytes, seed);
            if (r.errors) {
                notes.push_back(test + ": " + std::to_string(r.errors) +
                                " words wrong, first at byte " + std::to_string(r.firstError) +
                                " (see the peer's log)");
            }
            return r;
        };
        // the V80 posts count messages and waits for them
        auto post = [&](bool read, uint32_t count, uint32_t size, uint64_t remote) {
            uint64_t bytes = static_cast<uint64_t>(count) * size;
            uint32_t retrans0 = retransmissions();
            auto t0 = std::chrono::steady_clock::now();
            for (uint32_t k = 0; k < count; k++) {
                uint64_t off = static_cast<uint64_t>(k) * size;
                if (read) {
                    rdma.read(qp, buf, off, remote + off, size);
                } else {
                    rdma.write(qp, buf, off, remote + off, size);
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
                static_cast<uint32_t>(std::min<uint64_t>(opt.count, bufBytes / size));
            const uint64_t bytes = static_cast<uint64_t>(count) * size;
            const size_t words = bytes / 4;
            const std::string at = std::to_string(size) + " B ";
            std::array<Result, 4> r;

            // V80 WRITE: buf -> peer, checked by the peer
            uint32_t seed = newSeed();
            fill(words, seed, sinkAddr);
            if (armSink) {
                rdma.peerFill(sinkMr, 0, bytes, seed);
            }
            r[0] = post(false, count, size, sinkAddr);
            r[0].check = peerChecked(at + "V80 WRITE", bytes, seed);

            // peer READ: buf -> peer, checked by the peer
            seed = newSeed();
            fill(words, seed, sinkAddr);
            if (armSink) {
                rdma.peerFill(sinkMr, 0, bytes, seed);
            }
            auto run = rdma.peerRead(0, count, size, sinkAddr, buf.getPhysAddr());
            r[1].ns = run.ns;
            r[1].retrans = run.retrans;
            r[1].check = peerChecked(at + "peer READ", bytes, seed);

            // peer WRITE: peer -> buf, checked here
            seed = newSeed();
            rdma.peerFill(sourceMr, 0, bytes, seed);
            clear(words, seed);
            run = rdma.peerWrite(0, count, size, sourceAddr, buf.getPhysAddr());
            r[2].ns = run.ns;
            r[2].retrans = run.retrans;
            r[2].check = check(at + "peer WRITE", words, seed, sourceAddr);

            // V80 READ: peer -> buf, checked here
            seed = newSeed();
            rdma.peerFill(sourceMr, 0, bytes, seed);
            clear(words, seed);
            r[3] = post(true, count, size, sourceAddr);
            r[3].check = check(at + "V80 READ", words, seed, sourceAddr);

            uint64_t retrans = 0, errors = 0;
            for (const auto& x : r) {
                retrans += x.retrans;
                errors += x.check.errors;
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
