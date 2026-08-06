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

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include "accept.h"
#include "serve.h"
#include "state.h"
}

TEST(ConnectionIdentityTest, ExhaustionDoesNotReuseOwnerIdentity)
{
    vrtd state{};
    uint64_t connId = 0;

    state.next_conn_id = UINT64_MAX - 1;
    ASSERT_EQ(vrtd_allocate_conn_id(&state, &connId), 0);
    EXPECT_EQ(connId, UINT64_MAX);
    errno = 0;
    EXPECT_EQ(vrtd_allocate_conn_id(&state, &connId), -1);
    EXPECT_EQ(errno, EOVERFLOW);
    EXPECT_EQ(state.next_conn_id, UINT64_MAX);
}

TEST(ConnectionIdentityTest, ExhaustedAcceptDoesNotCloseStdin)
{
    int stdinBackup = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    int nullFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(nullFd, 0);
    ASSERT_EQ(dup2(nullFd, STDIN_FILENO), STDIN_FILENO);
    close(nullFd);

    int listenerFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK |
                            SOCK_CLOEXEC, 0);
    ASSERT_GE(listenerFd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
                  "vrtd-conn-id-%ld", static_cast<long>(getpid()));
    socklen_t addressLength = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 +
        std::strlen(address.sun_path + 1));
    ASSERT_EQ(bind(listenerFd, reinterpret_cast<sockaddr*>(&address),
                   addressLength), 0);
    ASSERT_EQ(listen(listenerFd, 1), 0);

    int peerFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ASSERT_GE(peerFd, 0);
    ASSERT_EQ(connect(peerFd, reinterpret_cast<sockaddr*>(&address),
                      addressLength), 0);

    vrtd state{};
    state.clients = client_ptr_array_init();
    state.next_conn_id = UINT64_MAX;
    sd_event* event = nullptr;
    sd_event_source* source = nullptr;
    ASSERT_GE(sd_event_new(&event), 0);
    ASSERT_GE(sd_event_add_io(event, &source, listenerFd, EPOLLIN,
                              on_event_new_connection, &state), 0);

    EXPECT_EQ(on_event_new_connection(source, listenerFd, EPOLLIN, &state), 0);
    EXPECT_NE(fcntl(STDIN_FILENO, F_GETFD), -1);
    EXPECT_EQ(state.clients.len, 0u);
    char byte;
    EXPECT_EQ(recv(peerFd, &byte, sizeof(byte), MSG_DONTWAIT), 0);

    sd_event_source_unref(source);
    sd_event_unref(event);
    client_ptr_array_free(&state.clients);
    close(peerFd);
    close(listenerFd);
    if (stdinBackup >= 0) {
        ASSERT_EQ(dup2(stdinBackup, STDIN_FILENO), STDIN_FILENO);
        close(stdinBackup);
    } else {
        close(STDIN_FILENO);
    }
}