/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <vrt/buffer.hpp>
#include <vrt/device.hpp>
#include <vrt/kernel.hpp>
#include <vrtd/bar.hpp>
#include <vrtd/bar_file.hpp>
#include <vrtd/device.hpp>

namespace {

constexpr uint64_t DCMAC_BASE        = 0x0200'0000ULL;
constexpr uint64_t GT_CTRL_BASE      = 0x0204'0000ULL;
constexpr uint64_t GT_MONITOR_BASE   = 0x0204'0200ULL;
constexpr uint64_t GT_DATAPATH_BASE  = 0x0204'0400ULL;
constexpr uint64_t IP_STRIDE         = 0x0100'0000ULL;

inline uint64_t ipBase(uint64_t base, int macId) {
    return base + IP_STRIDE * static_cast<uint64_t>(macId);
}

// --- BAR-mediated MMIO -----------------------------------------------------

class BarMmio {
   public:
    BarMmio(vrtd::BarFile& bar, uint64_t baseOffset)
        : bar_(bar), base_(baseOffset) {}

    uint32_t read(uint32_t off) const {
        auto p = bar_.getPtr<uint32_t>(vrtd::BarFile::Direction::Read,
                                       static_cast<size_t>(base_ + off));
        return *p;
    }

    void write(uint32_t off, uint32_t value) {
        auto p = bar_.getPtr<uint32_t>(vrtd::BarFile::Direction::Write,
                                       static_cast<size_t>(base_ + off));
        *p = value;
    }

    uint64_t readLong(uint32_t off) const {
        const uint32_t lo = read(off);
        const uint32_t hi = read(off + 4);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

   protected:
    vrtd::BarFile& bar_;
    uint64_t base_;
};

// --- AXI GPIO blocks -------------------------------------------------------

// AXI-GPIO data registers: gpio0 at +0x0, gpio1 at +0x8.
class AxiGpio : public BarMmio {
   public:
    using BarMmio::BarMmio;

    uint32_t readGpio(uint32_t gpio = 0) const { return read(8 * gpio); }
    void writeGpio(uint32_t gpio, uint32_t value) { write(8 * gpio, value); }
};

// Per axi_gt_controller.py: gt_reset is bit 0 of GPIO0 (1-bit field).
class AxiGtController : public AxiGpio {
   public:
    using AxiGpio::AxiGpio;

    void setGtReset(uint32_t value) {
        constexpr uint32_t shift = 0;
        constexpr uint32_t mask  = 0x1u;
        uint32_t cur = readGpio(0);
        cur &= ~(mask << shift);
        cur |= (value & mask) << shift;
        writeGpio(0, cur);
    }
};

// Per gpio_monitor.py shift map.
class AxiGpioMonitor : public AxiGpio {
   public:
    using AxiGpio::AxiGpio;

    uint32_t gt0TxResetDone() const { return field(0, 4); }
    uint32_t gt1TxResetDone() const { return field(4, 4); }
    uint32_t gt0RxResetDone() const { return field(8, 4); }
    uint32_t gt1RxResetDone() const { return field(12, 4); }

   private:
    uint32_t field(uint32_t shift, uint32_t bits) const {
        const uint32_t mask = (1u << bits) - 1u;
        return (readGpio(0) >> shift) & mask;
    }
};

// --- DCMAC ----------------------------------------------------------------

class Dcmac : public BarMmio {
   public:
    // Register offsets (subset of dcmac_reg.py needed for align-RX init + stats).
    static constexpr uint32_t GLOBAL_MODE                 = 0x0004;
    static constexpr uint32_t GLOBAL_CONTROL_REG_RX       = 0x00F0;
    static constexpr uint32_t GLOBAL_CONTROL_REG_TX       = 0x00F8;
    static constexpr uint32_t C0_CHANNEL_CONTROL_REG_RX   = 0x1030;
    static constexpr uint32_t C0_CHANNEL_CONTROL_REG_TX   = 0x1038;
    static constexpr uint32_t C0_TX_MODE_REG              = 0x1040;
    static constexpr uint32_t C0_RX_MODE_REG              = 0x1044;
    static constexpr uint32_t C0_PORT_CONTROL_REG_RX      = 0x10F0;
    static constexpr uint32_t C0_PORT_TICK_REG_RX         = 0x10F4;
    static constexpr uint32_t C0_PORT_CONTROL_REG_TX      = 0x10F8;
    static constexpr uint32_t C0_PORT_TICK_REG_TX         = 0x10FC;
    static constexpr uint32_t C0_STAT_CHAN_TX_MAC_STATUS  = 0x1100;
    static constexpr uint32_t C0_STAT_CHAN_TX_MAC_RT      = 0x1104;
    static constexpr uint32_t C0_STAT_CHAN_RX_MAC_STATUS  = 0x1140;
    static constexpr uint32_t C0_STAT_CHAN_RX_MAC_RT      = 0x1144;
    static constexpr uint32_t C0_STAT_PORT_RX_PHY_STATUS  = 0x1C00;
    static constexpr uint32_t C0_STAT_PORT_RX_PHY_RT      = 0x1C04;
    static constexpr uint32_t TX_STATS_READY              = 0x1808;  // baseoffset_tx + 0x808
    static constexpr uint32_t RX_STATS_READY              = 0x1C08;  // C0_STAT_PORT_RX_STATISTICS_READY

