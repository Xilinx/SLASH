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
 * @file serve.c
 * @brief Event-driven request dispatcher and client I/O engine for vrtd.
 *
 * This file implements the core request/response loop for the V80 Runtime
 * Daemon (vrtd).  vrtd multiplexes access to SLASH FPGA devices on behalf of
 * multiple unprivileged clients.  Clients connect over AF_UNIX SOCK_SEQPACKET
 * sockets and exchange record-oriented messages defined in wire.h.
 *
 * ## Architecture
 *
 * The daemon runs a single-threaded sd_event loop (systemd event loop).  Each
 * connected client is represented by a `struct client` whose socket fd is
 * registered as an sd_event I/O source.  The main callback, on_client_io(),
 * is invoked whenever a client socket becomes readable or writable, and drives
 * the per-client state machine described below.
 *
 * ## Client state machine
 *
 * Each client processes one request at a time.  Four boolean flags track
 * the progress of that single in-flight request:
 *
 *   have_request          -- A complete request message has been received into
 *                            client->inb and is ready for dispatch.
 *   have_response         -- A response message has been serialized into
 *                            client->outb and is ready to be sent.
 *   have_new_response     -- Set together with have_response so that we
 *                            attempt an immediate send in the same event
 *                            callback rather than waiting for the next
 *                            EPOLLOUT wakeup.
 *   pending_design_write  -- The DESIGN_WRITE request has been submitted
 *                            asynchronously and the client is blocked until
 *                            the transfer completes.  While this flag is set,
 *                            have_request remains true and no new EPOLLIN
 *                            events are armed, so the client cannot send
 *                            another request.  Completion is polled by
 *                            on_event_deferred_work() every 20 ms.
 *
 * Typical synchronous request lifecycle:
 *
 *   1. EPOLLIN fires  -> client_handle_in()   -> have_request = true
 *   2. (same callback) -> client_handle_request() dispatches the opcode
 *                        -> have_response = true, have_request = false
 *   3. EPOLLOUT fires -> client_handle_out()  -> have_response = false
 *      (or immediately via have_new_response in the same callback)
 *
 * Asynchronous DESIGN_WRITE lifecycle:
 *
 *   1. EPOLLIN fires  -> client_handle_in()   -> have_request = true
 *   2. client_handle_request() submits async write
 *      -> pending_design_write = true, response deferred
 *   3. on_event_deferred_work() polls completion every 20 ms
 *      -> on completion: have_response = true, have_request = false,
 *         pending_design_write = false
 *   4. EPOLLOUT fires -> client_handle_out()  -> have_response = false
 *
 * ## FD passing via SCM_RIGHTS
 *
 * Several operations pass file descriptors out-of-band using the Unix
 * SCM_RIGHTS ancillary-data mechanism:
 *
 *   Inbound (client -> daemon):
 *     - DESIGN_WRITE: The client sends an fd to the bitstream file that the
 *       daemon reads from asynchronously.  client_handle_in() extracts exactly
 *       one fd from the cmsg ancillary data and stores it in client->in_fd.
 *
 *   Outbound (daemon -> client):
 *     - GET_BAR_FD: Sends a BAR mmap-able fd.
 *     - QDMA_QPAIR_GET_FD: Sends a QDMA queue pair character-device fd.
 *     - BUFFER_OPEN: Sends the fd for the newly allocated buffer's qpair.
 *     client_handle_out() attaches client->out_fd as SCM_RIGHTS ancillary
 *     data on the sendmsg() call when client->have_out_fd is true.
 *
 * ## Authorization
 *
 * Every request handler calls an auth_request_*() function before doing any
 * work.  These functions (defined in auth.c) check the client's uid and
 * group memberships against the daemon's role-based access control policy.
 * A return of 0 means "denied" (-> VRTD_RET_AUTH_ERROR), -1 means "internal
 * error", and 1 means "permitted".
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-event.h>
#include <systemd/sd-journal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/syslog.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <stdio.h>
#include <limits.h>
#include <libgen.h>
#include <slash/ctldev.h>
#include <slash/hotplug.h>
#include <slash/qdma.h>

#include <ami.h>
#include <ami_device.h>
#include <ami_sensor.h>

#include "array.h"
#include "auth.h"
#include "clock.h"
#include "design_writer.h"
#include "flash.h"
#include "flash_worker.h"
#include "hotplug.h"
#include "reset.h"
#include "serve.h"
#include "utils.h"
#include "state.h"
#include "vrtd/wire.h"

/**
 * Polling interval (in microseconds) for the deferred-work timer.
 * on_event_deferred_work() fires every 20 ms to check whether any pending
 * asynchronous design writes have completed.
 */
#define VRTD_DEFERRED_WORK_INTERVAL_USEC (20ULL * 1000ULL)
#define VRTD_CFGMEM_MAX_PARTITION 15

/* ---- Forward declarations ------------------------------------------------ */

