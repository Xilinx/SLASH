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

/// @file qdma_driver_backend.cpp
/// @brief Implementation of the off-the-shelf QDMA-driver raw-transfer backend.

#include "qdma_driver_backend.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>

// qdma_nl.h defines unused file-scope static lookup arrays (xnl_attr_str /
// xnl_op_str); silence the resulting -Wunused warnings without touching the
// vendored upstream header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
extern "C" {
#include <qdma_nl.h>
}
#pragma GCC diagnostic pop

#include "bdf.hpp"

namespace smi::qdma_driver {

namespace {

/// Generous receive buffer: the device list dump grows with the number of
/// queues/functions, so keep this comfortably larger than XNL_RESP_BUFLEN_MAX.
constexpr size_t RESP_BUF_LEN = 256 * 1024;

[[noreturn]] void throwSystemError(const std::string& message) {
    throw std::runtime_error(message + ": " + std::strerror(errno));
}

} // namespace

/// Minimal generic-netlink client for the QDMA driver's "xnl_pf" family.
///
/// This is a focused port of the netlink plumbing in the upstream `dma-ctl`
/// utility (QDMA/linux-kernel/apps/dma-utils/dmactl.c): resolve the family id,
/// send a command carrying a handful of u32 attributes, and parse the reply's
/// attributes / generic message text.
class XnlClient {
public:
    XnlClient() {
        fd_ = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
        if (fd_ < 0) {
            throwSystemError("Failed to open QDMA netlink socket");
        }

        struct sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        if (bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            const int err = errno;
            close(fd_);
            fd_ = -1;
            errno = err;
            throwSystemError("Failed to bind QDMA netlink socket");
        }

        // Don't block forever if the driver isn't present / doesn't answer.
        struct timeval tv{};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        (void)setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        family_ = resolveFamily(XNL_NAME_PF);
    }

    ~XnlClient() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    XnlClient(const XnlClient&) = delete;
    XnlClient& operator=(const XnlClient&) = delete;

    /// Parsed netlink response: scalar attributes plus any generic message text.
    struct Response {
        std::array<uint32_t, XNL_ATTR_MAX> attrs{};
        std::array<bool, XNL_ATTR_MAX> present{};
        std::string genmsg;
    };

    /// Send command @p op for device index @p devIndex with the given u32
    /// attributes (DEV_IDX and a response-buffer-length hint are added
    /// automatically) and return the parsed response.
    Response sendCmd(uint8_t op, uint32_t devIndex,
                     const std::vector<std::pair<uint16_t, uint32_t>>& attrs) {
        std::vector<char> buf(RESP_BUF_LEN, 0);
        auto* n = reinterpret_cast<struct nlmsghdr*>(buf.data());

        n->nlmsg_type = family_;
        n->nlmsg_flags = NLM_F_REQUEST;
        n->nlmsg_pid = getpid();
        n->nlmsg_seq = seq_++;
        n->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);

        auto* g = reinterpret_cast<struct genlmsghdr*>(NLMSG_DATA(n));
        g->cmd = op;
        g->version = XNL_VERSION;

        addIntAttr(n, XNL_ATTR_DEV_IDX, devIndex);
        for (const auto& [type, val] : attrs) {
            addIntAttr(n, type, val);
        }
        // Tell the kernel how large a response we can accept.
        addIntAttr(n, XNL_ATTR_RSP_BUF_LEN, static_cast<uint32_t>(buf.size()));

        sendMsg(n);
        return recvMsg(buf);
    }

private:
    static uint16_t alignedAttrLen(uint16_t payload) {
        return static_cast<uint16_t>(NLA_HDRLEN + payload);
    }

    static void addIntAttr(struct nlmsghdr* n, uint16_t type, uint32_t value) {
        auto* attr = reinterpret_cast<struct nlattr*>(reinterpret_cast<char*>(n) + n->nlmsg_len);
        attr->nla_type = type;
        attr->nla_len = alignedAttrLen(sizeof(uint32_t));
        std::memcpy(reinterpret_cast<char*>(attr) + NLA_HDRLEN, &value, sizeof(value));
        n->nlmsg_len += NLMSG_ALIGN(attr->nla_len);
    }

