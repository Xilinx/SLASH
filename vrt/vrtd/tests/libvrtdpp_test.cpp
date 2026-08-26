/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <gtest/gtest.h>

#include <vrtd/error.hpp>
#include <vrtd/session.hpp>
#include <vrtd/wire.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

/** Captured wire request with byte-exact owned body storage. */
struct ObservedRequest {
    uint16_t opcode; ///< vrtd request opcode.
    std::vector<uint8_t> body; ///< Unmodified request body bytes.
};

/**
 * Minimal single-client vrtd protocol peer used by public API tests.
 *
 * The fake implements only the opcodes needed to exercise libvrtdpp. It records
 * each validated request before replying and can hold GET_NUM_DEVICES in flight
 * to test deterministic close/request ordering. All filesystem and socket
 * resources are released during teardown.
 */
class FakeVrtd {
public:
    /**
     * Create a unique pathname listener and start the server thread.
     *
     * @param blockNumDevices Hold GET_NUM_DEVICES until explicitly released.
     */
    explicit FakeVrtd(bool blockNumDevices = false)
        : blockNumDevices(blockNumDevices)
        , path(
            "vrtdpp-" + std::to_string(getpid()) + "-" +
            std::to_string(nextId.fetch_add(1)) + ".sock"
        )
    {
        /* Bind a short relative path so sockaddr_un limits are deterministic. */
        listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (listenFd < 0) {
            throw std::runtime_error("socket failed");
        }

        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            throw std::runtime_error("socket path is too long");
        }
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        unlink(path.c_str());
        if (bind(
                listenFd,
                reinterpret_cast<struct sockaddr *>(&address),
                sizeof(address)
            ) != 0 ||
            listen(listenFd, 1) != 0) {
            close(listenFd);
            listenFd = -1;
            unlink(path.c_str());
            throw std::runtime_error("bind/listen failed");
        }