static int client_update_wanted_epoll_events(struct client *client, sd_event_source *s);
static int client_handle_in(struct client *client);
static int client_handle_out(struct client *client);
static int client_handle_request(struct client *client);
static int client_finalize_pending_design_write(struct client *client);
static bool any_design_write_active(struct vrtd *state);
static bool client_request_is_cfgmem_status(struct client *client);
static void client_deliver_async_device_op_result(struct client *client, uint16_t result);
static int process_async_device_op_completion(struct vrtd *state);
static int drain_deferred_requests(struct vrtd *state);
static uint16_t client_handle_request_get_device_info(
    struct client *client,
    const struct vrtd_req_get_device_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_device_info *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_get_device_by_bdf(
    struct client *client,
    const struct vrtd_req_get_device_by_bdf *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_device_by_bdf *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_get_num_devices(
    struct client *client,
    const struct vrtd_req_get_num_devices *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_num_devices *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_get_bar_info(
    struct client *client,
    const struct vrtd_req_get_bar_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_bar_info *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_get_bar_fd(
    struct client *client,
    const struct vrtd_req_get_bar_fd *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_bar_fd *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
);
static uint16_t client_handle_request_qdma_get_info(
    struct client *client,
    const struct vrtd_req_qdma_get_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_get_info *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_qdma_qpair_add(
    struct client *client,
    const struct vrtd_req_qdma_qpair_add *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_add *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_qdma_qpair_op(
    struct client *client,
    const struct vrtd_req_qdma_qpair_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_op *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_qdma_qpair_get_fd(
    struct client *client,
    const struct vrtd_req_qdma_qpair_get_fd *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_get_fd *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
);
static uint16_t client_handle_request_buffer_open(
    struct client *client,
    const struct vrtd_req_buffer_open *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_open *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
);
static uint16_t client_handle_request_buffer_open_raw(
    struct client *client,
    const struct vrtd_req_buffer_open_raw *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_open_raw *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
);
static uint16_t client_handle_request_buffer_close(
    struct client *client,
    const struct vrtd_req_buffer_close *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_close *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_design_write(
    struct client *client,
    const struct vrtd_req_design_write *req_body,
    uint16_t req_size,
    struct vrtd_resp_design_write *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_set_shell_state(
    struct client *client,
    const struct vrtd_req_set_shell_state *req_body,
    uint16_t req_size,
    struct vrtd_resp_set_shell_state *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_cfgmem_program(
    struct client *client,
    const struct vrtd_req_cfgmem_program *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_cfgmem_program_start(
    struct client *client,
    const struct vrtd_req_cfgmem_program_start *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program_start *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_cfgmem_program_status(
    struct client *client,
    const struct vrtd_req_cfgmem_program_status *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program_status *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_device_hotplug_op(
    struct client *client,
    const struct vrtd_req_device_hotplug_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_device_hotplug_op *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_clock_op(
    struct client *client,
    const struct vrtd_req_clock_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_clock_op *resp_body,
    uint16_t *resp_size
);
static uint16_t client_handle_request_get_sensor_info(
    struct client *client,
    const struct vrtd_req_get_sensor_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_sensor_info *resp_body,
    uint16_t *resp_size
);

static uint16_t device_refresh_pf2_after_design_write(struct device *d);
static void cleanup_buffers_for_conn_id(struct vrtd *state, uint64_t conn_id);
static void cleanup_client_buffers(struct client *client);
static void drain_deferred_buffer_cleanups(struct vrtd *state);

/* ---- Helper: opcode / hotplug-op to human-readable string --------------- */

/**
 * Returns the human-readable name of a vrtd wire opcode.
 * Used for diagnostic log messages.
 *
 * @param opcode  One of the VRTD_REQ_* constants from wire.h.
 * @return Static string; "UNKNOWN" for unrecognized values.
 */
static const char *vrtd_opcode_to_string(uint16_t opcode)
{
    switch (opcode) {
    case VRTD_REQ_GET_NUM_DEVICES:   return "GET_NUM_DEVICES";
    case VRTD_REQ_GET_DEVICE_INFO:   return "GET_DEVICE_INFO";
    case VRTD_REQ_GET_DEVICE_BY_BDF: return "GET_DEVICE_BY_BDF";
    case VRTD_REQ_GET_BAR_INFO:      return "GET_BAR_INFO";
    case VRTD_REQ_GET_BAR_FD:        return "GET_BAR_FD";
    case VRTD_REQ_QDMA_GET_INFO:     return "QDMA_GET_INFO";
    case VRTD_REQ_QDMA_QPAIR_ADD:   return "QDMA_QPAIR_ADD";
    case VRTD_REQ_QDMA_QPAIR_OP:    return "QDMA_QPAIR_OP";
    case VRTD_REQ_QDMA_QPAIR_GET_FD:return "QDMA_QPAIR_GET_FD";
    case VRTD_REQ_DESIGN_WRITE:      return "DESIGN_WRITE";
    case VRTD_REQ_CFGMEM_PROGRAM:    return "CFGMEM_PROGRAM";
    case VRTD_REQ_CFGMEM_PROGRAM_START: return "CFGMEM_PROGRAM_START";
    case VRTD_REQ_CFGMEM_PROGRAM_STATUS: return "CFGMEM_PROGRAM_STATUS";
    case VRTD_REQ_CLOCK_OP:          return "CLOCK_OP";
    case VRTD_REQ_BUFFER_OPEN:       return "BUFFER_OPEN";
    case VRTD_REQ_BUFFER_CLOSE:      return "BUFFER_CLOSE";
    case VRTD_REQ_DEVICE_HOTPLUG_OP: return "DEVICE_HOTPLUG_OP";
    case VRTD_REQ_GET_SENSOR_INFO:   return "GET_SENSOR_INFO";
    case VRTD_REQ_BUFFER_OPEN_RAW:   return "BUFFER_OPEN_RAW";
    case VRTD_REQ_SET_SHELL_STATE:   return "SET_SHELL_STATE";
    default:                         return "UNKNOWN";
    }
}

/**
 * Returns the human-readable name of a hotplug operation.
 *
 * @param op  One of the VRTD_DEVICE_HOTPLUG_OP_* constants from wire.h.
 * @return Static string; "unknown" for unrecognized values.
 */
static const char *vrtd_hotplug_op_to_string(uint32_t op)
{
    switch (op) {
    case VRTD_DEVICE_HOTPLUG_OP_RESCAN:         return "rescan";
    case VRTD_DEVICE_HOTPLUG_OP_REMOVE:         return "remove";
    case VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR:     return "toggle_sbr";
    case VRTD_DEVICE_HOTPLUG_OP_HOTPLUG:        return "hotplug";
    case VRTD_DEVICE_HOTPLUG_OP_RESET_SEQUENCE: return "reset_sequence";
    default:                                    return "unknown";
    }
}

static struct device *device_ptr_array_find_by_bdf(
    struct device_ptr_array *devices,
    const char *bdf
)
{
    for (size_t i = 0; i < devices->len; i++) {
        struct device *d = devices->d[i];
        if (d != NULL && strcmp(d->pci_info.bdf, bdf) == 0) {
            return d;
        }
    }

    return NULL;
}

/* ---- Post-design-write device refresh ----------------------------------- */

/**
 * Refreshes PCI function 2 (PF2) after a design write completes.
 *
 * After a new bitstream is loaded, the PF2 device may have changed identity
 * or capabilities.  This function removes the old PF2 from the PCI bus and
 * triggers a rescan so that the kernel re-enumerates it with updated
 * configuration.
 *
 * @param d  The device whose PF2 should be refreshed.
 * @return   VRTD_RET_OK on success, or an appropriate VRTD_RET_* error code.
 */
static uint16_t device_refresh_pf2_after_design_write(struct device *d)
{
    char pf2_bdf[VRTD_PCI_BDF_LEN] = {0};
    if (pci_bdf_set_function(d->pci_info.bdf, 2, pf2_bdf) != 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    struct slash_hotplug *hotplug = slash_hotplug_open(NULL);
    if (hotplug == NULL) {
        return hotplug_errno_to_vrtd_ret(errno);
    }

    /* Remove the old PF2 device; ENODEV is tolerated (already absent). */
    int ret = slash_hotplug_remove(hotplug, pf2_bdf);
    if (ret != 0 && errno != ENODEV) {
        int err = errno;
        (void) slash_hotplug_close(hotplug);
        return hotplug_errno_to_vrtd_ret(err);
    }

    /* Rescan the PCI bus so the kernel discovers the new PF2. */
    if (slash_hotplug_rescan(hotplug) != 0) {
        int err = errno;
        (void) slash_hotplug_close(hotplug);
        return hotplug_errno_to_vrtd_ret(err);
    }

    if (slash_hotplug_close(hotplug) != 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    /*
     * The BAR dma-buf fds in d->bar_files[] were opened against the pre-PDI
     * PF2.  After partial reconfiguration the AXI fabric behind PF2's BAR
     * has changed; clients that mmap the old fd will hit an unresponsive AXI
     * slave and trigger a fatal PCIe completion timeout.  Close the stale
     * fds and reopen them against the freshly-probed PF2 so that subsequent
     * GET_BAR_FD requests return a valid mapping.
     */
    for (size_t i = 0; i < SIZEOF_ARRAY(d->bar_files); i++) {
        if (d->bar_files[i] != NULL) {
            (void) slash_bar_file_close(d->bar_files[i]);
            d->bar_files[i] = NULL;
        }
        if (d->bar_info[i] != NULL) {
            slash_bar_info_free(d->bar_info[i]);
            d->bar_info[i] = NULL;
        }
    }

    /*
     * The stable character-device path survives PF2 remove+rescan, but the
     * existing handle still refers to the pre-PDI device. Close it and reopen
     * the same path so subsequent BAR operations use the freshly-probed PF2.
     */
    (void) slash_ctldev_close(d->ctl);
    d->ctl = NULL;

    /*
     * After a hotplug rescan the kernel creates the device node immediately
     * but udev sets ownership (vrtd:vrtd) asynchronously.  Opening the node
     * before udev acts yields EACCES.  Retry with a short backoff to let udev
     * catch up; any other error is fatal immediately.
     */
    #define CTL_OPEN_RETRIES    10
    #define CTL_OPEN_RETRY_US   500000  /* 500 ms per attempt, 5 s total */
    for (int attempt = 1; attempt <= CTL_OPEN_RETRIES; attempt++) {
        d->ctl = slash_ctldev_open(d->path);
        if (d->ctl != NULL)
            break;
        if (errno != EACCES) {
            LOG(LOG_ERR, "device_refresh_pf2: failed to reopen ctl device %s: %m", d->path);
            return VRTD_RET_INTERNAL_ERROR;
        }
        LOG(LOG_INFO, "device_refresh_pf2: waiting for udev to set permissions on %s "
            "(attempt %d/%d)", d->path, attempt, CTL_OPEN_RETRIES);
        usleep(CTL_OPEN_RETRY_US);
    }
    if (d->ctl == NULL) {
        LOG(LOG_ERR, "device_refresh_pf2: timed out waiting for permissions on %s: %m", d->path);
        return VRTD_RET_INTERNAL_ERROR;
    }
    #undef CTL_OPEN_RETRIES
    #undef CTL_OPEN_RETRY_US

    for (size_t i = 0; i < SIZEOF_ARRAY(d->bar_info); i++) {
        d->bar_info[i] = slash_bar_info_read(d->ctl, i);
        if (d->bar_info[i] != NULL && d->bar_info[i]->usable) {
            d->bar_files[i] = slash_bar_file_open(d->ctl, i, O_CLOEXEC);
            if (d->bar_files[i] == NULL) {
                LOG(LOG_ERR, "device_refresh_pf2: failed to reopen bar_file %zu on %s: %m",
                    i, d->path);
            }
        }
    }

    return VRTD_RET_OK;
}

/* ---- Client cleanup ----------------------------------------------------- */

/* Buffer cleanup can be deferred while an async device op owns the device list. */
static bool buffer_cleanup_is_deferred(struct vrtd *state, uint64_t conn_id)
{
    if (state == NULL || conn_id == 0) {
        return false;
    }

    for (size_t i = 0; i < state->deferred_buffer_cleanup_conn_ids.len; i++) {
        if (state->deferred_buffer_cleanup_conn_ids.d[i] == conn_id) {
            return true;
        }
    }

    return false;
}

static int defer_buffer_cleanup(struct vrtd *state, uint64_t conn_id)
{
    if (state == NULL || conn_id == 0) {
        return 0;
    }
    if (buffer_cleanup_is_deferred(state, conn_id)) {
        return 0;
    }

    return uint64_array_push(&state->deferred_buffer_cleanup_conn_ids, conn_id);
}

static void cleanup_buffers_for_conn_id(struct vrtd *state, uint64_t conn_id)
{
    if (state == NULL || conn_id == 0) {
        return;
    }

    for (size_t dev_idx = 0; dev_idx < state->devices.len; ++dev_idx) {
        struct device *d = state->devices.d[dev_idx];
        if (d == NULL) {
            continue;
        }

        size_t i = 0;
        while (i < d->buffers.len) {
            struct buffer *buf = d->buffers.d[i];
            if (buf == NULL || buf->client_id != conn_id) {
                i++;
                continue;
            }

            /*
             * buffer_ptr_array_rm_by_reference() frees the buffer and
             * shrinks the array, so we do not increment i -- the next
             * element slides into position i.
             */
            buffer_ptr_array_rm_by_reference(&d->buffers, buf);
        }
    }
}

static void cleanup_client_buffers(struct client *client)
{
    if (client == NULL || client->state == NULL || client->conn_id == 0) {
        return;
    }

    LOG(LOG_DEBUG, "Cleaning up buffers for disconnecting client uid=%u conn_id=%llu",
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    if (client->state->async_device_op_in_progress) {
        int ret = defer_buffer_cleanup(client->state, client->conn_id);
        if (ret != 0) {
            LOG(LOG_ERR, "Failed to defer buffer cleanup for conn_id=%llu while async device op is active",
                (unsigned long long)client->conn_id);
        }
        return;
    }

    cleanup_buffers_for_conn_id(client->state, client->conn_id);
}

static void drain_deferred_buffer_cleanups(struct vrtd *state)
{
    if (state == NULL) {
        return;
    }

    for (size_t i = 0; i < state->deferred_buffer_cleanup_conn_ids.len; i++) {
        uint64_t conn_id = state->deferred_buffer_cleanup_conn_ids.d[i];
        LOG(LOG_DEBUG, "Draining deferred buffer cleanup for conn_id=%llu",
            (unsigned long long)conn_id);
        cleanup_buffers_for_conn_id(state, conn_id);
    }

    state->deferred_buffer_cleanup_conn_ids.len = 0;
}

/**
 * Tears down a client: releases buffers, closes fds, unregisters the event
 * source, and frees memory.
 *
 * Called when the client disconnects (EPOLLHUP / EPOLLRDHUP / EPOLLERR) or
 * when the daemon shuts down.  Safe to call with a NULL pointer.
 *
 * @param client  The client to destroy.
 */
void cleanup_client(struct client *client)
{
    if (client == NULL) {
        return;
    }

    cleanup_client_buffers(client);

    gid_t_array_free(&client->gids);

    /* Close the inbound SCM_RIGHTS fd if one was received but not consumed. */
    if (client->in_fd >= 0) {
        (void) close(client->in_fd);
        client->in_fd = -1;
    }

    /* Close the client's SOCK_SEQPACKET connection fd. */
    if (client->fd >= 0) {
        (void) close(client->fd);
        client->fd = -1;
    }

    (void) sd_event_source_disable_unrefp(&client->event_source);

    free(client);
}

/* ---- sd_event I/O callback ---------------------------------------------- */

/**
 * Main sd_event I/O callback for a connected client.
 *
 * This function is registered with sd_event_add_io() for each client socket
 * and is invoked whenever the socket has pending I/O events.  It drives the
 * client state machine:
 *
 *   1. If the socket has error/hangup events, the client is disconnected
 *      and removed from the client list (which frees it via the owning
 *      array destructor).
 *   2. If EPOLLIN is set and we have no pending request, we receive the
 *      next message (client_handle_in).
 *   3. If a request is pending and no response has been prepared yet,
 *      we dispatch it (client_handle_request).
 *   4. If a response is ready and EPOLLOUT is set (or the response was
 *      just prepared in step 3, flagged by have_new_response), we send
 *      the response (client_handle_out).
 *   5. Update the epoll event mask so that we only wake up for events
 *      relevant to the current state.
 *
 * @param s       The sd_event_source for this client's fd.
 * @param fd      The client's socket fd.
 * @param revents The epoll event bitmask that triggered this callback.
 * @param user    Pointer to the `struct client`.
 * @return 0 on success, negative errno on fatal error.
 */
int on_client_io(sd_event_source *s, int fd, uint32_t revents, void *user)
{
    struct client *client = user;
    (void) s;

    assert(client->fd == fd);

    int ret;

    /* Disconnect on error / hangup / remote close. */
    if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        LOG(LOG_DEBUG, "Client disconnected uid=%u conn_id=%llu fd=%d",
            (unsigned int)client->uid, (unsigned long long)client->conn_id, client->fd);
        client_ptr_array_rm_by_reference(&client->state->clients, client);
        return 0;
    }

    /* Step 1: Receive a new request if the slot is free. */
    if (!client->have_request && (revents & EPOLLIN)) {
        ret = client_handle_in(client);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to handle client input");
    }

    /*
     * Step 2: Dispatch the request synchronously (unless an async job is active).
     *
     * While a cfgmem program or standalone reset is running on the flash worker,
     * it mutates the shared device list, so we must not run any other handler
     * that touches device state.  Leave the request queued (have_request stays
     * set, EPOLLIN disarmed); on_event_deferred_work() drains it once the
     * operation completes.
     */
    if (client->have_request && !client->have_response &&
        (!client->state->async_device_op_in_progress ||
         client_request_is_cfgmem_status(client))) {
        ret = client_handle_request(client);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to handle client request");
    }

    /*
     * Step 3: Send the response.
     * have_new_response allows an immediate send attempt even if EPOLLOUT
     * was not in revents -- this avoids a round-trip through the event loop
     * when the socket is writable and we just prepared the response above.
     */
    if ((client->have_response && (revents & EPOLLOUT)) ||
         client->have_new_response) {
        client->have_new_response = false;

        ret = client_handle_out(client);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to handle client output");
    }

    /* Step 4: Adjust EPOLLIN/EPOLLOUT based on current state. */
    ret = client_update_wanted_epoll_events(client, s);
    PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to update epoll events");

    return 0;
}

/* ---- Deferred work timer ------------------------------------------------ */

/**
 * Timer callback that polls for completion of asynchronous design writes.
 *
 * Registered as a monotonic sd_event timer source.  Fires every
 * VRTD_DEFERRED_WORK_INTERVAL_USEC (20 ms).  For each client that has
 * pending_design_write == true, it calls client_finalize_pending_design_write()
 * to check whether the async write has finished.  If it has, that function
 * prepares the response and transitions the client state machine so the
 * response can be sent on the next I/O event.
 *
 * @param s     The sd_event_source for this timer.
 * @param usec  The monotonic timestamp at which this callback was scheduled.
 * @param user  Pointer to the global `struct vrtd` daemon state.
 * @return 0 on success, negative errno on fatal error.
 */
int on_event_deferred_work(sd_event_source *s, uint64_t usec, void *user)
{
    struct vrtd *state = user;

    if (state == NULL) {
        return -1;
    }

    /* Re-arm the timer for the next interval. */
    uint64_t next_usec = usec + VRTD_DEFERRED_WORK_INTERVAL_USEC;
    int ret = sd_event_source_set_time(s, next_usec);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set deferred work timer");

    ret = sd_event_source_set_enabled(s, SD_EVENT_ON);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to re-enable deferred work timer");

    /*
     * Async device operations hold the device list exclusively.  While one runs,
     * poll only for its completion and keep every other client request deferred.
     * When it finishes, drain the requests that queued up in the meantime.
     * (Design-write finalization is skipped here because a design write can
     * never be in flight alongside an async device operation -- see the busy
     * checks in the submit helpers.)
     */
    if (state->async_device_op_in_progress) {
        ret = process_async_device_op_completion(state);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to process async device op completion");

        if (state->async_device_op_in_progress) {
            return 0;
        }

        ret = drain_deferred_requests(state);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to drain deferred requests after async device op");

        return 0;
    }

    /* Check each client for a completed async design write. */
    for (size_t i = 0; i < state->clients.len; i++) {
        struct client *client = state->clients.d[i];
        if (client == NULL) {
            continue;
        }

        ret = client_finalize_pending_design_write(client);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to finalize deferred design write");

        /*
         * client_finalize_pending_design_write() returns 1 when the write
         * finished and the response was prepared.  In that case, re-arm the
         * client's epoll events so that EPOLLOUT triggers a send.
         */
        if (ret == 1) {
            ret = client_update_wanted_epoll_events(client, client->event_source);
            PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to update epoll events for deferred response");
        }
    }

    return 0;
}

/* ---- Epoll event management --------------------------------------------- */

/**
 * Recalculates and applies the set of epoll events we want for a client.
 *
 * The desired event set depends on the client's state machine position:
 *   - EPOLLRDHUP is always armed so we detect remote close.
 *   - EPOLLIN is armed only when we have no pending request (ready to
 *     receive the next one).
 *   - EPOLLOUT is armed only when we have a response to send.
 *
 * To avoid unnecessary sd_event_source_set_io_events() syscalls, the
 * previously applied mask is cached in client->wanted_epoll_events and
 * the call is skipped when nothing changed.
 *
 * @param client  The client whose epoll mask should be updated.
 * @param s       The client's sd_event I/O source.
 * @return 0 on success, negative errno on failure.
 */
static int client_update_wanted_epoll_events(struct client *client, sd_event_source *s)
{
    uint32_t events =
        EPOLLRDHUP |
        (!client->have_request ? EPOLLIN : 0) |
        (client->have_response ? EPOLLOUT : 0)
    ;

    if (events == client->wanted_epoll_events) {
        return 0;
    }
    client->wanted_epoll_events = events;

    int ret = sd_event_source_set_io_events(s, events);
    PROPAGATE_ERROR_SD_LOG(ret, LOG_ERR, "Failed to set io source io events");

    return 0;
}

/* ---- Inbound message reception (SCM_RIGHTS extraction) ------------------ */

/**
 * Receives the next request message from a client's socket.
 *
 * Uses recvmsg() to read both the message payload (into client->inb) and any
 * ancillary data carrying SCM_RIGHTS file descriptors (into client->in_fd).
 *
 * ## SCM_RIGHTS handling
 *
 * The cmsg ancillary-data buffer is sized for exactly one fd.  The loop over
 * CMSG_FIRSTHDR / CMSG_NXTHDR extracts fds as follows:
 *   - If the ancillary data is malformed (fractional fd size), all received
 *     fds are closed and the function returns -1.
 *   - If more than one fd was sent, or if a second SCM_RIGHTS header appears,
 *     all fds are closed and the function returns -1.  The daemon expects at
 *     most one inbound fd per message (currently only DESIGN_WRITE uses it).
 *   - On success the single fd is stored in client->in_fd and
 *     client->have_in_fd is set to true.
 *
 * ## Message validation
 *
 * After a successful recvmsg(), the function validates the framing:
 *   - The received byte count must be at least sizeof(vrtd_req_header).
 *   - header->size + sizeof(header) must equal the byte count.
 *   - header->size must not overflow the buffer.
 *
 * On success, client->have_request is set to true and the caller (on_client_io)
 * proceeds to dispatch.
 *
 * @param client  The client to receive from.
 * @return 0 on success (including EAGAIN -- no data ready yet),
 *        -1 on protocol error or I/O error.
 */
static int client_handle_in(struct client *client)
{
    assert(!client->have_request);

    /* Close any leftover inbound fd from a previous request cycle. */
    if (client->in_fd >= 0) {
        (void) close(client->in_fd);
        client->in_fd = -1;
        client->have_in_fd = false;
    }

    /* Set up the iovec to receive the message payload. */
    struct iovec iovec[1] = {
        { .iov_base = client->inb, .iov_len = VRTD_MSG_MAX_SIZE },
    };

    /*
     * Allocate a cmsg buffer large enough for one fd.
     * CMSG_SPACE includes alignment padding required by the kernel.
     */
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    struct msghdr msg = {
        .msg_name       = NULL,
        .msg_namelen    = 0,
        .msg_iov        = iovec,
        .msg_iovlen     = SIZEOF_ARRAY(iovec),
        .msg_control    = cbuf,
        .msg_controllen = sizeof(cbuf),
        .msg_flags      = 0,
    };

    ssize_t n;
retry:
    n = recvmsg(client->fd, &msg, MSG_DONTWAIT);
    if (n == -1) {
        switch (errno) {
        case EINTR:
            goto retry;
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            return 0;
        default:
            return -1;
        }
    }

    /* Reject truncated messages -- this should not happen with SEQPACKET. */
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        // TODO: handle error from client
        return -1;
    }

    /*
     * Walk the cmsg chain to extract any SCM_RIGHTS file descriptors.
     * We expect at most one fd; anything else is a protocol violation.
     */
    client->in_fd = -1;
    client->have_in_fd = false;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }

        size_t data_len = cmsg->cmsg_len - CMSG_LEN(0);
        size_t count = data_len / sizeof(int);
        int *fds = (int *) CMSG_DATA(cmsg);

        /* Reject malformed ancillary data (fractional fd). */
        if (data_len < sizeof(int) || (data_len % sizeof(int)) != 0) {
            for (size_t i = 0; i < count; ++i) {
                (void) close(fds[i]);
            }
            return -1;
        }

        /* Reject multiple fds or multiple SCM_RIGHTS headers. */
        if (count != 1 || client->have_in_fd) {
            for (size_t i = 0; i < count; ++i) {
                (void) close(fds[i]);
            }
            return -1;
        }

        client->in_fd = fds[0];
        client->have_in_fd = true;
    }

    /* Validate request framing. */
    struct vrtd_req_header *header = (struct vrtd_req_header *) client->inb;
    if (n < sizeof(struct vrtd_req_header) || header->size + sizeof(struct vrtd_req_header) != n || header->size > VRTD_MSG_MAX_SIZE - sizeof *header) {
        // TODO: handle error from client
        return -1;
    }

    client->have_request = true;

    return 0;
}

/* ---- Outbound message transmission (SCM_RIGHTS attachment) -------------- */

/**
 * Sends the prepared response message to a client.
 *
 * Uses sendmsg() to transmit the response from client->outb.  If a file
 * descriptor needs to be passed to the client (client->have_out_fd is true),
 * the fd stored in client->out_fd is attached as SCM_RIGHTS ancillary data.
 *
 * ## SCM_RIGHTS construction
 *
 * When have_out_fd is set:
 *   1. A cmsg control buffer (cbuf) is zeroed and attached to the msghdr.
 *   2. A single cmsghdr is constructed with level=SOL_SOCKET, type=SCM_RIGHTS,
 *      and len=CMSG_LEN(sizeof(int)).
 *   3. The fd is copied into the cmsg data area via memcpy.
 *   4. sendmsg() delivers both the response payload and the fd atomically.
 *
 * After a successful send, have_response and have_out_fd are cleared,
 * allowing the client to send a new request.
 *
 * @param client  The client to send to.
 * @return 0 on success (including EAGAIN), -1 on error.
 */
static int client_handle_out(struct client *client)
{
    assert(client->have_response);

    size_t size = sizeof(struct vrtd_resp_header) + ((struct vrtd_resp_header *) client->outb)->size;

    struct iovec iovec[1] = {
        { .iov_base = client->outb, .iov_len = size },
    };

    struct msghdr msg = {
        .msg_name       = NULL,
        .msg_namelen    = 0,
        .msg_iov        = iovec,
        .msg_iovlen     = SIZEOF_ARRAY(iovec),
        .msg_control    = NULL,
        .msg_controllen = 0,
        .msg_flags      = 0,
    };

    char cbuf[CMSG_SPACE(sizeof(int))];

    /*
     * If we have an outbound fd, construct SCM_RIGHTS ancillary data.
     * The cbuf is zeroed to satisfy kernel expectations about padding.
     */
    if (client->have_out_fd) {
        uint32_t fd_count = client->out_fd_count ? client->out_fd_count : 1;

        if (fd_count > 2) {
            LOG(LOG_ERR, "Invalid outbound fd count %u", (unsigned int)fd_count);
            return -1;
        }

        memset(cbuf, 0, sizeof cbuf);

        msg.msg_control = cbuf;
        msg.msg_controllen = CMSG_SPACE(fd_count * sizeof(int));

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(fd_count * sizeof(int));

        memcpy(CMSG_DATA(cmsg), client->out_fds, fd_count * sizeof(int));
    }

    ssize_t n;
retry:
    n = sendmsg(client->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == -1) {
        switch (errno) {
        case EINTR:
            goto retry;
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            return 0;
        default:
            return -1;
        }
    }

    /*
     * SOCK_SEQPACKET guarantees atomic delivery; a short write means
     * something went wrong.
     */
    if (n != size) {
        LOG(LOG_ERR, "Message truncated");
        return -1;
    }

    /* Response sent -- clear state so the client can send a new request. */
    client->have_response = false;
    client->have_out_fd = false;
    client->out_fd_count = 0;

    return 0;
}

/* ---- Request dispatch (opcode switch) ----------------------------------- */

/**
 * Dispatches a received request to the appropriate handler.
 *
 * Reads the opcode from the request header, logs the request, and switches
 * on the opcode to invoke the correct handler function.  Each handler follows
 * a uniform signature:
 *
 *   uint16_t handler(client, req_body, req_size, resp_body, resp_size
 *                    [, out_fd, have_out_fd])
 *
 * The handler returns a VRTD_RET_* status code which is stored in the
 * response header.  Handlers that pass an fd to the client (GET_BAR_FD,
 * QDMA_QPAIR_GET_FD, BUFFER_OPEN) also write to client->out_fd and
 * client->have_out_fd.
 *
 * Special case -- DESIGN_WRITE:
 *   The handler submits the write asynchronously and sets
 *   client->pending_design_write.  In that case this function returns early
 *   *without* marking have_response or clearing have_request.  The response
 *   is deferred until client_finalize_pending_design_write() detects
 *   completion.
 *
 * For all other (synchronous) opcodes, after the handler returns:
 *   - have_request is cleared
 *   - have_response and have_new_response are set
 *   - Any unconsumed inbound fd is closed
 *
 * @param client  The client whose request should be dispatched.
 * @return 0 on success, negative on fatal error.
 */
static int client_handle_request(struct client *client)
{
    assert(client->have_request);
    assert(!client->have_response);

    struct vrtd_req_header *req_header = CLIENT_IN_HEADER(*client);
    struct vrtd_resp_header *resp_header = CLIENT_OUT_HEADER(*client);

    /* Echo the client's sequence number back in the response. */
    resp_header->seqno = req_header->seqno;

    LOG(LOG_DEBUG, "Request opcode=%u(%s) uid=%u conn_id=%llu",
        (unsigned int)req_header->opcode, vrtd_opcode_to_string(req_header->opcode),
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    // Separate variable for allignment reasons
    uint16_t size = 0;

    switch (req_header->opcode) {
    case VRTD_REQ_GET_NUM_DEVICES:
        resp_header->ret =
            client_handle_request_get_num_devices(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_num_devices),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_num_devices),
                &size
            );
        break;
    case VRTD_REQ_GET_DEVICE_INFO:
        resp_header->ret =
            client_handle_request_get_device_info(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_device_info),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_device_info),
                &size
            );
        break;
    case VRTD_REQ_GET_DEVICE_BY_BDF:
        resp_header->ret =
            client_handle_request_get_device_by_bdf(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_device_by_bdf),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_device_by_bdf),
                &size
            );
        break;
    case VRTD_REQ_GET_BAR_INFO:
        resp_header->ret =
            client_handle_request_get_bar_info(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_bar_info),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_bar_info),
                &size
            );
        break;
    case VRTD_REQ_GET_BAR_FD:
        resp_header->ret =
            client_handle_request_get_bar_fd(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_bar_fd),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_bar_fd),
                &size,
                &client->out_fds[0],
                &client->have_out_fd
            );
        break;
    case VRTD_REQ_QDMA_GET_INFO:
        resp_header->ret =
            client_handle_request_qdma_get_info(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_qdma_get_info),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_qdma_get_info),
                &size
            );
        break;
    case VRTD_REQ_QDMA_QPAIR_ADD:
        resp_header->ret =
            client_handle_request_qdma_qpair_add(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_qdma_qpair_add),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_qdma_qpair_add),
                &size
            );
        break;
    case VRTD_REQ_QDMA_QPAIR_OP:
        resp_header->ret =
            client_handle_request_qdma_qpair_op(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_qdma_qpair_op),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_qdma_qpair_op),
                &size
            );
        break;
    case VRTD_REQ_QDMA_QPAIR_GET_FD:
        resp_header->ret =
            client_handle_request_qdma_qpair_get_fd(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_qdma_qpair_get_fd),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_qdma_qpair_get_fd),
                &size,
                &client->out_fds[0],
                &client->have_out_fd
            );
        break;
    case VRTD_REQ_BUFFER_OPEN:
        resp_header->ret =
            client_handle_request_buffer_open(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_buffer_open),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_buffer_open),
                &size,
                &client->out_fds[0],
                &client->have_out_fd
            );
        break;
    case VRTD_REQ_BUFFER_OPEN_RAW:
        resp_header->ret =
            client_handle_request_buffer_open_raw(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_buffer_open_raw),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_buffer_open_raw),
                &size,
                &client->out_fds[0],
                &client->have_out_fd
            );
        break;
    case VRTD_REQ_BUFFER_CLOSE:
        resp_header->ret =
            client_handle_request_buffer_close(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_buffer_close),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_buffer_close),
                &size
            );
        break;
    case VRTD_REQ_DESIGN_WRITE:
        resp_header->ret =
            client_handle_request_design_write(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_design_write),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_design_write),
                &size
            );
        break;
    case VRTD_REQ_SET_SHELL_STATE:
        resp_header->ret =
            client_handle_request_set_shell_state(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_set_shell_state),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_set_shell_state),
                &size
            );
        break;
    case VRTD_REQ_CFGMEM_PROGRAM:
        resp_header->ret =
            client_handle_request_cfgmem_program(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_cfgmem_program),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_cfgmem_program),
                &size
            );
        break;
    case VRTD_REQ_CFGMEM_PROGRAM_START:
        resp_header->ret =
            client_handle_request_cfgmem_program_start(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_cfgmem_program_start),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_cfgmem_program_start),
                &size
            );
        break;
    case VRTD_REQ_CFGMEM_PROGRAM_STATUS:
        resp_header->ret =
            client_handle_request_cfgmem_program_status(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_cfgmem_program_status),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_cfgmem_program_status),
                &size
            );
        break;
    case VRTD_REQ_CLOCK_OP:
        resp_header->ret =
            client_handle_request_clock_op(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_clock_op),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_clock_op),
                &size
            );
        break;
    case VRTD_REQ_DEVICE_HOTPLUG_OP:
        resp_header->ret =
            client_handle_request_device_hotplug_op(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_device_hotplug_op),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_device_hotplug_op),
                &size
            );
        break;
    case VRTD_REQ_GET_SENSOR_INFO:
        resp_header->ret =
            client_handle_request_get_sensor_info(
                client,
                CLIENT_IN_BODY(*client, vrtd_req_get_sensor_info),
                req_header->size,
                CLIENT_OUT_BODY(*client, vrtd_resp_get_sensor_info),
                &size
            );
        break;

    default:
        LOG(LOG_WARNING, "Unknown opcode=%u from uid=%u conn_id=%llu",
            (unsigned int)req_header->opcode,
            (unsigned int)client->uid, (unsigned long long)client->conn_id);
        resp_header->ret = VRTD_RET_BAD_REQUEST;
        resp_header->size = 0;

        break;
    }

    /*
     * DESIGN_WRITE and async device operations are asynchronous: the handler
     * hands the work to a background worker and sets the corresponding pending
     * flag.  The response is prepared later (by
     * client_finalize_pending_design_write() or
     * process_async_device_op_completion()), so do not transition the state
     * machine yet.
     */
    if (client->pending_design_write || client->pending_async_device_op) {
        return 0;
    }

    if (resp_header->ret != VRTD_RET_OK) {
        LOG(LOG_DEBUG, "Request opcode=%u(%s) failed ret=%u uid=%u conn_id=%llu",
            (unsigned int)req_header->opcode, vrtd_opcode_to_string(req_header->opcode),
            (unsigned int)resp_header->ret,
            (unsigned int)client->uid, (unsigned long long)client->conn_id);
    }

    resp_header->size = size;

    /* Close the inbound fd if the handler did not consume it. */
    if (client->have_in_fd) {
        (void) close(client->in_fd);
        client->in_fd = -1;
        client->have_in_fd = false;
    }

    /*
     * Transition the state machine: mark the request as consumed and the
     * response as ready.  have_new_response triggers an immediate send
     * attempt in the same on_client_io() invocation.
     */
    client->have_request = false;
    client->have_response = true;
    client->have_new_response = true;

    return 0;
}

