// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vrt/rdma.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t BASE = 0x020304000000ull;
constexpr uint64_t STACK = BASE + vrt::Rdma::STACK;
constexpr uint64_t DCMAC0 = 0x020302000000ull;
constexpr uint64_t DCMAC1 = 0x020303000000ull;

// Registers of the service and the stack as the RTL behaves: IDs, SQ_POST
// taking a new value only with room in the queue, 64-bit counters.
class FakeRegs : public vrt::RdmaRegisters {
   public:
    std::map<uint64_t, uint32_t> regs;
    std::vector<std::pair<uint64_t, uint32_t>> writes;
    struct Post { uint32_t ctrl, len; uint64_t laddr, raddr; };
    std::vector<Post> posts;
    unsigned refuse = 0;          // SQ_POST writes to refuse (queue full)

    FakeRegs() {
        regs[BASE + vrt::Rdma::SVC_ID] = vrt::Rdma::SVC_ID_VALUE;
        regs[BASE + vrt::Rdma::SVC_BUILD] = 1;
        regs[STACK + vrt::Rdma::ID] = vrt::Rdma::ID_VALUE;
    }
    void set64(uint64_t addr, uint64_t v) {
        regs[addr] = static_cast<uint32_t>(v);
        regs[addr + 4] = static_cast<uint32_t>(v >> 32);
    }
    uint32_t read(uint64_t addr) override { return regs.count(addr) ? regs[addr] : 0; }
    void write(uint64_t addr, uint32_t v) override {
        writes.emplace_back(addr, v);
        if (addr == STACK + vrt::Rdma::SQ_POST) {
            if (refuse) { refuse--; return; }
            if (v != regs[addr]) {
                auto r = [&](uint64_t o) { return uint64_t(regs[STACK + o]); };
                posts.push_back({uint32_t(r(vrt::Rdma::SQ_CTRL)), uint32_t(r(vrt::Rdma::SQ_LEN)),
                                 r(vrt::Rdma::SQ_LADDR) | r(vrt::Rdma::SQ_LADDR + 4) << 32,
                                 r(vrt::Rdma::SQ_RADDR) | r(vrt::Rdma::SQ_RADDR + 4) << 32});
            }
        }
        regs[addr] = v;
    }
};

vrt::RdmaInfo info(uint32_t eth = 0) {
    return {eth, BASE, 0x01000000, 0x060000000000ull, 0x000800000000ull};
}

struct FakeBuffer {
    uint64_t addr;
    uint64_t getPhysAddr() const { return addr; }
};

}  // namespace

TEST(RdmaTest, ChecksBothIds) {
    auto regs = std::make_shared<FakeRegs>();
    regs->regs[STACK + vrt::Rdma::ID] = 0;
    EXPECT_THROW(vrt::Rdma(info(), regs), std::runtime_error);
    regs = std::make_shared<FakeRegs>();
    regs->regs[BASE] = 0xFFFFFFFF;
    EXPECT_THROW(vrt::Rdma(info(), regs), std::runtime_error);
    EXPECT_THROW(vrt::Rdma(info(1), std::make_shared<FakeRegs>()), std::invalid_argument);
}

TEST(RdmaTest, ChecksTheCoresPort) {
    // the fake core reports eth_0
    EXPECT_THROW(vrt::Rdma(info(2), std::make_shared<FakeRegs>()), std::runtime_error);
    auto regs = std::make_shared<FakeRegs>();
    regs->regs[BASE + vrt::Rdma::SVC_ETH] = 2;
    EXPECT_NO_THROW(vrt::Rdma(info(2), regs));
}