        /* Do not start the peer until clients can connect successfully. */
        server = std::thread([this] { run(); });
    }

    /**
     * Stop the peer without leaving accept, recv, or the test gate blocked.
     *
     * Wake the condition variable before shutting sockets down, then join the
     * only server thread before unlinking its pathname.
     */
    ~FakeVrtd()
    {
        {
            std::lock_guard<std::mutex> lock(blockMutex);
            stop = true;
            releaseNumDevices = true;
        }
        blockCv.notify_all();

        int client = clientFd.load();
        if (client >= 0) {
            shutdown(client, SHUT_RDWR);
        }
        if (listenFd >= 0) {
            shutdown(listenFd, SHUT_RDWR);
            close(listenFd);
        }
        if (server.joinable()) {
            server.join();
        }
        unlink(path.c_str());
    }

    /** Native socket and thread ownership cannot be copied. */
    FakeVrtd(const FakeVrtd&) = delete;
    FakeVrtd& operator=(const FakeVrtd&) = delete;

    /** Return the listener path, valid for this fixture's lifetime. */
    const std::string& socketPath() const noexcept
    {
        return path;
    }

    /** Make subsequent BAR responses carry a deliberately unmappable FD. */
    void returnUnmappableBarFd() noexcept
    {
        unmappableBarFd.store(true);
    }

    /** Return a synchronized snapshot of the number of captured requests. */
    size_t requestCount() const
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        return requests.size();
    }

    /**
     * Return the most recently captured request for @p opcode.
     *
     * @throws std::runtime_error if no matching request was observed.
     */
    ObservedRequest lastRequest(uint16_t opcode) const
    {
        std::lock_guard<std::mutex> lock(requestMutex);
        for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
            if (it->opcode == opcode) {
                return *it;
            }
        }
        throw std::runtime_error("request not observed");
    }

    /**
     * Wait until GET_NUM_DEVICES reaches the deterministic server gate.
     *
     * A timeout releases the gate before throwing so future destruction cannot
     * deadlock on the blocked client request.
     */
    void waitForNumDevices()
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        if (!blockCv.wait_for(lock, 2s, [this] { return numDevicesSeen; })) {
            releaseNumDevices = true;
            lock.unlock();
            blockCv.notify_all();
            throw std::runtime_error("GET_NUM_DEVICES was not observed");
        }
    }

    /** Release a GET_NUM_DEVICES request held by the test gate. */
    void releaseBlockedNumDevices()
    {
        {
            std::lock_guard<std::mutex> lock(blockMutex);
            releaseNumDevices = true;
        }
        blockCv.notify_all();
    }

    /** Rethrow a server-thread failure in the calling test thread. */
    void rethrowServerError()
    {
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            error = serverError;
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

private:
    /**
     * Send a successful response matching one captured request.
     *
     * @tparam Body Packed wire response body type.
     * @param request Request whose sequence number must be echoed.
     * @param body Response body sent in the same packet as the header.
     * @param responseFd Optional descriptor transferred through SCM_RIGHTS.
     */
    template<typename Body>
    void sendResponse(
        const struct vrtd_req_header& request,
        const Body& body,
        int responseFd = -1
    )
    {
        struct vrtd_resp_header header = {};
        header.size = static_cast<uint16_t>(sizeof(body));
        header.ret = VRTD_RET_OK;
        header.seqno = request.seqno;
        struct iovec iov[] = {
            {&header, sizeof(header)},
            {const_cast<Body *>(&body), sizeof(body)},
        };
        struct msghdr message = {};
        message.msg_iov = iov;
        message.msg_iovlen = 2;

        std::array<char, CMSG_SPACE(sizeof(int))> control = {};
        if (responseFd >= 0) {
            /* SCM_RIGHTS duplicates the descriptor into the receiving process. */
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            auto *cmsg = CMSG_FIRSTHDR(&message);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &responseFd, sizeof(responseFd));
            message.msg_controllen = cmsg->cmsg_len;
        }

        if (sendmsg(clientFd.load(), &message, MSG_NOSIGNAL) < 0 && !stop) {
            throw std::runtime_error("sendmsg failed");
        }
    }

    /**
     * Decode a captured body after checking its exact wire size.
     *
     * Copying avoids alignment and aliasing assumptions about byte storage.
     */
    template<typename Request>
    static Request requestBody(const ObservedRequest& observed)
    {
        if (observed.body.size() != sizeof(Request)) {
            throw std::runtime_error("unexpected request size");
        }
        Request request = {};
        std::memcpy(&request, observed.body.data(), sizeof(request));
        return request;
    }

    /**
     * Dispatch one validated request to the small protocol script.
     *
     * Unsupported opcodes indicate that the C++ client issued traffic outside
     * this test's declared contract and are reported as server failures.
     */
    void handle(
        const struct vrtd_req_header& header,
        const ObservedRequest& observed
    )
    {
        switch (header.opcode) {
        case VRTD_REQ_GET_NUM_DEVICES: {
            if (blockNumDevices) {
                std::unique_lock<std::mutex> lock(blockMutex);
                numDevicesSeen = true;
                blockCv.notify_all();
                blockCv.wait(lock, [this] {
                    return releaseNumDevices || stop;
                });
            }
            struct vrtd_resp_get_num_devices response = {};
            response.num_devices = 1;
            sendResponse(header, response);
            break;
        }
        case VRTD_REQ_GET_DEVICE_INFO: {
            auto request =
                requestBody<struct vrtd_req_get_device_info>(observed);
            struct vrtd_resp_get_device_info response = {};
            std::strcpy(response.info.name, "fake-v80");
            std::strcpy(response.info.pci.bdf, "0000:65:00");
            response.info.pci.vendor_id = 0x10ee;
            response.info.pci.device_id = 0x50b4;
            response.info.pci.subsystem_vendor_id = 0x10ee;
            response.info.pci.subsystem_device_id =
                static_cast<uint16_t>(request.dev_number);
            sendResponse(header, response);
            break;
        }
        case VRTD_REQ_GET_BAR_INFO: {
            auto request =
                requestBody<struct vrtd_req_get_bar_info>(observed);
            struct vrtd_resp_get_bar_info response = {};
            response.bar_info.size = sizeof(response.bar_info);
            response.bar_info.bar_number = request.bar_number;
            response.bar_info.usable = 1;
            response.bar_info.start_address = 0x1000;
            response.bar_info.length = 4096;
            sendResponse(header, response);
            break;
        }
        case VRTD_REQ_GET_BAR_FD: {
            (void) requestBody<struct vrtd_req_get_bar_fd>(observed);

            int fd = -1;
            if (unmappableBarFd.load()) {
                int pipeFds[2] = {-1, -1};
                if (pipe2(pipeFds, O_CLOEXEC) != 0) {
                    throw std::runtime_error("pipe creation failed");
                }

                fd = pipeFds[0];
                close(pipeFds[1]);
            } else {
                fd = memfd_create("vrtdpp-bar", MFD_CLOEXEC);
                if (fd < 0 || ftruncate(fd, 4096) != 0) {
                    if (fd >= 0) {
                        close(fd);
                    }
                    throw std::runtime_error("memfd creation failed");
                }
            }

            struct vrtd_resp_get_bar_fd response = {};
            response.len = 4096;
            try {
                /*
                 * The client receives a duplicate through SCM_RIGHTS. Close the
                 * fake daemon's copy on both success and send failure.
                 */
                sendResponse(header, response, fd);
            } catch (...) {
                close(fd);
                throw;
            }
            close(fd);
            break;
        }
        case VRTD_REQ_QDMA_QPAIR_ADD: {
            auto request =
                requestBody<struct vrtd_req_qdma_qpair_add>(observed);
            struct vrtd_resp_qdma_qpair_add response = {};
            response.add = request.add;
            response.add.qid = 0;
            sendResponse(header, response);
            break;
        }
        case VRTD_REQ_QDMA_QPAIR_OP: {
            (void) requestBody<struct vrtd_req_qdma_qpair_op>(observed);
            struct vrtd_resp_qdma_qpair_op response = {};
            sendResponse(header, response);
            break;
        }
        default:
            throw std::runtime_error("unexpected opcode");
        }
    }

    /**
     * Accept one client and serve complete SOCK_SEQPACKET requests.
     *
     * Packets are validated before their bodies are sliced, recorded before
     * dispatch, and processed serially. Exceptions are saved for rethrow on the
     * test thread because failures cannot escape std::thread entry.
     */
    void run() noexcept
    {
        int accepted = -1;
        try {
            /* This fake intentionally models one libvrtd connection. */
            accepted = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (accepted < 0) {
                if (!stop) {
                    throw std::runtime_error("accept failed");
                }
                return;
            }
            clientFd.store(accepted);

            std::array<uint8_t, VRTD_MSG_MAX_SIZE> packet = {};
            while (!stop) {
                ssize_t length = recv(
                    accepted,
                    packet.data(),
                    packet.size(),
                    0
                );
                if (length <= 0) {
                    break;
                }
                if (static_cast<size_t>(length) <
                    sizeof(struct vrtd_req_header)) {
                    throw std::runtime_error("short request");
                }

                struct vrtd_req_header header = {};
                std::memcpy(&header, packet.data(), sizeof(header));
                if (static_cast<size_t>(length) !=
                    sizeof(header) + header.size) {
                    throw std::runtime_error("invalid request size");
                }

                ObservedRequest observed = {
                    header.opcode,
                    std::vector<uint8_t>(
                        packet.begin() + sizeof(header),
                        packet.begin() + length
                    ),
                };
                {
                    /* Publish the request before a scripted response unblocks the client. */
                    std::lock_guard<std::mutex> lock(requestMutex);
                    requests.push_back(observed);
                }
                handle(header, observed);
            }
        } catch (...) {
            /* Preserve the first server failure for deterministic test reporting. */
            std::lock_guard<std::mutex> lock(errorMutex);
            serverError = std::current_exception();
        }
        if (accepted >= 0) {
            shutdown(accepted, SHUT_RDWR);
            close(accepted);
        }
        clientFd.store(-1);
    }

    /** Monotonic suffix preventing socket-path collisions within the process. */
    inline static std::atomic<uint32_t> nextId{0};

    bool blockNumDevices; ///< Whether GET_NUM_DEVICES uses the test gate.
    std::string path; ///< Relative listener pathname.
    int listenFd{-1}; ///< Server-owned listening descriptor.
    std::atomic<int> clientFd{-1}; ///< Accepted descriptor visible to teardown.
    std::thread server; ///< Sole protocol-serving thread.
    std::atomic<bool> stop{false}; ///< Cross-thread terminal-state request.
    std::atomic<bool> unmappableBarFd{false}; ///< Return a pipe for BAR mmap failure.

    /** Protect captured request ordering and snapshots. */
    mutable std::mutex requestMutex;
    std::vector<ObservedRequest> requests; ///< Requests in receive order.

    /** Coordinate deterministic blocking of one GET_NUM_DEVICES response. */
    std::mutex blockMutex;
    std::condition_variable blockCv;
    bool numDevicesSeen{false}; ///< True after the blocked request arrives.
    bool releaseNumDevices{false}; ///< True when the scripted reply may proceed.

    /** Protect deferred exception transfer from the server thread. */
    std::mutex errorMutex;
    std::exception_ptr serverError; ///< Captured server failure, if any.
};

