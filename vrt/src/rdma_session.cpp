// SPDX-License-Identifier: MIT
// A session of the RDMA service with a peer: the sideband (docs/rdma-sideband.md)
// that swaps queue-pair and memory parameters, and requests to the peer.
#include <vrt/rdma.hpp>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <vrt/utils/logger.hpp>

#include "rdma_sideband.hpp"

namespace vrt {

namespace {

constexpr size_t MAX_LINE = 255;

std::string format(const char* f, ...) {
    char s[MAX_LINE + 1];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(s, sizeof(s), f, ap);
    va_end(ap);
    return s;
}

using Clock = std::chrono::steady_clock;

int remainingMs(Clock::time_point end) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    return ms > 0 ? static_cast<int>(ms) : 0;
}

std::runtime_error sidebandError(const std::string& what) {
    return std::runtime_error("Rdma sideband: " + what);
}

}  // namespace

// ---- the connection ----

Rdma::Sideband::Sideband(int fd_, std::chrono::milliseconds timeout_)
    : fd(fd_), timeout(timeout_) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

Rdma::Sideband::~Sideband() { ::close(fd); }

std::unique_ptr<Rdma::Sideband> Rdma::Sideband::listen(uint16_t port,
                                                       std::chrono::milliseconds timeout) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        throw sidebandError(std::string("socket: ") + std::strerror(errno));
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || ::listen(s, 1) < 0) {
        std::string e = std::strerror(errno);
        ::close(s);
        throw sidebandError("cannot listen on port " + std::to_string(port) + ": " + e);
    }
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "RDMA sideband: waiting for the peer on port {}", port);
    pollfd p{s, POLLIN, 0};
    int r = poll(&p, 1, static_cast<int>(timeout.count()));
    int fd = r > 0 ? accept(s, nullptr, nullptr) : -1;
    ::close(s);
    if (fd < 0) {
        throw sidebandError("no peer connected to port " + std::to_string(port));
    }
    return std::make_unique<Sideband>(fd, timeout);
}

std::unique_ptr<Rdma::Sideband> Rdma::Sideband::connect(const std::string& host, uint16_t port,
                                                        std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        throw sidebandError("unknown host '" + host + "'");
    }
    auto end = Clock::now() + timeout;
    int fd = -1;
    std::string err;
    // the peer may not listen yet: retry until the timeout
    do {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && ::connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            break;
        }
        err = std::strerror(errno);
        if (fd >= 0) {
            ::close(fd);
        }
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (Clock::now() < end);
    freeaddrinfo(res);
    if (fd < 0) {
        throw sidebandError("cannot connect to " + host + ":" + std::to_string(port) + ": " + err);
    }
    return std::make_unique<Sideband>(fd, timeout);
}

void Rdma::Sideband::send(const std::string& line) {
    if (line.size() > MAX_LINE) {
        throw sidebandError("line longer than " + std::to_string(MAX_LINE) + " bytes");
    }
    std::string s = line + "\n";
    for (size_t done = 0; done < s.size();) {
        ssize_t n = ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
        if (n <= 0) {
            throw sidebandError(std::string("send: ") + std::strerror(errno));
        }
        done += static_cast<size_t>(n);
    }
}

Rdma::Sideband::Line Rdma::Sideband::receive() {
    auto end = Clock::now() + timeout;
    for (;;) {
        size_t nl;
        while ((nl = in.find('\n')) != std::string::npos) {
            std::string line = in.substr(0, nl);
            in.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            Line l = parse(line);
            if (!l.verb.empty()) {
                return l;
            }
        }
        if (in.size() > 4 * MAX_LINE) {
            throw sidebandError("a line without end from the peer");
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, remainingMs(end)) <= 0) {
            throw sidebandError("no answer from the peer");
        }
        char buf[1024];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            throw sidebandError("the peer closed the connection");
        }
        in.append(buf, static_cast<size_t>(n));
    }
}