    // Bit positions inside per-channel mode regs.
    static constexpr uint32_t C0_CTL_TX_TICK_REG_MODE_SEL_BIT = 4;
    static constexpr uint32_t C0_CTL_RX_TICK_REG_MODE_SEL_BIT = 11;

    // Bit positions inside GLOBAL_MODE.
    static constexpr uint32_t CTL_TX_ALL_CH_TICK_REG_MODE_SEL_BIT = 1;
    static constexpr uint32_t CTL_RX_ALL_CH_TICK_REG_MODE_SEL_BIT = 5;

    // Status bit positions.
    static constexpr uint32_t TX_LOCAL_FAULT_BIT = 0;  // C0_STAT_CHAN_TX_MAC_RT
    static constexpr uint32_t RX_STATUS_BIT      = 0;  // C0_STAT_PORT_RX_PHY_RT
    static constexpr uint32_t RX_ALIGNED_BIT     = 2;

    Dcmac(vrtd::BarFile& bar, uint64_t baseOffset, int macId)
        : BarMmio(bar, baseOffset), macId_(macId) {
        setPmTickTrigger();
    }

    int macId() const { return macId_; }

    void setPmTickTrigger() {
        uint32_t v = read(GLOBAL_MODE);
        v |= (1u << CTL_TX_ALL_CH_TICK_REG_MODE_SEL_BIT);
        v |= (1u << CTL_RX_ALL_CH_TICK_REG_MODE_SEL_BIT);
        write(GLOBAL_MODE, v);
    }

    bool txAligned() const {
        return ((read(C0_STAT_CHAN_TX_MAC_RT) >> TX_LOCAL_FAULT_BIT) & 0x1u) == 0;
    }

    bool rxAligned() const {
        const uint32_t v = read(C0_STAT_PORT_RX_PHY_RT);
        const bool status  = ((v >> RX_STATUS_BIT)  & 0x1u) == 1;
        const bool aligned = ((v >> RX_ALIGNED_BIT) & 0x1u) == 1;
        return status && aligned;
    }

    bool linkUp() const { return txAligned() && rxAligned(); }