    static void addStrAttr(struct nlmsghdr* n, uint16_t type, const char* s) {
        auto* attr = reinterpret_cast<struct nlattr*>(reinterpret_cast<char*>(n) + n->nlmsg_len);
        const size_t len = std::strlen(s) + 1;
        attr->nla_type = type;
        attr->nla_len = alignedAttrLen(static_cast<uint16_t>(len));
        std::memcpy(reinterpret_cast<char*>(attr) + NLA_HDRLEN, s, len);
        n->nlmsg_len += NLMSG_ALIGN(attr->nla_len);
    }

    void sendMsg(struct nlmsghdr* n) {
        struct sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        ssize_t rv = sendto(fd_, n, n->nlmsg_len, 0,
                            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (rv < 0 || static_cast<uint32_t>(rv) != n->nlmsg_len) {
            throwSystemError("QDMA netlink send failed");
        }
    }

    Response recvMsg(std::vector<char>& buf) {
        std::memset(buf.data(), 0, buf.size());
        ssize_t rv = recv(fd_, buf.data(), buf.size(), 0);
        if (rv < 0) {
            throwSystemError("QDMA netlink receive failed");
        }

        auto* n = reinterpret_cast<struct nlmsghdr*>(buf.data());
        if (n->nlmsg_type == NLMSG_ERROR) {
            int err = 0;
            if (n->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                auto* nlerr = reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(n));
                err = nlerr->error;
            }
            throw std::runtime_error("QDMA netlink returned an error response (" +
                                     std::to_string(err) + ")");
        }

        Response resp;
        auto* p = reinterpret_cast<unsigned char*>(buf.data()) + NLMSG_LENGTH(GENL_HDRLEN);
        int maxlen = static_cast<int>(n->nlmsg_len) - static_cast<int>(NLMSG_LENGTH(GENL_HDRLEN));
        while (maxlen > 0) {
            auto* na = reinterpret_cast<struct nlattr*>(p);
            if (na->nla_len < NLA_HDRLEN) {
                break;
            }
            const int len = NLA_ALIGN(na->nla_len);
            const char* payload = reinterpret_cast<const char*>(na) + NLA_HDRLEN;

            if (na->nla_type == XNL_ATTR_GENMSG) {
                resp.genmsg.assign(payload);
            } else if (na->nla_type < XNL_ATTR_MAX) {
                uint32_t v = 0;
                std::memcpy(&v, payload, sizeof(v));
                resp.attrs[na->nla_type] = v;
                resp.present[na->nla_type] = true;
            }

            p += len;
            maxlen -= len;
        }
        return resp;
    }

    uint16_t resolveFamily(const char* name) {
        std::vector<char> buf(RESP_BUF_LEN, 0);
        auto* n = reinterpret_cast<struct nlmsghdr*>(buf.data());

        n->nlmsg_type = GENL_ID_CTRL;
        n->nlmsg_flags = NLM_F_REQUEST;
        n->nlmsg_pid = getpid();
        n->nlmsg_seq = seq_++;
        n->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);

        auto* g = reinterpret_cast<struct genlmsghdr*>(NLMSG_DATA(n));
        g->cmd = CTRL_CMD_GETFAMILY;
        g->version = XNL_VERSION;

        addStrAttr(n, CTRL_ATTR_FAMILY_NAME, name);
        sendMsg(n);

        std::memset(buf.data(), 0, buf.size());
        ssize_t rv = recv(fd_, buf.data(), buf.size(), 0);
        if (rv < 0) {
            throwSystemError(std::string("Failed to resolve QDMA netlink family '") + name +
                             "' (is the upstream qdma driver loaded?)");
        }
        if (n->nlmsg_type == NLMSG_ERROR) {
            int err = 0;
            if (n->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                auto* nlerr = reinterpret_cast<struct nlmsgerr*>(NLMSG_DATA(n));
                err = nlerr->error;
            }
            throw std::runtime_error(std::string("QDMA netlink family '") + name +
                                     "' not found (netlink error " + std::to_string(err) +
                                     "; is the upstream qdma driver loaded?)");
        }