/* ---- Async design-write completion -------------------------------------- */

/**
 * Checks whether a pending asynchronous design write has completed and,
 * if so, prepares the response.
 *
 * Called from on_event_deferred_work() every 20 ms for each client that has
 * pending_design_write == true.
 *
 * If the design_writer reports completion (success or failure):
 *   - The response header is populated with the appropriate status.
 *   - The client state machine is transitioned: pending_design_write is
 *     cleared, have_request is cleared, have_response and have_new_response
 *     are set.
 *   - Returns 1 to signal the caller that the epoll events need updating.
 *
 * If the write is still in progress, returns 0 (no-op).
 *
 * @param client  The client whose pending write should be checked.
 * @return 0 if still in progress or not applicable, 1 if the write completed
 *         and the response was prepared, negative on fatal error.
 */
static int client_finalize_pending_design_write(struct client *client)
{
    if (client == NULL || !client->pending_design_write) {
        return 0;
    }

    struct device *d = client->pending_design_write_device;
    bool done = false;
    int transfer_error = 0;
    if (d == NULL || d->design_writer == NULL) {
        done = true;
        transfer_error = EIO;
    } else {
        int ret = design_writer_poll_result(d->design_writer, &done, &transfer_error);
        if (ret != 0) {
            done = true;
            transfer_error = (errno != 0) ? errno : EIO;
        }
    }

    if (!done) {
        return 0;
    }

    /* Build the deferred response now that the async write has finished. */
    uint16_t design_write_ret = VRTD_RET_OK;
    if (transfer_error == 0) {
        design_write_ret = device_refresh_pf2_after_design_write(d);
        LOG(LOG_INFO, "Design write completed successfully for uid=%u conn_id=%llu",
            (unsigned int)client->uid, (unsigned long long)client->conn_id);
    } else {
        LOG(LOG_WARNING, "Design write failed (error=%d) for uid=%u conn_id=%llu",
            transfer_error, (unsigned int)client->uid, (unsigned long long)client->conn_id);
        design_write_ret = VRTD_RET_INTERNAL_ERROR;
    }

    struct vrtd_req_header *req_header = CLIENT_IN_HEADER(*client);
    struct vrtd_resp_header *resp_header = CLIENT_OUT_HEADER(*client);
    struct vrtd_resp_design_write *resp_body = CLIENT_OUT_BODY(*client, vrtd_resp_design_write);

    resp_header->seqno = req_header->seqno;
    resp_header->ret = design_write_ret;

    if (design_write_ret == VRTD_RET_OK) {
        resp_body->zero = 0;
        resp_header->size = sizeof(*resp_body);
    } else {
        resp_header->size = 0;
    }

    /*
     * Transition the state machine: the async operation is done, so clear
     * the blocking flag and allow the response to be sent.
     */
    client->pending_design_write = false;
    client->pending_design_write_device = NULL;
    client->have_request = false;
    client->have_response = true;
    client->have_new_response = true;

    return 1;
}

