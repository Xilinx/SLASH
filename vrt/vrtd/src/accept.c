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
 * @file accept.c
 * @brief New client connection handling for the vrtd daemon.
 *
 * When a client connects to vrtd's Unix domain socket (AF_UNIX / SOCK_SEQPACKET),
 * this module accepts the connection, extracts the peer's credentials (UID/GIDs),
 * and initialises a struct client that represents the connection for the lifetime
 * of the session.
 *
 * Credential extraction is security-critical: the UID comes from SO_PEERCRED
 * (kernel-verified, unforgeable), and the full set of supplementary GIDs is
 * obtained via getgrouplist() so that role-based access control in the config
 * layer can match against any group the connecting user belongs to.
 */

#define _GNU_SOURCE

#include "accept.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <grp.h>
#include <pwd.h>
#include <syslog.h>
#include <systemd/sd-event.h>
#include <systemd/sd-journal.h>

#include "utils.h"
#include "serve.h"
#include "state.h"

static int create_client_event(sd_event_source *listener_event_source, int cfd, struct vrtd *state, struct client **clientp);
static int populate_uid_gid(int cfd, struct client *client);

/**
 * sd_event I/O callback invoked when a new client connects to the listening socket.
 *
 * Because the listening socket is non-blocking and edge-triggered events are
 * possible, we loop calling accept4() until EAGAIN/EWOULDBLOCK to drain all
 * pending connections in a single callback invocation.  Each accepted
 * connection is wrapped in a struct client, registered with the event loop
 * for further I/O, and appended to state->clients.
 */
int on_event_new_connection(sd_event_source *s, int fd, uint32_t revents, void *userdata)
{
    struct vrtd *state = userdata;

    assert(state != NULL);

    if (!(revents & EPOLLIN)) {
        return 0;
    }

    /* Drain all pending connections from the accept queue. */
    for (;;) {
        struct sockaddr_un peer;
        socklen_t peerlen = sizeof(peer);
        int cfd = accept4(fd, (struct sockaddr*)&peer, &peerlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* all pending connections accepted */
            }
            LOG(LOG_ERR, "accept4() failed: %m");
            return -1;
        }

        _cleanup_(cleanup_clientp)
        struct client *client = NULL;
        int ret = create_client_event(s, cfd, state, &client);
        if (ret == -1) {
            close(cfd);
            continue;
        }

        assert(client != NULL);

        ret = client_ptr_array_push_move(&state->clients, &client);
        if (ret == -1) {
            LOG(LOG_ERR, "Failed to allocate memory when adding new client");
            continue;
        }
    }

    return 0;
}


/**
 * Allocate a struct client for an accepted connection and register it with
 * the sd_event loop for read/hangup events.
 *
 * On success, *clientp is set to the new client (caller takes ownership via
 * the cleanup attribute).  On failure, the event source is automatically
 * disabled and unreffed by the _cleanup_ attribute on @source, and the
 * client is freed by the _cleanup_ attribute on @client.
 *
 * Initialisation steps:
 *  1. Allocate and zero-initialise the client struct.
 *  2. Register the client fd with EPOLLIN|EPOLLRDHUP so we are notified
 *     of incoming messages and peer disconnections.
 *  3. Extract peer credentials (UID + supplementary GIDs) via
 *     populate_uid_gid() for later role-based access checks.
 *  4. Assign a monotonically increasing connection ID (conn_id) used as
 *     the client_id for buffer/allocator ownership tracking.
 */
static int create_client_event(sd_event_source *listener_event_source, int cfd, struct vrtd *state, struct client **clientp)
{
    *clientp = calloc(1, sizeof **clientp);
    PROPAGATE_ERROR_NULL_STDC_LOG(clientp, LOG_ERR, "Out of memory allocating client data");

    _cleanup_(cleanup_clientp)
    struct client *client = *clientp;

    _cleanup_(cleanup_free)
    char *description = NULL;

    // If something fails, we should disable + unref.
    _cleanup_(sd_event_source_disable_unrefp)
    sd_event_source *source = NULL;
    
    sd_event *ev = sd_event_source_get_event(listener_event_source);
    PROPAGATE_ERROR_NULL_LOG(ev, LOG_ERR, "Failed to get event for source");

    int ret = sd_event_add_io(ev, &source, cfd, EPOLLIN | EPOLLRDHUP, on_client_io, client);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to add client as event source");

    /* Build a human-readable description for the event source (used by
     * sd_event debugging/logging).  SO_PEERCRED is queried here purely
     * for the description string; the authoritative credential extraction
     * happens in populate_uid_gid() below. */
    {
        struct ucred cred;
        socklen_t clen = sizeof(cred);
        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0) {
            ret = asprintf(&description, "client fd=%d pid=%d uid=%d gid=%d",
                            cfd, (int)cred.pid, (int)cred.uid, (int)cred.gid);
        } else {
            ret = asprintf(&description, "client fd=%d", cfd);
        }
    }
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to allocate description for client");

    ret = populate_uid_gid(cfd, client);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Failed to obtain user/group information for lcient");

    ret = sd_event_source_set_description(source, description);
    PROPAGATE_ERROR_STDC_LOG(ret, LOG_ERR, "Could not set description for client fd");

    /* Assign a unique, non-zero connection ID.  IDs are monotonically
     * increasing; on overflow we wrap to 1 (0 is reserved as "no owner"
     * in the allocator's client_id tracking). */
    state->next_conn_id++;
    if (state->next_conn_id == 0) {
        state->next_conn_id = 1;
    }

    /* Finish initialising the client struct fields. */
    client->fd = cfd;
    client->in_fd = -1;               /* no ancillary fd received yet */
    client->conn_id = state->next_conn_id;
    LOG(LOG_DEBUG, "New client connection uid=%u conn_id=%llu fd=%d", (unsigned int)client->uid, (unsigned long long)client->conn_id, cfd);
    client->state = state;
    client->event_source = source;

    // Nothing went wrong. Do not unref.
    source = NULL;

    // Nothing went wrong. Do not remove client.
    client = NULL;

    return 0;
}

