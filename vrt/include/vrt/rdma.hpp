// SPDX-License-Identifier: MIT
/**
 * @file rdma.hpp
 * @brief The RDMA service of the service layer: a RoCE v2 stack on one DCMAC
 *        port ([network] rdma=eth_<n> in the linker configuration).
 *
 * The host sets up the stack (MAC, IP, queue pairs) and posts RDMA WRITEs and
 * READs through registers on PF2 BAR2. The payload of a request is at its
 * local address:
 *   - card memory: the DDR window (Buffer with MemoryRangeType::DDR,
 *     getPhysAddr()), also what a peer's WRITEs and READs address;
 *   - a stream: Rdma::streamAddress(lane), the user-region stream
 *     rdma.tx<lane> for what is sent and rdma.rx<lane> for what is received.
 *     A WRITE from a stream takes ceil(length / 64) beats of the stream; data
 *     received for a stream address arrives with tlast on the last beat of
 *     each message. Use QPs 0..7 for WRITEs from a stream: only those are
 *     retransmitted from the stack's own buffer.
 */

#ifndef VRT_RDMA_HPP
#define VRT_RDMA_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace vrt {

/**
 * @brief Where the service is: from <ServiceLayer><Rdma .../> of the system map.
 */
struct RdmaInfo {
    uint32_t eth = 0;         ///< Ethernet port (0: DCMAC0 / QSFP0, 2: DCMAC1 / QSFP2)
    uint64_t baseAddr = 0;    ///< register window of the service (AXI address)
    uint64_t range = 0;       ///< its size
    uint64_t memBase = 0;     ///< card memory the service reaches
    uint64_t memRange = 0;    ///< its size
};

/**
 * @brief 32-bit register access at AXI addresses (hardware: PF2 BAR2).
 */
class RdmaRegisters {
   public:
    virtual ~RdmaRegisters() = default;
    virtual uint32_t read(uint64_t addr) = 0;
    virtual void write(uint64_t addr, uint32_t value) = 0;
};

/**
 * @brief A queue pair to connect. The two ends swap the PSNs: if this end has
 *        localPsn 0x100 and remotePsn 0x300, the peer has 0x300 and 0x100.
 */
struct RdmaQp {
    uint32_t qpn = 0;          ///< local QPN, 0..63
    uint32_t remoteQpn = 0;    ///< the peer's QPN
    std::string remoteIp;      ///< the peer's IPv4 address, dotted
    uint32_t localPsn = 0;     ///< first PSN expected from the peer
    uint32_t remotePsn = 0;    ///< first PSN sent to the peer
    uint32_t rkey = 0;
    uint16_t remotePort = 0;   ///< UDP port, 0 = 4791
};

/**
 * @brief Counters of the stack and of the service.
 */
struct RdmaStats {
    /// stack: RX frames, TX frames, ARP RX, ARP TX, ICMP RX, ICMP TX, RoCE RX,
    /// RoCE TX, PSN drops, retransmissions, ICRC drops, ACK hold cycles, TX
    /// packets trimmed, stray completions, bad-FCS drops, RX room drops
    std::array<uint32_t, 16> stack{};
    uint64_t requests = 0;         ///< requests (up to 4 KiB) handed to the transport
    uint64_t completions = 0;      ///< completed requests
    uint64_t messages = 0;         ///< completed messages
    uint32_t rdMem = 0, rdStream = 0, rdBad = 0;   ///< payload read requests by kind
    uint32_t wrMem = 0, wrStream = 0, wrBad = 0;   ///< payload write requests by kind
    uint32_t mm2sErrors = 0, s2mmErrors = 0;       ///< memory accesses not OKAY
    uint32_t rdLenErrors = 0, wrLenErrors = 0;     ///< packets ending where not expected
    /// frames the port dropped before the stack: bad FCS, no room (the stack
    /// was not taking them); 0 on services before version 1.1
    uint32_t rxFcsDrops = 0, rxRoomDrops = 0;
};

class Rdma {
   public:
    /// stream address bits of a local address
    static constexpr uint64_t STREAM_BIT = 1ull << 47;
    static constexpr uint64_t streamAddress(unsigned lane) {
        return STREAM_BIT | (static_cast<uint64_t>(lane & 1u) << 46);
    }

    // service registers (offsets in the service window)
    static constexpr uint64_t SVC_ID = 0x000, SVC_BUILD = 0x008, SVC_ETH = 0x00C;
    static constexpr uint64_t SVC_VERSION = 0x004, SVC_MEM_PORTS = 0x01C;
    static constexpr uint64_t SVC_PHY_CTRL = 0x020, SVC_PHY_STATUS = 0x024;
    static constexpr uint64_t SVC_RX_FCS_DROPS = 0x02C, SVC_RX_ROOM_DROPS = 0x030;
    static constexpr uint64_t SVC_COUNTERS = 0x100;
    static constexpr uint32_t SVC_ID_VALUE = 0x52444D53;    ///< "RDMS"
    // the stack's registers (rdma_200 register map), at STACK in the window
    static constexpr uint64_t STACK = 0x40'0000;
    static constexpr uint64_t ID = 0x000, MAC_LO = 0x020, MAC_HI = 0x024, IP = 0x028;
    static constexpr uint64_t ARP_LOOKUP = 0x02C, PENDING = 0x030;
    static constexpr uint64_t QP_LOCAL_QPN = 0x040, QP_REMOTE_QPN = 0x044, QP_LOCAL_PSN = 0x048;
    static constexpr uint64_t QP_REMOTE_PSN = 0x04C, QP_REMOTE_IP = 0x050, QP_REMOTE_PORT = 0x054;
    static constexpr uint64_t QP_RKEY = 0x058, QP_SET = 0x05C;
    static constexpr uint64_t SQ_CTRL = 0x080, SQ_LEN = 0x084, SQ_LADDR = 0x088, SQ_RADDR = 0x090;
    static constexpr uint64_t SQ_POST = 0x098, SQ_REQS = 0x0A0;
    static constexpr uint64_t CQ_COUNT = 0x100, CQ_MSGS = 0x108, STATS = 0x200;
    static constexpr uint32_t ID_VALUE = 0x52444D41;        ///< "RDMA"

