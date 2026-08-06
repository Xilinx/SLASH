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

#include <gtest/gtest.h>

#include "ancillary.h"
#include <vrtd/wire.h>
#include <vrtd/vrtd.h>

extern "C" {
#include "serve.h"
#include "state.h"
}

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace {

template <size_t Size>
struct ControlBuffer {
    alignas(cmsghdr) std::array<char, Size> bytes{};
};

class Fd {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { int fd = fd_; fd_ = -1; return fd; }
    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }
  private:
    int fd_;
};

size_t openFdCount()
{
    DIR* dir = opendir("/proc/self/fd");
    EXPECT_NE(dir, nullptr);
    if (!dir) return 0;
    size_t count = 0;
    while (dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0)
            count++;
    }
    closedir(dir);
    return count - 1;
}

std::array<Fd, 2> socketPair()
{
    int sockets[2];
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets), 0);
    return {Fd(sockets[0]), Fd(sockets[1])};
}

void sendFds(int socket, const int* fds, size_t count)
{
    char byte = 0;
    iovec iov = {.iov_base = &byte, .iov_len = sizeof(byte)};
    ControlBuffer<CMSG_SPACE(VRTD_ANCILLARY_RECV_FDS * sizeof(int))> control{};
    msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    if (count) {
        msg.msg_control = control.bytes.data();
        msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), fds, count * sizeof(int));
    }
    ASSERT_EQ(sendmsg(socket, &msg, 0), 1);
}

/*
 * Reads one request and returns the sequence number a reply has to carry to
 * answer it, which is what the daemon echoes.
 */
uint32_t takeRequestSeqno(int socket)
{
    std::array<char, VRTD_MSG_MAX_SIZE> request{};
    ssize_t received = recv(socket, request.data(), request.size(), 0);
    EXPECT_GE(received, static_cast<ssize_t>(sizeof(vrtd_req_header)));
    vrtd_req_header header{};
    if (received >= static_cast<ssize_t>(sizeof(header)))
        std::memcpy(&header, request.data(), sizeof(header));
    return header.seqno;
}

void sendResponse(int socket, uint32_t seqno, uint16_t ret,
                  uint16_t declaredSize,
                  const int* fds = nullptr, size_t fdCount = 0,
                  const void* body = nullptr, size_t bodySize = 0)
{
    vrtd_resp_header header = {
        .size = declaredSize,
        .ret = ret,
        .seqno = seqno,
    };
    iovec iov[] = {{.iov_base = &header, .iov_len = sizeof(header)},
                   {.iov_base = const_cast<void*>(body), .iov_len = bodySize}};
    ControlBuffer<CMSG_SPACE(VRTD_ANCILLARY_RECV_FDS * sizeof(int))> control{};
    msghdr msg = {.msg_iov = iov, .msg_iovlen = bodySize ? 2u : 1u};
    if (fdCount) {
        msg.msg_control = control.bytes.data();
        msg.msg_controllen = CMSG_SPACE(fdCount * sizeof(int));
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(fdCount * sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), fds, fdCount * sizeof(int));
    }
    ASSERT_EQ(sendmsg(socket, &msg, 0),
              static_cast<ssize_t>(sizeof(header) + bodySize));
}

std::thread fakeServer(int socket, uint16_t ret, uint16_t declaredSize,
                       int fd = -1)
{
    return std::thread([socket, ret, declaredSize, fd]() {
        uint32_t seqno = takeRequestSeqno(socket);
        if (fd >= 0)
            sendResponse(socket, seqno, ret, declaredSize, &fd, 1);
        else
            sendResponse(socket, seqno, ret, declaredSize);
    });
}

/*
 * Answers one request with a body. What the header declares and how much is
 * actually sent are separate arguments so a test can make them disagree.
 */
template <typename Body>
std::thread fakeServerWithBody(int socket, uint16_t ret, const Body& body,
                               size_t declaredSize = sizeof(Body),
                               size_t sentSize = sizeof(Body))
{
    return std::thread([socket, ret, body, declaredSize, sentSize]() {
        uint32_t seqno = takeRequestSeqno(socket);
        sendResponse(socket, seqno, ret, static_cast<uint16_t>(declaredSize),
                     nullptr, 0, &body, sentSize);
    });
}

struct ReceivedMessage {
    char byte = 0;
    iovec iov{.iov_base = &byte, .iov_len = 1};
    ControlBuffer<VRTD_ANCILLARY_BUFSIZE> control{};
    msghdr msg{.msg_iov = &iov, .msg_iovlen = 1,
               .msg_control = control.bytes.data(),
               .msg_controllen = control.bytes.size()};

    explicit ReceivedMessage(int socket) {
        EXPECT_EQ(recvmsg(socket, &msg, MSG_CMSG_CLOEXEC), 1);
    }
};

TEST(Ancillary, AcceptsNoDescriptors)
{
    msghdr msg{};
    EXPECT_EQ(vrtd_ancillary_extract(&msg, nullptr, 0), VRTD_ANCILLARY_OK);
}

TEST(Ancillary, AcceptsExactDescriptorCounts)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_GE(source.get(), 0);
    int raw = source.get();
    sendFds(sockets[0].get(), &raw, 1);

    ReceivedMessage receivedMessage(sockets[1].get());
    int received = -1;
    EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, &received, 1),
              VRTD_ANCILLARY_OK);
    EXPECT_GE(received, 0);
    EXPECT_NE(fcntl(received, F_GETFD), -1);
    EXPECT_NE(fcntl(received, F_GETFD) & FD_CLOEXEC, 0);
    close(received);
}