/**
 * Extract the connecting client's UID and full set of supplementary GIDs.
 *
 * The UID and primary GID are obtained from the kernel via SO_PEERCRED on the
 * Unix socket -- this is unforgeable by the peer.  We then resolve the
 * username through getpwuid_r() and call getgrouplist() to retrieve all
 * supplementary groups the user belongs to.  The complete GID list is stored
 * in client->gids so the config/role layer can grant permissions based on
 * any group membership, not just the primary GID.
 *
 * The function updates client->uid and client->gids atomically: if any step
 * fails after we begin writing, we roll back to the "unset" state
 * (uid == (uid_t)-1, empty gids) so partial credentials are never visible.
 */
static
int populate_uid_gid(int cfd, struct client *client)
{
    if (!client || cfd < 0) {
        LOG(LOG_ERR, "populate_uid_gid: invalid arguments");
        return -1;
    }

    /* Step 1: Obtain the peer's PID, UID and primary GID from the kernel
     * via SO_PEERCRED.  This is the only trustworthy source of identity
     * for a Unix domain socket peer. */
    struct ucred cred = {0};
    socklen_t len = sizeof(cred);
    int rc = getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &len);
    PROPAGATE_ERROR_STDC_LOG(rc, LOG_ERR, "SO_PEERCRED failed");

    uid_t new_uid = cred.uid;

    /* Step 2: Resolve the UID to a username so we can call getgrouplist().
     * _SC_GETPW_R_SIZE_MAX may return -1 on some systems; we clamp to a
     * sensible default of 16 KiB in that case. */
    long buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (buflen <= 0 || buflen > (1 << 20)) buflen = 16384;

    _cleanup_(cleanup_free) char *pwbuf = malloc((size_t)buflen);
    PROPAGATE_ERROR_NULL_STDC_LOG(pwbuf, LOG_ERR, "malloc pwbuf");

    struct passwd pwent, *pw = NULL;
    int pr = getpwuid_r(new_uid, &pwent, pwbuf, (size_t)buflen, &pw);
    if (pr != 0 || !pw) {
        LOG(LOG_ERR, "getpwuid_r(%u) failed: %s",
                         (unsigned)new_uid,
                         pr ? strerrordesc_np(pr) : "not found");
        return -1;
    }

    /* Step 3: Retrieve all supplementary GIDs.  We call getgrouplist()
     * twice: once with a NULL buffer to learn the required count (it will
     * return -1 and set ngroups), then again with a properly sized buffer.
     * This two-pass approach avoids hard-coding NGROUPS_MAX. */
    int ngroups = 0;
    (void)getgrouplist(pw->pw_name, cred.gid, NULL, &ngroups); /* expected to return -1 */
    if (ngroups <= 0) {
        LOG(LOG_ERR, "getgrouplist probe returned non-positive size for user %s", pw->pw_name);
        return -1;
    }

    _cleanup_(cleanup_free) gid_t *groups = malloc((size_t)ngroups * sizeof(gid_t));
    PROPAGATE_ERROR_NULL_STDC_LOG(groups, LOG_ERR, "malloc groups[%d]", ngroups);

    int gl = getgrouplist(pw->pw_name, cred.gid, groups, &ngroups);
    if (gl < 0 || ngroups <= 0) {
        LOG(LOG_ERR, "getgrouplist fetch failed for user %s", pw->pw_name);
        return -1;
    }

    /* Step 4: Commit the credentials to the client struct.  We defer all
     * mutations until this point so that earlier failures leave the client
     * in its prior state.  From here on, any failure triggers a manual
     * rollback to the "unset" state. */
    gid_t_array_free(&client->gids);
    client->uid = new_uid;

    for (int i = 0; i < ngroups; ++i) {
        int r = gid_t_array_push(&client->gids, groups[i]);
        if (r == -1) {
            LOG(LOG_ERR, "gid_t_array_push failed at index %d", i);
            // Roll back to consistent "unset" state
            gid_t_array_free(&client->gids);
            client->uid = (uid_t)-1;
            return -1;
        }
    }

    return 0;
}