Rdma::Sideband::Line Rdma::Sideband::parse(const std::string& line) {
    Line l;
    std::istringstream s(line);
    std::string w;
    s >> l.verb;
    while (s >> w) {
        size_t eq = w.find('=');
        if (w[0] == '#') {
            l.tag = std::strtol(w.c_str() + 1, nullptr, 10);
        } else if (eq != std::string::npos) {
            l.keys[w.substr(0, eq)] = w.substr(eq + 1);
        }
    }
    return l;
}

uint64_t Rdma::Sideband::number(const Line& l, const char* key) {
    auto it = l.keys.find(key);
    if (it == l.keys.end()) {
        throw sidebandError(l.verb + " without " + key);
    }
    return std::strtoull(it->second.c_str(), nullptr, 0);
}

std::string Rdma::Sideband::text(const Line& l, const char* key) {
    auto it = l.keys.find(key);
    return it == l.keys.end() ? "" : it->second;
}

// ---- the session ----

Rdma::~Rdma() = default;
Rdma::Rdma(Rdma&&) noexcept = default;
Rdma& Rdma::operator=(Rdma&&) noexcept = default;

void Rdma::setLocalIp(const std::string& ip) {
    parseIp(ip);
    localIp = ip;
}

void Rdma::setLocalMac(uint64_t mac) { localMac = mac & 0xFFFF'FFFF'FFFFull; }

void Rdma::setRemoteIp(const std::string& ip) {
    parseIp(ip);
    wantedPeerIp = ip;
}

void Rdma::setRole(Role r) { role = r; }

void Rdma::setPort(uint16_t port) { sidebandPort = port; }

void Rdma::setRemoteHost(const std::string& host) { remoteHost = host; }

void Rdma::setQp(unsigned n) {
    if (n < 1 || n > 63) {
        throw std::invalid_argument("Rdma: " + std::to_string(n) + " queue pairs (1..63)");
    }
    numQps = n;
}

void Rdma::addMemory(uint64_t addr, uint64_t len) { localMemory.push_back({addr, len}); }

void Rdma::setTimeout(std::chrono::milliseconds t) { timeout = t; }

bool Rdma::connected() const { return sideband != nullptr; }

uint32_t Rdma::qp(unsigned i) const {
    if (i >= numQps) {
        throw std::out_of_range("Rdma: queue pair " + std::to_string(i) + " of " +
                                std::to_string(numQps));
    }
    return i + 1;
}