TEST(Ancillary, AcceptsTwoDescriptors)
{
    auto sockets = socketPair();
    Fd sourceA(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd sourceB(open("/dev/null", O_RDONLY | O_CLOEXEC));
    int raw[] = {sourceA.get(), sourceB.get()};
    sendFds(sockets[0].get(), raw, 2);

    ReceivedMessage receivedMessage(sockets[1].get());
    int received[2] = {-1, -1};
    EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, received, 2),
              VRTD_ANCILLARY_OK);
    EXPECT_NE(fcntl(received[0], F_GETFD), -1);
    EXPECT_NE(fcntl(received[1], F_GETFD), -1);
    close(received[0]);
    close(received[1]);
}

TEST(Ancillary, RejectsMissingDescriptor)
{
    msghdr msg{};
    int received = -1;
    EXPECT_EQ(vrtd_ancillary_extract(&msg, &received, 1),
              VRTD_ANCILLARY_BAD_COUNT);
    EXPECT_EQ(errno, EMSGSIZE);
    EXPECT_EQ(received, -1);
}

TEST(Ancillary, AcceptsDescriptorsAcrossMultipleRightsHeaders)
{
    ControlBuffer<CMSG_SPACE(sizeof(int)) * 2> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    Fd sourceA(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd sourceB(open("/dev/null", O_RDONLY | O_CLOEXEC));
    cmsghdr* first = reinterpret_cast<cmsghdr*>(control.bytes.data());
    first->cmsg_level = SOL_SOCKET;
    first->cmsg_type = SCM_RIGHTS;
    first->cmsg_len = CMSG_LEN(sizeof(int));
    cmsghdr* second = reinterpret_cast<cmsghdr*>(
        control.bytes.data() + CMSG_SPACE(sizeof(int)));
    second->cmsg_level = SOL_SOCKET;
    second->cmsg_type = SCM_RIGHTS;
    second->cmsg_len = CMSG_LEN(sizeof(int));
    int sourceBFd = sourceB.get();
    int sourceAFd = sourceA.get();
    std::memcpy(CMSG_DATA(first), &sourceAFd, sizeof(int));
    std::memcpy(CMSG_DATA(second), &sourceBFd, sizeof(int));

    int received[2] = {-1, -1};
    EXPECT_EQ(vrtd_ancillary_extract(&msg, received, 2),
              VRTD_ANCILLARY_OK);
    EXPECT_EQ(received[0], sourceA.get());
    EXPECT_EQ(received[1], sourceB.get());
    sourceA.release();
    sourceB.release();
    close(received[0]);
    close(received[1]);
}

TEST(Ancillary, RejectsAndClosesExtraDescriptors)
{
    auto sockets = socketPair();
    Fd sourceA(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd sourceB(open("/dev/null", O_RDONLY | O_CLOEXEC));
    int raw[] = {sourceA.get(), sourceB.get()};
    sendFds(sockets[0].get(), raw, 2);

    ReceivedMessage receivedMessage(sockets[1].get());
    int* received = reinterpret_cast<int*>(
        CMSG_DATA(CMSG_FIRSTHDR(&receivedMessage.msg)));
    int receivedA = received[0];
    int receivedB = received[1];
    int output = -1;
    EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, &output, 1),
              VRTD_ANCILLARY_BAD_COUNT);
    EXPECT_EQ(fcntl(receivedA, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(fcntl(receivedB, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(Ancillary, ClosesDescriptorsBeyondReceiveCapacity)
{
    /* A peer can attach more descriptors than the receive array holds.  They
     * are charged to this process on arrival, so the surplus has to be closed
     * rather than dropped. */
    constexpr size_t surplus = VRTD_ANCILLARY_RECV_FDS + 2;
    ControlBuffer<CMSG_SPACE(surplus * sizeof(int))> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    std::array<int, surplus> raw{};
    for (int& fd : raw) {
        fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(fd, 0);
    }
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(raw.size() * sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), raw.data(), raw.size() * sizeof(int));

    int received = -1;
    EXPECT_EQ(vrtd_ancillary_extract(&msg, &received, 1),
              VRTD_ANCILLARY_BAD_COUNT);
    EXPECT_EQ(received, -1);
    for (int fd : raw) {
        EXPECT_EQ(fcntl(fd, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
    }
}

TEST(Ancillary, RejectsDescriptorWhenNoneExpected)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    int raw = source.get();
    sendFds(sockets[0].get(), &raw, 1);

    ReceivedMessage receivedMessage(sockets[1].get());
    int received = reinterpret_cast<int*>(
        CMSG_DATA(CMSG_FIRSTHDR(&receivedMessage.msg)))[0];
    EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, nullptr, 0),
              VRTD_ANCILLARY_BAD_COUNT);
    EXPECT_EQ(fcntl(received, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(Ancillary, RejectsUnsupportedControlRecord)
{
    ControlBuffer<CMSG_SPACE(sizeof(int))> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    EXPECT_EQ(vrtd_ancillary_extract(&msg, nullptr, 0),
              VRTD_ANCILLARY_MALFORMED);
    EXPECT_EQ(errno, EBADMSG);
}

TEST(Ancillary, RejectsMalformedRightsLength)
{
    ControlBuffer<CMSG_SPACE(sizeof(int))> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) - 1);
    EXPECT_EQ(vrtd_ancillary_extract(&msg, nullptr, 0),
              VRTD_ANCILLARY_MALFORMED);
    EXPECT_EQ(errno, EBADMSG);
}

TEST(Ancillary, DoesNotInterpretMalformedRightsPayload)
{
    ControlBuffer<CMSG_SPACE(sizeof(int))> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_GE(source.get(), 0);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) + 1);
    int sourceFd = source.get();
    std::memcpy(CMSG_DATA(cmsg), &sourceFd, sizeof(sourceFd));

    EXPECT_EQ(vrtd_ancillary_extract(&msg, nullptr, 0),
              VRTD_ANCILLARY_MALFORMED);
    EXPECT_NE(fcntl(source.get(), F_GETFD), -1);
}

TEST(Ancillary, RejectsOversizedControlHeader)
{
    ControlBuffer<CMSG_SPACE(sizeof(int))> control{};
    msghdr msg = {.msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = control.bytes.size() + 1;
    EXPECT_EQ(vrtd_ancillary_extract(&msg, nullptr, 0),
              VRTD_ANCILLARY_MALFORMED);
    EXPECT_EQ(errno, EBADMSG);
}

TEST(Ancillary, RejectsTruncationAfterClosingVisibleDescriptors)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    int raw = source.get();
    sendFds(sockets[0].get(), &raw, 1);

    ReceivedMessage receivedMessage(sockets[1].get());
    int received = reinterpret_cast<int*>(
        CMSG_DATA(CMSG_FIRSTHDR(&receivedMessage.msg)))[0];
    receivedMessage.msg.msg_flags |= MSG_CTRUNC;
    int output = -1;
    EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, &output, 1),
              VRTD_ANCILLARY_MALFORMED);
    EXPECT_EQ(fcntl(received, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(Ancillary, RepeatedRejectionDoesNotLeakDescriptors)
{
    size_t before = openFdCount();
    for (int iteration = 0; iteration < 1000; iteration++) {
        auto sockets = socketPair();
        Fd sourceA(open("/dev/null", O_RDONLY | O_CLOEXEC));
        Fd sourceB(open("/dev/null", O_RDONLY | O_CLOEXEC));
        int raw[] = {sourceA.get(), sourceB.get()};
        sendFds(sockets[0].get(), raw, 2);
        ReceivedMessage receivedMessage(sockets[1].get());
        int output = -1;
        EXPECT_EQ(vrtd_ancillary_extract(&receivedMessage.msg, &output, 1),
              VRTD_ANCILLARY_BAD_COUNT);
    }
    EXPECT_EQ(openFdCount(), before);
}

TEST(Ancillary, RequestOpcodeDescriptorPolicy)
{
    size_t expectedSize = 0;

    EXPECT_EQ(vrtd_request_expected_fds(VRTD_REQ_DESIGN_WRITE), 1);
    EXPECT_EQ(vrtd_request_expected_fds(VRTD_REQ_CFGMEM_PROGRAM), 1);
    EXPECT_EQ(vrtd_request_expected_fds(VRTD_REQ_CFGMEM_PROGRAM_START), 1);
    EXPECT_EQ(vrtd_request_expected_fds(VRTD_REQ_GET_NUM_DEVICES), 0);
    EXPECT_EQ(vrtd_request_expected_fds(UINT16_MAX), 0);
    EXPECT_EQ(vrtd_response_expected_fds(VRTD_REQ_GET_BAR_FD, nullptr), 1);
    EXPECT_EQ(vrtd_response_expected_fds(VRTD_REQ_QDMA_QPAIR_GET_FD, nullptr), 1);
    EXPECT_EQ(vrtd_response_expected_fds(VRTD_REQ_BUFFER_OPEN, nullptr), 1);
    EXPECT_EQ(vrtd_response_expected_fds(VRTD_REQ_GET_NUM_DEVICES, nullptr), 0);
    EXPECT_TRUE(vrtd_response_expected_size(VRTD_REQ_GET_NUM_DEVICES,
                                            &expectedSize));
    EXPECT_EQ(expectedSize, sizeof(vrtd_resp_get_num_devices));
    EXPECT_TRUE(vrtd_response_expected_size(VRTD_REQ_GET_SENSOR_INFO,
                                            &expectedSize));
    EXPECT_EQ(expectedSize, SIZE_MAX);
    EXPECT_FALSE(vrtd_response_expected_size(UINT16_MAX, &expectedSize));
}

TEST(AncillaryIntegration, FatalInputRemovesClient)
{
    auto sockets = socketPair();
    vrtd state{};
    state.clients = client_ptr_array_init();
    client* ownedClient = static_cast<client*>(std::calloc(1, sizeof(client)));
    ASSERT_NE(ownedClient, nullptr);
    ownedClient->fd = sockets[1].release();
    ownedClient->in_fd = -1;
    ownedClient->uid = getuid();
    ownedClient->conn_id = 1;
    ownedClient->state = &state;
    int clientFd = ownedClient->fd;
    ASSERT_EQ(client_ptr_array_push_move(&state.clients, &ownedClient), 0);

    vrtd_req_header header = {
        .size = sizeof(vrtd_req_design_write),
        .opcode = VRTD_REQ_DESIGN_WRITE,
        .seqno = 1,
    };
    vrtd_req_design_write body{};
    iovec iov[] = {{.iov_base = &header, .iov_len = sizeof(header)},
                   {.iov_base = &body, .iov_len = sizeof(body)}};
    Fd extraA(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd extraB(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd extraC(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd extraD(open("/dev/null", O_RDONLY | O_CLOEXEC));
    Fd extraE(open("/dev/null", O_RDONLY | O_CLOEXEC));
    int raw[] = {extraA.get(), extraB.get(), extraC.get(), extraD.get(),
                 extraE.get()};
    ControlBuffer<CMSG_SPACE(sizeof(raw))> control{};
    msghdr msg = {.msg_iov = iov, .msg_iovlen = 2,
                  .msg_control = control.bytes.data(),
                  .msg_controllen = control.bytes.size()};
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(raw));
    std::memcpy(CMSG_DATA(cmsg), raw, sizeof(raw));
    ASSERT_EQ(sendmsg(sockets[0].get(), &msg, 0),
              static_cast<ssize_t>(sizeof(header) + sizeof(body)));

    EXPECT_EQ(on_client_io(nullptr, clientFd, EPOLLIN,
                           state.clients.d[0]), 0);
    EXPECT_EQ(state.clients.len, 0u);
    EXPECT_EQ(fcntl(clientFd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    client_ptr_array_free(&state.clients);
}

TEST(AncillaryIntegration, WrongCountReturnsBadRequestAndKeepsSession)
{
    auto sockets = socketPair();
    vrtd state{};
    state.clients = client_ptr_array_init();
    sd_event* event = nullptr;
    ASSERT_GE(sd_event_new(&event), 0);
    client* ownedClient = static_cast<client*>(std::calloc(1, sizeof(client)));
    ASSERT_NE(ownedClient, nullptr);
    role queryRole{};
    queryRole.query = true;
    ownedClient->fd = sockets[1].release();
    ownedClient->in_fd = -1;
    ownedClient->uid = getuid();
    ownedClient->conn_id = 1;
    ownedClient->state = &state;
    ownedClient->role = &queryRole;
    int clientFd = ownedClient->fd;
    ASSERT_GE(sd_event_add_io(event, &ownedClient->event_source, clientFd,
                              EPOLLIN, on_client_io, ownedClient), 0);
    ASSERT_EQ(client_ptr_array_push_move(&state.clients, &ownedClient), 0);

    vrtd_req_header header = {
        .size = sizeof(vrtd_req_design_write),
        .opcode = VRTD_REQ_DESIGN_WRITE,
        .seqno = 1,
    };
    vrtd_req_design_write body{};
    iovec iov[] = {{.iov_base = &header, .iov_len = sizeof(header)},
                   {.iov_base = &body, .iov_len = sizeof(body)}};
    msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    ASSERT_EQ(sendmsg(sockets[0].get(), &msg, 0),
              static_cast<ssize_t>(sizeof(header) + sizeof(body)));

    EXPECT_EQ(on_client_io(state.clients.d[0]->event_source, clientFd, EPOLLIN,
                           state.clients.d[0]), 0);
    EXPECT_EQ(state.clients.len, 1u);
    vrtd_resp_header response{};
    ASSERT_EQ(recv(sockets[0].get(), &response, sizeof(response), 0),
              static_cast<ssize_t>(sizeof(response)));
    EXPECT_EQ(response.ret, VRTD_RET_BAD_REQUEST);

    vrtd_req_get_num_devices secondBody{};
    header.size = sizeof(secondBody);
    header.opcode = VRTD_REQ_GET_NUM_DEVICES;
    header.seqno = 2;
    iovec secondIov[] = {{.iov_base = &header, .iov_len = sizeof(header)},
                         {.iov_base = &secondBody, .iov_len = sizeof(secondBody)}};
    msghdr secondMsg = {.msg_iov = secondIov, .msg_iovlen = 2};
    ASSERT_EQ(sendmsg(sockets[0].get(), &secondMsg, 0),
              static_cast<ssize_t>(sizeof(header) + sizeof(secondBody)));
    EXPECT_EQ(on_client_io(state.clients.d[0]->event_source, clientFd, EPOLLIN,
                           state.clients.d[0]), 0);
    std::array<char, sizeof(vrtd_resp_header) +
                         sizeof(vrtd_resp_get_num_devices)> secondResponse{};
    ASSERT_EQ(recv(sockets[0].get(), secondResponse.data(),
                   secondResponse.size(), 0),
              static_cast<ssize_t>(secondResponse.size()));
    auto* secondHeader =
        reinterpret_cast<vrtd_resp_header*>(secondResponse.data());
    EXPECT_EQ(secondHeader->ret, VRTD_RET_OK);
    EXPECT_EQ(secondHeader->seqno, 2u);

    client_ptr_array_free(&state.clients);
    sd_event_unref(event);
}

TEST(AncillaryIntegration, LibvrtdGetNumDevicesSatisfiesDaemonRequestSize)
{
    auto sockets = socketPair();
    vrtd state{};
    state.clients = client_ptr_array_init();
    sd_event* event = nullptr;
    ASSERT_GE(sd_event_new(&event), 0);
    client* ownedClient = static_cast<client*>(std::calloc(1, sizeof(client)));
    ASSERT_NE(ownedClient, nullptr);
    role queryRole{};
    queryRole.query = true;
    ownedClient->fd = sockets[1].release();
    ownedClient->in_fd = -1;
    ownedClient->uid = getuid();
    ownedClient->conn_id = 1;
    ownedClient->state = &state;
    ownedClient->role = &queryRole;
    int clientFd = ownedClient->fd;
    ASSERT_GE(sd_event_add_io(event, &ownedClient->event_source, clientFd,
                              EPOLLIN, on_client_io, ownedClient), 0);
    ASSERT_EQ(client_ptr_array_push_move(&state.clients, &ownedClient), 0);

    /* The daemon rejects a body shorter than vrtd_req_get_num_devices, so the
     * placeholder byte has to reach it for the query to be answered at all. */
    uint32_t devices = UINT32_MAX;
    enum vrtd_ret result = VRTD_RET_INTERNAL_ERROR;
    std::thread caller([&, socket = sockets[0].get()]() {
        result = vrtd_get_num_devices(socket, &devices);
    });

    pollfd waiting = {.fd = clientFd, .events = POLLIN, .revents = 0};
    ASSERT_GT(poll(&waiting, 1, 5000), 0);
    EXPECT_EQ(on_client_io(state.clients.d[0]->event_source, clientFd, EPOLLIN,
                           state.clients.d[0]), 0);
    caller.join();

    EXPECT_EQ(result, VRTD_RET_OK);
    EXPECT_EQ(devices, 0u);
    client_ptr_array_free(&state.clients);
    sd_event_unref(event);
}

TEST(AncillaryIntegration, LibvrtdRejectsMissingResponseDescriptor)
{
    auto sockets = socketPair();
    auto server = fakeServer(sockets[0].get(), VRTD_RET_OK,
                             sizeof(vrtd_resp_get_bar_fd));
    int received = -1;
    uint64_t length = 0;
    EXPECT_EQ(vrtd_get_bar_fd(sockets[1].get(), 0, 0, &received, &length),
              VRTD_RET_BAD_CONN);
    EXPECT_EQ(received, -1);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsErrorResponseDescriptor)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    size_t before = openFdCount();
    auto server = fakeServer(sockets[0].get(), VRTD_RET_BAD_REQUEST, 0,
                             source.get());
    uint32_t devices = 0;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_BAD_CONN);
    server.join();
    EXPECT_EQ(openFdCount(), before);
}

TEST(AncillaryIntegration, LibvrtdRejectsUnknownReturnValue)
{
    auto sockets = socketPair();
    auto server = fakeServer(sockets[0].get(), UINT16_MAX, 0);
    uint32_t devices = 0;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_BAD_CONN);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsSuccessfulUnknownOpcode)
{
    auto sockets = socketPair();
    std::thread server = fakeServerWithBody(
        sockets[0].get(), VRTD_RET_OK, uint32_t{0x12345678});
    uint32_t body = 0;
    EXPECT_EQ(vrtd_raw_request(sockets[1].get(), UINT16_MAX,
                               nullptr, 0, &body, sizeof(body),
                               nullptr, nullptr),
              VRTD_RET_BAD_CONN);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdPreservesUnknownOpcodeBadRequest)
{
    auto sockets = socketPair();
    auto server = fakeServer(sockets[0].get(), VRTD_RET_BAD_REQUEST, 0);
    EXPECT_EQ(vrtd_raw_request(sockets[1].get(), UINT16_MAX,
                               nullptr, 0, nullptr, 0,
                               nullptr, nullptr),
              VRTD_RET_BAD_REQUEST);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdClosesDescriptorOnBadFraming)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    auto server = fakeServer(sockets[0].get(), VRTD_RET_OK, 1,
                             source.get());
    int received = -1;
    uint64_t length = 0;
    EXPECT_EQ(vrtd_get_bar_fd(sockets[1].get(), 0, 0, &received, &length),
              VRTD_RET_BAD_CONN);
    EXPECT_EQ(received, -1);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsShortSuccessBody)
{
    auto sockets = socketPair();
    auto server = fakeServer(sockets[0].get(), VRTD_RET_OK, 0);
    uint32_t devices = UINT32_MAX;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_BAD_CONN);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsBodyShorterThanDeclaredSize)
{
    auto sockets = socketPair();
    vrtd_resp_get_num_devices shortBody{.num_devices = 7};
    std::thread server = fakeServerWithBody(
        sockets[0].get(), VRTD_RET_OK, shortBody, sizeof(shortBody),
        sizeof(shortBody) - 2);
    /* The header declares a full body but the datagram carries less, so
     * accepting it would leave the tail of the output holding stale bytes. */
    uint32_t devices = UINT32_MAX;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_BAD_CONN);
    EXPECT_EQ(devices, UINT32_MAX);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsEmptySuccessForUnknownOpcode)
{
    auto sockets = socketPair();
    auto server = fakeServer(sockets[0].get(), VRTD_RET_OK, 0);
    EXPECT_EQ(vrtd_raw_request(sockets[1].get(), UINT16_MAX,
                               nullptr, 0, nullptr, 0,
                               nullptr, nullptr),
              VRTD_RET_BAD_CONN);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsMismatchedSensorCount)
{
    auto sockets = socketPair();
    std::thread server([socket = sockets[0].get()]() {
        uint32_t seqno = takeRequestSeqno(socket);
        struct {
            uint32_t num_sensors;
            vrtd_sensor_entry entries[1];
        } __attribute__((packed)) body{};
        body.num_sensors = 2;
        sendResponse(socket, seqno, VRTD_RET_OK, sizeof(body), nullptr, 0,
                     &body, sizeof(body));
    });
    /* The count claims more entries than the body can hold, so trusting it
     * would walk the caller past what the daemon actually sent. */
    std::array<vrtd_sensor_entry, 4> entries{};
    uint32_t count = UINT32_MAX;
    EXPECT_EQ(vrtd_get_sensor_info(sockets[1].get(), 0, entries.data(),
                                   entries.size(), &count),
              VRTD_RET_BAD_CONN);
    EXPECT_EQ(count, UINT32_MAX);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsUnrelatedErrorBody)
{
    auto sockets = socketPair();
    std::thread server = fakeServerWithBody(
        sockets[0].get(), VRTD_RET_INTERNAL_ERROR,
        vrtd_resp_get_num_devices{.num_devices = 1});
    uint32_t devices = 0;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_BAD_CONN);
    server.join();
}