/** Assert that local misuse reports VRTD_RET_BAD_LIB_CALL specifically. */
void expectBadCall(const std::function<void()>& operation)
{
    try {
        operation();
        FAIL() << "operation did not throw";
    } catch (const vrtd::Error& error) {
        EXPECT_EQ(error.getErrorCode(), VRTD_RET_BAD_LIB_CALL);
    }
}

/** Child handles retain the connection and delete valid queue identifier zero. */
TEST(LibvrtdppTest, ChildrenKeepConnectionAliveAndDeleteQidZero)
{
    FakeVrtd daemon;
    std::optional<vrtd::Bar> bar;
    std::optional<vrtd::QdmaQpair> qpair;

    {
        vrtd::Session session(daemon.socketPath().c_str());
        auto device = session.getDevice(0);
        bar.emplace(device.getBar(2));
        struct slash_qdma_qpair_add config = {};
        config.size = sizeof(config);
        qpair.emplace(device.createQdmaQpair(config));
        EXPECT_EQ(qpair->getQid(), 0u);
    }

    {
        auto mapping = bar->openBarFile();
        EXPECT_EQ(mapping.getLen(), 4096u);
        mapping.close();
        mapping.close();
        EXPECT_TRUE(mapping.isClosed());
    }
    qpair.reset();

    auto deletion = daemon.lastRequest(VRTD_REQ_QDMA_QPAIR_OP);
    ASSERT_EQ(deletion.body.size(), sizeof(struct vrtd_req_qdma_qpair_op));
    struct vrtd_req_qdma_qpair_op request = {};
    std::memcpy(&request, deletion.body.data(), sizeof(request));
    EXPECT_EQ(request.dev_number, 0u);
    EXPECT_EQ(request.qid, 0u);
    EXPECT_EQ(request.op, static_cast<uint32_t>(SLASH_QDMA_QUEUE_OP_DEL));
    daemon.rethrowServerError();
}