        auto* p = reinterpret_cast<unsigned char*>(buf.data()) + NLMSG_LENGTH(GENL_HDRLEN);
        int maxlen = static_cast<int>(n->nlmsg_len) - static_cast<int>(NLMSG_LENGTH(GENL_HDRLEN));
        while (maxlen > 0) {
            auto* na = reinterpret_cast<struct nlattr*>(p);
            if (na->nla_len < NLA_HDRLEN) {
                break;
            }
            if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
                uint16_t id = 0;
                std::memcpy(&id, reinterpret_cast<char*>(na) + NLA_HDRLEN, sizeof(id));
                return id;
            }
            const int len = NLA_ALIGN(na->nla_len);
            p += len;
            maxlen -= len;
        }
        throw std::runtime_error(std::string("QDMA netlink family '") + name +
                                 "' id not present in response");
    }

    int fd_ = -1;
    uint16_t family_ = 0;
    uint32_t seq_ = 0;
};

namespace {

/// Queue flags for a bidirectional AXI-MM queue pair.
constexpr uint32_t QFLAG_MM_BI = XNL_F_QMODE_MM | XNL_F_QDIR_BOTH;

/// Queue flags for `q start`.  In addition to mode/direction, this must enable
/// the descriptor-ring writeback/completion-status reporting and fetch credit,
/// exactly as `dma-ctl q start` does by default (see
/// QDMA/linux-kernel/apps/dma-ctl/cmd_parse.c). Without the writeback bits the
/// poll-mode driver never observes MM completion and every transfer times out.
constexpr uint32_t QFLAG_MM_BI_START =
    QFLAG_MM_BI |
    XNL_F_CMPL_STATUS_EN | XNL_F_CMPL_STATUS_ACC_EN |
    XNL_F_CMPL_STATUS_PEND_CHK | XNL_F_CMPL_STATUS_DESC_EN |
    XNL_F_FETCH_CREDIT;

/// Default descriptor-ring size index for `q start`, matching `dma-ctl`'s
/// default ("ring size set to 2048").
constexpr uint32_t QRNGSZ_IDX_DEFAULT = 9;

} // namespace

QdmaDriverDevice::QdmaDriverDevice(const std::string& boardBdf,
                                   std::optional<uint32_t> ringSizeIndex)
    : nl_(std::make_unique<XnlClient>()),
      ringSizeIndex_(ringSizeIndex.value_or(QRNGSZ_IDX_DEFAULT)) {
    const ParsedBdf board = parseBdf(boardBdf);

    // Enumerate the driver's devices and find the QDMA function on this board.
    // Each PF line looks like: "qdma61001\t0000:61:00.1\tmax QP: 512, 0~511".
    XnlClient::Response resp = nl_->sendCmd(XNL_CMD_DEV_LIST, /*devIndex=*/0, {});
    if (resp.genmsg.empty()) {
        throw std::runtime_error(
            "Upstream QDMA driver reported no devices (dev list empty). "
            "Ensure the stock qdma driver is bound to the board.");
    }

    bool found = false;
    std::istringstream lines(resp.genmsg);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string name;
        std::string bdfStr;
        if (!(tokens >> name >> bdfStr)) {
            continue;
        }
        if (name.rfind("qdma", 0) != 0 || name.rfind("qdmavf", 0) == 0) {
            continue; // not a PF entry
        }

        ParsedBdf entry;
        try {
            entry = parseBdf(bdfStr);
        } catch (const std::exception&) {
            continue;
        }
        if (entry.base() != board.base()) {
            continue;
        }

        index_ = static_cast<unsigned>(std::stoul(name.substr(4), nullptr, 16));
        functionBdf_ = bdfStr;

        const auto pos = line.find("max QP:");
        if (pos != std::string::npos) {
            qmax_ = static_cast<unsigned>(std::strtoul(line.c_str() + pos + 7, nullptr, 10));
        }
        found = true;
        if (entry.function.value_or(0) == 1) {
            break; // Prefer the QDMA PF used by SLASH/V80.
        }
    }

    if (!found) {
        throw std::runtime_error(
            "No upstream QDMA function found for board " + board.base() +
            " (is the stock qdma driver bound to this board's PF?)");
    }

    // Ask the driver how many MM (memory-mapped) DMA engine channels this
    // function exposes so we can spread queues across them.  CPM5 (V80)
    // reports 2; older/soft IPs report 1.  Best-effort: if the query fails or
    // the attribute is absent, fall back to a single channel (channel 0).
    try {
        XnlClient::Response info = nl_->sendCmd(XNL_CMD_DEV_INFO, index_, {});
        if (info.present[XNL_ATTR_DEV_MM_CHANNEL_MAX] &&
            info.attrs[XNL_ATTR_DEV_MM_CHANNEL_MAX] > 0) {
            mmChannelMax_ = info.attrs[XNL_ATTR_DEV_MM_CHANNEL_MAX];
        }
    } catch (const std::exception&) {
        mmChannelMax_ = 1;
    }
}

