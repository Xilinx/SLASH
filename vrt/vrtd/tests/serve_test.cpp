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

#include <cstdlib>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <systemd/sd-event.h>
#include <unistd.h>

extern "C" {
#include "serve.h"
#include "state.h"
}

namespace {

int ignoreIo(sd_event_source*, int, uint32_t, void*)
{
    return 0;
}

int ignoreTimer(sd_event_source*, uint64_t, void*)
{
    return 0;
}

/**
 * Two clients on one deferred-work pass, where the first is disconnected
 * partway through the loop that walks them.
 *
 * A client whose design write has finished gets its epoll mask re-armed. That
 * call fails for a client with no event source, which disconnects it and
 * closes the gap in the client array. The client behind it slides into the
 * slot the loop is standing on, so the loop must not advance past it.
 */
class DeferredWorkFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_GE(sd_event_new(&event_), 0);
        ASSERT_GE(sd_event_add_time(event_, &timer_, CLOCK_MONOTONIC, 0, 0,
                                    ignoreTimer, nullptr), 0);
        state_.clients = client_ptr_array_init();
    }

    void TearDown() override
    {
        client_ptr_array_free(&state_.clients);
        sd_event_source_unref(timer_);
        sd_event_unref(event_);
    }

    /**
     * Appends a client waiting on a design write that has already finished.
     *
     * The write is reported against a null device, which
     * client_finalize_pending_design_write() completes immediately with an
     * error rather than polling a writer, so the test needs no hardware.
     *
     * @param withEventSource  Give the client a working event source. Without
     *                         one the epoll re-arm fails and it is dropped.
     */
    struct client* addFinishedWriter(bool withEventSource)
    {
        auto* c = static_cast<struct client*>(calloc(1, sizeof(struct client)));
        EXPECT_NE(c, nullptr);
        c->fd = -1;
        c->in_fd = -1;
        c->state = &state_;
        c->conn_id = ++state_.next_conn_id;
        c->have_request = true;
        c->pending_design_write = true;
        c->pending_design_write_device = nullptr;

        if (withEventSource) {
            c->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            EXPECT_GE(c->fd, 0);
            EXPECT_GE(sd_event_add_io(event_, &c->event_source, c->fd,
                                      EPOLLRDHUP, ignoreIo, c), 0);
        }

        struct client* raw = c;
        EXPECT_EQ(client_ptr_array_push_move(&state_.clients, &c), 0);
        return raw;
    }

    vrtd state_{};
    sd_event* event_{nullptr};
    sd_event_source* timer_{nullptr};
};

TEST_F(DeferredWorkFixture, DisconnectingOneClientStillVisitsTheNext)
{
    struct client* dropped = addFinishedWriter(false);
    struct client* survivor = addFinishedWriter(true);
    ASSERT_EQ(state_.clients.len, 2u);
    ASSERT_NE(dropped, survivor);

    ASSERT_EQ(on_event_deferred_work(timer_, 0, &state_), 0);

    ASSERT_EQ(state_.clients.len, 1u);
    EXPECT_EQ(state_.clients.d[0], survivor);
    /* The survivor moved into the slot the dropped client vacated, so it is
     * only reached if the loop held its index there. */
    EXPECT_FALSE(survivor->pending_design_write);
    EXPECT_TRUE(survivor->have_response);
}

TEST_F(DeferredWorkFixture, DisconnectingEveryClientDrainsTheArray)
{
    addFinishedWriter(false);
    addFinishedWriter(false);
    addFinishedWriter(false);
    ASSERT_EQ(state_.clients.len, 3u);

    ASSERT_EQ(on_event_deferred_work(timer_, 0, &state_), 0);

    EXPECT_EQ(state_.clients.len, 0u);
}

TEST_F(DeferredWorkFixture, FinishedWritersWithoutDisconnectAreAllFinalized)
{
    struct client* first = addFinishedWriter(true);
    struct client* second = addFinishedWriter(true);

    ASSERT_EQ(on_event_deferred_work(timer_, 0, &state_), 0);

    ASSERT_EQ(state_.clients.len, 2u);
    EXPECT_TRUE(first->have_response);
    EXPECT_TRUE(second->have_response);
}

} // namespace