/** BarFile moves reject either endpoint while it owns a live access session. */
TEST(LibvrtdppTest, BarFileMoveRejectsLiveAccess)
{
    FakeVrtd daemon;
    vrtd::Session session(daemon.socketPath().c_str());
    auto bar = session.getDevice(0).getBar(2);
    auto source = bar.openBarFile();

    {
        auto access = source.getPtr<uint32_t>(vrtd::BarFile::Direction::Read);

        EXPECT_THROW(
            (void) vrtd::BarFile(std::move(source)),
            std::runtime_error
        );
        EXPECT_FALSE(source.isClosed());
    }

    auto destination = bar.openBarFile();
    {
        auto access = source.getPtr<uint32_t>(vrtd::BarFile::Direction::Read);

        EXPECT_THROW(destination = std::move(source), std::runtime_error);
        EXPECT_FALSE(source.isClosed());
        EXPECT_FALSE(destination.isClosed());
    }

    {
        auto access =
            destination.getPtr<uint32_t>(vrtd::BarFile::Direction::Write);

        EXPECT_THROW(destination = std::move(source), std::runtime_error);
        EXPECT_FALSE(source.isClosed());
        EXPECT_FALSE(destination.isClosed());
    }

    daemon.rethrowServerError();
}

/** A BAR mmap failure closes only the received BAR FD, not the session socket. */
TEST(LibvrtdppTest, BarMapFailureKeepsSessionUsable)
{
    FakeVrtd daemon;
    daemon.returnUnmappableBarFd();

    vrtd::Session session(daemon.socketPath().c_str());
    auto bar = session.getDevice(0).getBar(2);

    try {
        (void) bar.openBarFile();
        FAIL() << "unmappable BAR did not fail";
    } catch (const vrtd::Error& error) {
        EXPECT_EQ(error.getErrorCode(), VRTD_RET_INTERNAL_ERROR);
    }

    EXPECT_EQ(session.getNumDevices(), 1u);
    daemon.rethrowServerError();
}