/* ---- Async device operation completion ---------------------------------- */

/**
 * Reports whether any bitstream design write is currently active.
 *
 * A design write is considered active if a writer thread is busy on any device
 * or if any client is still awaiting an async design-write result.  Used to
 * reject an async device operation that would otherwise race a design write on
 * the shared device state.
 *
 * @param state  The daemon state.
 * @return true if a design write is in flight, false otherwise.
 */
static bool any_design_write_active(struct vrtd *state)
{
    if (state == NULL) {
        return false;
    }

    for (size_t i = 0; i < state->devices.len; i++) {
        struct device *d = state->devices.d[i];
        if (d != NULL && design_writer_is_busy(d->design_writer)) {
            return true;
        }
    }

    for (size_t i = 0; i < state->clients.len; i++) {
        struct client *c = state->clients.d[i];
        if (c != NULL && c->pending_design_write) {
            return true;
        }
    }

    return false;
}

static bool client_request_is_cfgmem_status(struct client *client)
{
    if (client == NULL || !client->have_request) {
        return false;
    }

    struct vrtd_req_header *req_header = CLIENT_IN_HEADER(*client);
    return req_header->opcode == VRTD_REQ_CFGMEM_PROGRAM_STATUS;
}

/**
 * Prepares a deferred async-device-operation response for a client and
 * transitions its state machine so the response can be sent on the next I/O
 * event.
 *
 * @param client  The client awaiting the async operation result.
 * @param result  The VRTD_RET_* code produced by the flash worker.
 */
static void client_deliver_async_device_op_result(struct client *client, uint16_t result)
{
    struct vrtd_req_header *req_header = CLIENT_IN_HEADER(*client);
    struct vrtd_resp_header *resp_header = CLIENT_OUT_HEADER(*client);

    resp_header->seqno = req_header->seqno;
    resp_header->ret = result;
    if (result == VRTD_RET_OK) {
        switch (req_header->opcode) {
        case VRTD_REQ_DEVICE_HOTPLUG_OP: {
            struct vrtd_resp_device_hotplug_op *resp_body =
                CLIENT_OUT_BODY(*client, vrtd_resp_device_hotplug_op);
            resp_body->zero = 0;
            resp_header->size = sizeof(*resp_body);
            break;
        }
        case VRTD_REQ_CFGMEM_PROGRAM:
        default: {
            struct vrtd_resp_cfgmem_program *resp_body =
                CLIENT_OUT_BODY(*client, vrtd_resp_cfgmem_program);
            resp_body->zero = 0;
            resp_header->size = sizeof(*resp_body);
            break;
        }
        }
    } else {
        resp_header->size = 0;
    }

    client->pending_async_device_op = false;
    client->have_request = false;
    client->have_response = true;
    client->have_new_response = true;

    LOG(LOG_INFO, "Async device op opcode=%u(%s) completed ret=%u uid=%u conn_id=%llu",
        (unsigned int)req_header->opcode, vrtd_opcode_to_string(req_header->opcode), (unsigned int)result,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);
}

/**
 * Polls the flash worker for completion of an in-progress async device op.
 *
 * If the worker is still running, this is a no-op and
 * async_device_op_in_progress remains set.  On completion it clears the guard,
 * delivers the response to the
 * waiting client (if still connected), and re-arms that client's epoll events.
 * The worker result is delivered regardless of client presence so the guard is
 * always cleared -- e.g. if the requesting client disconnected mid-operation.
 *
 * @param state  The daemon state.
 * @return 0 on success, negative on fatal error.
 */
static int process_async_device_op_completion(struct vrtd *state)
{
    if (state->flash_worker == NULL) {
        state->async_device_op_in_progress = false;
        return 0;
    }

    bool done = false;
    uint16_t result = 0;
    int ret = flash_worker_poll_result(state->flash_worker, &done, &result);
    if (ret != 0) {
        done = true;
        result = VRTD_RET_INTERNAL_ERROR;
    }

    if (!done) {
        return 0;
    }

    state->async_device_op_in_progress = false;
    drain_deferred_buffer_cleanups(state);

    for (size_t i = 0; i < state->clients.len; i++) {
        struct client *client = state->clients.d[i];
        if (client == NULL || !client->pending_async_device_op) {
            continue;
        }

        client_deliver_async_device_op_result(client, result);

        ret = client_update_wanted_epoll_events(client, client->event_source);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to update epoll after async device op completion");
    }

    return 0;
}

/**
 * Dispatches requests that were queued while an async device op held the device
 * list exclusively.
 *
 * For each client with a fully-received request awaiting dispatch (and no async
 * operation pending), runs the handler and re-arms its epoll events so the
 * response is sent.  If a drained request starts a new async device op,
 * draining stops so the remaining requests stay queued until that operation
 * completes.
 *
 * @param state  The daemon state.
 * @return 0 on success, negative on fatal error.
 */
static int drain_deferred_requests(struct vrtd *state)
{
    for (size_t i = 0; i < state->clients.len; i++) {
        struct client *client = state->clients.d[i];
        if (client == NULL) {
            continue;
        }
        if (!client->have_request || client->have_response) {
            continue;
        }
        if (client->pending_design_write || client->pending_async_device_op) {
            continue;
        }

        int ret = client_handle_request(client);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to dispatch deferred request");

        ret = client_update_wanted_epoll_events(client, client->event_source);
        PROPAGATE_ERROR_LOG(ret, LOG_ERR, "Failed to update epoll for deferred dispatch");

        if (state->async_device_op_in_progress) {
            break;
        }
    }

    return 0;
}

/* ======================================================================== */
/* Request handler functions                                                 */
/*                                                                           */
/* Each handler follows a uniform pattern:                                   */
/*   1. Authorize the request via auth_request_*().                          */
/*   2. Validate req_size and request-specific parameters.                   */
/*   3. Perform the operation (device lookup, ioctl, etc.).                  */
/*   4. Populate resp_body and *resp_size.                                   */
/*   5. Return a VRTD_RET_* status code.                                    */
/*                                                                           */
/* Handlers that pass an fd back to the client additionally accept out_fd    */
/* and have_out_fd output parameters.  The fd is delivered via SCM_RIGHTS    */
/* in client_handle_out().                                                   */
/* ======================================================================== */

/**
 * Handles VRTD_REQ_GET_NUM_DEVICES.
 *
 * Returns the number of SLASH devices currently known to the daemon.
 * Devices are numbered 0..n-1 and the count may be used by clients to
 * enumerate them.
 *
 * Auth: auth_request_get_num_devices (typically unrestricted).
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_num_devices  (placeholder zero byte)
 *   Response body: vrtd_resp_get_num_devices { uint32_t num_devices }
 *
 * @return VRTD_RET_OK on success, or VRTD_RET_AUTH_ERROR / VRTD_RET_BAD_REQUEST.
 */