QdmaDriverDevice::~QdmaDriverDevice() = default;

void QdmaDriverDevice::refreshQmax() {
    XnlClient::Response resp = nl_->sendCmd(XNL_CMD_DEV_LIST, /*devIndex=*/0, {});
    std::istringstream lines(resp.genmsg);
    std::string line;

    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string name;
        std::string bdfStr;
        if (!(tokens >> name >> bdfStr) || bdfStr != functionBdf_) {
            continue;
        }

        const auto pos = line.find("max QP:");
        if (pos == std::string::npos) {
            throw std::runtime_error("QDMA device list entry for " + functionBdf_ +
                                     " does not report max QP");
        }

        qmax_ = static_cast<unsigned>(std::strtoul(line.c_str() + pos + 7, nullptr, 10));
        return;
    }

    throw std::runtime_error("QDMA function " + functionBdf_ +
                             " disappeared from driver device list after qmax update");
}

void QdmaDriverDevice::ensureQmax(unsigned needed) {
    if (qmax_ >= needed) {
        return;
    }

    const std::string path = "/sys/bus/pci/devices/" + functionBdf_ + "/qdma/qmax";
    std::ofstream qmaxFile(path);
    if (!qmaxFile.is_open()) {
        throw std::runtime_error(
            "Need at least " + std::to_string(needed) + " queues but qmax is " +
            std::to_string(qmax_) + " and cannot open " + path +
            " to raise it (run as root, or set qmax manually with dma-ctl)");
    }
    qmaxFile << needed << std::endl;
    qmaxFile.close();
    if (qmaxFile.fail()) {
        throw std::runtime_error(
            "Failed to write qmax=" + std::to_string(needed) + " to " + path +
            " (queues may be active; stop them or reload the driver)");
    }
    refreshQmax();
    if (qmax_ < needed) {
        throw std::runtime_error(
            "QDMA qmax update requested " + std::to_string(needed) +
            " queues, but driver reports only " + std::to_string(qmax_));
    }
}

void QdmaDriverDevice::queueAdd(uint32_t qid) {
    XnlClient::Response resp = nl_->sendCmd(XNL_CMD_Q_ADD, index_,
        {{XNL_ATTR_QIDX, qid}, {XNL_ATTR_NUM_Q, 1}, {XNL_ATTR_QFLAG, QFLAG_MM_BI}});
    if (resp.present[XNL_ATTR_ERROR] && resp.attrs[XNL_ATTR_ERROR] != 0) {
        throw std::runtime_error("QDMA q add failed for qid " + std::to_string(qid) + ": " +
                                 (resp.genmsg.empty() ? "netlink error" : resp.genmsg));
    }
}