TEST(RdmaTest, WriteAndReadArePostedWithTheirFields) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    rdma.write(3, 0x060000001000ull, 0x123456789ABCull, 4096);
    rdma.readToStream(5, 0x40ull, 64);
    ASSERT_EQ(regs->posts.size(), 2u);
    EXPECT_EQ(regs->posts[0].ctrl, 3u << 8);
    EXPECT_EQ(regs->posts[0].len, 4096u);
    EXPECT_EQ(regs->posts[0].laddr, 0x060000001000ull);
    EXPECT_EQ(regs->posts[0].raddr, 0x123456789ABCull);
    EXPECT_EQ(regs->posts[1].ctrl, (5u << 8) | 1);
    EXPECT_EQ(regs->posts[1].laddr, 0x800000000000ull);   // the stream
    EXPECT_EQ(rdma.posted(), 2u);
}

TEST(RdmaTest, PostRetriesWhileTheQueueIsFull) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    regs->refuse = 3;
    rdma.write(1, 0x060000000000ull, 0, 8);
    EXPECT_EQ(regs->posts.size(), 1u);
    EXPECT_EQ(rdma.posted(), 1u);
}

TEST(RdmaTest, LengthMustBeAPositiveMultipleOfFour) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    EXPECT_THROW(rdma.write(1, 0x060000000000ull, 0, 0), std::invalid_argument);
    EXPECT_THROW(rdma.write(1, 0x060000000000ull, 0, 6), std::invalid_argument);
    EXPECT_TRUE(regs->posts.empty());
}

TEST(RdmaTest, BuffersMustBeInTheMemoryWindow) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    rdma.write(1, FakeBuffer{0x060000000000ull}, 0x100, 0x0, 64);
    EXPECT_EQ(regs->posts.back().laddr, 0x060000000100ull);
    EXPECT_THROW(rdma.write(1, FakeBuffer{0x4000000000ull}, 0, 0, 64), std::invalid_argument);  // HBM
    EXPECT_THROW(rdma.read(1, FakeBuffer{0x060000000000ull + 0x000800000000ull - 32}, 0, 0, 64),
                 std::invalid_argument);
}

TEST(RdmaTest, WaitCountsMessagesFromConstruction) {
    auto regs = std::make_shared<FakeRegs>();
    regs->set64(STACK + vrt::Rdma::CQ_MSGS, 0x1'0000'0005ull);   // before this object
    vrt::Rdma rdma(info(), regs);
    rdma.write(1, 0x060000000000ull, 0, 8);
    rdma.write(1, 0x060000000000ull, 0, 8);
    EXPECT_EQ(rdma.completed(), 0u);
    EXPECT_FALSE(rdma.wait(std::chrono::milliseconds(1)));
    regs->set64(STACK + vrt::Rdma::CQ_MSGS, 0x1'0000'0007ull);
    EXPECT_EQ(rdma.completed(), 2u);
    EXPECT_TRUE(rdma.wait(std::chrono::milliseconds(1)));
}

TEST(RdmaTest, ConfigureAndConnect) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    rdma.configure(0x0200'0000'0A01ull, "10.0.0.1");
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::MAC_LO], 0x0000'0A01u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::MAC_HI], 0x0200u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::IP], 0x0A000001u);
    rdma.connect({1, 2, "10.0.0.3", 0x100, 0x200, 7, 0});
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_LOCAL_QPN], 1u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_REMOTE_QPN], 2u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_REMOTE_IP], 0x0A000003u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_LOCAL_PSN], 0x100u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_REMOTE_PSN], 0x200u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_RKEY], 7u);
    EXPECT_EQ(regs->writes.back().first, STACK + vrt::Rdma::QP_SET);
    EXPECT_THROW(rdma.connect({64, 2, "10.0.0.3", 0, 0, 0, 0}), std::invalid_argument);
    EXPECT_THROW(rdma.configure(0, "10.0.0"), std::invalid_argument);
}