TEST(AncillaryIntegration, LibvrtdRejectsClientLocalReturnValues)
{
    for (uint16_t ret : {VRTD_RET_BAD_LIB_CALL, VRTD_RET_BAD_CONN,
                         VRTD_RET_RESPONSE_MISMATCH}) {
        auto sockets = socketPair();
        auto server = fakeServer(sockets[0].get(), ret, 0);
        uint32_t devices = 0;
        EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
                  VRTD_RET_BAD_CONN);
        server.join();
    }
}

/*
 * Pins the sentinel to the run of codes the server may send. A new server code
 * appended after the client-local ones and taken as the new end of the wire
 * range fails here instead of being accepted by the client as a reply status.
 */
/*
 * The per-opcode tables all have a default arm, so an opcode added without an
 * entry compiles. The client reads a reply for such an opcode as a bad
 * connection, so walk the whole set here rather than rely on the compiler.
 */
TEST(Ancillary, EveryOpcodeDeclaresAResponseSize)
{
    for (uint16_t opcode = 0; opcode < VRTD_OPCODE_COUNT; opcode++) {
        size_t size = 0;
        EXPECT_TRUE(vrtd_response_expected_size(opcode, &size))
            << "opcode " << opcode;
        EXPECT_GT(size, 0u) << "opcode " << opcode;
    }

    size_t size = 0;
    EXPECT_FALSE(vrtd_response_expected_size(VRTD_OPCODE_COUNT, &size));
}

