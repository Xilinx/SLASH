// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <vrt/rdma.hpp>

#include <map>
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

TEST(RdmaTest, WriteAndReadArePostedWithTheirFields) {
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma rdma(info(), regs);
    rdma.write(3, 0x060000001000ull, 0x123456789ABCull, 4096);
    rdma.read(5, vrt::Rdma::streamAddress(1), 0x40ull, 64);
    ASSERT_EQ(regs->posts.size(), 2u);
    EXPECT_EQ(regs->posts[0].ctrl, 3u << 8);
    EXPECT_EQ(regs->posts[0].len, 4096u);
    EXPECT_EQ(regs->posts[0].laddr, 0x060000001000ull);
    EXPECT_EQ(regs->posts[0].raddr, 0x123456789ABCull);
    EXPECT_EQ(regs->posts[1].ctrl, (5u << 8) | 1);
    EXPECT_EQ(regs->posts[1].laddr, 0xC00000000000ull);   // stream, lane 1
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
    auto regs = std::make_shared<FakeRegs>();
    vrt::Rdma eth0(info(0), regs);
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
    auto s = rdma.stats();
    EXPECT_EQ(s.stack[9], 109u);
    EXPECT_EQ(s.requests, 0x2'0000'0001ull);
    EXPECT_EQ(s.rdMem, 1u);
    EXPECT_EQ(s.wrLenErrors, 10u);
}