TEST(RdmaTest, LinkFromTheDcmacOfThePort) {
    // one register map for both: it reports the port of the core being opened
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma eth0(info(0), regs);
    regs->regs[BASE + vrt::Rdma::SVC_ETH] = 2;
    vrt::Rdma eth2(info(2), regs);
    regs->regs[DCMAC0 + 0x1C04] = 0x5;
    EXPECT_TRUE(eth0.linkUp());
    EXPECT_FALSE(eth2.linkUp());
    regs->regs[DCMAC0 + 0x1104] = 0x1;                  // TX local fault
    EXPECT_FALSE(eth0.linkUp());
    regs->regs[DCMAC1 + 0x1C04] = 0x5;
    EXPECT_TRUE(eth2.linkUp());
}

TEST(RdmaTest, BringUpPulsesTheRxDatapathResetFirst) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    EXPECT_FALSE(rdma.bringUp(std::chrono::milliseconds(1)));
    std::vector<uint32_t> ctrl;
    for (auto& w : regs->writes)
        if (w.first == BASE + vrt::Rdma::SVC_PHY_CTRL) ctrl.push_back(w.second);
    EXPECT_EQ(ctrl, (std::vector<uint32_t>{1u << 4, 0, 1u << 7, 0}));
}

TEST(RdmaTest, StatsReadEveryCounter) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    for (unsigned i = 0; i < 16; i++) regs->regs[STACK + vrt::Rdma::STATS + 4 * i] = 100 + i;
    for (unsigned i = 0; i < 10; i++) regs->regs[BASE + vrt::Rdma::SVC_COUNTERS + 4 * i] = i + 1;
    regs->set64(STACK + vrt::Rdma::SQ_REQS, 0x2'0000'0001ull);
    regs->regs[BASE + vrt::Rdma::SVC_RX_FCS_DROPS] = 5000;
    regs->regs[BASE + vrt::Rdma::SVC_RX_ROOM_DROPS] = 37;
    auto s = rdma.stats();
    EXPECT_EQ(s.stack[9], 109u);
    EXPECT_EQ(s.requests, 0x2'0000'0001ull);
    EXPECT_EQ(s.rdMem, 1u);
    EXPECT_EQ(s.wrLenErrors, 10u);
    EXPECT_EQ(s.rxFcsDrops, 0u);     // version 1.0: not there
    regs->regs[BASE + vrt::Rdma::SVC_VERSION] = 0x0001'0001;
    s = rdma.stats();
    EXPECT_EQ(s.rxFcsDrops, 5000u);
    EXPECT_EQ(s.rxRoomDrops, 37u);
}

namespace {

// The far end of the sideband, a peer that serves all requests.
class FakePeer {
   public:
    int fd = -1;
    std::vector<std::string> got;          // every line received
    ~FakePeer() { if (fd >= 0) ::close(fd); }

    void send(const std::string& s) { ::send(fd, s.data(), s.size(), MSG_NOSIGNAL); }
    std::string line() {
        size_t nl;
        while ((nl = in.find('\n')) == std::string::npos) {
            char b[256];
            ssize_t n = recv(fd, b, sizeof(b), 0);
            if (n <= 0) return "";
            in.append(b, n);
        }
        std::string l = in.substr(0, nl);
        in.erase(0, nl + 1);
        got.push_back(l);
        return l;
    }
    void listenOn(uint16_t port) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(s, 1);
        fd = accept(s, nullptr, nullptr);
        ::close(s);
    }
    void connectTo(uint16_t port) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        for (int i = 0; i < 100; i++) {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return;
            ::close(fd);
            fd = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    // the session, then the peer closes the connection
    void serve(const std::string& roceIp = "10.0.0.3") {
        session(roceIp);
        ::close(fd);
        fd = -1;
    }
    // announcements split over segments, then requests until BYE
    void session(const std::string& roceIp) {
        send("HELLO v=1 name=peer ro");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        send("ce_ip=" + roceIp + " qps=1 mrs=1\r\nMR i=0 addr=0x400000000000 len=0x10000000\n"
             "QP i=0 qpn=1 send_psn=0x200 rkey=0 udp=4791\n");
        for (std::string l = line(); l.rfind("READY", 0) != 0; l = line()) {
            if (l.empty() || l.rfind("ERR", 0) == 0) return;
        }
        send("READY\n");
        for (;;) {
            std::string l = line();
            if (l.empty()) return;
            if (l.rfind("BYE", 0) == 0) { send("OK\n"); return; }
            std::string tag = l.substr(l.find('#'), l.find(' ', l.find('#')) - l.find('#'));
            if (l.rfind("FILL", 0) == 0) send("OK " + tag + "\n");
            if (l.rfind("CHECK", 0) == 0) send("OK " + tag + " errors=3 first=0x40\n");
            if (l.rfind("RUN", 0) == 0) {
                send("STATS #7\n");          // a request of its own in between
                line();
                send("OK " + tag + " ns=1000 retrans=2 psn_drops=0\n");
            }
            if (l.rfind("STATS", 0) == 0) send("OK " + tag + " retrans=5 rx_frames=9\n");
        }
    }

   private:
    std::string in;
};

uint16_t freePort() {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    ::close(s);
    return ntohs(a.sin_port);
}

std::shared_ptr<FakeRegs> linkedRegs() {
    auto regs = std::make_shared<FakeRegs>();
    regs->regs[DCMAC0 + 0x1C04] = 0x5;
    return regs;
}

}  // namespace