TEST(AncillaryIntegration, WireReturnCodeRangeEndsBeforeTheClientLocalCodes)
{
    EXPECT_GE(VRTD_RET_WIRE_MAX, VRTD_RET_BAD_REQUEST);
    EXPECT_LT(VRTD_RET_WIRE_MAX, VRTD_RET_RESPONSE_MISMATCH);
}

/*
 * Every code in that run has to reach the caller unchanged. Written against the
 * sentinel rather than a named code, so a check that stops short of it fails.
 */
TEST(AncillaryIntegration, LibvrtdReportsEveryWireReturnCode)
{
    for (uint16_t ret = VRTD_RET_BAD_REQUEST; ret <= VRTD_RET_WIRE_MAX; ret++) {
        auto sockets = socketPair();
        auto server = fakeServer(sockets[0].get(), ret, 0);
        uint32_t devices = 0;
        EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices), ret);
        server.join();
    }
}

TEST(AncillaryIntegration, LibvrtdNumbersRequestsApart)
{
    auto sockets = socketPair();
    std::array<uint32_t, 3> seen{};
    std::thread server([socket = sockets[0].get(), &seen]() {
        for (uint32_t& seqno : seen) {
            seqno = takeRequestSeqno(socket);
            vrtd_resp_get_num_devices body{.num_devices = 7};
            sendResponse(socket, seqno, VRTD_RET_OK, sizeof(body), nullptr, 0,
                         &body, sizeof(body));
        }
    });

    uint32_t devices = 0;
    for (size_t i = 0; i < seen.size(); i++)
        EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
                  VRTD_RET_OK);
    server.join();

    /* Requests sharing a connection have to be told apart, or a reply left
     * queued by one would answer the next. */
    EXPECT_LT(seen[0], seen[1]);
    EXPECT_LT(seen[1], seen[2]);
}

