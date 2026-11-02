// SPDX-License-Identifier: MIT
#include <vrt/rdma.hpp>

#include <arpa/inet.h>

#include <stdexcept>
#include <thread>

#include <vrt/utils/logger.hpp>

namespace vrt {

namespace {

// DCMAC registers of port 0 of each DCMAC (SLASH: 0x203_0200_0000 for eth_0,
// 0x203_0300_0000 for eth_2)
constexpr uint64_t DCMAC0_BASE = 0x0203'0200'0000ull;
constexpr uint64_t DCMAC_STRIDE = 0x0100'0000ull;
constexpr uint64_t DCMAC_RX_PHY_STATUS = 0x1C04;   // C0_STAT_PORT_RX_PHY_RT_STATUS_REG
constexpr uint64_t DCMAC_TX_MAC_STATUS = 0x1104;   // C0_STAT_CHAN_TX_MAC_RT_STATUS_REG

// PHY_CTRL reset bits: 4 GT RX datapath, 7 the whole reset sequence
constexpr unsigned PHY_RX_DATAPATH = 4;
constexpr unsigned PHY_SEQUENCE = 7;

constexpr unsigned SQ_POST_TRIES = 100000;

std::string hex(uint64_t v) {
    char s[32];
    std::snprintf(s, sizeof(s), "0x%llx", static_cast<unsigned long long>(v));
    return s;
}

}  // namespace

Rdma::Rdma(const RdmaInfo& info_, std::shared_ptr<RdmaRegisters> regs_)
    : info(info_), regs(std::move(regs_)) {
    if (!regs) {
        throw std::invalid_argument("Rdma: no register access");
    }
    if (info.eth != 0 && info.eth != 2) {
        throw std::invalid_argument("Rdma: eth_" + std::to_string(info.eth) +
                                    " has no DCMAC (eth_0 or eth_2)");
    }
    uint32_t id = svcRead(SVC_ID);
    if (id != SVC_ID_VALUE) {
        throw std::runtime_error("Rdma: no RDMA service at " + hex(info.baseAddr) + " (ID " +
                                 hex(id) + ")");
    }
    id = stackRead(ID);
    if (id != ID_VALUE) {
        throw std::runtime_error("Rdma: no RDMA stack at " + hex(info.baseAddr + STACK) +
                                 " (ID " + hex(id) + ")");
    }
    msgsAtStart = stackRead64(CQ_MSGS);
}

uint32_t Rdma::getBuild() { return svcRead(SVC_BUILD); }

uint64_t Rdma::stackRead64(uint64_t off) {
    // not latched: HI, LO, HI until the two HI reads agree
    for (;;) {
        uint32_t hi = stackRead(off + 4);
        uint32_t lo = stackRead(off);
        if (stackRead(off + 4) == hi) {
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }
    }
}

uint64_t Rdma::dcmacBase() const { return DCMAC0_BASE + (info.eth == 2 ? DCMAC_STRIDE : 0); }

bool Rdma::linkUp() {
    uint32_t rx = regs->read(dcmacBase() + DCMAC_RX_PHY_STATUS);
    uint32_t tx = regs->read(dcmacBase() + DCMAC_TX_MAC_STATUS);
    return (rx & 1) && (rx & 4) && !(tx & 1);
}

uint32_t Rdma::phyStatus() { return svcRead(SVC_PHY_STATUS); }

void Rdma::phyPulse(unsigned bit) {
    uint32_t ctrl = svcRead(SVC_PHY_CTRL) & ~0xF0u;
    svcWrite(SVC_PHY_CTRL, ctrl | (1u << bit));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    svcWrite(SVC_PHY_CTRL, ctrl);
}

bool Rdma::waitLink(std::chrono::milliseconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    do {
        // stable: up in five reads 20 ms apart
        bool up = true;
        for (int i = 0; i < 5 && up; i++) {
            up = linkUp();
            if (up && i < 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (up) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

bool Rdma::bringUp(std::chrono::milliseconds timeout) {
    if (waitLink(std::chrono::milliseconds(0))) {
        return true;
    }
    // A receiver that lost the far end's signal does not recover by itself: a
    // GT RX datapath reset leaves our TX, and so the far end, untouched. Then
    // the whole reset sequence of the port.
    for (unsigned bit : {PHY_RX_DATAPATH, PHY_SEQUENCE}) {
        utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                           "RDMA eth_{}: link down, {}", info.eth,
                           bit == PHY_RX_DATAPATH ? "GT RX datapath reset" : "reset sequence");
        phyPulse(bit);
        if (waitLink(timeout)) {
            return true;
        }
    }
    return false;
}

uint32_t Rdma::parseIp(const std::string& ip) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) {
        throw std::invalid_argument("Rdma: invalid IPv4 address '" + ip + "'");
    }
    return ntohl(a.s_addr);
}

void Rdma::configure(uint64_t mac, const std::string& ip) {
    stackWrite(MAC_LO, static_cast<uint32_t>(mac));
    stackWrite(MAC_HI, static_cast<uint32_t>(mac >> 32) & 0xFFFF);
    stackWrite(IP, parseIp(ip));
}

void Rdma::resolve(const std::string& ip) {
    stackWrite(ARP_LOOKUP, parseIp(ip));
    for (unsigned i = 0; i < SQ_POST_TRIES && (stackRead(PENDING) & 1); i++) {
    }
    // the reply comes back within microseconds on a direct link
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Rdma::connect(const RdmaQp& qp) {
    if (qp.qpn > 63) {
        throw std::invalid_argument("Rdma: QPN " + std::to_string(qp.qpn) + " > 63");
    }
    stackWrite(QP_LOCAL_QPN, qp.qpn);
    stackWrite(QP_REMOTE_QPN, qp.remoteQpn);
    stackWrite(QP_LOCAL_PSN, qp.localPsn);
    stackWrite(QP_REMOTE_PSN, qp.remotePsn);
    stackWrite(QP_REMOTE_IP, parseIp(qp.remoteIp));
    stackWrite(QP_REMOTE_PORT, qp.remotePort);
    stackWrite(QP_RKEY, qp.rkey);
    stackWrite(QP_SET, 0);
    unsigned i = 0;
    while (stackRead(PENDING) & 2) {
        if (++i == SQ_POST_TRIES) {
            throw std::runtime_error("Rdma: the stack did not take QP " + std::to_string(qp.qpn));
        }
    }
}

uint64_t Rdma::memoryAddress(uint64_t phys, uint64_t offsetBytes, uint32_t length) const {
    uint64_t a = phys + offsetBytes;
    if (a < info.memBase || a + length > info.memBase + info.memRange) {
        throw std::invalid_argument("Rdma: buffer at " + hex(a) +
                                    " is outside the memory the RDMA service reaches (" +
                                    hex(info.memBase) + " + " + hex(info.memRange) +
                                    "): use a DDR buffer");
    }
    return a;
}

void Rdma::post(uint32_t ctrl, uint64_t localAddr, uint64_t remoteAddr, uint32_t length) {
    if (length == 0 || length % 4) {
        throw std::invalid_argument("Rdma: length " + std::to_string(length) +
                                    " is not a positive multiple of 4");
    }
    stackWrite(SQ_CTRL, ctrl);
    stackWrite(SQ_LEN, length);
    stackWrite(SQ_LADDR, static_cast<uint32_t>(localAddr));
    stackWrite(SQ_LADDR + 4, static_cast<uint32_t>(localAddr >> 32) & 0xFFFF);
    stackWrite(SQ_RADDR, static_cast<uint32_t>(remoteAddr));
    stackWrite(SQ_RADDR + 4, static_cast<uint32_t>(remoteAddr >> 32) & 0xFFFF);
    // SQ_POST takes a new value only when the queue has room
    uint32_t seq = stackRead(SQ_POST);
    for (unsigned i = 0;; i++) {
        stackWrite(SQ_POST, seq + 1);
        if (stackRead(SQ_POST) == seq + 1) {
            break;
        }
        if (i == SQ_POST_TRIES) {
            throw std::runtime_error("Rdma: send queue stays full");
        }
    }
    postedMsgs++;
}

void Rdma::write(uint32_t qpn, uint64_t localAddr, uint64_t remoteAddr, uint32_t length) {
    post((qpn & 0x3F) << 8, localAddr, remoteAddr, length);
}

void Rdma::read(uint32_t qpn, uint64_t localAddr, uint64_t remoteAddr, uint32_t length) {
    post(((qpn & 0x3F) << 8) | 1, localAddr, remoteAddr, length);
}

uint64_t Rdma::completed() { return stackRead64(CQ_MSGS) - msgsAtStart; }

bool Rdma::wait(std::chrono::milliseconds timeout) {
    auto end = std::chrono::steady_clock::now() + timeout;
    while (completed() < postedMsgs) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    return true;
}

RdmaStats Rdma::stats() {
    RdmaStats s;
    for (size_t i = 0; i < s.stack.size(); i++) {
        s.stack[i] = stackRead(STATS + 4 * i);
    }
    s.requests = stackRead64(SQ_REQS);
    s.completions = stackRead64(CQ_COUNT);
    s.messages = stackRead64(CQ_MSGS);
    uint32_t c[10];
    for (unsigned i = 0; i < 10; i++) {
        c[i] = svcRead(SVC_COUNTERS + 4 * i);
    }
    s.rdMem = c[0];
    s.rdStream = c[1];
    s.rdBad = c[2];
    s.wrMem = c[3];
    s.wrStream = c[4];
    s.wrBad = c[5];
    s.mm2sErrors = c[6];
    s.s2mmErrors = c[7];
    s.rdLenErrors = c[8];
    s.wrLenErrors = c[9];
    return s;
}

}  // namespace vrt