static uint16_t client_handle_request_get_num_devices(
    struct client *client,
    const struct vrtd_req_get_num_devices *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_num_devices *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_get_num_devices(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_num_devices: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    resp_body->num_devices = client->state->devices.len;

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "get_num_devices: count=%zu uid=%u conn_id=%llu",
        (size_t)resp_body->num_devices,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- DESIGN_WRITE ------------------------------------------------------- */

/**
 * Handles VRTD_REQ_DESIGN_WRITE -- initiates an asynchronous bitstream load.
 *
 * The client sends a file descriptor (via SCM_RIGHTS in the request message)
 * pointing to the bitstream file.  This handler takes ownership of that fd,
 * submits it to the device's design_writer for asynchronous DMA transfer,
 * and sets the pending_design_write flag.
 *
 * Because the transfer is asynchronous, this handler does NOT prepare a
 * response.  The response is deferred until on_event_deferred_work() detects
 * completion via client_finalize_pending_design_write().  While
 * pending_design_write is true the client is blocked from sending further
 * requests (EPOLLIN is disarmed).
 *
 * Auth: auth_request_design_write (typically requires elevated privileges).
 * FD passing: inbound -- the client sends the bitstream fd via SCM_RIGHTS.
 *
 * Wire format:
 *   Request body:  vrtd_req_design_write { uint32_t dev_number }
 *   Response body: vrtd_resp_design_write { uint8_t zero } (deferred)
 *
 * @return VRTD_RET_OK if the write was successfully submitted,
 *         VRTD_RET_BUSY if the design writer is already active,
 *         or other VRTD_RET_* codes on error.
 */
static uint16_t client_handle_request_design_write(
    struct client *client,
    const struct vrtd_req_design_write *req_body,
    uint16_t req_size,
    struct vrtd_resp_design_write *resp_body,
    uint16_t *resp_size
)
{
    (void)resp_body;

    int ret = auth_request_design_write(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "design_write: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "design_write: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->design_writer == NULL) {
        LOG(LOG_NOTICE, "design_write: device %u has no design writer", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    if (!client->have_in_fd || client->in_fd < 0) {
        LOG(LOG_WARNING, "design_write: no input fd provided");
        return VRTD_RET_BAD_REQUEST;
    }

    enum vrtd_shell_type required_shell = (enum vrtd_shell_type)req_body->required_shell;
    uint32_t boot_partition = 0;
    if (shell_boot_partition(required_shell, &boot_partition) != 0) {
        LOG(LOG_WARNING, "design_write: invalid required shell %u",
            (unsigned int)req_body->required_shell);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (design_writer_is_busy(d->design_writer)) {
        LOG(LOG_NOTICE, "design_write: writer busy for device %u", (unsigned int)req_body->dev_number);
        return VRTD_RET_BUSY;
    }

    if (shell_switch_blocked_by_jtag(d->current_shell, required_shell, d->jtag)) {
        LOG(LOG_NOTICE,
            "design_write: refusing automatic shell reset for JTAG-booted device dev=%u bdf=%s current=%u required=%u",
            (unsigned int)req_body->dev_number, d->pci_info.bdf,
            (unsigned int)d->current_shell, (unsigned int)required_shell);
        return VRTD_RET_SHELL_LOCKED;
    }

    if (shell_reset_required(d->current_shell, required_shell)) {
        char target_bdf[VRTD_PCI_BDF_LEN] = {0};
        strncpy(target_bdf, d->pci_info.bdf, sizeof(target_bdf) - 1);

        LOG(LOG_INFO,
            "design_write: shell switch required for dev=%u bdf=%s current=%u required=%u partition=%u",
            (unsigned int)req_body->dev_number, target_bdf,
            (unsigned int)d->current_shell, (unsigned int)required_shell,
            boot_partition);

        uint16_t reset_ret = reset_with_ami(d, &client->state->devices, required_shell);
        if (reset_ret != VRTD_RET_OK) {
            return reset_ret;
        }

        d = device_ptr_array_find_by_bdf(&client->state->devices, target_bdf);
        if (d == NULL || d->design_writer == NULL) {
            LOG(LOG_ERR, "design_write: device %s missing after shell reset", target_bdf);
            return VRTD_RET_NOEXIST;
        }
    }

    /*
     * Submit the fd for asynchronous DMA.  design_writer_submit_fd_async()
     * takes ownership of the fd -- we must not close it on success.
     */
    int fd = client->in_fd;
    ret = design_writer_submit_fd_async(d->design_writer, fd);
    if (ret != 0) {
        if (design_writer_is_busy(d->design_writer)) {
            LOG(LOG_NOTICE, "design_write: writer busy for device %u", (unsigned int)req_body->dev_number);
            return VRTD_RET_BUSY;
        }
        LOG(LOG_WARNING, "design_write: failed to submit async write for device %u", (unsigned int)req_body->dev_number);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /* The design writer now owns the fd; clear our reference. */
    client->in_fd = -1;
    client->have_in_fd = false;

    /*
     * Enter the async-waiting state.  client_handle_request() will see
     * pending_design_write and skip the normal response path.
     */
    client->pending_design_write = true;
    client->pending_design_write_device = d;

    LOG(LOG_INFO, "Design write submitted dev=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    *resp_size = 0;
    return VRTD_RET_OK;
}

/* ---- SET_SHELL_STATE ---------------------------------------------------- */

static uint16_t client_handle_request_set_shell_state(
    struct client *client,
    const struct vrtd_req_set_shell_state *req_body,
    uint16_t req_size,
    struct vrtd_resp_set_shell_state *resp_body,
    uint16_t *resp_size
)
{
    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "set_shell_state: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    int ret = auth_request_set_shell_state(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "set_shell_state: device %u does not exist",
            (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL) {
        LOG(LOG_NOTICE, "set_shell_state: device %u is null",
            (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    enum vrtd_shell_type shell_type = (enum vrtd_shell_type)req_body->shell_type;
    uint32_t unused_partition = 0;
    if (shell_boot_partition(shell_type, &unused_partition) != 0) {
        LOG(LOG_WARNING, "set_shell_state: invalid shell type %u",
            (unsigned int)req_body->shell_type);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (d->current_shell != VRTD_SHELL_UNKNOWN && d->current_shell != shell_type) {
        LOG(LOG_NOTICE,
            "set_shell_state: refusing to change known shell state dev=%u current=%u requested=%u",
            (unsigned int)req_body->dev_number,
            (unsigned int)d->current_shell, (unsigned int)shell_type);
        return VRTD_RET_BUSY;
    }

    if (req_body->jtag == 0 && d->jtag) {
        LOG(LOG_NOTICE, "set_shell_state: refusing to clear JTAG flag for dev=%u",
            (unsigned int)req_body->dev_number);
        return VRTD_RET_BUSY;
    }

    d->current_shell = shell_type;
    d->jtag = req_body->jtag != 0;

    memset(resp_body, 0, sizeof(*resp_body));
    *resp_size = sizeof(*resp_body);

    LOG(LOG_INFO, "set_shell_state: dev=%u shell=%u jtag=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)d->current_shell,
        (unsigned int)d->jtag, (unsigned int)client->uid,
        (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- CFGMEM_PROGRAM ----------------------------------------------------- */

static uint16_t client_submit_cfgmem_program(
    struct client *client,
    uint32_t dev_number,
    uint8_t boot_device,
    const uint8_t reserved[3],
    uint32_t partition,
    uint64_t *job_id_out
)
{
    struct vrtd_req_cfgmem_program auth_req = {
        .dev_number = dev_number,
        .boot_device = boot_device,
        .reserved = { reserved[0], reserved[1], reserved[2] },
        .partition = partition,
    };

    int ret = auth_request_cfgmem_program(client, &auth_req);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    if (dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "cfgmem_program: device %u does not exist", (unsigned int)dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[dev_number];
    if (d == NULL) {
        LOG(LOG_NOTICE, "cfgmem_program: device %u is null", (unsigned int)dev_number);
        return VRTD_RET_NOEXIST;
    }

    if (!client->have_in_fd || client->in_fd < 0) {
        LOG(LOG_WARNING, "cfgmem_program: no input fd provided");
        return VRTD_RET_BAD_REQUEST;
    }

    if (boot_device >= AMI_BOOT_DEVICES_MAX) {
        LOG(LOG_WARNING, "cfgmem_program: invalid boot device %u", (unsigned int)boot_device);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (partition > VRTD_CFGMEM_MAX_PARTITION) {
        LOG(LOG_WARNING, "cfgmem_program: invalid partition %u", (unsigned int)partition);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (reserved[0] != 0 || reserved[1] != 0 || reserved[2] != 0) {
        LOG(LOG_WARNING, "cfgmem_program: reserved fields must be zero");
        return VRTD_RET_INVALID_ARGUMENT;
    }

    /*
     * A cfgmem program takes exclusive control of the device list during its
     * reset step.  Refuse to start one if another is already running or if any
     * design write is in flight -- letting them overlap would race on the
     * shared device state.
     */
    if (client->state->async_device_op_in_progress) {
        LOG(LOG_NOTICE, "cfgmem_program: another async device op is already in progress");
        return VRTD_RET_BUSY;
    }
    if (any_design_write_active(client->state)) {
        LOG(LOG_NOTICE, "cfgmem_program: a design write is in progress");
        return VRTD_RET_BUSY;
    }
    if (client->state->flash_worker == NULL) {
        LOG(LOG_ERR, "cfgmem_program: flash worker unavailable");
        return VRTD_RET_INTERNAL_ERROR;
    }

    LOG(LOG_INFO, "Cfgmem program submitted dev=%u bdf=%s boot_device=%u partition=%u uid=%u conn_id=%llu",
        (unsigned int)dev_number,
        d->pci_info.bdf,
        (unsigned int)boot_device,
        (unsigned int)partition,
        (unsigned int)client->uid,
        (unsigned long long)client->conn_id);

    /*
     * Hand the PDI fd to the worker.  On success it owns the fd and will close
     * it when done, so mark it consumed to stop client_handle_request() from
     * closing it out from under the worker.
     */
    int submit_ret = flash_worker_submit_async(
        client->state->flash_worker,
        d,
        &client->state->devices,
        client->in_fd,
        boot_device,
        partition,
        client->conn_id,
        job_id_out
    );
    if (submit_ret != 0) {
        LOG(LOG_NOTICE, "cfgmem_program: flash worker busy");
        return VRTD_RET_BUSY;
    }

    client->in_fd = -1;
    client->have_in_fd = false;
    client->state->async_device_op_in_progress = true;

    return VRTD_RET_OK;
}

/**
 * Handles VRTD_REQ_CFGMEM_PROGRAM -- programs cfgmem through AMI.
 *
 * This is the legacy blocking request: it submits the worker job and keeps the
 * client request pending until process_async_device_op_completion() delivers the final
 * result.
 */
static uint16_t client_handle_request_cfgmem_program(
    struct client *client,
    const struct vrtd_req_cfgmem_program *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program *resp_body,
    uint16_t *resp_size
)
{
    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "cfgmem_program: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    uint64_t job_id = 0;
    uint16_t ret = client_submit_cfgmem_program(
        client,
        req_body->dev_number,
        req_body->boot_device,
        req_body->reserved,
        req_body->partition,
        &job_id
    );
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    /*
     * Enter the async-waiting state.  client_handle_request() sees
     * pending_async_device_op and skips the normal response path;
     * async_device_op_in_progress makes the event loop defer every other
     * request until the program completes.
     */
    client->pending_async_device_op = true;

    (void) resp_body;
    (void) job_id;
    *resp_size = 0;
    return VRTD_RET_OK;
}

static uint16_t client_handle_request_cfgmem_program_start(
    struct client *client,
    const struct vrtd_req_cfgmem_program_start *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program_start *resp_body,
    uint16_t *resp_size
)
{
    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "cfgmem_program_start: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    uint64_t job_id = 0;
    uint16_t ret = client_submit_cfgmem_program(
        client,
        req_body->dev_number,
        req_body->boot_device,
        req_body->reserved,
        req_body->partition,
        &job_id
    );
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    resp_body->job_id = job_id;
    *resp_size = sizeof(*resp_body);
    return VRTD_RET_OK;
}

static uint16_t client_handle_request_cfgmem_program_status(
    struct client *client,
    const struct vrtd_req_cfgmem_program_status *req_body,
    uint16_t req_size,
    struct vrtd_resp_cfgmem_program_status *resp_body,
    uint16_t *resp_size
)
{
    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "cfgmem_program_status: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }
    if (client->state->flash_worker == NULL) {
        LOG(LOG_ERR, "cfgmem_program_status: flash worker unavailable");
        return VRTD_RET_INTERNAL_ERROR;
    }

    errno = 0;
    int ret = flash_worker_poll_status_for_owner(
        client->state->flash_worker,
        req_body->job_id,
        client->conn_id,
        &resp_body->status
    );
    if (ret != 0) {
        if (errno == ENOENT) {
            return VRTD_RET_NOEXIST;
        }
        if (errno == EACCES) {
            return VRTD_RET_AUTH_ERROR;
        }
        return VRTD_RET_INTERNAL_ERROR;
    }

    *resp_size = sizeof(*resp_body);
    return VRTD_RET_OK;
}

static uint16_t client_submit_device_reset(
    struct client *client,
    uint32_t dev_number,
    struct device *device,
    uint32_t partition
)
{
    if (device == NULL) {
        LOG(LOG_NOTICE, "reset_sequence: device %u is null", (unsigned int)dev_number);
        return VRTD_RET_NOEXIST;
    }

    /*
     * Reset takes exclusive control of the device list.  Refuse to start while
     * another async device operation or a design write is active; otherwise the
     * worker and event loop could race on shared device state.
     */
    if (client->state->async_device_op_in_progress) {
        LOG(LOG_NOTICE, "reset_sequence: another async device op is already in progress");
        return VRTD_RET_BUSY;
    }
    if (any_design_write_active(client->state)) {
        LOG(LOG_NOTICE, "reset_sequence: a design write is in progress");
        return VRTD_RET_BUSY;
    }
    if (client->state->flash_worker == NULL) {
        LOG(LOG_ERR, "reset_sequence: flash worker unavailable");
        return VRTD_RET_INTERNAL_ERROR;
    }

    uint64_t job_id = 0;
    int submit_ret = flash_worker_submit_reset_async(
        client->state->flash_worker,
        device,
        &client->state->devices,
        partition,
        client->conn_id,
        &job_id
    );
    if (submit_ret != 0) {
        LOG(LOG_NOTICE, "reset_sequence: flash worker busy");
        return VRTD_RET_BUSY;
    }

    LOG(LOG_INFO, "Reset sequence submitted dev=%u bdf=%s uid=%u conn_id=%llu",
        (unsigned int)dev_number,
        device->pci_info.bdf,
        (unsigned int)client->uid,
        (unsigned long long)client->conn_id);

    client->state->async_device_op_in_progress = true;
    client->pending_async_device_op = true;
    (void) job_id;

    return VRTD_RET_OK;
}

/* ---- DEVICE_HOTPLUG_OP -------------------------------------------------- */

/**
 * Handles VRTD_REQ_DEVICE_HOTPLUG_OP -- performs a PCIe hotplug operation.
 *
 * Dispatches one of several PCIe topology-management operations on the
 * specified device:
 *
 *   RESCAN         -- Triggers a PCI bus rescan (all devices) and refreshes
 *                     vrtd's discovered device list.
 *   REMOVE         -- Removes one PF, or all V80 PFs when function is
 *                     VRTD_DEVICE_HOTPLUG_FUNCTION_ALL, then removes the
 *                     board from vrtd's tracked device list.
 *   TOGGLE_SBR     -- Toggles Secondary Bus Reset on the device's upstream
 *                     bridge.
 *   HOTPLUG        -- Performs a hotplug cycle (remove + rescan) for one PF,
 *                     or all V80 PFs when function is
 *                     VRTD_DEVICE_HOTPLUG_FUNCTION_ALL.
 *   RESET_SEQUENCE -- Submits an asynchronous reset using the AMI-based reset
 *                     flow (reset_with_ami), which includes SBR, device
 *                     removal, rescan, and re-enumeration.
 *
 * Auth: RESCAN is unauthenticated and ignores dev_number. Other operations use
 *       auth_request_device_hotplug_op (typically requires elevated privileges,
 *       as these operations can disrupt other users).
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_device_hotplug_op { uint32_t dev_number,
 *                                               uint8_t op }
 *   Response body: vrtd_resp_device_hotplug_op { uint8_t zero }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_INVALID_ARGUMENT for unknown ops.
 */
static uint16_t client_handle_request_device_hotplug_op(
    struct client *client,
    const struct vrtd_req_device_hotplug_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_device_hotplug_op *resp_body,
    uint16_t *resp_size
)
{
    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "hotplug_op: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->op == VRTD_DEVICE_HOTPLUG_OP_RESCAN) {
        LOG(LOG_INFO, "Hotplug op=rescan(%u) uid=%u conn_id=%llu",
            (unsigned int)req_body->op,
            (unsigned int)client->uid, (unsigned long long)client->conn_id);

        int ret = slash_hotplug_rescan(g_hotplug);
        if (ret != 0) {
            LOG(LOG_WARNING, "hotplug_op: rescan failed: %m");
            return hotplug_errno_to_vrtd_ret(errno);
        }

        /* Device nodes appear asynchronously after PCI rescan.
         * TODO(vserbu): Move this wait and device discovery to a background
         * thread so rescan does not block the daemon event loop. */
        usleep(5000000);

        ret = devices_discover_and_open(&client->state->devices);
        if (ret != 0) {
            LOG(LOG_WARNING, "hotplug_op: failed to refresh devices after rescan");
        }

        resp_body->zero = 0;
        *resp_size = sizeof(*resp_body);
        return VRTD_RET_OK;
    }

    int ret = auth_request_device_hotplug_op(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "hotplug_op: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL) {
        LOG(LOG_NOTICE, "hotplug_op: device %u is null", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    LOG(LOG_INFO, "Hotplug op=%s(%u) bdf=%s dev=%u uid=%u conn_id=%llu",
        vrtd_hotplug_op_to_string(req_body->op), (unsigned int)req_body->op,
        d->pci_info.bdf, (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    switch (req_body->op) {
    case VRTD_DEVICE_HOTPLUG_OP_REMOVE:
    case VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR:
    case VRTD_DEVICE_HOTPLUG_OP_HOTPLUG: {
        if (req_body->function == VRTD_DEVICE_HOTPLUG_FUNCTION_ALL) {
            if (req_body->op == VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR) {
                LOG(LOG_ERR, "hotplug_op: toggle_sbr requires a specific function number");
                return VRTD_RET_INVALID_ARGUMENT;
            }

            bool any_removed = false;
            for (uint8_t func = 0; func < 3; func++) {
                char pf_bdf[VRTD_PCI_BDF_LEN];
                if (pci_bdf_set_function(d->pci_info.bdf, func, pf_bdf) != 0) {
                    LOG(LOG_ERR, "hotplug_op: %s: failed to construct PF%u BDF from %s",
                        vrtd_hotplug_op_to_string(req_body->op),
                        (unsigned int)func, d->pci_info.bdf);
                    return VRTD_RET_INTERNAL_ERROR;
                }

                ret = slash_hotplug_remove(g_hotplug, pf_bdf);
                if (ret != 0 && errno != ENODEV) {
                    int err = errno;
                    LOG(LOG_WARNING, "hotplug_op: %s failed removing PF%u bdf=%s: %m",
                        vrtd_hotplug_op_to_string(req_body->op),
                        (unsigned int)func, pf_bdf);
                    if (any_removed) {
                        device_ptr_array_rm_by_reference(&client->state->devices, d);
                        d = NULL;
                    }
                    return hotplug_errno_to_vrtd_ret(err);
                }
                any_removed = true;
            }

            device_ptr_array_rm_by_reference(&client->state->devices, d);
            d = NULL;

            if (req_body->op == VRTD_DEVICE_HOTPLUG_OP_HOTPLUG) {
                ret = slash_hotplug_rescan(g_hotplug);
                if (ret != 0) {
                    LOG(LOG_WARNING, "hotplug_op: hotplug rescan failed: %m");
                    return hotplug_errno_to_vrtd_ret(errno);
                }

                usleep(5000000);

                ret = devices_discover_and_open(&client->state->devices);
                if (ret != 0) {
                    LOG(LOG_WARNING, "hotplug_op: failed to refresh devices after hotplug");
                }
            }

            ret = 0;
            break;
        }

        /* Individual hotplug operations are PCI-function-level (the hotplug
         * interface is SLASH-agnostic).  Construct a full DDDD:BB:DD.F BDF
         * from the device's board-level address and the requested function. */
        if (req_body->function > 7) {
            LOG(LOG_ERR, "hotplug_op: %s: invalid function number %u",
                vrtd_hotplug_op_to_string(req_body->op),
                (unsigned int)req_body->function);
            return VRTD_RET_INVALID_ARGUMENT;
        }

        char pf_bdf[VRTD_PCI_BDF_LEN];
        if (pci_bdf_set_function(d->pci_info.bdf, req_body->function, pf_bdf) != 0) {
            LOG(LOG_ERR, "hotplug_op: %s: failed to construct PF%u BDF from %s",
                vrtd_hotplug_op_to_string(req_body->op),
                (unsigned int)req_body->function, d->pci_info.bdf);
            return VRTD_RET_INTERNAL_ERROR;
        }

        switch (req_body->op) {
        case VRTD_DEVICE_HOTPLUG_OP_REMOVE:
            ret = slash_hotplug_remove(g_hotplug, pf_bdf);
            if (ret == 0) {
                device_ptr_array_rm_by_reference(&client->state->devices, d);
                d = NULL;
            }
            break;
        case VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR:
            ret = slash_hotplug_toggle_sbr(g_hotplug, pf_bdf);
            break;
        case VRTD_DEVICE_HOTPLUG_OP_HOTPLUG:
            ret = slash_hotplug_hotplug(g_hotplug, pf_bdf);
            break;
        default:
            break;
        }
        break;
    }
    case VRTD_DEVICE_HOTPLUG_OP_RESET_SEQUENCE: {
        (void) resp_body;
        (void) resp_size;
        uint32_t boot_partition = 0;
        if (shell_boot_partition((enum vrtd_shell_type)req_body->shell_type,
                                 &boot_partition) != 0) {
            LOG(LOG_WARNING, "reset_sequence: invalid shell type %u",
                (unsigned int)req_body->shell_type);
            return VRTD_RET_INVALID_ARGUMENT;
        }
        return client_submit_device_reset(client, req_body->dev_number, d,
                                          boot_partition);
    }
    default:
        LOG(LOG_WARNING, "hotplug_op: invalid op %u for device %u",
            (unsigned int)req_body->op, (unsigned int)req_body->dev_number);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (ret != 0) {
        LOG(LOG_WARNING, "hotplug_op: %s failed for device %u bdf=%s: %m",
            vrtd_hotplug_op_to_string(req_body->op),
            (unsigned int)req_body->dev_number, d->pci_info.bdf);
        return hotplug_errno_to_vrtd_ret(errno);
    }

    resp_body->zero = 0;
    *resp_size = sizeof(*resp_body);
    return VRTD_RET_OK;
}

/* ---- QDMA_GET_INFO ------------------------------------------------------ */

/**
 * Handles VRTD_REQ_QDMA_GET_INFO -- queries QDMA capabilities of a device.
 *
 * Reads the QDMA information structure (slash_qdma_info) from the device's
 * QDMA subsystem and returns it to the client.
 *
 * Auth: auth_request_qdma_get_info.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_qdma_get_info { uint32_t dev_number }
 *   Response body: vrtd_resp_qdma_get_info { slash_qdma_info info }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_NOEXIST if the device has no QDMA.
 */
static uint16_t client_handle_request_qdma_get_info(
    struct client *client,
    const struct vrtd_req_qdma_get_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_get_info *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_qdma_get_info(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "qdma_get_info: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "qdma_get_info: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL) {
        LOG(LOG_NOTICE, "qdma_get_info: device %u has no QDMA", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    /* resp_body is packed; libslash expects a normally-aligned pointer. */
    struct slash_qdma_info info;
    if (slash_qdma_info_read(d->qdma, &info) != 0) {
        LOG(LOG_WARNING, "qdma_get_info: failed to read info for device %u: %m", (unsigned int)req_body->dev_number);
        return VRTD_RET_INTERNAL_ERROR;
    }
    memcpy(&resp_body->info, &info, sizeof(info));

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "qdma_get_info: dev=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- QDMA_QPAIR_ADD ----------------------------------------------------- */

/**
 * Handles VRTD_REQ_QDMA_QPAIR_ADD -- creates a QDMA queue pair on a device.
 *
 * Creates a new queue pair with the parameters specified in the request.
 * The kernel allocates a queue ID (qid) which is returned in the response
 * body.  The caller can then start the queue pair and obtain its fd.
 *
 * Auth: auth_request_qdma_qpair_add.
 * FD passing: none (use QDMA_QPAIR_GET_FD after starting the queue).
 *
 * Wire format:
 *   Request body:  vrtd_req_qdma_qpair_add { uint32_t dev_number,
 *                                             slash_qdma_qpair_add add }
 *   Response body: vrtd_resp_qdma_qpair_add { slash_qdma_qpair_add add }
 *                  (with qid filled in by the kernel)
 *
 * @return VRTD_RET_OK on success.
 */
static uint16_t client_handle_request_qdma_qpair_add(
    struct client *client,
    const struct vrtd_req_qdma_qpair_add *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_add *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_qdma_qpair_add(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "qdma_qpair_add: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "qdma_qpair_add: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL) {
        LOG(LOG_NOTICE, "qdma_qpair_add: device %u has no QDMA", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    /* resp_body is packed; libslash expects a normally-aligned pointer. */
    struct slash_qdma_qpair_add add = req_body->add;

    if (slash_qdma_qpair_add(d->qdma, &add) != 0) {
        LOG(LOG_WARNING, "qdma_qpair_add: failed for device %u: %m", (unsigned int)req_body->dev_number);
        return VRTD_RET_INTERNAL_ERROR;
    }
    memcpy(&resp_body->add, &add, sizeof(add));

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "qdma_qpair_add: dev=%u qid=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)add.qid,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- QDMA_QPAIR_OP ----------------------------------------------------- */

/**
 * Handles VRTD_REQ_QDMA_QPAIR_OP -- performs an operation on a QDMA queue pair.
 *
 * Dispatches one of the following operations on the specified queue pair:
 *   - SLASH_QDMA_QUEUE_OP_START: Activates the queue for DMA transfers.
 *   - SLASH_QDMA_QUEUE_OP_STOP:  Halts the queue.
 *   - SLASH_QDMA_QUEUE_OP_DEL:   Deletes the queue pair and releases its
 *                                 resources.
 *
 * Auth: auth_request_qdma_qpair_op.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_qdma_qpair_op { uint32_t dev_number,
 *                                            uint32_t qid, uint32_t op }
 *   Response body: vrtd_resp_qdma_qpair_op { uint8_t zero }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_INVALID_ARGUMENT for unknown ops.
 */
static uint16_t client_handle_request_qdma_qpair_op(
    struct client *client,
    const struct vrtd_req_qdma_qpair_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_op *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_qdma_qpair_op(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "qdma_qpair_op: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "qdma_qpair_op: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL) {
        LOG(LOG_NOTICE, "qdma_qpair_op: device %u has no QDMA", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    switch (req_body->op) {
    case SLASH_QDMA_QUEUE_OP_START:
        ret = slash_qdma_qpair_start(d->qdma, req_body->qid);
        break;
    case SLASH_QDMA_QUEUE_OP_STOP:
        ret = slash_qdma_qpair_stop(d->qdma, req_body->qid);
        break;
    case SLASH_QDMA_QUEUE_OP_DEL:
        ret = slash_qdma_qpair_del(d->qdma, req_body->qid);
        break;
    default:
        LOG(LOG_WARNING, "qdma_qpair_op: invalid op %u for device %u qid=%u",
            (unsigned int)req_body->op, (unsigned int)req_body->dev_number, (unsigned int)req_body->qid);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (ret != 0) {
        LOG(LOG_WARNING, "qdma_qpair_op: op %u failed for device %u qid=%u: %m",
            (unsigned int)req_body->op, (unsigned int)req_body->dev_number, (unsigned int)req_body->qid);
        return VRTD_RET_INTERNAL_ERROR;
    }

    resp_body->zero = 0;
    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "qdma_qpair_op: dev=%u qid=%u op=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)req_body->qid,
        (unsigned int)req_body->op,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- QDMA_QPAIR_GET_FD ------------------------------------------------- */

/**
 * Handles VRTD_REQ_QDMA_QPAIR_GET_FD -- obtains the character-device fd for
 * a QDMA queue pair.
 *
 * Returns a file descriptor that the client can read/write to perform DMA
 * transfers through the specified queue pair.  The fd is delivered to the
 * client via SCM_RIGHTS in the response message.
 *
 * Auth: auth_request_qdma_qpair_get_fd.
 * FD passing: outbound -- the qpair fd is sent via SCM_RIGHTS.
 *
 * Wire format:
 *   Request body:  vrtd_req_qdma_qpair_get_fd { uint32_t dev_number,
 *                                                uint32_t qid,
 *                                                uint32_t flags }
 *   Response body: vrtd_resp_qdma_qpair_get_fd { uint8_t zero }
 *                  + SCM_RIGHTS fd
 *
 * @return VRTD_RET_OK on success.
 */
static uint16_t client_handle_request_qdma_qpair_get_fd(
    struct client *client,
    const struct vrtd_req_qdma_qpair_get_fd *req_body,
    uint16_t req_size,
    struct vrtd_resp_qdma_qpair_get_fd *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
)
{
    int ret = auth_request_qdma_qpair_get_fd(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;
    *have_out_fd = false;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "qdma_qpair_get_fd: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "qdma_qpair_get_fd: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL) {
        LOG(LOG_NOTICE, "qdma_qpair_get_fd: device %u has no QDMA", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    int fd = slash_qdma_qpair_get_fd(d->qdma, req_body->qid, (int)req_body->flags);
    if (fd < 0) {
        LOG(LOG_WARNING, "qdma_qpair_get_fd: failed for device %u qid=%u: %m",
            (unsigned int)req_body->dev_number, (unsigned int)req_body->qid);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /* Schedule this fd for delivery via SCM_RIGHTS in client_handle_out(). */
    *out_fd = fd;
    *have_out_fd = true;

    resp_body->zero = 0;
    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "qdma_qpair_get_fd: dev=%u qid=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)req_body->qid,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- BUFFER_OPEN -------------------------------------------------------- */

/**
 * Handles VRTD_REQ_BUFFER_OPEN -- allocates a DMA buffer and returns its fd.
 *
 * Creates a new buffer on the specified device with the requested allocation
 * type (DDR, HBM, HBM_VNOC), direction, and size.  The buffer consists of a
 * device-memory allocation and an associated QDMA queue pair.  The qpair fd
 * is returned to the client via SCM_RIGHTS so the client can read/write
 * directly to perform DMA.
 *
 * The buffer is tracked in the device's buffer list with the client's conn_id
 * so that it can be automatically freed if the client disconnects without
 * calling BUFFER_CLOSE (see cleanup_client_buffers).
 *
 * Auth: auth_request_buffer_open.
 * FD passing: outbound -- the buffer qpair fd is sent via SCM_RIGHTS.
 *
 * Wire format:
 *   Request body:  vrtd_req_buffer_open { uint32_t dev_number,
 *                                         uint32_t alloc_type,
 *                                         uint32_t alloc_dir,
 *                                         uint64_t alloc_arg,
 *                                         uint64_t size }
 *   Response body: vrtd_resp_buffer_open { uint64_t size,
 *                                          uint64_t phys_addr }
 *                  + SCM_RIGHTS fd
 *
 * @return VRTD_RET_OK on success, VRTD_RET_BUSY if memory is exhausted,
 *         VRTD_RET_INVALID_ARGUMENT for bad allocation parameters.
 */
static uint16_t client_handle_request_buffer_open(
    struct client *client,
    const struct vrtd_req_buffer_open *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_open *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
)
{
    int ret = auth_request_buffer_open(client, req_body);
    if (ret == -1) {
        char pwbuf[1024];
        LOG(LOG_WARNING, "Failed to authorize buffer open request for uid %u(%s): %m",
            (unsigned int) client->uid, uid_to_username(client->uid, pwbuf, sizeof(pwbuf)));
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;
    *have_out_fd = false;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "Received malformed buffer open request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_WARNING, "Received buffer open request for non-existent device");
        return VRTD_RET_NOEXIST;
    }

    if (req_body->size == 0) {
        LOG(LOG_WARNING, "Received buffer open request with zero size");
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (req_body->mm_channel > SLASH_QDMA_MM_CHANNEL_1) {
        LOG(LOG_WARNING, "Received buffer open request with invalid mm_channel %u",
            (unsigned int)req_body->mm_channel);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL || d->memory_map == NULL) {
        LOG(LOG_WARNING, "Received buffer open request for non-existent or non-functional device");
        return VRTD_RET_NOEXIST;
    }

    uint64_t client_id = client->conn_id;
    if (client_id == 0) {
        LOG(LOG_ERR, "Invalid client connection id");
        return VRTD_RET_INTERNAL_ERROR;
    }

    /*
     * Create the buffer (allocation + qpair).  _cleanup_(cleanup_bufferp)
     * ensures the buffer is freed if we return early before transferring
     * ownership to the device's buffer array.
     */
    _cleanup_(cleanup_bufferp)
    struct buffer *buf = buffer_create(
        d->qdma,
        d->memory_map,
        (enum allocation_type) req_body->alloc_type,
        (enum vrtd_alloc_dir) req_body->alloc_dir,
        req_body->size,
        req_body->alloc_arg,
        client_id,
        req_body->mm_channel,
        NULL
    );
    if (buf == NULL) {
        if (errno == EINVAL) {
            LOG(LOG_WARNING, "buffer_open: invalid allocation arguments for device %u", (unsigned int)req_body->dev_number);
            return VRTD_RET_INVALID_ARGUMENT;
        }
        if (errno == ENOMEM) {
            LOG(LOG_NOTICE, "buffer_open: out of memory for device %u size=%llu",
                (unsigned int)req_body->dev_number, (unsigned long long)req_body->size);
            return VRTD_RET_BUSY;
        }

        LOG(LOG_ERR, "Failed to create buffer for buffer open request: %m");
        return VRTD_RET_INTERNAL_ERROR;
    }

    if (buf->qpair_count == 0 || buf->qpair_count > VRTD_BUFFER_MAX_QPAIR_FDS ||
        buf->fd < 0) {
        LOG(LOG_ERR, "Buffer created without valid qpair fd");
        return VRTD_RET_INTERNAL_ERROR;
    }

    uint64_t real_size = buf->size;
    uint64_t phys_addr = buf->addr;
    uint32_t qpair_count = buf->qpair_count;
    int fd = buf->fd;

    /*
     * Transfer ownership of the buffer into the device's buffer list.
     * buffer_ptr_array_push_move() nullifies our local pointer so that the
     * _cleanup_ destructor becomes a no-op.
     */
    if (buffer_ptr_array_push_move(&d->buffers, &buf) != 0) {
        LOG(LOG_ERR, "Failed to add buffer to device buffer list");
        return VRTD_RET_INTERNAL_ERROR;
    }

    resp_body->size = real_size;
    resp_body->phys_addr = phys_addr;
    resp_body->qpair_count = qpair_count;
    /* A single transfer fd owns all qpairs; the client selects channels by
     * qpair_index per sub-transfer. */
    client->out_fds[0] = fd;
    client->out_fd_count = 1;
    *out_fd = fd;
    *have_out_fd = true;
    *resp_size = sizeof(*resp_body);

    LOG(LOG_INFO, "Buffer opened size=%llu phys_addr=0x%llx dev=%u uid=%u conn_id=%llu",
        (unsigned long long)real_size, (unsigned long long)phys_addr,
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- BUFFER_OPEN_RAW ---------------------------------------------------- */

/**
 * Handles VRTD_REQ_BUFFER_OPEN_RAW -- creates a QDMA qpair at a caller-specified
 * device address, bypassing the allocator entirely.
 *
 * The caller is responsible for ensuring the address is valid and not in use.
 * Requires the raw-mem-access permission.  The qpair fd is returned to the client
 * via SCM_RIGHTS.  The buffer is tracked in the device's buffer list so the qpair
 * is torn down automatically if the client disconnects.
 *
 * Auth: auth_request_buffer_open_raw.
 * FD passing: outbound -- the qpair fd is sent via SCM_RIGHTS.
 *
 * Wire format:
 *   Request body:  vrtd_req_buffer_open_raw { uint32_t dev_number,
 *                                             uint32_t alloc_dir,
 *                                             uint64_t phys_addr,
 *                                             uint64_t size }
 *   Response body: vrtd_resp_buffer_open_raw { uint8_t zero }
 *                  + SCM_RIGHTS fd
 *
 * @return VRTD_RET_OK on success, error code otherwise.
 */
static uint16_t client_handle_request_buffer_open_raw(
    struct client *client,
    const struct vrtd_req_buffer_open_raw *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_open_raw *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
)
{
    int ret = auth_request_buffer_open_raw(client, req_body);
    if (ret == -1) {
        char pwbuf[1024];
        LOG(LOG_WARNING, "Failed to authorize raw buffer open request for uid %u(%s): %m",
            (unsigned int) client->uid, uid_to_username(client->uid, pwbuf, sizeof(pwbuf)));
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;
    *have_out_fd = false;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "Received malformed raw buffer open request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_WARNING, "Received raw buffer open request for non-existent device");
        return VRTD_RET_NOEXIST;
    }

    if (req_body->size == 0) {
        LOG(LOG_WARNING, "Received raw buffer open request with zero size");
        return VRTD_RET_INVALID_ARGUMENT;
    }

    if (req_body->mm_channel > SLASH_QDMA_MM_CHANNEL_1) {
        LOG(LOG_WARNING, "Received raw buffer open request with invalid mm_channel %u",
            (unsigned int)req_body->mm_channel);
        return VRTD_RET_INVALID_ARGUMENT;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->qdma == NULL) {
        LOG(LOG_WARNING, "Received raw buffer open request for non-existent or non-functional device");
        return VRTD_RET_NOEXIST;
    }

    uint64_t client_id = client->conn_id;
    if (client_id == 0) {
        LOG(LOG_ERR, "Invalid client connection id");
        return VRTD_RET_INTERNAL_ERROR;
    }

    _cleanup_(cleanup_bufferp)
    struct buffer *buf = buffer_create_raw(
        d->qdma,
        req_body->phys_addr,
        req_body->size,
        (enum vrtd_alloc_dir) req_body->alloc_dir,
        client_id,
        req_body->mm_channel
    );
    if (buf == NULL) {
        if (errno == EINVAL) {
            LOG(LOG_WARNING, "buffer_open_raw: invalid arguments for device %u", (unsigned int)req_body->dev_number);
            return VRTD_RET_INVALID_ARGUMENT;
        }
        LOG(LOG_ERR, "Failed to create raw buffer: %m");
        return VRTD_RET_INTERNAL_ERROR;
    }

    if (buf->qpair_count == 0 || buf->qpair_count > VRTD_BUFFER_MAX_QPAIR_FDS ||
        buf->fd < 0) {
        LOG(LOG_ERR, "Raw buffer created without valid qpair fd");
        return VRTD_RET_INTERNAL_ERROR;
    }

    uint32_t qpair_count = buf->qpair_count;
    int fd = buf->fd;

    if (buffer_ptr_array_push_move(&d->buffers, &buf) != 0) {
        LOG(LOG_ERR, "Failed to add raw buffer to device buffer list");
        return VRTD_RET_INTERNAL_ERROR;
    }

    resp_body->qpair_count = qpair_count;
    client->out_fds[0] = fd;
    client->out_fd_count = 1;
    *out_fd = fd;
    *have_out_fd = true;
    *resp_size = sizeof(*resp_body);

    LOG(LOG_WARNING, "Raw buffer opened phys_addr=0x%llx size=%llu dev=%u uid=%u conn_id=%llu",
        (unsigned long long)req_body->phys_addr, (unsigned long long)req_body->size,
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- BUFFER_CLOSE ------------------------------------------------------- */

/**
 * Handles VRTD_REQ_BUFFER_CLOSE -- releases a previously opened DMA buffer.
 *
 * Looks up the buffer by its physical address on the specified device,
 * verifies that the requesting client is the owner (by conn_id), checks
 * that the size matches, and then removes and frees the buffer.
 *
 * Auth: auth_request_buffer_close, plus ownership check (conn_id must match).
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_buffer_close { uint32_t dev_number,
 *                                          uint64_t phys_addr,
 *                                          uint64_t size }
 *   Response body: vrtd_resp_buffer_close { uint8_t zero }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_AUTH_ERROR if the client does not
 *         own the buffer, VRTD_RET_NOEXIST if no buffer was found at that
 *         address.
 */
static uint16_t client_handle_request_buffer_close(
    struct client *client,
    const struct vrtd_req_buffer_close *req_body,
    uint16_t req_size,
    struct vrtd_resp_buffer_close *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_buffer_close(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "buffer_close: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "buffer_close: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    if (req_body->size == 0) {
        LOG(LOG_WARNING, "buffer_close: zero size");
        return VRTD_RET_INVALID_ARGUMENT;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL) {
        LOG(LOG_NOTICE, "buffer_close: device %u is null", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    /*
     * Search for the caller's buffer by physical address.  Raw buffers bypass
     * the allocator and use caller-specified addresses, so distinct clients can
     * hold buffers at the same address; scan all matches and pick the one owned
     * by this connection rather than rejecting on the first address match.
     */
    struct buffer *found = NULL;
    bool addr_size_match_foreign = false; /* same addr+size, owned by another conn */
    bool addr_match_size_mismatch = false; /* same addr, different size */
    for (size_t i = 0; i < d->buffers.len; ++i) {
        struct buffer *buf = d->buffers.d[i];
        if (buf == NULL) {
            continue;
        }
        if (buf->addr != req_body->phys_addr) {
            continue;
        }
        if (buf->size != req_body->size) {
            addr_match_size_mismatch = true;
            continue;
        }
        if (buf->client_id != client->conn_id) {
            addr_size_match_foreign = true;
            continue;
        }
        found = buf;
        break;
    }

    if (found == NULL) {
        if (addr_size_match_foreign) {
            char pwbuf[1024];
            LOG(
                LOG_WARNING,
                "Permission denied for uid %u(%s): 'buffer_close' requires buffer ownership",
                (unsigned int) client->uid,
                uid_to_username(client->uid, pwbuf, sizeof(pwbuf))
            );
            return VRTD_RET_AUTH_ERROR;
        }
        if (addr_match_size_mismatch) {
            LOG(LOG_WARNING, "buffer_close: size mismatch at addr=0x%llx (got %llu)",
                (unsigned long long)req_body->phys_addr,
                (unsigned long long)req_body->size);
            return VRTD_RET_INVALID_ARGUMENT;
        }
        LOG(LOG_NOTICE, "buffer_close: no buffer at addr=0x%llx on device %u",
            (unsigned long long)req_body->phys_addr, (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    LOG(LOG_INFO, "Buffer closed addr=0x%llx size=%llu dev=%u uid=%u conn_id=%llu",
        (unsigned long long)found->addr, (unsigned long long)found->size,
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    /* Remove and free the buffer (owning array destructor handles cleanup). */
    buffer_ptr_array_rm_by_reference(&d->buffers, found);

    resp_body->zero = 0;
    *resp_size = sizeof(*resp_body);
    return VRTD_RET_OK;
}

/* ---- CLOCK_OP ----------------------------------------------------------- */

/**
 * Handles VRTD_REQ_CLOCK_OP -- gets or sets a clock rate for a device region.
 *
 * Supports two clock regions (service and user) and two operations (get and
 * set).  For SET operations, the requested rate must be non-zero.  The
 * response always contains the current (or achieved) rate.
 *
 * Auth: auth_request_clock_op.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_clock_op { uint32_t dev_number,
 *                                      uint32_t rate_hz,
 *                                      uint8_t op, uint8_t region }
 *   Response body: vrtd_resp_clock_op { uint32_t rate_hz }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_INVALID_ARGUMENT for bad op/region.
 */
static uint16_t client_handle_request_clock_op(
    struct client *client,
    const struct vrtd_req_clock_op *req_body,
    uint16_t req_size,
    struct vrtd_resp_clock_op *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_clock_op(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "clock_op: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "clock_op: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];
    if (d == NULL || d->clock_driver == NULL) {
        LOG(LOG_NOTICE, "clock_op: device %u has no clock driver", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    uint32_t rate = req_body->rate_hz;

    switch (req_body->region) {
    case VRTD_CLOCK_REGION_SERVICE:
        if (req_body->op == VRTD_CLOCK_OP_GET) {
            if (clock_driver_get_service_region_rate_hz(d->clock_driver, &rate) != 0) {
                LOG(LOG_WARNING, "clock_op: failed to get service region rate for device %u: %m",
                    (unsigned int)req_body->dev_number);
                return VRTD_RET_INTERNAL_ERROR;
            }
        } else if (req_body->op == VRTD_CLOCK_OP_SET) {
            if (rate == 0) {
                LOG(
                    LOG_WARNING,
                    "Received set frequency request with zero rate for service region"
                );
                return VRTD_RET_INVALID_ARGUMENT;
            }
            if (clock_driver_set_service_region_rate_hz(d->clock_driver, &rate) != 0) {
                LOG(
                    LOG_ERR,
                    "Failed to set service region frequency to %u Hz: %m",
                    req_body->rate_hz
                );
                return VRTD_RET_INTERNAL_ERROR;
            }
        } else {
            LOG(
                LOG_WARNING,
                "Received invalid clock op %u for service region",
                (unsigned int)req_body->op
            );
            return VRTD_RET_INVALID_ARGUMENT;
        }
        break;
    case VRTD_CLOCK_REGION_USER:
        if (req_body->op == VRTD_CLOCK_OP_GET) {
            if (clock_driver_get_user_region_rate_hz(d->clock_driver, &rate) != 0) {
                LOG(LOG_WARNING, "clock_op: failed to get user region rate for device %u: %m",
                    (unsigned int)req_body->dev_number);
                return VRTD_RET_INTERNAL_ERROR;
            }
        } else if (req_body->op == VRTD_CLOCK_OP_SET) {
            if (rate == 0) {
                LOG(
                    LOG_WARNING,
                    "Received set frequency request with zero rate for user region"
                );
                return VRTD_RET_INVALID_ARGUMENT;
            }
            if (clock_driver_set_user_region_rate_hz(d->clock_driver, &rate) != 0) {
                LOG(
                    LOG_ERR,
                    "Failed to set user region frequency to %u Hz: %m",
                    req_body->rate_hz
                );
                return VRTD_RET_INTERNAL_ERROR;
            }
        } else {
            LOG(
                LOG_WARNING,
                "Received invalid clock op %u for user region",
                (unsigned int)req_body->op
            );
            return VRTD_RET_INVALID_ARGUMENT;
        }
        break;
    default:
        LOG(
            LOG_WARNING,
            "Received clock request with invalid region %u",
            (unsigned int)req_body->region
        );
        return VRTD_RET_INVALID_ARGUMENT;
    }

    resp_body->rate_hz = rate;
    *resp_size = sizeof(*resp_body);

    LOG(LOG_INFO, "clock_op: op=%u region=%u rate_hz=%u dev=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->op, (unsigned int)req_body->region, rate,
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- GET_DEVICE_INFO ---------------------------------------------------- */

/**
 * Handles VRTD_REQ_GET_DEVICE_INFO -- returns name and PCI metadata for a
 * device.
 *
 * Populates a vrtd_device_info structure containing:
 *   - name: the basename of the device's /dev path (e.g. "slash_ctl0").
 *   - pci:  BDF string, vendor/device/subsystem IDs.
 *
 * Auth: auth_request_get_device_info.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_device_info { uint32_t dev_number }
 *   Response body: vrtd_resp_get_device_info { vrtd_device_info info }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_NOEXIST if dev_number is invalid.
 */
static uint16_t client_handle_request_get_device_info(
    struct client *client,
    const struct vrtd_req_get_device_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_device_info *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_get_device_info(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_device_info: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "get_device_info: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];

    /*
     * basename() may modify its argument, so we duplicate the path first.
     * The _cleanup_ attribute ensures the copy is freed on all return paths.
     */
    _cleanup_(cleanup_free)
    char *path = strdup(d->path);
    if (unlikely(path == NULL)) {
        LOG(LOG_WARNING, "get_device_info: allocation failure");
        return VRTD_RET_INTERNAL_ERROR;
    }

    memset(resp_body, 0, sizeof(*resp_body));
    snprintf(resp_body->info.name, sizeof(resp_body->info.name), "%s", basename(path));
    memcpy(&resp_body->info.pci, &d->pci_info, sizeof(struct vrtd_pci_info));
    resp_body->info.shell_type = (uint8_t)d->current_shell;
    resp_body->info.jtag = d->jtag ? 1 : 0;

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "get_device_info: dev=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- GET_DEVICE_BY_BDF -------------------------------------------------- */

/**
 * Handles VRTD_REQ_GET_DEVICE_BY_BDF -- looks up a device index by PCI BDF
 * string.
 *
 * Iterates over all known devices and compares their BDF against the
 * client-provided string.  Returns the 0-based device index on match.
 *
 * Auth: auth_request_get_device_by_bdf.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_device_by_bdf { char bdf[32] }
 *   Response body: vrtd_resp_get_device_by_bdf { uint32_t dev_number }
 *
 * @return VRTD_RET_OK on match, VRTD_RET_NOEXIST if no device has the BDF.
 */
static uint16_t client_handle_request_get_device_by_bdf(
    struct client *client,
    const struct vrtd_req_get_device_by_bdf *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_device_by_bdf *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_get_device_by_bdf(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_device_by_bdf: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    /* Defensively NUL-terminate the BDF string to prevent overreads. */
    char bdf[VRTD_PCI_BDF_LEN];
    memcpy(bdf, req_body->bdf, sizeof(bdf));
    bdf[sizeof(bdf) - 1] = '\0';

    if (bdf[0] == '\0') {
        LOG(LOG_WARNING, "get_device_by_bdf: empty BDF string");
        return VRTD_RET_INVALID_ARGUMENT;
    }

    /* Normalize to board-level BDF (DDDD:BB:DD) for matching.
     * Strip any function suffix (.F) since devices are stored board-level.
     * Prepend domain 0000: if only one colon is present (short BDF). */
    {
        char *dot = strrchr(bdf, '.');
        if (dot != NULL) {
            LOG(LOG_WARNING,
                "get_device_by_bdf: client sent PF-level BDF '%s'; "
                "stripping function %s — use board address instead",
                req_body->bdf, dot);
            *dot = '\0';
        }

        /* Count colons to detect short BDF (BB:DD vs DDDD:BB:DD). */
        int colons = 0;
        for (const char *p = bdf; *p != '\0'; ++p) {
            if (*p == ':') colons++;
        }
        if (colons == 1) {
            /* Short BDF — prepend default domain. */
            char tmp[VRTD_PCI_BDF_LEN];
            int n = snprintf(tmp, sizeof(tmp), "0000:%s", bdf);
            if (n > 0 && (size_t)n < sizeof(tmp)) {
                memcpy(bdf, tmp, (size_t)n + 1);
            }
        }
    }

    /* Linear scan; the device count is small (single digits). */
    for (size_t i = 0; i < client->state->devices.len; ++i) {
        struct device *d = client->state->devices.d[i];

        if (strcmp(d->pci_info.bdf, bdf) == 0) {
            resp_body->dev_number = (uint32_t) i;
            *resp_size = sizeof(*resp_body);
            LOG(LOG_DEBUG, "get_device_by_bdf: bdf=%s -> dev=%u uid=%u conn_id=%llu",
                bdf, (unsigned int)i,
                (unsigned int)client->uid, (unsigned long long)client->conn_id);
            return VRTD_RET_OK;
        }
    }

    LOG(LOG_NOTICE, "get_device_by_bdf: no device found for bdf=%s", bdf);
    return VRTD_RET_NOEXIST;
}

/* ---- GET_BAR_INFO ------------------------------------------------------- */

/**
 * Handles VRTD_REQ_GET_BAR_INFO -- returns metadata about a device BAR.
 *
 * Returns the slash_ioctl_bar_info structure for the specified BAR, which
 * contains the BAR's size and resource type.  PCI devices have at most 6
 * BARs (indices 0-5).
 *
 * Auth: auth_request_get_bar_info.
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_bar_info { uint32_t dev_number,
 *                                          uint8_t bar_number }
 *   Response body: vrtd_resp_get_bar_info { slash_ioctl_bar_info bar_info }
 *
 * @return VRTD_RET_OK on success, VRTD_RET_NOEXIST if the BAR is not present.
 */
static uint16_t client_handle_request_get_bar_info(
    struct client *client,
    const struct vrtd_req_get_bar_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_bar_info *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_get_bar_info(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_bar_info: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "get_bar_info: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    if (req_body->bar_number >= 6) {
        LOG(LOG_WARNING, "get_bar_info: invalid BAR number %u", (unsigned int)req_body->bar_number);
        return VRTD_RET_BAD_REQUEST;
    }

    // TODO: Free this
    struct slash_ioctl_bar_info *bar_info = client->state->devices.d[req_body->dev_number]->bar_info[req_body->bar_number];
    if (bar_info == NULL) {
        LOG(LOG_NOTICE, "get_bar_info: BAR %u not available on device %u",
            (unsigned int)req_body->bar_number, (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    resp_body->bar_info = *bar_info;

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "get_bar_info: dev=%u bar=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)req_body->bar_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- GET_BAR_FD --------------------------------------------------------- */

/**
 * Handles VRTD_REQ_GET_BAR_FD -- returns a mmap-able fd for a device BAR.
 *
 * The returned file descriptor can be mmap'd by the client to obtain direct
 * userspace access to the BAR's MMIO region.  The fd and the BAR's length
 * are delivered together: the length in the response body and the fd via
 * SCM_RIGHTS ancillary data.
 *
 * Auth: auth_request_get_bar_fd.
 * FD passing: outbound -- the BAR fd is sent via SCM_RIGHTS.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_bar_fd { uint32_t dev_number,
 *                                        uint8_t bar_number }
 *   Response body: vrtd_resp_get_bar_fd { uint64_t len }
 *                  + SCM_RIGHTS fd
 *
 * @return VRTD_RET_OK on success, VRTD_RET_NOEXIST if the BAR is not present.
 */
static uint16_t client_handle_request_get_bar_fd(
    struct client *client,
    const struct vrtd_req_get_bar_fd *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_bar_fd *resp_body,
    uint16_t *resp_size,
    int *out_fd,
    bool *have_out_fd
)
{
    int ret = auth_request_get_bar_fd(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;
    *have_out_fd = false;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_bar_fd: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "get_bar_fd: device %u does not exist", (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    if (req_body->bar_number >= 6) {
        LOG(LOG_WARNING, "get_bar_fd: invalid BAR number %u", (unsigned int)req_body->bar_number);
        return VRTD_RET_BAD_REQUEST;
    }

    struct slash_bar_file *bar_file = client->state->devices.d[req_body->dev_number]->bar_files[req_body->bar_number];
    if (bar_file == NULL) {
        LOG(LOG_NOTICE, "get_bar_fd: BAR %u not available on device %u",
            (unsigned int)req_body->bar_number, (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    resp_body->len = bar_file->len;

    /* Schedule this fd for delivery via SCM_RIGHTS in client_handle_out(). */
    *out_fd = bar_file->fd;
    *have_out_fd = true;

    *resp_size = sizeof(*resp_body);

    LOG(LOG_DEBUG, "get_bar_fd: dev=%u bar=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, (unsigned int)req_body->bar_number,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}

/* ---- GET_SENSOR_INFO ---------------------------------------------------- */

/**
 * Helper: read one sensor type's value and unit modifier, populate an entry.
 *
 * @return true if the entry was populated, false on error (entry is skipped).
 */
static bool sensor_read_type(
    ami_device *ami_dev,
    const char *sensor_name,
    enum ami_sensor_type type,
    struct vrtd_sensor_entry *entry
)
{
    long value = 0;
    enum ami_sensor_status status = AMI_SENSOR_STATUS_INVALID;
    enum ami_sensor_unit_mod mod = AMI_SENSOR_UNIT_MOD_NONE;
    int ret;

    switch (type) {
    case AMI_SENSOR_TYPE_TEMP:
        ret = ami_sensor_get_temp_value(ami_dev, sensor_name, &value, &status);
        if (ret != AMI_STATUS_OK) return false;
        ami_sensor_get_temp_unit_mod(ami_dev, sensor_name, &mod);
        break;
    case AMI_SENSOR_TYPE_CURRENT:
        ret = ami_sensor_get_current_value(ami_dev, sensor_name, &value, &status);
        if (ret != AMI_STATUS_OK) return false;
        ami_sensor_get_current_unit_mod(ami_dev, sensor_name, &mod);
        break;
    case AMI_SENSOR_TYPE_VOLTAGE:
        ret = ami_sensor_get_voltage_value(ami_dev, sensor_name, &value, &status);
        if (ret != AMI_STATUS_OK) return false;
        ami_sensor_get_voltage_unit_mod(ami_dev, sensor_name, &mod);
        break;
    case AMI_SENSOR_TYPE_POWER:
        ret = ami_sensor_get_power_value(ami_dev, sensor_name, &value, &status);
        if (ret != AMI_STATUS_OK) return false;
        ami_sensor_get_power_unit_mod(ami_dev, sensor_name, &mod);
        break;
    default:
        return false;
    }

    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", sensor_name);
    entry->type = (uint8_t)type;
    entry->status = (uint8_t)status;
    entry->unit_mod = (int8_t)mod;
    entry->value = (int32_t)value;

    return true;
}

/**
 * Handles VRTD_REQ_GET_SENSOR_INFO -- queries all sensors for a device via
 * the AMI (Alveo Management Interface) library and returns their current
 * values and statuses.
 *
 * The AMI device handle is opened on-demand for PF0 (the AVED management
 * function), sensors are discovered and read, then the handle is closed.
 *
 * Auth: auth_request_get_sensor_info (query-only).
 * FD passing: none.
 *
 * Wire format:
 *   Request body:  vrtd_req_get_sensor_info { uint32_t dev_number }
 *   Response body: vrtd_resp_get_sensor_info { uint32_t num_sensors,
 *                    vrtd_sensor_entry sensors[] }
 *
 * @return VRTD_RET_OK on success, or an appropriate error code.
 */
static uint16_t client_handle_request_get_sensor_info(
    struct client *client,
    const struct vrtd_req_get_sensor_info *req_body,
    uint16_t req_size,
    struct vrtd_resp_get_sensor_info *resp_body,
    uint16_t *resp_size
)
{
    int ret = auth_request_get_sensor_info(client, req_body);
    if (ret == -1) {
        return VRTD_RET_INTERNAL_ERROR;
    } else if (ret == 0) {
        return VRTD_RET_AUTH_ERROR;
    }

    *resp_size = 0;

    if (req_size < sizeof(*req_body)) {
        LOG(LOG_WARNING, "get_sensor_info: malformed request");
        return VRTD_RET_BAD_REQUEST;
    }

    if (req_body->dev_number >= client->state->devices.len) {
        LOG(LOG_NOTICE, "get_sensor_info: device %u does not exist",
            (unsigned int)req_body->dev_number);
        return VRTD_RET_NOEXIST;
    }

    struct device *d = client->state->devices.d[req_body->dev_number];

    /* Compute PF0 BDF for AMI (AMI runs on PF0, the AVED function). */
    char pf0_bdf[VRTD_PCI_BDF_LEN] = {0};
    if (pci_bdf_set_function(d->pci_info.bdf, 0, pf0_bdf) != 0) {
        LOG(LOG_ERR, "get_sensor_info: failed to compute PF0 BDF from %s",
            d->pci_info.bdf);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /* Open AMI device handle on PF0. */
    ami_device *ami_dev = NULL;
    ret = ami_dev_find(pf0_bdf, &ami_dev);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "get_sensor_info: ami_dev_find(%s) failed: %s",
            pf0_bdf, ami_get_last_error());
        return VRTD_RET_INTERNAL_ERROR;
    }

    /* Discover sensors on the device. */
    ret = ami_sensor_discover(ami_dev);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "get_sensor_info: ami_sensor_discover(%s) failed: %s",
            pf0_bdf, ami_get_last_error());
        ami_dev_delete(&ami_dev);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /* Get the list of sensors (grouped by name). */
    struct ami_sensor *sensors = NULL;
    int num_sensors = 0;
    ret = ami_sensor_get_sensors(ami_dev, &sensors, &num_sensors);
    if (ret != AMI_STATUS_OK) {
        LOG(LOG_ERR, "get_sensor_info: ami_sensor_get_sensors(%s) failed: %s",
            pf0_bdf, ami_get_last_error());
        ami_dev_delete(&ami_dev);
        return VRTD_RET_INTERNAL_ERROR;
    }

    /*
     * Iterate over each sensor name and each sensor type (temp, current,
     * voltage, power).  For each combination that exists, read the value
     * and populate an entry in the response.
     */
    static const enum ami_sensor_type sensor_types[] = {
        AMI_SENSOR_TYPE_TEMP,
        AMI_SENSOR_TYPE_CURRENT,
        AMI_SENSOR_TYPE_VOLTAGE,
        AMI_SENSOR_TYPE_POWER,
    };

    uint32_t count = 0;

    for (struct ami_sensor *s = sensors; s != NULL; s = s->next) {
        /* Check which types this sensor supports. */
        uint32_t type_mask = 0;
        if (ami_sensor_get_type(ami_dev, s->name, &type_mask) != AMI_STATUS_OK) {
            continue;
        }

        for (size_t t = 0; t < sizeof(sensor_types) / sizeof(sensor_types[0]); t++) {
            if (!(type_mask & sensor_types[t])) {
                continue;
            }

            if (count >= VRTD_SENSOR_MAX_ENTRIES) {
                LOG(LOG_WARNING, "get_sensor_info: sensor count exceeds message limit, "
                    "truncating at %u entries", count);
                goto done;
            }

            if (sensor_read_type(ami_dev, s->name, sensor_types[t],
                                 &resp_body->sensors[count])) {
                count++;
            }
        }
    }

done:
    ami_dev_delete(&ami_dev);

    resp_body->num_sensors = count;
    *resp_size = (uint16_t)(sizeof(resp_body->num_sensors)
                            + count * sizeof(struct vrtd_sensor_entry));

    LOG(LOG_DEBUG, "get_sensor_info: dev=%u sensors=%u uid=%u conn_id=%llu",
        (unsigned int)req_body->dev_number, count,
        (unsigned int)client->uid, (unsigned long long)client->conn_id);

    return VRTD_RET_OK;
}