void QdmaDriverDevice::queueStart(uint32_t qid, uint32_t channel) {
    // The caller chooses the MM engine channel for this queue pair.  It has to
    // be carried on `q start`: the driver only reads XNL_ATTR_MM_CHANNEL in its
    // start handler (via qdma_queue_config) and defaults the queue to channel 0
    // whenever the attribute is absent.  mmChannelMax_ is always >= 1, so the
    // modulo keeps an out-of-range request inside the device's channel count.
    channel %= mmChannelMax_;
    XnlClient::Response resp = nl_->sendCmd(XNL_CMD_Q_START, index_,
        {{XNL_ATTR_QIDX, qid}, {XNL_ATTR_NUM_Q, 1}, {XNL_ATTR_QFLAG, QFLAG_MM_BI_START},
         {XNL_ATTR_QRNGSZ_IDX, ringSizeIndex_}, {XNL_ATTR_MM_CHANNEL, channel}});
    if (resp.present[XNL_ATTR_ERROR] && resp.attrs[XNL_ATTR_ERROR] != 0) {
        throw std::runtime_error("QDMA q start failed for qid " + std::to_string(qid) + ": " +
                                 (resp.genmsg.empty() ? "netlink error" : resp.genmsg));
    }
}

void QdmaDriverDevice::queueStop(uint32_t qid) noexcept {
    try {
        (void)nl_->sendCmd(XNL_CMD_Q_STOP, index_,
            {{XNL_ATTR_QIDX, qid}, {XNL_ATTR_NUM_Q, 1}, {XNL_ATTR_QFLAG, QFLAG_MM_BI}});
    } catch (...) {
        // Best-effort teardown.
    }
}

void QdmaDriverDevice::queueDel(uint32_t qid) noexcept {
    try {
        (void)nl_->sendCmd(XNL_CMD_Q_DEL, index_,
            {{XNL_ATTR_QIDX, qid}, {XNL_ATTR_NUM_Q, 1}, {XNL_ATTR_QFLAG, QFLAG_MM_BI}});
    } catch (...) {
        // Best-effort teardown.
    }
}

std::string QdmaDriverDevice::charDevPath(uint32_t qid) const {
    char name[64];
    std::snprintf(name, sizeof(name), "/dev/qdma%05x-MM-%u", index_, qid);
    return std::string(name);
}

QdmaDriverBuffer::QdmaDriverBuffer(QdmaDriverDevice& device, uint32_t qid,
                                   uint64_t physAddr, uint64_t size,
                                   int mmChannel)
    : device_(&device), qid_(qid), physAddr_(physAddr) {
    try {
        mapping_ = raw::createHostMapping(size, physAddr);

        // mmChannel < 0 means auto: spread the queue across channels by qid.
        const uint32_t channel = (mmChannel < 0)
            ? qid_
            : static_cast<uint32_t>(mmChannel);

        device_->queueAdd(qid_);
        queueAdded_ = true;
        device_->queueStart(qid_, channel);
        queueStarted_ = true;

        const std::string path = device_->charDevPath(qid_);
        fd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            throwSystemError("Failed to open QDMA char device " + path);
        }
    } catch (...) {
        cleanup();
        throw;
    }
}

QdmaDriverBuffer::~QdmaDriverBuffer() {
    cleanup();
}

void QdmaDriverBuffer::moveFrom(QdmaDriverBuffer& other) noexcept {
    device_ = other.device_;
    qid_ = other.qid_;
    queueAdded_ = other.queueAdded_;
    queueStarted_ = other.queueStarted_;
    fd_ = other.fd_;
    physAddr_ = other.physAddr_;
    mapping_ = other.mapping_;

    other.device_ = nullptr;
    other.qid_ = 0;
    other.queueAdded_ = false;
    other.queueStarted_ = false;
    other.fd_ = -1;
    other.physAddr_ = 0;
    other.mapping_ = raw::HostMapping{};
}

void QdmaDriverBuffer::cleanup() noexcept {
    if (fd_ >= 0) {
        (void)close(fd_);
        fd_ = -1;
    }
    if (device_ != nullptr && queueStarted_) {
        device_->queueStop(qid_);
        queueStarted_ = false;
    }
    if (device_ != nullptr && queueAdded_) {
        device_->queueDel(qid_);
        queueAdded_ = false;
    }
    raw::destroyHostMapping(mapping_);
}

} // namespace smi::qdma_driver