    /**
     * @brief The service at @p info, through @p regs. Checks both IDs.
     *        Usually obtained with Device::getRdmaDevice().
     */
    Rdma(const RdmaInfo& info, std::shared_ptr<RdmaRegisters> regs);

    const RdmaInfo& getInfo() const { return info; }
    uint32_t getBuild();

    /// DCMAC port 0 RX aligned and no TX local fault
    bool linkUp();
    /// raw PHY status of the port (GT resets done, power good, MMCM lock ...)
    uint32_t phyStatus();
    /**
     * @brief Bring the link up: a GT RX datapath reset, then (if still down)
     *        the whole GT / DCMAC reset sequence; true when it is up.
     *        The stack keeps its addresses and queue pairs.
     */
    bool bringUp(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /// own MAC (48 bits) and IPv4 address (dotted); the subnet is /24
    void configure(uint64_t mac, const std::string& ip);
    /// look up @p ip (ARP); packets to an unresolved address are dropped
    void resolve(const std::string& ip);
    /// hand a queue pair to the stack
    void connect(const RdmaQp& qp);

    /**
     * @brief Post an RDMA WRITE of @p length bytes (a multiple of 4) from
     *        @p localAddr to @p remoteAddr at the peer of QP @p qpn.
     */
    void write(uint32_t qpn, uint64_t localAddr, uint64_t remoteAddr, uint32_t length);
    /// post an RDMA READ of @p length bytes from @p remoteAddr into @p localAddr
    void read(uint32_t qpn, uint64_t localAddr, uint64_t remoteAddr, uint32_t length);

    /// WRITE from a vrt::Buffer (MemoryRangeType::DDR), @p offsetBytes into it
    template <typename Buf>
    void write(uint32_t qpn, const Buf& src, uint64_t offsetBytes, uint64_t remoteAddr,
               uint32_t length) {
        write(qpn, memoryAddress(src.getPhysAddr(), offsetBytes, length), remoteAddr, length);
    }
    /// READ into a vrt::Buffer (MemoryRangeType::DDR), @p offsetBytes into it
    template <typename Buf>
    void read(uint32_t qpn, const Buf& dst, uint64_t offsetBytes, uint64_t remoteAddr,
              uint32_t length) {
        read(qpn, memoryAddress(dst.getPhysAddr(), offsetBytes, length), remoteAddr, length);
    }
    /// WRITE the next bytes of user-region stream rdma.tx<lane> to @p remoteAddr
    void writeFromStream(uint32_t qpn, unsigned lane, uint64_t remoteAddr, uint32_t length) {
        write(qpn, streamAddress(lane), remoteAddr, length);
    }
    /// READ @p remoteAddr into user-region stream rdma.rx<lane>
    void readToStream(uint32_t qpn, unsigned lane, uint64_t remoteAddr, uint32_t length) {
        read(qpn, streamAddress(lane), remoteAddr, length);
    }

    /// messages posted through this object
    uint64_t posted() const { return postedMsgs; }
    /// messages completed since this object was made
    uint64_t completed();
    /// wait until every posted message completed; false on timeout
    bool wait(std::chrono::milliseconds timeout = std::chrono::seconds(10));

    RdmaStats stats();

    /// IPv4 dotted -> 32 bits, first octet on top
    static uint32_t parseIp(const std::string& ip);

   private:
    uint32_t svcRead(uint64_t off) { return regs->read(info.baseAddr + off); }
    void svcWrite(uint64_t off, uint32_t v) { regs->write(info.baseAddr + off, v); }
    uint32_t stackRead(uint64_t off) { return regs->read(info.baseAddr + STACK + off); }
    void stackWrite(uint64_t off, uint32_t v) { regs->write(info.baseAddr + STACK + off, v); }
    uint64_t stackRead64(uint64_t off);
    uint64_t dcmacBase() const;
    void post(uint32_t ctrl, uint64_t localAddr, uint64_t remoteAddr, uint32_t length);
    void phyPulse(unsigned bit);
    bool waitLink(std::chrono::milliseconds timeout);
    uint64_t memoryAddress(uint64_t phys, uint64_t offsetBytes, uint32_t length) const;

    RdmaInfo info;
    std::shared_ptr<RdmaRegisters> regs;
    uint64_t postedMsgs = 0;
    uint64_t msgsAtStart = 0;
};

}  // namespace vrt

#endif  // VRT_RDMA_HPP
