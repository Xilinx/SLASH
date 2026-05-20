/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

/**
 * @file serve.h
 * @brief Client connection state and I/O handling for the vrtd daemon.
 *
 * Each connected userspace application is represented by a @c struct @c client.
 * The daemon communicates with clients over AF_UNIX sockets using a simple
 * request/response protocol defined in vrtd/wire.h.  File descriptors (e.g.
 * BAR mmap regions, QDMA queue-pair fds) are passed out-of-band via
 * SCM_RIGHTS ancillary data.
 *
 * The client I/O state machine is driven by the systemd sd-event loop:
 *   1. A complete request is read into @c inb.
 *   2. The request is dispatched (auth-checked, then executed).
 *   3. The response is written from @c outb.
 *
 * Return codes from request handlers use the SERVE_CONTINUE / SERVE_DISCONNECT
 * convention to tell the I/O loop whether to keep or drop the connection.
 */

#ifndef VRTD_SERVE_H
#define VRTD_SERVE_H

#include <stdbool.h>
#include <stdint.h>

#include <vrtd/wire.h>
#include <systemd/sd-event.h>

#include "array.h"

struct device;

/**
 * @brief Per-client connection state for the vrtd daemon.
 *
 * One instance exists for every connected userspace application.  The struct
 * owns the socket fd, the message buffers, and holds the credentials and
 * role resolved at connection time.
 */
struct client {
    /** @brief Inbound message buffer (request from client).
     *  Sized to VRTD_MSG_MAX_SIZE bytes. */
    uint8_t inb[VRTD_MSG_MAX_SIZE];
    /** @brief Outbound message buffer (response to client).
     *  Sized to VRTD_MSG_MAX_SIZE bytes. */
    uint8_t outb[VRTD_MSG_MAX_SIZE];

    /** @brief Client AF_UNIX socket file descriptor. */
    int fd;

    /** @brief File descriptor received from the client via SCM_RIGHTS ancillary data. */
    int in_fd;
    /** @brief True when @c in_fd contains a valid received file descriptor. */
    bool have_in_fd;

    /** @brief File descriptor to send back to the client via SCM_RIGHTS ancillary data. */
    int out_fd;
    /** @brief True when @c out_fd contains a valid file descriptor to transmit. */
    bool have_out_fd;

    /** @brief True when a complete request has been read into @c inb and is awaiting dispatch. */
    bool have_request;
    /** @brief True when a response in @c outb is ready (or partially written) for the client. */
    bool have_response;
    /** @brief True when a new response has just been prepared and needs initial write. */
    bool have_new_response;
    /** @brief True while an asynchronous bitstream design write is in progress for this client. */
    bool pending_design_write;
    /** @brief The device on which the pending asynchronous design write is running (non-owning). */
    struct device *pending_design_write_device;

    /** @brief Bitmask of epoll events currently requested for this client's fd. */
    uint32_t wanted_epoll_events;

    /** @brief UID of the connected client process, obtained via SO_PEERCRED. */
    uid_t uid;
    /** @brief Unique monotonically increasing connection identifier assigned at accept time. */
    uint64_t conn_id;
    /** @brief Supplementary group IDs of the connected client process, obtained via SO_PEERCRED. */
    struct gid_t_array gids;

    /** @brief Back-pointer to the global daemon state (non-owning). */
    struct vrtd *state;
    /** @brief Role assigned to this client based on UID/GID credential lookup (non-owning). */
    struct role *role;

    /** @brief Systemd event source registration for this client's socket I/O. */
    sd_event_source *event_source;
};

/**
 * @brief Cast the inbound buffer of client @p C to a request header pointer.
 * @param C A @c struct @c client (by value or lvalue).
 */
#define CLIENT_IN_HEADER(C) ((struct vrtd_req_header *) (C).inb)
/**
 * @brief Cast the outbound buffer of client @p C to a response header pointer.
 * @param C A @c struct @c client (by value or lvalue).
 */
#define CLIENT_OUT_HEADER(C) ((struct vrtd_resp_header *) (C).outb)

/**
 * @brief Cast the inbound buffer of client @p C to a request body of type @p T.
 * @param C A @c struct @c client (by value or lvalue).
 * @param T The struct type name of the request body (without "struct" prefix in the macro argument).
 */
#define CLIENT_IN_BODY(C, T) ((struct T *) ((C).inb + sizeof(struct vrtd_req_header)))
/**
 * @brief Cast the outbound buffer of client @p C to a response body of type @p T.
 * @param C A @c struct @c client (by value or lvalue).
 * @param T The struct type name of the response body (without "struct" prefix in the macro argument).
 */
#define CLIENT_OUT_BODY(C, T) ((struct T *) ((C).outb + sizeof(struct vrtd_resp_header)))

/**
 * @brief Release all resources owned by a client (socket, event source, buffers).
 * @param client Pointer to the client to clean up. Fields are zeroed after release.
 */
void cleanup_client(struct client *client);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 *
 * Calls cleanup_client on the pointed-to client pointer, then NULLs it.
 *
 * @param clientp Address of a @c struct @c client pointer.
 */
static inline
void cleanup_clientp(struct client **clientp)
{
    if (clientp == NULL) {
        return;
    }

    cleanup_client(*clientp);

    *clientp = NULL;
}

DECLARE_OWNING_PTR_ARRAY(client_ptr_array, struct client *, cleanup_client)

/**
 * @brief Callback invoked by the systemd event loop when a client socket is ready for I/O.
 *
 * Drives the per-client state machine: reads requests, dispatches them through
 * the auth and handler layers, and writes responses.  File descriptors are
 * exchanged via SCM_RIGHTS ancillary messages.
 *
 * @param s      The event source that fired.
 * @param fd     The client socket file descriptor.
 * @param revents The epoll event mask (EPOLLIN, EPOLLOUT, ...).
 * @param user   Opaque pointer to the owning @c struct @c client.
 * @return 0 on success (SERVE_CONTINUE), or a negative errno on fatal error
 *         (SERVE_DISCONNECT causes the event source to be removed and the
 *         client to be freed).
 */
int on_client_io(sd_event_source *s, int fd, uint32_t revents, void *user);

/**
 * @brief Deferred-work timer callback for completing asynchronous operations.
 *
 * Invoked by the systemd event loop after a short delay to poll for completion
 * of asynchronous design writes and finalize the response to the client.
 *
 * @param s    The timer event source that fired.
 * @param usec The scheduled wakeup time in microseconds.
 * @param user Opaque pointer to the owning @c struct @c client.
 * @return 0 on success, or a negative errno on error.
 */
int on_event_deferred_work(sd_event_source *s, uint64_t usec, void *user);

#endif // VRTD_SERVE_H