/** Explicit close invalidates every handle without issuing another request. */
TEST(LibvrtdppTest, ExplicitCloseInvalidatesAllSharedHandles)
{
    FakeVrtd daemon;
    vrtd::Session session(daemon.socketPath().c_str());
    auto device = session.getDevice(0);
    auto bar = device.getBar(2);
    size_t requestCount = daemon.requestCount();

    session.close();

    EXPECT_TRUE(session.isClosed());
    EXPECT_FALSE(session);
    expectBadCall([&] { (void) device.getBar(2); });
    expectBadCall([&] { (void) bar.openBarFile(); });
    EXPECT_EQ(daemon.requestCount(), requestCount);
    daemon.rethrowServerError();
}

/** Moving a Session facade leaves existing connection-owning children usable. */
TEST(LibvrtdppTest, SessionMovePreservesExistingChildren)
{
    FakeVrtd daemon;
    vrtd::Session source(daemon.socketPath().c_str());
    auto device = source.getDevice(0);

    vrtd::Session destination(std::move(source));

    EXPECT_TRUE(source.isClosed());
    EXPECT_TRUE(destination);
    EXPECT_TRUE(device.getBar(2).isUsable());
    daemon.rethrowServerError();
}

/** Session-level methods reject foreign Device indices before wire I/O. */
TEST(LibvrtdppTest, SessionRejectsDeviceFromAnotherConnection)
{
    FakeVrtd firstDaemon;
    FakeVrtd secondDaemon;
    vrtd::Session first(firstDaemon.socketPath().c_str());
    vrtd::Session second(secondDaemon.socketPath().c_str());
    auto foreignDevice = second.getDevice(0);
    size_t requestCount = firstDaemon.requestCount();

    expectBadCall([&] { (void) first.getQdmaInfo(foreignDevice); });

    EXPECT_EQ(firstDaemon.requestCount(), requestCount);
    firstDaemon.rethrowServerError();
    secondDaemon.rethrowServerError();
}

/** Close drains an in-flight RPC and rejects all subsequent operations. */
TEST(LibvrtdppTest, CloseWaitsForInFlightRequest)
{
    FakeVrtd daemon(true);
    vrtd::Session session(daemon.socketPath().c_str());

    auto request = std::async(std::launch::async, [&] {
        return session.getNumDevices();
    });
    daemon.waitForNumDevices();
    std::promise<void> closeStartedPromise;
    auto closeStarted = closeStartedPromise.get_future();
    auto close = std::async(std::launch::async, [&] {
        closeStartedPromise.set_value();
        session.close();
    });

    if (closeStarted.wait_for(2s) != std::future_status::ready) {
        daemon.releaseBlockedNumDevices();
        FAIL() << "close task did not start";
    }
    EXPECT_EQ(close.wait_for(50ms), std::future_status::timeout);
    daemon.releaseBlockedNumDevices();

    ASSERT_EQ(request.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(request.get(), 1u);
    ASSERT_EQ(close.wait_for(2s), std::future_status::ready);
    close.get();
    expectBadCall([&] { (void) session.getNumDevices(); });
    daemon.rethrowServerError();
}

} // namespace
