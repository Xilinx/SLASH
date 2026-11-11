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

// RDMA throughput of a design with two RDMA cores, one on each DCMAC port
// (eth_0 and eth_2), each with its own peer. On each core in turn, the tests
// of 08_rdma: for each message size, data sent by the V80 (its WRITEs, the
// peer's READs) and received by it (the peer's WRITEs, its READs), each
// checked by the end that receives the data. Each peer is set up over the
// sideband of docs/rdma-sideband.md and runs its half of the tests on request.
//
// With --pair, QSFP0 is cabled to QSFP2 and the two cores are each other's
// peer. The program drives both ends itself, through the stack registers and
// without a sideband: each core WRITEs into the other's memory and READs from
// it, then both WRITE at the same time. All the buffers are in card memory, so
// every transfer is checked here. On one card, though, every byte is read from
// and written to the same DDR, which caps the rate well below the link's.
//
// With --pair --stream the same tests run between the user-region streams
// instead: rdma_gen_0 feeds rdma_0.tx and rdma_check_0 takes rdma_0.rx, and
// _1 does the same for rdma_2 (config.cfg). No card memory is involved, and
// rdma_check on the receiving core checks every transfer.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrt/rdma.hpp>

namespace {

// the settings of one core's session
struct CoreOptions {
    std::string ip;
    std::string peerIp;
    std::string peerHost;          // empty: server
    uint16_t port = vrt::Rdma::SIDEBAND_PORT;
};

struct Options {
    std::string bdf;
    std::string vrtbin;
    // one entry per core, in port order; a single entry is used for every core
    std::vector<std::string> ips;
    std::vector<std::string> peerIps;
    std::vector<std::string> peerHosts;
    std::vector<std::string> ports;
    std::vector<uint32_t> eths;    // the cores to test; empty: all
    uint32_t count = 2000;
    uint32_t minSize = 128;
    uint32_t maxSize = 1u << 20;
    uint64_t window = 256ull << 20;
    bool program = true;
    std::string pattern = "auto";   // seq, addr or auto
    bool pair = false;              // the two cores to each other
    bool stream = false;            // --pair between the user-region streams
};

void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " <BDF> <vrtbin file> [options]\n"
        << "The options of a core's session take a comma-separated list, one value per\n"
        << "core in port order (eth_0, eth_2), or one value for every core:\n"
        << "  --ip <addr,...>       RoCE IPv4 address of the V80 (default 192.168.<n>.1 on eth_<n>)\n"
        << "  --peer-ip <addr,...>  the peer's RoCE address, checked when it connects\n"
        << "  --connect <host,...>  connect to the peer's sideband (default: listen for it)\n"
        << "  --port <n,...>        sideband TCP port (default 18515)\n"
        << "Others:\n"
        << "  --eth <n>            test only the core on eth_<n> (repeatable)\n"
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
        << "  --pair               QSFP0 cabled to QSFP2: the two cores test each other, with\n"
        << "                       no peer and no sideband; --ip gives their addresses\n"
        << "                       (default 192.168.0.1,192.168.0.2, the same /24)\n"
        << "  --stream             with --pair: between the user-region streams, through\n"
        << "                       rdma_gen_0/1 and rdma_check_0/1, not card memory\n"
        << "The cores are tested one after the other. Sizes are multiples of 8 bytes,\n"
        << "of 128 with --stream.\n";
}

std::vector<std::string> splitList(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(item);
    }
    return out;
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
            o.ips = splitList(value());
        } else if (a == "--peer-ip") {
            o.peerIps = splitList(value());
        } else if (a == "--connect") {
            o.peerHosts = splitList(value());
        } else if (a == "--port") {
            o.ports = splitList(value());
        } else if (a == "--eth") {
            o.eths.push_back(static_cast<uint32_t>(std::stoul(value())));
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
        } else if (a == "--pair") {
            o.pair = true;
        } else if (a == "--stream") {
            o.stream = true;
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
    if (o.pair && (!o.peerIps.empty() || !o.peerHosts.empty() || !o.ports.empty() ||
                   !o.eths.empty())) {
        std::cerr << "--pair takes no --peer-ip, --connect, --port or --eth\n";
        std::exit(1);
    }
    if (o.stream && (!o.pair || o.minSize % 128)) {
        std::cerr << "--stream needs --pair and sizes that are multiples of 128\n";
        std::exit(1);
    }
    return o;
}

