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

#pragma once

#include "protocol.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// Error types
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Category of a transport-layer error.
 *
 * Callers that need to surface peer-gone situations (e.g. to return -ENODEV)
 * can test for TransportError::kind == ErrorKind::Transport.
 */
enum class ErrorKind {
    Transport, /**< OS-level failure: EPIPE, ECONNRESET, peer closed, etc. */
    Protocol,  /**< Protocol violation: datagram truncated, sequence mismatch, etc. */
};

struct TransportError {
    ErrorKind   kind;
    std::string message;
};

/**
 * @brief Lightweight Result<T, TransportError> for the transport layer.
 *
 * std::expected is C++23; this small stand-in avoids that dependency while
 * keeping a similar API surface.  Use ok() / err() to construct, has_value()
 * to test, and value() / error() to extract.
 */
template <typename T>
class Result {
public:
    // Success factory
    static Result ok(T v) {
        Result r;
        r.storage_ = std::move(v);
        return r;
    }

    // Error factory
    static Result err(TransportError e) {
        Result r;
        r.storage_ = std::move(e);
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] TransportError& error() & { return std::get<TransportError>(storage_); }
    [[nodiscard]] const TransportError& error() const& { return std::get<TransportError>(storage_); }

private:
    std::variant<T, TransportError> storage_;
};

// Specialisation for void-like success results.
template <>
class Result<void> {
public:
    static Result ok() {
        Result r;
        r.ok_ = true;
        return r;
    }

    static Result err(TransportError e) {
        Result r;
        r.ok_ = false;
        r.error_ = std::move(e);
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] TransportError& error() & {
        assert(!ok_ && "error() called on a successful Result<void>");
        return error_;
    }
    [[nodiscard]] const TransportError& error() const& {
        assert(!ok_ && "error() called on a successful Result<void>");
        return error_;
    }

private:
    bool ok_{false};
    TransportError error_;
};

// ─────────────────────────────────────────────────────────────────────────────
// RAII file descriptor
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Owning wrapper around a POSIX file descriptor.
 *
 * On destruction the descriptor is closed unless it was released with release()
 * or the object was default-constructed (fd == -1).
 */
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& o) noexcept : fd_(o.release()) {}
    UniqueFd& operator=(UniqueFd&& o) noexcept {
        if (this != &o) {
            reset(o.release());
        }
        return *this;
    }

    ~UniqueFd() { reset(); }

    /** Return the raw fd without transferring ownership. */
    [[nodiscard]] int get() const noexcept { return fd_; }

    /** Returns true if the fd is valid (not -1). */
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    /** Release ownership and return the raw fd (caller must close). */
    [[nodiscard]] int release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    /** Close the current fd (if any) and take ownership of @p new_fd. */
    void reset(int new_fd = -1) noexcept;

private:
    int fd_{-1};
};

// ─────────────────────────────────────────────────────────────────────────────
// Received message
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A fully-received datagram: header, payload bytes, and any received FDs.
 *
 * The FDs are wrapped in UniqueFd so they are released automatically if the
 * caller does not explicitly take ownership.
 */
struct ReceivedMessage {
    slash_sysemu_socket_header  header;
    std::vector<uint8_t>     payload;
    std::vector<UniqueFd>    fds;      /**< Received SCM_RIGHTS FDs, close-on-exec. */
};

// ─────────────────────────────────────────────────────────────────────────────
// Socket send / receive
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Maximum number of FDs that may be sent or received in a single datagram.
 *
 * POSIX and Linux impose a per-message limit on SCM_RIGHTS FDs; we impose our
 * own lower cap to keep the control-message buffer statically sized.
 */
static constexpr std::size_t kMaxFdsPerMessage = 64;

/**
 * @brief Maximum payload size (excluding header) accepted by the transport layer.
 *
 * Datagrams exceeding this size are rejected as protocol errors.  The value is
 * chosen to be larger than any current IOCTL argument struct while still being
 * safely stackable.
 */
static constexpr std::size_t kMaxPayloadBytes = 65536;

/**
 * @brief Send one SEQPACKET datagram on @p sockfd.
 *
 * The datagram wire format is: header bytes followed immediately by payload
 * bytes.  If @p fds is non-empty they are sent as a single SCM_RIGHTS control
 * message.
 *
 * @param sockfd   Connected SEQPACKET socket.
 * @param header   Message header (copied by value; caller may reuse).
 * @param payload  Arbitrary argument-struct bytes (may be empty).
 * @param fds      File descriptors to attach via SCM_RIGHTS (may be empty).
 * @return Result<void>: ok() or err() with ErrorKind::Transport on OS failure.
 */