TEST(AncillaryIntegration, LibvrtdRejectsReplyToAnEarlierRequest)
{
    auto sockets = socketPair();
    std::thread server([socket = sockets[0].get()]() {
        /* Answer the first request, then answer the second one with the first
         * one's sequence number. That is the reply an interrupted receive
         * leaves queued, arriving one request late. */
        uint32_t stale = takeRequestSeqno(socket);
        vrtd_resp_get_num_devices body{.num_devices = 7};
        sendResponse(socket, stale, VRTD_RET_OK, sizeof(body), nullptr, 0,
                     &body, sizeof(body));
        (void)takeRequestSeqno(socket);
        sendResponse(socket, stale, VRTD_RET_OK, sizeof(body), nullptr, 0,
                     &body, sizeof(body));
    });

    uint32_t devices = 0;
    ASSERT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices), VRTD_RET_OK);
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices),
              VRTD_RET_RESPONSE_MISMATCH);
    server.join();

    /* Replies are now a request behind, so the connection has to be out of
     * use rather than left handing out other requests' answers. Probed with a
     * bare send, which reports the shutdown without waiting for a reply that
     * would never come. */
    char probe = 0;
    errno = 0;
    EXPECT_EQ(send(sockets[1].get(), &probe, sizeof(probe), MSG_NOSIGNAL), -1);
    EXPECT_EQ(errno, EPIPE);
}

