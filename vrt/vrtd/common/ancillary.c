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

#include "ancillary.h"
#include <vrtd/wire.h>

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

static void close_fds(int *fds, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (fds[i] >= 0) {
            (void)close(fds[i]);
            fds[i] = -1;
        }
    }
}

enum vrtd_ancillary_result vrtd_ancillary_extract(
    struct msghdr *msg, int *fds, size_t expected_fds)
{
    int received[VRTD_ANCILLARY_RECV_FDS];
    struct cmsghdr *cmsg;
    size_t count = 0;
    size_t i;
    bool invalid = false;

    if (msg == NULL || expected_fds > VRTD_ANCILLARY_MAX_FDS ||
        (expected_fds > 0 && fds == NULL)) {
        errno = EINVAL;
        return VRTD_ANCILLARY_MALFORMED;
    }
    for (i = 0; i < VRTD_ANCILLARY_RECV_FDS; i++)
        received[i] = -1;
    for (i = 0; i < expected_fds; i++)
        fds[i] = -1;

    for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(msg, cmsg)) {
        size_t data_len;
        size_t fd_count;
        size_t offset;
        int *cmsg_fds;

        offset = (size_t)((char *)cmsg - (char *)msg->msg_control);
        if (cmsg->cmsg_len < CMSG_LEN(0) || offset > msg->msg_controllen ||
            cmsg->cmsg_len > msg->msg_controllen - offset) {
            invalid = true;
            break;
        }
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS) {
            invalid = true;
            continue;
        }
        data_len = cmsg->cmsg_len - CMSG_LEN(0);
        if (data_len == 0 || data_len % sizeof(int) != 0) {
            invalid = true;
            continue;
        }
        fd_count = data_len / sizeof(int);
        cmsg_fds = (int *)CMSG_DATA(cmsg);
        for (i = 0; i < fd_count; i++) {
            if (count < VRTD_ANCILLARY_RECV_FDS)
                received[count++] = cmsg_fds[i];
            else
                (void)close(cmsg_fds[i]);
        }
    }

    if (msg->msg_flags & (MSG_TRUNC | MSG_CTRUNC))
        invalid = true;
    if (invalid) {
        close_fds(received, count);
        errno = EBADMSG;
        return VRTD_ANCILLARY_MALFORMED;
    }
    if (count != expected_fds) {
        close_fds(received, count);
        errno = EMSGSIZE;
        return VRTD_ANCILLARY_BAD_COUNT;
    }
    for (i = 0; i < count; i++) {
        fds[i] = received[i];
        received[i] = -1;
    }
    return VRTD_ANCILLARY_OK;
}

size_t vrtd_request_expected_fds(uint16_t opcode)
{
    switch (opcode) {
    case VRTD_REQ_DESIGN_WRITE:
    case VRTD_REQ_CFGMEM_PROGRAM:
    case VRTD_REQ_CFGMEM_PROGRAM_START:
        return 1;
    default:
        return 0;
    }
}

size_t vrtd_response_expected_fds(uint16_t opcode, bool *owned)
{
    size_t count = 0;
    bool sender_owned = false;

    switch (opcode) {
    case VRTD_REQ_QDMA_QPAIR_GET_FD:
        /* a fresh descriptor from the kernel, so the sender holds the only
         * other copy and has to close it */
        count = 1;
        sender_owned = true;
        break;
    case VRTD_REQ_GET_BAR_FD:
    case VRTD_REQ_BUFFER_OPEN:
    case VRTD_REQ_BUFFER_OPEN_RAW:
        /* borrowed from the BAR file or buffer that keeps it open, so closing
         * it would break the owner */
        count = 1;
        break;
    default:
        break;
    }

    if (owned != NULL)
        *owned = sender_owned;
    return count;
}

bool vrtd_response_expected_size(uint16_t opcode, size_t *expected_size)
{
    if (expected_size == NULL)
        return false;

    switch (opcode) {
    case VRTD_REQ_GET_NUM_DEVICES:
        *expected_size = sizeof(struct vrtd_resp_get_num_devices);
        break;
    case VRTD_REQ_GET_DEVICE_INFO:
        *expected_size = sizeof(struct vrtd_resp_get_device_info);
        break;
    case VRTD_REQ_GET_DEVICE_BY_BDF:
        *expected_size = sizeof(struct vrtd_resp_get_device_by_bdf);
        break;
    case VRTD_REQ_GET_BAR_INFO:
        *expected_size = sizeof(struct vrtd_resp_get_bar_info);
        break;
    case VRTD_REQ_GET_BAR_FD:
        *expected_size = sizeof(struct vrtd_resp_get_bar_fd);
        break;
    case VRTD_REQ_QDMA_GET_INFO:
        *expected_size = sizeof(struct vrtd_resp_qdma_get_info);
        break;
    case VRTD_REQ_QDMA_QPAIR_ADD:
        *expected_size = sizeof(struct vrtd_resp_qdma_qpair_add);
        break;
    case VRTD_REQ_QDMA_QPAIR_OP:
        *expected_size = sizeof(struct vrtd_resp_qdma_qpair_op);
        break;
    case VRTD_REQ_QDMA_QPAIR_GET_FD:
        *expected_size = sizeof(struct vrtd_resp_qdma_qpair_get_fd);
        break;
    case VRTD_REQ_BUFFER_OPEN:
        *expected_size = sizeof(struct vrtd_resp_buffer_open);
        break;
    case VRTD_REQ_BUFFER_CLOSE:
        *expected_size = sizeof(struct vrtd_resp_buffer_close);
        break;
    case VRTD_REQ_BUFFER_OPEN_RAW:
        *expected_size = sizeof(struct vrtd_resp_buffer_open_raw);
        break;
    case VRTD_REQ_DESIGN_WRITE:
        *expected_size = sizeof(struct vrtd_resp_design_write);
        break;
    case VRTD_REQ_CFGMEM_PROGRAM:
        *expected_size = sizeof(struct vrtd_resp_cfgmem_program);
        break;
    case VRTD_REQ_CFGMEM_PROGRAM_START:
        *expected_size = sizeof(struct vrtd_resp_cfgmem_program_start);
        break;
    case VRTD_REQ_CFGMEM_PROGRAM_STATUS:
        *expected_size = sizeof(struct vrtd_resp_cfgmem_program_status);
        break;
    case VRTD_REQ_DEVICE_HOTPLUG_OP:
        *expected_size = sizeof(struct vrtd_resp_device_hotplug_op);
        break;
    case VRTD_REQ_CLOCK_OP:
        *expected_size = sizeof(struct vrtd_resp_clock_op);
        break;
    case VRTD_REQ_GET_SENSOR_INFO:
        *expected_size = SIZE_MAX;
        break;
    case VRTD_REQ_SET_SHELL_STATE:
        *expected_size = sizeof(struct vrtd_resp_set_shell_state);
        break;
    default:
        return false;
    }

    return true;
}