TEST(RdmaSessionTest, ServerSwapsParametersAndConnectsTheQueuePair) {
    auto regs = linkedRegs();
    vrt::Rdma rdma(info(), regs);
    uint16_t port = freePort();
    rdma.setLocalIp("10.0.0.1");
    rdma.setRemoteIp("10.0.0.3");
    rdma.setRole(vrt::Rdma::Role::Server);
    rdma.setPort(port);
    rdma.setTimeout(std::chrono::seconds(5));
    FakePeer peer;
    std::thread t([&] { peer.connectTo(port); peer.serve(); });
    rdma.connect();
    EXPECT_TRUE(rdma.connected());
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::IP], 0x0A000001u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::MAC_LO], 0x0A000001u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::MAC_HI], 0x0200u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_LOCAL_QPN], 1u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_REMOTE_QPN], 1u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_REMOTE_IP], 0x0A000003u);
    EXPECT_EQ(regs->regs[STACK + vrt::Rdma::QP_LOCAL_PSN], 0x200u);   // the peer's send_psn
    EXPECT_EQ(rdma.remoteIp(), "10.0.0.3");
    ASSERT_EQ(rdma.remoteMemory().size(), 1u);
    EXPECT_EQ(rdma.remoteMemory()[0].addr, 0x400000000000ull);
    EXPECT_EQ(rdma.remoteMemory()[0].len, 0x10000000ull);
    EXPECT_EQ(rdma.qp(0), 1u);

    rdma.peerFill(0, 0, 4096, 0x1234);
    auto c = rdma.peerCheck(0, 0, 4096, 0x1234);
    EXPECT_EQ(c.errors, 3u);
    EXPECT_EQ(c.firstError, 0x40);
    auto r = rdma.peerWrite(0, 10, 4096, 0x400000000000ull, 0x060000000000ull);
    EXPECT_EQ(r.ns, 1000u);
    EXPECT_EQ(r.retrans, 2u);
    EXPECT_EQ(rdma.peerStats()["retrans"], "5");
    rdma.close();
    EXPECT_FALSE(rdma.connected());
    t.join();

    // what this end announced and asked for
    auto has = [&](const std::string& s) {
        for (auto& l : peer.got) if (l.find(s) != std::string::npos) return true;
        return false;
    };
    EXPECT_TRUE(has("HELLO v=1 name=v80 roce_ip=10.0.0.1 qps=1 mrs=1"));
    EXPECT_TRUE(has("MR i=0 addr=0x60000000000 len=0x800000000"));   // default: the window
    char psn[64];
    std::snprintf(psn, sizeof(psn), "send_psn=0x%x ", regs->regs[STACK + vrt::Rdma::QP_REMOTE_PSN]);
    EXPECT_TRUE(has(std::string("QP i=0 qpn=1 ") + psn));
    EXPECT_TRUE(has("FILL #1 mr=0 off=0x0 len=0x1000 seed=0x1234"));
    EXPECT_TRUE(has("RUN #3 op=write qp=0 count=10 size=4096 laddr=0x400000000000 raddr=0x60000000000"));
    EXPECT_TRUE(has("OK #7 rx_frames="));                             // its STATS answered
    EXPECT_TRUE(has("BYE"));
}