/*
 * Puts SIGUSR1 back however the test leaves, including on a failed assertion,
 * since a handler left installed would be inherited by whichever test runs
 * next.
 */
class InterruptingHandler {
public:
    InterruptingHandler() {
        struct sigaction action = {};
        action.sa_handler = [](int) { delivered = delivered + 1; };
        /* No SA_RESTART, so the call under test is cut short rather than
         * resumed by the kernel. */
        sigemptyset(&action.sa_mask);
        delivered = 0;
        installed = sigaction(SIGUSR1, &action, &previous) == 0;
    }

    ~InterruptingHandler() {
        if (installed) sigaction(SIGUSR1, &previous, nullptr);
    }

    bool ok() const { return installed; }
    static int count() { return delivered; }

private:
    static volatile sig_atomic_t delivered;
    struct sigaction previous = {};
    bool installed = false;
};

volatile sig_atomic_t InterruptingHandler::delivered;

TEST(AncillaryIntegration, LibvrtdResumesAnInterruptedReceive)
{
    InterruptingHandler handler;
    ASSERT_TRUE(handler.ok());

    auto sockets = socketPair();
    pthread_t caller = pthread_self();
    std::thread server([socket = sockets[0].get(), caller]() {
        uint32_t seqno = takeRequestSeqno(socket);
        /* The reply is withheld, so the client is certainly blocked receiving
         * while these are delivered. */
        for (int i = 0; i < 50; i++) {
            pthread_kill(caller, SIGUSR1);
            usleep(1000);
        }
        vrtd_resp_get_num_devices body{.num_devices = 7};
        sendResponse(socket, seqno, VRTD_RET_OK, sizeof(body), nullptr, 0,
                     &body, sizeof(body));
    });

    uint32_t devices = 0;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices), VRTD_RET_OK);
    EXPECT_EQ(devices, 7u);
    server.join();

    EXPECT_GT(InterruptingHandler::count(), 0);
}

