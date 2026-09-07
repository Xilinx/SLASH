// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#include "transport.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// UniqueFd::reset
// ─────────────────────────────────────────────────────────────────────────────

void UniqueFd::reset(int new_fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Size of the control-message buffer for kMaxFdsPerMessage file descriptors.
constexpr std::size_t kCmsgBufSize = CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage);

// Build a transport-category error string from errno.
TransportError transport_errno(const char* context) {
    return TransportError{ErrorKind::Transport,
                          std::string(context) + ": " + std::strerror(errno)};
}

// Build a protocol-category error.
TransportError protocol_error(const char* message) {
    return TransportError{ErrorKind::Protocol, message};
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// send_message
// ─────────────────────────────────────────────────────────────────────────────

Result<void> send_message(int sockfd,
                          const slash_sysemu_socket_header& header,
                          std::span<const uint8_t>       payload,
                          std::span<const int>           fds) {
    // Issue 3: enforce the send-side payload cap symmetrically with recv.
    if (payload.size() > kMaxPayloadBytes) {
        return Result<void>::err(protocol_error("send_message: payload too large"));
    }

    // Build scatter-gather: header then payload.
    std::array<iovec, 2> iov{};
    iov[0].iov_base = const_cast<slash_sysemu_socket_header*>(&header);
    iov[0].iov_len  = sizeof(header);
    iov[1].iov_base = const_cast<uint8_t*>(payload.data());
    iov[1].iov_len  = payload.size();

    msghdr msg{};
    msg.msg_iov    = iov.data();
    msg.msg_iovlen = (payload.empty()) ? 1u : 2u;

    // Attach SCM_RIGHTS control message if FDs were supplied.
    alignas(cmsghdr) std::array<char, kCmsgBufSize> cmsg_buf{};
    if (!fds.empty()) {
        if (fds.size() > kMaxFdsPerMessage) {
            return Result<void>::err(protocol_error("send_message: too many FDs"));
        }
        std::size_t fd_bytes      = sizeof(int) * fds.size();
        msg.msg_control    = cmsg_buf.data();
        msg.msg_controllen = static_cast<socklen_t>(CMSG_SPACE(fd_bytes));

        cmsghdr* cmsg    = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = static_cast<socklen_t>(CMSG_LEN(fd_bytes));
        std::memcpy(CMSG_DATA(cmsg), fds.data(), fd_bytes);
    }

    ssize_t sent = ::sendmsg(sockfd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        return Result<void>::err(transport_errno("sendmsg"));
    }
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_message
// ─────────────────────────────────────────────────────────────────────────────

Result<ReceivedMessage> recv_message(int sockfd) {
    // Receive buffer: header + maximum payload.
    std::vector<uint8_t> data_buf(sizeof(slash_sysemu_socket_header) + kMaxPayloadBytes);

    iovec iov{};
    iov.iov_base = data_buf.data();
    iov.iov_len  = data_buf.size();

    alignas(cmsghdr) std::array<char, kCmsgBufSize> cmsg_buf{};

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.data();
    msg.msg_controllen = static_cast<socklen_t>(cmsg_buf.size());

    ssize_t n = ::recvmsg(sockfd, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
        return Result<ReceivedMessage>::err(transport_errno("recvmsg"));
    }
    if (n == 0) {
        // Peer closed the connection.
        return Result<ReceivedMessage>::err(
            TransportError{ErrorKind::Transport, "recvmsg: peer closed connection"});
    }

    // Detect data truncation.
    if (msg.msg_flags & MSG_TRUNC) {
        return Result<ReceivedMessage>::err(
            protocol_error("recvmsg: datagram truncated (MSG_TRUNC)"));
    }
    // Detect control-message truncation.
    if (msg.msg_flags & MSG_CTRUNC) {
        return Result<ReceivedMessage>::err(
            protocol_error("recvmsg: control message truncated (MSG_CTRUNC)"));
    }

    // The datagram must be at least as large as the header.
    if (static_cast<std::size_t>(n) < sizeof(slash_sysemu_socket_header)) {
        return Result<ReceivedMessage>::err(
            protocol_error("recvmsg: datagram too small to contain header"));
    }

    ReceivedMessage result;

    // Deserialise header.
    std::memcpy(&result.header, data_buf.data(), sizeof(slash_sysemu_socket_header));

    // Copy payload (bytes after the header).
    std::size_t payload_len = static_cast<std::size_t>(n) - sizeof(slash_sysemu_socket_header);
    result.payload.assign(data_buf.begin() + sizeof(slash_sysemu_socket_header),
                          data_buf.begin() + sizeof(slash_sysemu_socket_header) + payload_len);

    // Extract received SCM_RIGHTS FDs.
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        std::size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
        std::size_t fd_count = data_len / sizeof(int);
        const auto* fd_ptr   = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        // Reserve before the loop so that a single allocation is performed up
        // front.  UniqueFd's constructor and move are noexcept, so once
        // reserve() succeeds, the emplace_back calls below cannot throw and
        // leave received FDs unwrapped (and therefore leaked).
        result.fds.reserve(result.fds.size() + fd_count);
        for (std::size_t i = 0; i < fd_count; ++i) {
            result.fds.emplace_back(fd_ptr[i]);
        }
    }

    return Result<ReceivedMessage>::ok(std::move(result));
}

// ─────────────────────────────────────────────────────────────────────────────
// FD-index mapping helpers
// ─────────────────────────────────────────────────────────────────────────────

Result<UniqueFd> resolve_fd_index(ReceivedMessage& msg, uint32_t index) {
    if (index >= msg.fds.size()) {
        return Result<UniqueFd>::err(
            TransportError{ErrorKind::Protocol,
                           "resolve_fd_index: index " + std::to_string(index) +
                               " out of range (have " + std::to_string(msg.fds.size()) + " FDs)"});
    }
    UniqueFd resolved = std::move(msg.fds[index]);
    // Detect double-resolve: after moving, the slot holds fd==-1.  If the
    // resolved fd is already invalid, the caller is resolving the same index
    // twice, which is a protocol error.
    if (!resolved) {
        return Result<UniqueFd>::err(
            TransportError{ErrorKind::Protocol,
                           "resolve_fd_index: index " + std::to_string(index) +
                               " already resolved (double-resolve)"});
    }
    return Result<UniqueFd>::ok(std::move(resolved));
}

Result<void> collect_fds_and_rewrite(std::vector<int>&                                    fd_list,
                                     std::initializer_list<std::reference_wrapper<int>>   field_refs) {
    // Reject the entire batch atomically: check the combined count before
    // touching any field so that struct fields are never partially rewritten.
    if (fd_list.size() + field_refs.size() > kMaxFdsPerMessage) {
        return Result<void>::err(
            TransportError{ErrorKind::Protocol,
                           "collect_fds_and_rewrite: would exceed kMaxFdsPerMessage (" +
                               std::to_string(kMaxFdsPerMessage) + ")"});
    }
    for (auto ref : field_refs) {
        int& field = ref.get();
        int  index = static_cast<int>(fd_list.size());
        fd_list.push_back(field);
        field = index;
    }
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Request / response helper
// ─────────────────────────────────────────────────────────────────────────────

Result<ReceivedMessage> send_request(int sockfd,
                                     const slash_sysemu_socket_header& request_header,
                                     std::span<const uint8_t>       request_payload,
                                     std::span<const int>           request_fds) {
    auto send_result = send_message(sockfd, request_header, request_payload, request_fds);
    if (!send_result) {
        return Result<ReceivedMessage>::err(std::move(send_result.error()));
    }

    auto recv_result = recv_message(sockfd);
    if (!recv_result) {
        return recv_result;
    }

    const ReceivedMessage& response = recv_result.value();

    // Validate that the response matches the request.
    if (response.header.sequence_id != request_header.sequence_id) {
        return Result<ReceivedMessage>::err(TransportError{
            ErrorKind::Protocol,
            "send_request: sequence_id mismatch (sent " +
                std::to_string(request_header.sequence_id) + ", got " +
                std::to_string(response.header.sequence_id) + ")"});
    }
    if (response.header.ioctl_op != request_header.ioctl_op) {
        return Result<ReceivedMessage>::err(TransportError{
            ErrorKind::Protocol,
            "send_request: ioctl_op mismatch (sent " +
                std::to_string(request_header.ioctl_op) + ", got " +
                std::to_string(response.header.ioctl_op) + ")"});
    }

    return recv_result;
}

} // namespace slash_sysemu