void Rdma::connect() {
    if (localIp.empty()) {
        throw std::invalid_argument("Rdma: setLocalIp() before connect()");
    }
    if (role == Role::Client && remoteHost.empty()) {
        throw std::invalid_argument("Rdma: a client needs setRemoteHost() before connect()");
    }
    // own addresses and the link first: the peer may ARP for us as soon as
    // it has our HELLO
    if (!bringUp(std::min<std::chrono::milliseconds>(timeout, std::chrono::seconds(5)))) {
        throw std::runtime_error("Rdma: eth_" + std::to_string(info.eth) + " link is down");
    }
    configure(localMac ? localMac : 0x0200'0000'0000ull | parseIp(localIp), localIp);

    sideband.reset();
    sideband = role == Role::Server ? Sideband::listen(sidebandPort, timeout)
                                    : Sideband::connect(remoteHost, sidebandPort, timeout);
    Sideband& sb = *sideband;

    std::vector<RdmaRemoteMemory> mine = localMemory;
    if (mine.empty()) {
        mine.push_back({info.memBase, info.memRange});
    }
    std::random_device rd;
    std::vector<uint32_t> sendPsn(numQps);
    sb.send(format("HELLO v=%u name=v80 roce_ip=%s qps=%u mrs=%zu", SIDEBAND_VERSION,
                   localIp.c_str(), numQps, mine.size()));
    for (unsigned i = 0; i < numQps; i++) {
        sendPsn[i] = rd() & 0xFF'FFFF;
        sb.send(format("QP i=%u qpn=%u send_psn=0x%x rkey=0 udp=4791", i, qp(i),
                       sendPsn[i]));
    }
    for (size_t i = 0; i < mine.size(); i++) {
        sb.send(format("MR i=%zu addr=0x%llx len=0x%llx", i,
                       static_cast<unsigned long long>(mine[i].addr),
                       static_cast<unsigned long long>(mine[i].len)));
    }

    // the peer's announcements, in any order after its HELLO
    bool hello = false, ready = false;
    size_t qps = 0, mrs = 0, gotQps = 0, gotMrs = 0;
    std::vector<uint32_t> peerQpn, peerPsn;
    peerMemory.clear();
    while (!hello || gotQps < qps || gotMrs < mrs) {
        Sideband::Line l = sb.receive();
        if (l.verb == "HELLO") {
            if (Sideband::number(l, "v") != SIDEBAND_VERSION) {
                sb.send("ERR msg=version");
                throw sidebandError("the peer speaks version " + Sideband::text(l, "v"));
            }
            peerIp = Sideband::text(l, "roce_ip");
            parseIp(peerIp);
            qps = Sideband::number(l, "qps");
            mrs = Sideband::number(l, "mrs");
            peerQpn.assign(qps, 0);
            peerPsn.assign(qps, 0);
            peerMemory.assign(mrs, {});
            hello = true;
        } else if (l.verb == "QP" && hello && Sideband::number(l, "i") < qps) {
            peerQpn[Sideband::number(l, "i")] = static_cast<uint32_t>(Sideband::number(l, "qpn"));
            peerPsn[Sideband::number(l, "i")] = static_cast<uint32_t>(Sideband::number(l, "send_psn")) & 0xFF'FFFF;
            gotQps++;
        } else if (l.verb == "MR" && hello && Sideband::number(l, "i") < mrs) {
            peerMemory[Sideband::number(l, "i")] = {Sideband::number(l, "addr"), Sideband::number(l, "len")};
            gotMrs++;
        } else if (l.verb == "READY") {
            ready = true;
        } else if (l.verb == "ERR") {
            throw sidebandError("the peer refused: " + Sideband::text(l, "msg"));
        } else {
            throw sidebandError("unexpected '" + l.verb + "' while connecting");
        }
    }
    if (qps != numQps) {
        sb.send("ERR msg=queue_pairs");
        throw sidebandError("the peer has " + std::to_string(qps) + " queue pairs, this end " +
                            std::to_string(numQps));
    }
    if (!wantedPeerIp.empty() && wantedPeerIp != peerIp) {
        sb.send("ERR msg=roce_ip");
        throw sidebandError("the peer is " + peerIp + ", expected " + wantedPeerIp);
    }

    resolve(peerIp);
    for (unsigned i = 0; i < numQps; i++) {
        connect(RdmaQp{qp(i), peerQpn[i], peerIp, peerPsn[i], sendPsn[i], 0, 0});
    }
    sb.send("READY");
    while (!ready) {
        Sideband::Line l = sb.receive();
        if (l.verb == "READY") {
            ready = true;
        } else if (l.verb == "ERR") {
            throw sidebandError("the peer refused: " + Sideband::text(l, "msg"));
        } else {
            throw sidebandError("unexpected '" + l.verb + "' while connecting");
        }
    }
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                       "RDMA eth_{}: connected to {} ({} queue pairs, {} memory regions)",
                       info.eth, peerIp, numQps, peerMemory.size());
}

void Rdma::close() {
    if (!sideband) {
        return;
    }
    try {
        sideband->send("BYE");
        for (;;) {
            Sideband::Line l = sideband->receive();
            if (l.verb == "OK" && l.tag < 0) {
                break;
            }
        }
    } catch (const std::runtime_error&) {
        // the peer is gone already
    }
    sideband.reset();
}