TEST(AncillaryIntegration, LibvrtdResumesAnInterruptedSend)
{
    InterruptingHandler handler;
    ASSERT_TRUE(handler.ok());

    auto sockets = socketPair();
    int capacity = 1024;
    ASSERT_EQ(setsockopt(sockets[1].get(), SOL_SOCKET, SO_SNDBUF, &capacity,
                         sizeof(capacity)), 0);

    /* Leave no room at all, so the client's own send has to wait for some. */
    std::array<char, 256> filler{};
    size_t queued = 0;
    for (size_t chunk : {filler.size(), size_t{1}}) {
        while (send(sockets[1].get(), filler.data(), chunk,
                    MSG_DONTWAIT | MSG_NOSIGNAL) > 0)
            queued++;
    }
    ASSERT_GT(queued, 0u);
    /* A kernel that sized SO_SNDBUF differently could leave room here, and the
     * client's send would then complete without ever being interrupted. That
     * would pass while testing nothing, so check the buffer really is full. */
    errno = 0;
    ASSERT_EQ(send(sockets[1].get(), filler.data(), 1,
                   MSG_DONTWAIT | MSG_NOSIGNAL), -1);
    ASSERT_EQ(errno, EAGAIN);

    pthread_t caller = pthread_self();
    std::thread server([socket = sockets[0].get(), caller, queued]() {
        for (int i = 0; i < 50; i++) {
            pthread_kill(caller, SIGUSR1);
            usleep(1000);
        }
        /* Draining lets the waiting send through. */
        std::array<char, VRTD_MSG_MAX_SIZE> drain{};
        for (size_t i = 0; i < queued; i++)
            ASSERT_GT(recv(socket, drain.data(), drain.size(), 0), 0);
        /* A client that abandoned the send instead of repeating it never gets
         * here, so wait for the request rather than block on it for ever. */
        pollfd waiting = {.fd = socket, .events = POLLIN, .revents = 0};
        ASSERT_GT(poll(&waiting, 1, 5000), 0);
        uint32_t seqno = takeRequestSeqno(socket);
        vrtd_resp_get_num_devices body{.num_devices = 7};
        sendResponse(socket, seqno, VRTD_RET_OK, sizeof(body), nullptr, 0,
                     &body, sizeof(body));
    });

    uint32_t devices = 0;
    EXPECT_EQ(vrtd_get_num_devices(sockets[1].get(), &devices), VRTD_RET_OK);
    EXPECT_EQ(devices, 7u);
    server.join();

    EXPECT_GT(InterruptingHandler::count(), 0);
}

TEST(AncillaryIntegration, RawRequestAllowsUnusedDescriptorOutput)
{
    auto sockets = socketPair();
    std::thread server = fakeServerWithBody(
        sockets[0].get(), VRTD_RET_OK,
        vrtd_resp_get_num_devices{.num_devices = 3});
    vrtd_resp_get_num_devices body{};
    int responseFd = -1;
    EXPECT_EQ(vrtd_raw_request(sockets[1].get(), VRTD_REQ_GET_NUM_DEVICES,
                               nullptr, 0, &body, sizeof(body),
                               &responseFd, nullptr),
              VRTD_RET_OK);
    EXPECT_EQ(body.num_devices, 3u);
    EXPECT_EQ(responseFd, -1);
    server.join();
}

TEST(AncillaryIntegration, RawRequestRejectsNowhereToPutADescriptor)
{
    auto sockets = socketPair();
    Fd source(open("/dev/null", O_RDONLY | O_CLOEXEC));
    size_t before = openFdCount();
    std::thread server([socket = sockets[0].get(), fd = source.get()]() {
        uint32_t seqno = takeRequestSeqno(socket);
        vrtd_resp_get_bar_fd body{};
        sendResponse(socket, seqno, VRTD_RET_OK, sizeof(body), &fd, 1,
                     &body, sizeof(body));
    });
    /* An opcode that returns a descriptor, asked for without anywhere to put
     * it. Reporting success would leave the caller believing it had one. */
    vrtd_resp_get_bar_fd body{};
    EXPECT_EQ(vrtd_raw_request(sockets[1].get(), VRTD_REQ_GET_BAR_FD,
                               nullptr, 0, &body, sizeof(body),
                               nullptr, nullptr),
              VRTD_RET_BAD_LIB_CALL);
    server.join();
    EXPECT_EQ(openFdCount(), before);
}

/*
 * Only the queue pair descriptor is the daemon's own to close. The other three
 * are borrowed from a BAR file or a buffer that goes on using them, so closing
 * one would break every later transfer on it.
 */
TEST(Ancillary, OnlyTheQueuePairResponseOwnsItsDescriptor)
{
    bool owned = true;

    EXPECT_EQ(vrtd_response_expected_fds(VRTD_REQ_QDMA_QPAIR_GET_FD, &owned), 1);
    EXPECT_TRUE(owned);

    for (uint16_t opcode : {VRTD_REQ_GET_BAR_FD, VRTD_REQ_BUFFER_OPEN,
                            VRTD_REQ_BUFFER_OPEN_RAW}) {
        owned = true;
        EXPECT_EQ(vrtd_response_expected_fds(opcode, &owned), 1)
            << "opcode " << opcode;
        EXPECT_FALSE(owned) << "opcode " << opcode;
    }

    /* An opcode carrying nothing owns nothing, so the send path cannot be told
     * to close a descriptor it was never given. */
    for (uint16_t opcode = 0; opcode < VRTD_OPCODE_COUNT; opcode++) {
        owned = true;
        if (vrtd_response_expected_fds(opcode, &owned) == 0)
            EXPECT_FALSE(owned) << "opcode " << opcode;
    }
}

/*
 * The send path has to act on that classification rather than on a flag a
 * handler remembered to set, so drive it through the same table the daemon
 * uses. Reaching this through a real request needs a QDMA device.
 */