Result<void> send_message(int sockfd,
                          const slash_sysemu_socket_header& header,
                          std::span<const uint8_t>       payload,
                          std::span<const int>           fds);

/**
 * @brief Receive one SEQPACKET datagram from @p sockfd.
 *
 * The receive buffer is sized to kMaxPayloadBytes + sizeof(header).  MSG_TRUNC
 * or MSG_CTRUNC flags in the returned msghdr are treated as protocol errors.
 * Received FDs are marked O_CLOEXEC via MSG_CMSG_CLOEXEC.
 *
 * @param sockfd   Connected SEQPACKET socket.
 * @return Result<ReceivedMessage> with the decoded datagram, or an error.
 *   - ErrorKind::Transport: OS-level failure or peer closed (zero-byte recv).
 *   - ErrorKind::Protocol:  Datagram too small, MSG_TRUNC, or MSG_CTRUNC.
 */
Result<ReceivedMessage> recv_message(int sockfd);

// ─────────────────────────────────────────────────────────────────────────────
// FD-index mapping helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Resolve an FD index from a received message to the corresponding UniqueFd.
 *
 * Per the protocol, argument structs that carry file descriptors store them as
 * indices into the list of SCM_RIGHTS FDs that arrived with the datagram.  This
 * helper resolves one such index.
 *
 * The FD at @p index is moved out of @p msg.fds (leaving -1 in its place), so
 * each index may only be resolved once per message.
 *
 * @param msg   The received message whose fds list is consulted.
 * @param index Zero-based index into msg.fds.
 * @return Result<UniqueFd> with the resolved fd, or ErrorKind::Protocol if the
 *         index is out of range.
 */
Result<UniqueFd> resolve_fd_index(ReceivedMessage& msg, uint32_t index);

/**
 * @brief Collect raw file descriptors to send and rewrite struct fields to indices.
 *
 * @p field_refs is a list of references to integer fields inside an argument
 * struct that currently hold actual file descriptors.  This function:
 *   1. Appends each raw fd to @p fd_list.
 *   2. Overwrites the referenced field with the zero-based index the fd will
 *      occupy in the ancillary data (i.e. its position in @p fd_list before the
 *      append).
 *
 * After the call, @p fd_list contains all fds to pass to send_message(), and the
 * struct fields contain the corresponding indices.
 *
 * If appending the new FDs would cause @p fd_list to exceed kMaxFdsPerMessage
 * entries, no fields are rewritten and ErrorKind::Protocol is returned — the
 * struct fields are left unchanged (no partial corruption).
 *
 * @param fd_list    Accumulator of raw fds; may already contain entries from
 *                   previous calls.
 * @param field_refs References to struct fields that hold fds and should be
 *                   rewritten to indices.
 * @return Result<void>: ok() on success, or ErrorKind::Protocol if the
 *         combined FD count would exceed kMaxFdsPerMessage.
 */
Result<void> collect_fds_and_rewrite(std::vector<int>&                          fd_list,
                                     std::initializer_list<std::reference_wrapper<int>> field_refs);

// ─────────────────────────────────────────────────────────────────────────────
// Request / response helper
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Send a request datagram and receive the matching response.
 *
 * Sends the request (header + payload + fds) and then receives one response
 * datagram.  If the response's sequence_id or ioctl_op does not match the
 * request, the function returns ErrorKind::Protocol.
 *
 * The caller is responsible for supplying a monotonically-increasing
 * sequence_id in @p request_header.
 *
 * @param sockfd          Connected SEQPACKET socket.
 * @param request_header  Header for the outgoing request.
 * @param request_payload Payload bytes (may be empty).
 * @param request_fds     FDs to attach as SCM_RIGHTS (may be empty).
 * @return Result<ReceivedMessage> containing the response, or an error.
 */
Result<ReceivedMessage> send_request(int sockfd,
                                     const slash_sysemu_socket_header& request_header,
                                     std::span<const uint8_t>       request_payload,
                                     std::span<const int>           request_fds);

} // namespace slash_sysemu