std::map<std::string, std::string> Rdma::request(const std::string& line) {
    if (!sideband) {
        throw std::logic_error("Rdma: connect() before requests to the peer");
    }
    Sideband& sb = *sideband;
    long tag = sb.nextTag++;
    size_t sp = line.find(' ');
    sb.send(line.substr(0, sp) + " #" + std::to_string(tag) +
            (sp == std::string::npos ? "" : line.substr(sp)));
    for (;;) {
        Sideband::Line l = sb.receive();
        if ((l.verb == "OK" || l.verb == "ERR") && l.tag == tag) {
            if (l.verb == "ERR") {
                throw sidebandError("the peer refused '" + line + "': " + Sideband::text(l, "msg"));
            }
            return l.keys;
        }
        if (l.verb == "BYE") {
            sb.send("OK");
            sideband.reset();
            throw sidebandError("the peer ended the session");
        }
        if (l.verb == "STATS" && l.tag >= 0) {
            RdmaStats s = stats();
            sb.send(format("OK #%ld rx_frames=%u tx_frames=%u psn_drops=%u retrans=%u "
                           "rx_fcs_drops=%u rx_room_drops=%u",
                           l.tag, s.stack[0], s.stack[1], s.stack[8], s.stack[9], s.rxFcsDrops,
                           s.rxRoomDrops));
        } else if (l.tag >= 0) {
            // this end drives the tests: it serves no FILL, CHECK or RUN
            sb.send(format("ERR #%ld msg=unsupported", l.tag));
        }
    }
}

void Rdma::peerFill(unsigned mr, uint64_t off, uint64_t len, uint32_t seed) {
    request(format("FILL mr=%u off=0x%llx len=0x%llx seed=0x%x", mr,
                   static_cast<unsigned long long>(off), static_cast<unsigned long long>(len),
                   seed));
}

RdmaCheckResult Rdma::peerCheck(unsigned mr, uint64_t off, uint64_t len, uint32_t seed) {
    auto k = request(format("CHECK mr=%u off=0x%llx len=0x%llx seed=0x%x", mr,
                            static_cast<unsigned long long>(off),
                            static_cast<unsigned long long>(len), seed));
    RdmaCheckResult r;
    r.errors = std::strtoull(k["errors"].c_str(), nullptr, 0);
    r.firstError = k.count("first") ? std::strtoll(k["first"].c_str(), nullptr, 0) : -1;
    return r;
}

RdmaRunResult Rdma::peerRun(const char* op, unsigned qp, uint32_t count, uint32_t size,
                            uint64_t peerAddr, uint64_t remoteAddr) {
    this->qp(qp);   // checks the index
    auto k = request(format("RUN op=%s qp=%u count=%u size=%u laddr=0x%llx raddr=0x%llx", op, qp,
                            count, size, static_cast<unsigned long long>(peerAddr),
                            static_cast<unsigned long long>(remoteAddr)));
    RdmaRunResult r;
    r.ns = std::strtoull(k["ns"].c_str(), nullptr, 0);
    r.retrans = std::strtoull(k["retrans"].c_str(), nullptr, 0);
    r.psnDrops = std::strtoull(k["psn_drops"].c_str(), nullptr, 0);
    return r;
}

RdmaRunResult Rdma::peerWrite(unsigned qp, uint32_t count, uint32_t size, uint64_t peerAddr,
                              uint64_t remoteAddr) {
    return peerRun("write", qp, count, size, peerAddr, remoteAddr);
}

RdmaRunResult Rdma::peerRead(unsigned qp, uint32_t count, uint32_t size, uint64_t peerAddr,
                             uint64_t remoteAddr) {
    return peerRun("read", qp, count, size, peerAddr, remoteAddr);
}

std::map<std::string, std::string> Rdma::peerStats() { return request("STATS"); }

void Rdma::fillPattern(uint32_t* words, size_t n, uint32_t seed) {
    for (size_t k = 0; k < n; k++) {
        words[k] = seed + static_cast<uint32_t>(k);
    }
}

RdmaCheckResult Rdma::checkPattern(const uint32_t* words, size_t n, uint32_t seed) {
    RdmaCheckResult r;
    for (size_t k = 0; k < n; k++) {
        if (words[k] != seed + static_cast<uint32_t>(k)) {
            if (r.errors++ == 0) {
                r.firstError = static_cast<int64_t>(4 * k);
            }
        }
    }
    return r;
}

}  // namespace vrt