TEST(AncillaryIntegration, ClaimedOutputClosesOnlyTheDaemonsOwnDescriptor)
{
    struct Response {
        uint16_t opcode;
        bool owned;
    };

    for (Response expected : {Response{VRTD_REQ_QDMA_QPAIR_GET_FD, true},
                              Response{VRTD_REQ_GET_BAR_FD, false}}) {
        uint16_t opcode = expected.opcode;
        bool owned = !expected.owned;
        ASSERT_EQ(vrtd_response_expected_fds(opcode, &owned), 1);
        ASSERT_EQ(owned, expected.owned) << "opcode " << opcode;

        auto sockets = socketPair();
        vrtd state{};
        state.clients = client_ptr_array_init();
        client* sender = static_cast<client*>(std::calloc(1, sizeof(client)));
        ASSERT_NE(sender, nullptr);
        sender->fd = sockets[1].release();
        sender->in_fd = -1;
        sender->state = &state;
        sender->have_response = true;
        sender->have_out_fd = true;
        sender->wanted_epoll_events = EPOLLRDHUP | EPOLLIN;
        sender->out_fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(sender->out_fds[0], 0);
        client_claim_out_fds(sender, opcode);
        ASSERT_EQ(sender->out_fd_count, 1u);
        int senderFd = sender->out_fds[0];
        auto* response = reinterpret_cast<vrtd_resp_header*>(sender->outb);
        response->ret = VRTD_RET_OK;
        response->size = 0;
        int clientFd = sender->fd;
        ASSERT_EQ(client_ptr_array_push_move(&state.clients, &sender), 0);

        EXPECT_EQ(on_client_io(nullptr, clientFd, EPOLLOUT, state.clients.d[0]),
                  0);
        ReceivedMessage received(sockets[0].get());
        int receivedFd = reinterpret_cast<int*>(
            CMSG_DATA(CMSG_FIRSTHDR(&received.msg)))[0];
        EXPECT_NE(fcntl(receivedFd, F_GETFD), -1) << "opcode " << opcode;
        close(receivedFd);

        if (owned) {
            EXPECT_EQ(fcntl(senderFd, F_GETFD), -1) << "opcode " << opcode;
            EXPECT_EQ(errno, EBADF);
        } else {
            EXPECT_NE(fcntl(senderFd, F_GETFD), -1) << "opcode " << opcode;
            close(senderFd);
        }
        client_ptr_array_free(&state.clients);
    }
}

/*
 * The client state built here is what VRTD_REQ_QDMA_QPAIR_GET_FD leaves
 * behind: it opens a queue pair descriptor and marks it owned, so the daemon
 * closes its copy once the send hands it over. Built directly because reaching
 * it through the request needs a QDMA device.
 */
TEST(AncillaryIntegration, SuccessfulOutputClosesOwnedSenderDescriptor)
{
    auto sockets = socketPair();
    vrtd state{};
    state.clients = client_ptr_array_init();
    client* ownedClient = static_cast<client*>(std::calloc(1, sizeof(client)));
    ASSERT_NE(ownedClient, nullptr);
    ownedClient->fd = sockets[1].release();
    ownedClient->in_fd = -1;
    ownedClient->state = &state;
    ownedClient->have_response = true;
    ownedClient->have_out_fd = true;
    ownedClient->out_fd_count = 1;
    ownedClient->wanted_epoll_events = EPOLLRDHUP | EPOLLIN;
    ownedClient->out_fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(ownedClient->out_fds[0], 0);
    ownedClient->out_fds_owned[0] = true;
    int senderFd = ownedClient->out_fds[0];
    auto* response = reinterpret_cast<vrtd_resp_header*>(ownedClient->outb);
    response->ret = VRTD_RET_OK;
    response->size = 0;
    int clientFd = ownedClient->fd;
    ASSERT_EQ(client_ptr_array_push_move(&state.clients, &ownedClient), 0);

    EXPECT_EQ(on_client_io(nullptr, clientFd, EPOLLOUT, state.clients.d[0]), 0);
    EXPECT_EQ(state.clients.len, 1u);
    EXPECT_EQ(fcntl(senderFd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    ReceivedMessage received(sockets[0].get());
    int receivedFd = reinterpret_cast<int*>(
        CMSG_DATA(CMSG_FIRSTHDR(&received.msg)))[0];
    EXPECT_NE(fcntl(receivedFd, F_GETFD), -1);
    close(receivedFd);
    client_ptr_array_free(&state.clients);
}

TEST(AncillaryIntegration, FatalOutputClosesOwnedDescriptorAndClient)
{
    auto sockets = socketPair();
    vrtd state{};
    state.clients = client_ptr_array_init();
    client* ownedClient = static_cast<client*>(std::calloc(1, sizeof(client)));
    ASSERT_NE(ownedClient, nullptr);
    ownedClient->fd = sockets[1].release();
    ownedClient->in_fd = -1;
    ownedClient->state = &state;
    ownedClient->have_response = true;
    ownedClient->have_out_fd = true;
    ownedClient->out_fd_count = 1;
    ownedClient->out_fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(ownedClient->out_fds[0], 0);
    ownedClient->out_fds_owned[0] = true;
    int senderFd = ownedClient->out_fds[0];
    auto* response = reinterpret_cast<vrtd_resp_header*>(ownedClient->outb);
    response->ret = VRTD_RET_OK;
    response->size = 0;
    int clientFd = ownedClient->fd;
    ASSERT_EQ(client_ptr_array_push_move(&state.clients, &ownedClient), 0);
    sockets[0].reset();

    EXPECT_EQ(on_client_io(nullptr, clientFd, EPOLLOUT, state.clients.d[0]), 0);
    EXPECT_EQ(state.clients.len, 0u);
    EXPECT_EQ(fcntl(senderFd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
    client_ptr_array_free(&state.clients);
}

}  // namespace