// entry i of a per-core list: the only entry when there is one, else
// fallback when the list is empty
std::string pick(const std::vector<std::string>& list, size_t i, const std::string& name,
                 const std::string& fallback) {
    if (list.empty()) {
        return fallback;
    }
    if (list.size() == 1) {
        return list[0];
    }
    if (i >= list.size()) {
        throw std::invalid_argument(name + " has " + std::to_string(list.size()) +
                                    " values, fewer than the cores");
    }
    return list[i];
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

// the tests of 08_rdma on the core on eth_<eth>; true when every check passed
bool runCore(vrt::Device& device, uint32_t eth, const CoreOptions& c, const Options& opt,
             const std::vector<uint32_t>& sizes, uint64_t bufBytes) {
    vrt::Rdma rdma = device.getRdmaDevice(eth);
    vrt::Buffer<uint32_t> buf(device, bufBytes / 4, vrt::MemoryRangeType::DDR);

    rdma.setLocalIp(c.ip);
    if (!c.peerIp.empty()) {
        rdma.setRemoteIp(c.peerIp);
    }
    if (!c.peerHost.empty()) {
        rdma.setRole(vrt::Rdma::Role::Client);
        rdma.setRemoteHost(c.peerHost);
    }
    rdma.setPort(c.port);
    rdma.addMemory(buf, bufBytes);    // our MR 0: buf, both in and out
    std::cout << "\n=== eth_" << eth << ": RDMA service build " << rdma.getBuild() << ", "
              << c.ip << ", "
              << (c.peerHost.empty() ? "waiting for the peer on port "
                                     : "connecting to " + c.peerHost + ":")
              << c.port << std::endl;
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
    std::printf("\neth_%u: %s\n", eth, pass ? "PASS" : "FAIL");

    rdma.close();
    return pass;
}

// rdma_gen and rdma_check (examples/09_rdma_stream/hls), HLS s_axilite
// registers
namespace gen_reg {
constexpr uint32_t SEED = 0x10, BASE = 0x18, MSG_BYTES = 0x24, TOTAL_BYTES = 0x2c, EPOCH = 0x38,
                   SENT = 0x40;
}
namespace check_reg {
constexpr uint32_t SEED = 0x10, BASE = 0x18, EPOCH = 0x24, BYTES = 0x2c, ERRORS = 0x44,
                   FIRST_ERROR = 0x5c, MSGS = 0x74;
}
// one kernel's registers; 64-bit ones are two words, not latched
struct Regs {
    vrt::Kernel k;
    uint32_t rd(uint32_t off) { return k.read(off); }
    void wr(uint32_t off, uint32_t v) { k.write(off, v); }
    uint64_t rd64(uint32_t off) {
        for (;;) {
            uint32_t hi = rd(off + 4);
            uint32_t lo = rd(off);
            if (rd(off + 4) == hi) {
                return (static_cast<uint64_t>(hi) << 32) | lo;
            }
        }
    }
    void wr64(uint32_t off, uint64_t v) {
        wr(off, static_cast<uint32_t>(v));
        wr(off + 4, static_cast<uint32_t>(v >> 32));
    }
};

// A message received on rdma.rx ends with tlast once per PMTU (4 KiB): the
// stack posts each request of at most one PMTU as an operation of its own.
uint32_t rxMessages(uint32_t count, uint32_t size) { return count * ((size + 4095) / 4096); }

// --pair --stream, once the queue pairs are up: rdma_gen_<i> feeds core i's
// rdma.tx, rdma_check_<i> takes core i's rdma.rx. Every transfer uses the
// addr pattern from remote address Rdma::STREAM up.
bool pairStream(vrt::Device& device, const std::array<vrt::Rdma*, 2>& rdma,
                const std::array<uint32_t, 2>& eths, uint32_t qpn, std::mt19937& rng,
                const Options& opt, const std::vector<uint32_t>& sizes) {
    std::array<Regs, 2> gen{Regs{device.getKernel("rdma_gen_0")},
                            Regs{device.getKernel("rdma_gen_1")}};
    std::array<Regs, 2> chk{Regs{device.getKernel("rdma_check_0")},
                            Regs{device.getKernel("rdma_check_1")}};
    const uint64_t base = vrt::Rdma::STREAM;
    auto newSeed = [&]() { return static_cast<uint32_t>(rng() & 0x1FFFF); };
    uint32_t epoch = rng() | 1;
    std::vector<std::string> notes;

    // rdma_gen: bytes of the pattern in messages of size, stopped while set up
    auto armGen = [&](unsigned i, uint32_t seed, uint32_t size, uint64_t bytes) {
        gen[i].wr64(gen_reg::TOTAL_BYTES, 0);
        gen[i].wr(gen_reg::SEED, seed);
        gen[i].wr64(gen_reg::BASE, base);
        gen[i].wr(gen_reg::MSG_BYTES, size);
        gen[i].wr(gen_reg::EPOCH, ++epoch);
        gen[i].wr64(gen_reg::TOTAL_BYTES, bytes);
    };
    // rdma_check: starts over with the next beat it takes
    auto armCheck = [&](unsigned i, uint32_t seed) {
        chk[i].wr(check_reg::SEED, seed);
        chk[i].wr64(check_reg::BASE, base);
        chk[i].wr(check_reg::EPOCH, ++epoch);
    };
    // rdma_check_<dst> has all the bytes and rdma_gen_<src> sent them
    auto checked = [&](unsigned src, unsigned dst, const std::string& test, uint64_t bytes,
                       uint32_t msgs) {
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        uint64_t got = 0;
        while ((got = chk[dst].rd64(check_reg::BYTES)) < bytes &&
               std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        uint64_t errors = chk[dst].rd64(check_reg::ERRORS);
        uint32_t m = chk[dst].rd(check_reg::MSGS);
        if (got != bytes || m != msgs) {
            notes.push_back(test + ": rdma_check_" + std::to_string(dst) + " took " +
                            std::to_string(got) + " bytes in " + std::to_string(m) +
                            " messages, expected " + std::to_string(bytes) + " in " +
                            std::to_string(msgs));
            errors += got < bytes ? (bytes - got) / 8 : 1;
        }
        if (errors) {
            notes.push_back(test + ": " + std::to_string(errors) + " words wrong, first at byte " +
                            std::to_string(chk[dst].rd64(check_reg::FIRST_ERROR)));
        }
        uint64_t sent = gen[src].rd64(gen_reg::SENT);
        if (sent != bytes) {
            notes.push_back(test + ": rdma_gen_" + std::to_string(src) + " sent " +
                            std::to_string(sent) + " bytes, expected " + std::to_string(bytes));
            errors++;
        }
        return errors;
    };
    auto retransmissions = [&]() {
        return static_cast<uint64_t>(rdma[0]->stats().stack[9]) + rdma[1]->stats().stack[9];
    };
    // the cores in @p who post count messages each, interleaved, and wait:
    // WRITEs from their rdma.tx, or READs from the other's rdma.tx into their
    // rdma.rx
    auto post = [&](const std::vector<unsigned>& who, bool read, uint32_t count, uint32_t size) {
        const uint64_t bytes = static_cast<uint64_t>(count) * size;
        const uint64_t retrans0 = retransmissions();
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < count; k++) {
            const uint64_t off = static_cast<uint64_t>(k) * size;
            for (unsigned i : who) {
                if (read) {
                    rdma[i]->readToStream(qpn, base + off, size);
                } else {
                    rdma[i]->writeFromStream(qpn, base + off, size);
                }
            }
        }
        for (unsigned i : who) {
            if (!rdma[i]->wait(std::chrono::seconds(10 + bytes / 1'000'000'000))) {
                throw std::runtime_error("eth_" + std::to_string(eths[i]) + " " +
                                         (read ? "READs" : "WRITEs") + " of " +
                                         std::to_string(size) + " B did not complete");
            }
        }
        Result r;
        r.ns = nsSince(t0);
        r.retrans = retransmissions() - retrans0;
        return r;
    };

    std::printf("\n%10s %7s | %-21s | %-21s | %10s | %7s %8s\n", "", "", "  eth_0 -> eth_2",
                "  eth_2 -> eth_0", " both", "", "");
    std::printf("%10s %7s | %10s %10s | %10s %10s | %10s | %7s %8s\n", "bytes", "count",
                "0 WRITE", "2 READ", "2 WRITE", "0 READ", "WRITE", "retrans", "errors");
    bool pass = true;
    for (uint32_t size : sizes) {
        const uint32_t count = opt.count;
        const uint64_t bytes = static_cast<uint64_t>(count) * size;
        const uint32_t msgs = rxMessages(count, size);
        const std::string at = std::to_string(size) + " B ";
        std::array<Result, 5> r;
        uint64_t errors = 0;

        // data from core src's rdma.tx to core dst's rdma.rx, moved by a WRITE
        // of src or a READ of dst
        auto one = [&](unsigned src, unsigned dst, bool read, const std::string& test) {
            uint32_t seed = newSeed();
            armCheck(dst, seed);
            armGen(src, seed, size, bytes);
            Result x = post({read ? dst : src}, read, count, size);
            x.check.errors = checked(src, dst, at + test, bytes, msgs);
            return x;
        };
        r[0] = one(0, 1, false, "eth_0 WRITE");
        r[1] = one(0, 1, true, "eth_2 READ");
        r[2] = one(1, 0, false, "eth_2 WRITE");
        r[3] = one(1, 0, true, "eth_0 READ");

        // both WRITE at once
        const std::array<uint32_t, 2> seeds{newSeed(), newSeed()};
        for (unsigned i = 0; i < 2; i++) {
            armCheck(1 - i, seeds[i]);
            armGen(i, seeds[i], size, bytes);
        }
        r[4] = post({0, 1}, false, count, size);
        r[4].check.errors = checked(0, 1, at + "both WRITE, eth_0's", bytes, msgs) +
                            checked(1, 0, at + "both WRITE, eth_2's", bytes, msgs);

        uint64_t retrans = 0;
        for (const auto& x : r) {
            retrans += x.retrans;
            errors += x.check.errors;
        }
        pass &= errors == 0;
        std::printf("%10u %7u | %10.2f %10.2f | %10.2f %10.2f | %10.2f | %7llu %8llu\n", size,
                    count, gbps(bytes, r[0].ns), gbps(bytes, r[1].ns), gbps(bytes, r[2].ns),
                    gbps(bytes, r[3].ns), gbps(2 * bytes, r[4].ns),
                    static_cast<unsigned long long>(retrans),
                    static_cast<unsigned long long>(errors));
        std::fflush(stdout);
    }
    std::printf("Between the user-region streams, all posted by the host, one register write\n"
                "per message; \"both\" is the sum of the two directions.\n");
    for (const auto& n : notes) {
        std::printf("  %s\n", n.c_str());
    }
    for (unsigned i = 0; i < 2; i++) {
        auto s = rdma[i]->stats();
        std::printf("eth_%u: psn_drops %u retrans %u rx_fcs_drops %u rx_room_drops %u\n",
                    eths[i], s.stack[8], s.stack[9], s.rxFcsDrops, s.rxRoomDrops);
    }
    std::printf("eth_0 <-> eth_2 (streams): %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

// --pair: eth_0 and eth_2 cabled to each other, both driven from here. Each
// core has a buffer it sends from (out) and one it receives into (in).
bool runPair(vrt::Device& device, const Options& opt, const std::vector<uint32_t>& sizes,
             uint64_t bufBytes) {
    const std::array<uint32_t, 2> eths{0, 2};
    const std::array<std::string, 2> ips{pick(opt.ips, 0, "--ip", "192.168.0.1"),
                                         pick(opt.ips, 1, "--ip", "192.168.0.2")};
    if (ips[0] == ips[1] || (vrt::Rdma::parseIp(ips[0]) >> 8) != (vrt::Rdma::parseIp(ips[1]) >> 8)) {
        throw std::invalid_argument("--pair needs two different addresses in the same /24");
    }
    vrt::Rdma rdma0 = device.getRdmaDevice(0);
    vrt::Rdma rdma2 = device.getRdmaDevice(2);
    const std::array<vrt::Rdma*, 2> rdma{&rdma0, &rdma2};

    std::cout << "\n=== eth_0 <-> eth_2: RDMA service builds " << rdma0.getBuild() << " / "
              << rdma2.getBuild() << ", " << ips[0] << " / " << ips[1] << std::endl;
    // both ends of the cable are ours: a reset of one port's GT takes the
    // other's link down too, so try both twice before giving up
    for (int attempt = 0; attempt < 2; attempt++) {
        for (auto* r : rdma) {
            r->bringUp(std::chrono::seconds(5));
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        if (!rdma[i]->linkUp()) {
            throw std::runtime_error("eth_" + std::to_string(eths[i]) +
                                     " link is down (is QSFP0 cabled to QSFP2?)");
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        rdma[i]->configure(0x0200'0000'0000ull | vrt::Rdma::parseIp(ips[i]), ips[i]);
    }
    for (unsigned i = 0; i < 2; i++) {
        rdma[i]->resolve(ips[1 - i]);
    }
    // queue pair 1 on both, with new PSNs: psn[i] is the first that core i sends
    std::mt19937 rng(std::random_device{}());
    const std::array<uint32_t, 2> psn{static_cast<uint32_t>(rng() & 0xFF'FFFF),
                                      static_cast<uint32_t>(rng() & 0xFF'FFFF)};
    const uint32_t qpn = 1;
    for (unsigned i = 0; i < 2; i++) {
        rdma[i]->connect(vrt::RdmaQp{qpn, qpn, ips[1 - i], psn[1 - i], psn[i], 0, 0});
    }
    if (opt.stream) {
        return pairStream(device, rdma, eths, qpn, rng, opt, sizes);
    }

    vrt::Buffer<uint32_t> out0(device, bufBytes / 4, vrt::MemoryRangeType::DDR);
    vrt::Buffer<uint32_t> out2(device, bufBytes / 4, vrt::MemoryRangeType::DDR);
    vrt::Buffer<uint32_t> in0(device, bufBytes / 4, vrt::MemoryRangeType::DDR);
    vrt::Buffer<uint32_t> in2(device, bufBytes / 4, vrt::MemoryRangeType::DDR);
    const std::array<vrt::Buffer<uint32_t>*, 2> out{&out0, &out2};
    const std::array<vrt::Buffer<uint32_t>*, 2> in{&in0, &in2};

    auto newSeed = [&]() { return static_cast<uint32_t>(rng()); };
    std::vector<std::string> notes;
    auto fill = [](vrt::Buffer<uint32_t>& b, size_t words, uint32_t seed) {
        vrt::Rdma::fillPattern(b.get(), words, seed);
        b.sync(vrt::SyncType::HOST_TO_DEVICE);
    };
    auto check = [&](vrt::Buffer<uint32_t>& b, const std::string& test, size_t words,
                     uint32_t seed) {
        b.sync(vrt::SyncType::DEVICE_TO_HOST);
        auto r = vrt::Rdma::checkPattern(b.get(), words, seed);
        if (r.errors) {
            size_t i0 = static_cast<size_t>(r.firstError) / 4;
            for (size_t i = i0; i < i0 + 4 && i < words; i++) {
                char line[128];
                std::snprintf(line, sizeof(line), "%s: byte 0x%06zx got 0x%08x expected 0x%08x",
                              test.c_str(), 4 * i, b.get()[i], seed + static_cast<uint32_t>(i));
                notes.push_back(line);
            }
        }
        return r;
    };
    auto retransmissions = [&]() {
        return static_cast<uint64_t>(rdma0.stats().stack[9]) + rdma2.stats().stack[9];
    };
    // the cores in @p who post count messages each, interleaved, and wait:
    // WRITEs from their out buffer to the other's in, or READs of the other's
    // out into their in
    auto post = [&](const std::vector<unsigned>& who, bool read, uint32_t count, uint32_t size) {
        const uint64_t bytes = static_cast<uint64_t>(count) * size;
        const uint64_t retrans0 = retransmissions();
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t k = 0; k < count; k++) {
            const uint64_t off = static_cast<uint64_t>(k) * size;
            for (unsigned i : who) {
                if (read) {
                    rdma[i]->read(qpn, *in[i], off, out[1 - i]->getPhysAddr() + off, size);
                } else {
                    rdma[i]->write(qpn, *out[i], off, in[1 - i]->getPhysAddr() + off, size);
                }
            }
        }
        for (unsigned i : who) {
            if (!rdma[i]->wait(std::chrono::seconds(10 + bytes / 1'000'000'000))) {
                throw std::runtime_error("eth_" + std::to_string(eths[i]) + " " +
                                         (read ? "READs" : "WRITEs") + " of " +
                                         std::to_string(size) + " B did not complete");
            }
        }
        Result r;
        r.ns = nsSince(t0);
        r.retrans = retransmissions() - retrans0;
        return r;
    };

    std::printf("\n%10s %7s | %-21s | %-21s | %10s | %7s %8s\n", "", "", "  eth_0 -> eth_2",
                "  eth_2 -> eth_0", " both", "", "");
    std::printf("%10s %7s | %10s %10s | %10s %10s | %10s | %7s %8s\n", "bytes", "count",
                "0 WRITE", "2 READ", "2 WRITE", "0 READ", "WRITE", "retrans", "errors");
    bool pass = true;
    for (uint32_t size : sizes) {
        const uint32_t count =
            static_cast<uint32_t>(std::min<uint64_t>(opt.count, bufBytes / size));
        const uint64_t bytes = static_cast<uint64_t>(count) * size;
        const size_t words = bytes / 4;
        const std::string at = std::to_string(size) + " B ";
        std::array<Result, 5> r;
        uint64_t errors = 0;

        // data from core src to core dst, moved by a WRITE of src or a READ of dst
        auto one = [&](unsigned src, unsigned dst, bool read, const std::string& test) {
            uint32_t seed = newSeed();
            fill(*out[src], words, seed);
            fill(*in[dst], words, ~seed);
            Result x = post({read ? dst : src}, read, count, size);
            x.check = check(*in[dst], at + test, words, seed);
            return x;
        };
        r[0] = one(0, 1, false, "eth_0 WRITE");
        r[1] = one(0, 1, true, "eth_2 READ");
        r[2] = one(1, 0, false, "eth_2 WRITE");
        r[3] = one(1, 0, true, "eth_0 READ");

        // both WRITE at once
        const std::array<uint32_t, 2> seeds{newSeed(), newSeed()};
        for (unsigned i = 0; i < 2; i++) {
            fill(*out[i], words, seeds[i]);
            fill(*in[i], words, ~seeds[1 - i]);
        }
        r[4] = post({0, 1}, false, count, size);
        r[4].check = check(*in[1], at + "both WRITE, eth_0's", words, seeds[0]);
        auto c2 = check(*in[0], at + "both WRITE, eth_2's", words, seeds[1]);
        r[4].check.errors += c2.errors;

        uint64_t retrans = 0;
        for (const auto& x : r) {
            retrans += x.retrans;
            errors += x.check.errors;
        }
        pass &= errors == 0;
        std::printf("%10u %7u | %10.2f %10.2f | %10.2f %10.2f | %10.2f | %7llu %8llu\n", size,
                    count, gbps(bytes, r[0].ns), gbps(bytes, r[1].ns), gbps(bytes, r[2].ns),
                    gbps(bytes, r[3].ns), gbps(2 * bytes, r[4].ns),
                    static_cast<unsigned long long>(retrans),
                    static_cast<unsigned long long>(errors));
        std::fflush(stdout);
    }
    std::printf("All posted by the host, one register write per message; \"both\" is the sum\n"
                "of the two directions.\n");
    for (const auto& n : notes) {
        std::printf("  %s\n", n.c_str());
    }
    for (unsigned i = 0; i < 2; i++) {
        auto s = rdma[i]->stats();
        std::printf("eth_%u: psn_drops %u retrans %u rx_fcs_drops %u rx_room_drops %u\n",
                    eths[i], s.stack[8], s.stack[9], s.rxFcsDrops, s.rxRoomDrops);
    }
    std::printf("eth_0 <-> eth_2: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

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
        const std::vector<uint32_t> ports = device.getRdmaPorts();
        if (ports.empty()) {
            throw std::runtime_error("the design has no RDMA core");
        }
        if (opt.pair) {
            for (uint32_t eth : {0u, 2u}) {
                if (std::find(ports.begin(), ports.end(), eth) == ports.end()) {
                    throw std::runtime_error("--pair needs a core on eth_0 and on eth_2");
                }
            }
            bool pass = runPair(device, opt, sizes, bufBytes);
            device.cleanup();
            return pass ? 0 : 2;
        }
        const std::vector<uint32_t> eths = opt.eths.empty() ? ports : opt.eths;
        for (uint32_t eth : eths) {
            if (std::find(ports.begin(), ports.end(), eth) == ports.end()) {
                throw std::runtime_error("the design has no RDMA core on eth_" +
                                         std::to_string(eth));
            }
        }
        bool pass = true;
        std::vector<std::string> summary;
        for (size_t i = 0; i < ports.size(); i++) {
            const uint32_t eth = ports[i];
            if (std::find(eths.begin(), eths.end(), eth) == eths.end()) {
                continue;
            }
            CoreOptions c;
            c.ip = pick(opt.ips, i, "--ip", "192.168." + std::to_string(eth) + ".1");
            c.peerIp = pick(opt.peerIps, i, "--peer-ip", "");
            c.peerHost = pick(opt.peerHosts, i, "--connect", "");
            c.port = static_cast<uint16_t>(
                std::stoul(pick(opt.ports, i, "--port", std::to_string(c.port))));
            bool ok = runCore(device, eth, c, opt, sizes, bufBytes);
            summary.push_back("eth_" + std::to_string(eth) + (ok ? " PASS" : " FAIL"));
            pass &= ok;
        }
        std::printf("\n");
        for (const auto& s : summary) {
            std::printf("%s\n", s.c_str());
        }
        std::printf("%s\n", pass ? "PASS" : "FAIL");

        device.cleanup();
        return pass ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