TEST(RdmaSessionTest, ClientConnectsToThePeersPort) {
    auto regs = linkedRegs();
    vrt::Rdma rdma(info(), regs);
    uint16_t port = freePort();
    rdma.setLocalIp("10.0.0.1");
    rdma.setRole(vrt::Rdma::Role::Client);
    rdma.setRemoteHost("127.0.0.1");
    rdma.setPort(port);
    rdma.addMemory(0x060000100000ull, 0x2000);
    rdma.setTimeout(std::chrono::seconds(5));
    FakePeer peer;
    std::thread t([&] { peer.listenOn(port); peer.serve(); });
    rdma.connect();
    rdma.close();
    t.join();
    bool mr = false;
    for (auto& l : peer.got) mr |= l == "MR i=0 addr=0x60000100000 len=0x2000";
    EXPECT_TRUE(mr);
}

TEST(RdmaSessionTest, AnotherPeerIpIsRefused) {
    auto regs = linkedRegs();
    vrt::Rdma rdma(info(), regs);
    uint16_t port = freePort();
    rdma.setLocalIp("10.0.0.1");
    rdma.setRemoteIp("10.0.0.3");
    rdma.setPort(port);
    rdma.setTimeout(std::chrono::seconds(5));
    FakePeer peer;
    std::thread t([&] { peer.connectTo(port); peer.serve("10.0.0.9"); });
    EXPECT_THROW(rdma.connect(), std::runtime_error);
    rdma.close();
    t.join();
    EXPECT_EQ(peer.got.back(), "ERR msg=roce_ip");
}

TEST(RdmaSessionTest, ConnectNeedsTheSettings) {
    auto regs = linkedRegs();
    vrt::Rdma rdma(info(), regs);
    EXPECT_THROW(rdma.connect(), std::invalid_argument);           // no local IP
    rdma.setLocalIp("10.0.0.1");
    rdma.setRole(vrt::Rdma::Role::Client);
    EXPECT_THROW(rdma.connect(), std::invalid_argument);           // no remote host
    EXPECT_THROW(rdma.setLocalIp("10.0.0"), std::invalid_argument);
    EXPECT_THROW(rdma.setQp(0), std::invalid_argument);
    EXPECT_THROW(rdma.peerStats(), std::logic_error);              // not connected
    auto down = std::make_shared<FakeRegs>();
    vrt::Rdma noLink(info(), down);
    noLink.setLocalIp("10.0.0.1");
    noLink.setTimeout(std::chrono::milliseconds(1));
    EXPECT_THROW(noLink.connect(), std::runtime_error);            // link down
}

TEST(RdmaSessionTest, Pattern) {
    std::vector<uint32_t> w(1024);
    vrt::Rdma::fillPattern(w.data(), w.size(), 0xFFFFFFF0u);
    EXPECT_EQ(w[0], 0xFFFFFFF0u);
    EXPECT_EQ(w[16], 0u);                                          // wraps
    EXPECT_EQ(vrt::Rdma::checkPattern(w.data(), w.size(), 0xFFFFFFF0u).errors, 0u);
    w[5] ^= 1;
    w[9] = 0;
    auto r = vrt::Rdma::checkPattern(w.data(), w.size(), 0xFFFFFFF0u);
    EXPECT_EQ(r.errors, 2u);
    EXPECT_EQ(r.firstError, 20);
}