    // Mirrors DCMAC.reset_tx() in dcmac_mmio.py:306.
    bool resetTx(bool clearStatusHistory = true) {
        constexpr uint32_t ALL_ONES = 0xFFFFFFFFu;
        write(GLOBAL_CONTROL_REG_TX, ALL_ONES);
        for (int i = 0; i < 6; ++i) write(C0_PORT_CONTROL_REG_TX + 0x1000u * i, ALL_ONES);
        for (int i = 0; i < 6; ++i) write(C0_CHANNEL_CONTROL_REG_TX + 0x1000u * i, ALL_ONES);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Release port resets, then core reset.
        for (int i = 0; i < 6; ++i) write(C0_PORT_CONTROL_REG_TX + 0x1000u * i, 0);
        write(GLOBAL_CONTROL_REG_TX, 0);

        bool ok = false;
        for (int i = 0; i < 10; ++i) {
            if (txAligned()) { ok = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!ok) std::cout << "TX status: local fault" << std::endl;

        // Release channel flush.
        for (int i = 0; i < 6; ++i) write(C0_CHANNEL_CONTROL_REG_TX + 0x1000u * i, 0);

        if (clearStatusHistory) clearLatchedFlags();
        return ok;
    }

    // Mirrors DCMAC.reset_rx() in dcmac_mmio.py:341.
    bool resetRx(bool clearStatusHistory = true) {
        constexpr int ACTIVE_PORTS = 6;
        write(GLOBAL_CONTROL_REG_RX, 7);
        for (int i = 0; i < 6; ++i) write(C0_PORT_CONTROL_REG_RX + 0x1000u * i, 2);
        for (int i = 0; i < 6; ++i) write(C0_CHANNEL_CONTROL_REG_RX + 0x1000u * i, 1);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Release: core, then flush, then serdes (ACTIVE_PORTS).
        write(GLOBAL_CONTROL_REG_RX, 0);
        for (int i = 0; i < ACTIVE_PORTS; ++i) write(C0_CHANNEL_CONTROL_REG_RX + 0x1000u * i, 0);
        for (int i = 0; i < ACTIVE_PORTS; ++i) write(C0_PORT_CONTROL_REG_RX + 0x1000u * i, 0);

        bool ok = false;
        for (int i = 0; i < 10; ++i) {
            if (rxAligned()) { ok = true; break; }
            std::cout << "." << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (!ok) std::cout << "\nWARN: Chn 0 RX failed to achieve alignment" << std::endl;
        else     std::cout << std::endl;

        if (clearStatusHistory) clearLatchedFlags();
        return ok;
    }

    // Same idea as DCMAC.clear_latched_flags(), but enumerated explicitly
    // rather than driven by a register dictionary.
    void clearLatchedFlags() {
        constexpr uint32_t ALL_ONES = 0xFFFFFFFFu;
        // Per-channel latched MAC status.
        write(C0_STAT_CHAN_TX_MAC_STATUS, ALL_ONES);
        write(C0_STAT_CHAN_RX_MAC_STATUS, ALL_ONES);
        // Per-port latched PHY status.
        write(C0_STAT_PORT_RX_PHY_STATUS, ALL_ONES);
    }

    // Trigger a PM tick on the TX stats counters and dump headline values.
    void txStats(uint8_t port = 0) {
        if (port != 0) throw std::out_of_range("port must be 0");
        const uint32_t portBase = 0x1000u * (port + 1);  // 0x1000
        const uint32_t statsBase = portBase + 0x0200u;   // 0x1200

        // Switch per-channel TX tick to register-driven mode.
        uint32_t mode = read(portBase + 0x40);  // C0_TX_MODE_REG
        mode |= (1u << C0_CTL_TX_TICK_REG_MODE_SEL_BIT);
        write(portBase + 0x40, mode);

        // Pulse the per-port tick register.
        write(portBase + 0xFC, 0);  // C0_PORT_TICK_REG_TX
        write(portBase + 0xFC, 1);

        // Wait for stats-ready word to go non-zero (best effort).
        for (int i = 0; i < 10; ++i) {
            if (read(portBase + 0x808) != 0) break;
        }

        // Headline counters (offsets relative to statsBase, all 64-bit).
        const uint64_t totalBytes       = readLong(statsBase + 0x00);
        const uint64_t totalPackets     = readLong(statsBase + 0x10);
        const uint64_t totalGoodPackets = readLong(statsBase + 0x18);
        const uint64_t frameError       = readLong(statsBase + 0x20);
        const uint64_t badFcs           = readLong(statsBase + 0x28);

        std::cout << "DCMAC " << macId_ << " TX[" << +port << "]:"
                  << " TOTAL_BYTES=" << totalBytes
                  << " TOTAL_PACKETS=" << totalPackets
                  << " TOTAL_GOOD_PACKETS=" << totalGoodPackets
                  << " FRAME_ERROR=" << frameError
                  << " BAD_FCS=" << badFcs
                  << std::endl;
    }

    void rxStats(uint8_t port = 0) {
        if (port != 0) throw std::out_of_range("port must be 0");
        const uint32_t portBase = 0x1000u * (port + 1);  // 0x1000
        const uint32_t statsBase = portBase + 0x0400u;   // 0x1400

        // Switch per-channel RX tick to register-driven mode.
        uint32_t mode = read(portBase + 0x44);  // C0_RX_MODE_REG
        mode |= (1u << C0_CTL_RX_TICK_REG_MODE_SEL_BIT);
        write(portBase + 0x44, mode);

        // Pulse the per-port tick register.
        write(portBase + 0xF4, 0);  // C0_PORT_TICK_REG_RX
        write(portBase + 0xF4, 1);

        for (int i = 0; i < 10; ++i) {
            if (read(portBase + 0xC08) != 0) break;
        }

        // RX layout differs from TX (see dcmac_reg.py:558).
        const uint64_t totalBytes       = readLong(statsBase + 0x00);
        const uint64_t totalPackets     = readLong(statsBase + 0x10);
        const uint64_t totalGoodPackets = readLong(statsBase + 0x18);
        const uint64_t badCodeCount     = readLong(statsBase + 0x28);
        const uint64_t badFcs           = readLong(statsBase + 0x30);

        std::cout << "DCMAC " << macId_ << " RX[" << +port << "]:"
                  << " TOTAL_BYTES=" << totalBytes
                  << " TOTAL_PACKETS=" << totalPackets
                  << " TOTAL_GOOD_PACKETS=" << totalGoodPackets
                  << " BAD_CODE=" << badCodeCount
                  << " BAD_FCS=" << badFcs
                  << std::endl;
    }

   private:
    int macId_;
};

// Mirrors the args.align_rx=1, args.init=False branch of
// dcmac_logic_init() in dcmac_init.py. First attempt does RX-only datapath
// reset; on retry, also resets DCMAC TX (matches reset_tx = tx_rst_success
// flip in the original).
bool dcmacAlignRx(Dcmac& dcmac, AxiGtController& gtCtrl, AxiGpioMonitor& monitor,
                  AxiGpio& gtDatapath) {
    constexpr int NUM_RETRIES = 10;

    bool resetTx = false;  // start as args.init = false
    for (int attempt = 0; attempt < NUM_RETRIES; ++attempt) {
        std::cout << "DCMAC " << dcmac.macId() << " bring-up attempt "
                  << attempt + 1 << "/" << NUM_RETRIES;

        // 1: GT reset.
        if (resetTx) {
            std::cout << " (full GT reset)" << std::flush;
            gtCtrl.setGtReset(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            gtCtrl.setGtReset(0);
        } else {
            std::cout << " (RX datapath reset)" << std::flush;
            gtDatapath.writeGpio(0, 0xF);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            gtDatapath.writeGpio(0, 0x0);
        }

        // 2: wait for GT reset done.
        bool ready = false;
        for (int i = 0; i < 20; ++i) {
            if (monitor.gt0TxResetDone() == 0xF && monitor.gt0RxResetDone() == 0xF) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!ready) {
            std::cout << " | GT reset_done not asserted, retrying" << std::endl;
            continue;
        }

        // 3: optional DCMAC TX reset.
        bool txOk = true;
        if (resetTx) {
            std::cout << " | resetting DCMAC TX" << std::flush;
            txOk = dcmac.resetTx(false);  // status cleared after RX reset
        }

        // 4: DCMAC RX reset.
        std::cout << " | resetting DCMAC RX " << std::flush;
        const bool rxOk = dcmac.resetRx(true);

        if (txOk && rxOk) {
            std::cout << "DCMAC " << dcmac.macId() << " aligned after "
                      << attempt + 1 << " attempt(s)" << std::endl;
            return true;
        }
        // From next attempt onward, also include DCMAC TX in the reset.
        resetTx = txOk;
    }

    std::cerr << "DCMAC " << dcmac.macId() << " failed to align after "
              << NUM_RETRIES << " attempts" << std::endl;
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " <BDF> <vrtbin file>" << std::endl;
            return 1;
        }
        const std::string bdf         = argv[1];
        const std::string vrtbinFile  = argv[2];
        vrt::utils::Logger::setLogLevel(vrt::utils::LogLevel::DEBUG);

        vrt::Device device(bdf, vrtbinFile);

        // BAR 2 carries the DCMAC + GT control registers. Open once and
        // share the mapping across all helper wrappers below.
        vrtd::BarFile barFile =
            device.getHandle()->getVrtdDevice().getBar(2).openBarFile();

        // Per-MAC IP wrappers.
        Dcmac           dcmac0(barFile, ipBase(DCMAC_BASE,       0), 0);
        AxiGtController gtCtrl0(barFile, ipBase(GT_CTRL_BASE,    0));
        AxiGpioMonitor  monitor0(barFile, ipBase(GT_MONITOR_BASE, 0));
        AxiGpio         gtDatapath0(barFile, ipBase(GT_DATAPATH_BASE, 0));

        Dcmac           dcmac1(barFile, ipBase(DCMAC_BASE,       1), 1);
        AxiGtController gtCtrl1(barFile, ipBase(GT_CTRL_BASE,    1));
        AxiGpioMonitor  monitor1(barFile, ipBase(GT_MONITOR_BASE, 1));
        AxiGpio         gtDatapath1(barFile, ipBase(GT_DATAPATH_BASE, 1));

        if (!dcmacAlignRx(dcmac0, gtCtrl0, monitor0, gtDatapath0) ||
            !dcmacAlignRx(dcmac1, gtCtrl1, monitor1, gtDatapath1)) {
            device.cleanup();
            return 1;
        }

        vrt::Kernel traffic_producer_0(device, "traffic_producer_0");
        vrt::Kernel traffic_producer_1(device, "traffic_producer_1");
        traffic_producer_0.start(100, 0);
        traffic_producer_0.wait();
        traffic_producer_1.start(100, 1);
        traffic_producer_1.wait();

        dcmac0.txStats(0);
        dcmac0.rxStats(0);
        dcmac1.txStats(0);
        dcmac1.rxStats(0);

        device.cleanup();
    } catch (std::exception const& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
