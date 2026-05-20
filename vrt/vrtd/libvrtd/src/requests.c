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
 * @file requests.c
 *
 * Wire protocol request/response marshalling for the vrtd C client library.
 *
 * Each public vrtd_*() function builds a wire protocol message (header +
 * body), sends it to the daemon over the AF_UNIX SOCK_SEQPACKET socket,
 * and receives the response.  File descriptors (BAR fds, QDMA qpair fds)
 * are passed out-of-band via SCM_RIGHTS ancillary data on the Unix socket.
 *
 * The protocol is strictly request-response: one sendmsg() followed by
 * one recvmsg().  Sequence numbers are included for future pipelining
 * but currently always set to 1.
 *
 * All functions are synchronous and thread-safe only if each thread uses
 * its own connection fd (obtained from vrtd_connect()).
 */

#define _GNU_SOURCE

#include <slash/uapi/slash_interface.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <vrtd/vrtd.h>

/**
 * vrtd_recv_response() - Receive a response message from the daemon.
 * @fd:            Connection socket.
 * @resp_body_buf: Buffer for the response body (may be NULL if no body expected).
 * @resp_bufsz:    Size of @resp_body_buf.
 * @resp_fd:       If non-NULL, receives an out-of-band file descriptor
 *                 sent by the daemon via SCM_RIGHTS (e.g. a BAR fd or
 *                 QDMA qpair fd).  Set to -1 if no fd was received.
 *
 * Uses recvmsg() with scatter-gather I/O: the header and body are read
 * into separate buffers in a single system call.  MSG_CMSG_CLOEXEC
 * ensures any received fd is close-on-exec.
 *
 * Return: VRTD_RET_OK on success, or an error code.
 */
static enum vrtd_ret vrtd_recv_response(
    int fd,
    void *resp_body_buf,
    size_t resp_bufsz,
    int *resp_fd
)
{
    struct vrtd_resp_header rh = {0};

    struct iovec riov[2];
    riov[0].iov_base = &rh;
    riov[0].iov_len  = sizeof(rh);
    riov[1].iov_base = resp_body_buf;
    riov[1].iov_len  = resp_bufsz;

    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr rmsg = {
        .msg_iov        = riov,
        .msg_iovlen     = resp_bufsz ? 2 : 1,
        .msg_control    = resp_fd ? cbuf : NULL,
        .msg_controllen = resp_fd ? sizeof(cbuf) : 0,
    };

    if (resp_fd) {
        *resp_fd = -1;
    }

    ssize_t rn = recvmsg(fd, &rmsg, MSG_CMSG_CLOEXEC);
    if (rn == -1) {
        return VRTD_RET_BAD_CONN;
    }

    if (rmsg.msg_flags & MSG_TRUNC) {
        return VRTD_RET_BAD_LIB_CALL;
    }
    if (rmsg.msg_flags & MSG_CTRUNC) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    if ((size_t)rn < sizeof(rh)) {
        return VRTD_RET_BAD_CONN;
    }

    size_t expect = sizeof(rh) + rh.size;
    if ((size_t) rn != expect) {
        return VRTD_RET_BAD_CONN;
    }

    /* Extract file descriptor from SCM_RIGHTS ancillary data, if any. */
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&rmsg); c != NULL; c = CMSG_NXTHDR(&rmsg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            assert(resp_fd != NULL);
            memcpy(resp_fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    }

    return (enum vrtd_ret) rh.ret;
}

int vrtd_connect(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        return -1;
    }

    struct sockaddr_un sun = {0};
    sun.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sun.sun_path)) {
        errno = ENAMETOOLONG;
        close(fd);
        return -1;
    }
    strcpy(sun.sun_path, path);

    int ret = connect(fd, (struct sockaddr *) &sun, sizeof(sun));
    if (ret == -1) {
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * vrtd_raw_request() - Send a request and receive the response.
 * @fd:            Connection socket (from vrtd_connect()).
 * @opcode:        Wire protocol opcode (VRTD_REQ_*).
 * @req_body:      Request body payload (may be NULL if @req_size is 0).
 * @req_size:      Size of @req_body in bytes.
 * @resp_body_buf: Buffer for the response body.
 * @resp_bufsz:    Size of @resp_body_buf.
 * @resp_fd:       If non-NULL, receives an out-of-band fd from the daemon.
 * @req_fd:        If non-NULL and *req_fd >= 0, sends this fd to the daemon
 *                 via SCM_RIGHTS (e.g. a bitstream fd for design_write).
 *
 * Builds a request message (header + body), optionally attaches an fd
 * via SCM_RIGHTS ancillary data, sends it with sendmsg(), then waits
 * for the response via vrtd_recv_response().
 *
 * Return: VRTD_RET_OK on success, or an error code.
 */
enum vrtd_ret vrtd_raw_request(
    int fd,
    uint16_t opcode,
    const void *req_body, uint16_t req_size,
    void *resp_body_buf, size_t resp_bufsz,
    int *resp_fd,
    const int *req_fd
)
{
    if (req_size > VRTD_MSG_MAX_SIZE - sizeof(struct vrtd_req_header)) { errno = EMSGSIZE; return -1; }

    /* ---- Send ---- */
    struct vrtd_req_header h = {
        .size  = req_size,
        .opcode= opcode,
        .seqno = 1,
    };

    struct iovec siov[2];
    siov[0].iov_base = &h;
    siov[0].iov_len  = sizeof(h);
    siov[1].iov_base = (void*) req_body;
    siov[1].iov_len  = req_size;

    char cbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr smsg = {
        .msg_iov        = siov,
        .msg_iovlen     = req_size ? 2 : 1,
        .msg_control    = NULL,
        .msg_controllen = 0,
    };

    if (req_fd && *req_fd >= 0) {
        smsg.msg_control = cbuf;
        smsg.msg_controllen = sizeof(cbuf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&smsg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), req_fd, sizeof(int));
    }

    ssize_t sn = sendmsg(fd, &smsg, MSG_NOSIGNAL);
    if (sn == -1) {
        return VRTD_RET_BAD_CONN;
    }
    if ((size_t) sn != sizeof(h) + req_size) {
        return VRTD_RET_BAD_CONN;
    }

    return vrtd_recv_response(fd, resp_body_buf, resp_bufsz, resp_fd);
}


enum vrtd_ret vrtd_get_num_devices(int fd, uint32_t *out)
{
    if (out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_resp_get_num_devices resp = {0};
    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_NUM_DEVICES,
                              NULL, 0,
                              &resp, sizeof(resp),
                              NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *out = resp.num_devices;

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_get_device_info(int fd, uint32_t dev, struct vrtd_device_info *info_out)
{
    if (info_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_get_device_info req = {
        .dev_number = dev,
    };
    struct vrtd_resp_get_device_info resp = {0};
    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_DEVICE_INFO,
                              &req, sizeof(req),
                              &resp, sizeof(resp),
                              NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    memcpy(info_out, &resp.info, sizeof(*info_out));

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_get_device_by_bdf(int fd, const char *bdf, uint32_t *dev_out)
{
    if (bdf == NULL || dev_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_get_device_by_bdf req = {0};
    strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
    req.bdf[sizeof(req.bdf) - 1] = '\0';

    struct vrtd_resp_get_device_by_bdf resp = {0};
    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_DEVICE_BY_BDF,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *dev_out = resp.dev_number;
    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_get_bar_info(int fd, uint32_t dev, uint8_t bar, struct slash_ioctl_bar_info *bar_info_out)
{
    if (bar_info_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_get_bar_info req = {
        .dev_number = dev,
        .bar_number = bar,
    };
    struct vrtd_resp_get_bar_info resp = {0};
    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_BAR_INFO,
                              &req, sizeof(req),
                              &resp, sizeof(resp),
                              NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    memcpy(bar_info_out, &resp.bar_info, sizeof(struct slash_ioctl_bar_info));

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_get_bar_fd(int fd, uint32_t dev, uint8_t bar, int *fd_out, uint64_t *len_out)
{
    if (fd_out == NULL || len_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_get_bar_fd req = {
        .dev_number = dev,
        .bar_number = bar,
    };
    struct vrtd_resp_get_bar_fd resp = {0};
    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_BAR_FD,
                              &req, sizeof(req),
                              &resp, sizeof(resp),
                              fd_out, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *len_out = resp.len;

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_qdma_get_info(int fd, uint32_t dev, struct slash_qdma_info *info_out)
{
    if (info_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_qdma_get_info req = {
        .dev_number = dev,
    };
    struct vrtd_resp_qdma_get_info resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_QDMA_GET_INFO,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    memcpy(info_out, &resp.info, sizeof(struct slash_qdma_info));

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_qdma_qpair_add(int fd, uint32_t dev, struct slash_qdma_qpair_add *qpair_inout)
{
    if (qpair_inout == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_qdma_qpair_add req = {
        .dev_number = dev,
        .add        = *qpair_inout,
    };
    struct vrtd_resp_qdma_qpair_add resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_QDMA_QPAIR_ADD,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *qpair_inout = resp.add;

    return VRTD_RET_OK;
}

static enum vrtd_ret vrtd_qdma_qpair_op(int fd, uint32_t dev, uint32_t qid, uint32_t op)
{
    struct vrtd_req_qdma_qpair_op req = {
        .dev_number = dev,
        .qid        = qid,
        .op         = op,
    };
    struct vrtd_resp_qdma_qpair_op resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_QDMA_QPAIR_OP,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_qdma_qpair_start(int fd, uint32_t dev, uint32_t qid)
{
    return vrtd_qdma_qpair_op(fd, dev, qid, SLASH_QDMA_QUEUE_OP_START);
}

enum vrtd_ret vrtd_qdma_qpair_stop(int fd, uint32_t dev, uint32_t qid)
{
    return vrtd_qdma_qpair_op(fd, dev, qid, SLASH_QDMA_QUEUE_OP_STOP);
}

enum vrtd_ret vrtd_qdma_qpair_del(int fd, uint32_t dev, uint32_t qid)
{
    return vrtd_qdma_qpair_op(fd, dev, qid, SLASH_QDMA_QUEUE_OP_DEL);
}

enum vrtd_ret vrtd_qdma_qpair_get_fd(
    int fd,
    uint32_t dev,
    uint32_t qid,
    uint32_t flags,
    int *fd_out
)
{
    if (fd_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_qdma_qpair_get_fd req = {
        .dev_number = dev,
        .qid        = qid,
        .flags      = flags,
    };
    struct vrtd_resp_qdma_qpair_get_fd resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_QDMA_QPAIR_GET_FD,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               fd_out, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_open(
    int fd,
    uint32_t dev,
    uint32_t alloc_type,
    uint32_t alloc_dir,
    uint64_t alloc_arg,
    uint64_t size_in,
    struct vrtd_buffer **buffer_out
)
{
    if (buffer_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }
    *buffer_out = NULL;

    struct vrtd_req_buffer_open req = {
        .dev_number = dev,
        .alloc_type = alloc_type,
        .alloc_dir = alloc_dir,
        .alloc_arg = alloc_arg,
        .size = size_in,
    };
    struct vrtd_resp_buffer_open resp = {0};

    int qpair_fd = -1;
    int ret = vrtd_raw_request(fd, VRTD_REQ_BUFFER_OPEN,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               &qpair_fd, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    if (qpair_fd < 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    ret = vrtd_buffer_create_raw(
        fd,
        dev,
        alloc_type,
        alloc_dir,
        alloc_arg,
        resp.size,
        resp.phys_addr,
        qpair_fd,
        buffer_out
    );
    if (ret != VRTD_RET_OK) {
        (void) close(qpair_fd);
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_buffer_open_raw(
    int fd,
    uint32_t dev,
    uint64_t phys_addr,
    uint64_t size,
    uint32_t alloc_dir,
    struct vrtd_buffer **buffer_out
)
{
    if (buffer_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }
    *buffer_out = NULL;

    struct vrtd_req_buffer_open_raw req = {
        .dev_number = dev,
        .alloc_dir = alloc_dir,
        .phys_addr = phys_addr,
        .size = size,
    };
    struct vrtd_resp_buffer_open_raw resp = {0};

    int qpair_fd = -1;
    int ret = vrtd_raw_request(fd, VRTD_REQ_BUFFER_OPEN_RAW,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               &qpair_fd, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    if (qpair_fd < 0) {
        return VRTD_RET_INTERNAL_ERROR;
    }

    ret = vrtd_buffer_create_raw(
        fd,
        dev,
        0,           /* alloc_type: not used for raw buffers */
        alloc_dir,
        0,           /* alloc_arg: not used for raw buffers */
        size,
        phys_addr,
        qpair_fd,
        buffer_out
    );
    if (ret != VRTD_RET_OK) {
        (void) close(qpair_fd);
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_design_write(
    int fd,
    uint32_t dev,
    int input_fd
)
{
    if (input_fd < 0) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_design_write req = {
        .dev_number = dev,
    };
    struct vrtd_resp_design_write resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_DESIGN_WRITE,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, &input_fd);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_design_write_file(
    int fd,
    uint32_t dev,
    const char *path
)
{
    if (path == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    int input_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (input_fd < 0) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    enum vrtd_ret ret = vrtd_design_write(fd, dev, input_fd);
    (void) close(input_fd);
    return ret;
}

enum vrtd_ret vrtd_device_hotplug_op(
    int fd,
    uint32_t dev,
    uint8_t op,
    uint8_t function
)
{
    struct vrtd_req_device_hotplug_op req = {
        .dev_number = dev,
        .op = op,
        .function = function,
    };
    struct vrtd_resp_device_hotplug_op resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_DEVICE_HOTPLUG_OP,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_device_hotplug_rescan(int fd, uint32_t dev)
{
    return vrtd_device_hotplug_op(fd, dev, VRTD_DEVICE_HOTPLUG_OP_RESCAN, 0);
}

enum vrtd_ret vrtd_device_hotplug_remove(int fd, uint32_t dev, uint8_t function)
{
    return vrtd_device_hotplug_op(fd, dev, VRTD_DEVICE_HOTPLUG_OP_REMOVE, function);
}

enum vrtd_ret vrtd_device_hotplug_toggle_sbr(int fd, uint32_t dev, uint8_t function)
{
    return vrtd_device_hotplug_op(fd, dev, VRTD_DEVICE_HOTPLUG_OP_TOGGLE_SBR, function);
}

enum vrtd_ret vrtd_device_hotplug_hotplug(int fd, uint32_t dev, uint8_t function)
{
    return vrtd_device_hotplug_op(fd, dev, VRTD_DEVICE_HOTPLUG_OP_HOTPLUG, function);
}

enum vrtd_ret vrtd_clock_get_rate(
    int fd,
    uint32_t dev,
    uint32_t region,
    uint32_t *rate_hz_out
)
{
    if (rate_hz_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_clock_op req = {
        .dev_number = dev,
        .region = region,
        .op = VRTD_CLOCK_OP_GET,
        .rate_hz = 0,
    };
    struct vrtd_resp_clock_op resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_CLOCK_OP,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *rate_hz_out = resp.rate_hz;
    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_clock_set_rate(
    int fd,
    uint32_t dev,
    uint32_t region,
    uint32_t rate_hz_in,
    uint32_t *rate_hz_out
)
{
    if (rate_hz_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_clock_op req = {
        .dev_number = dev,
        .region = region,
        .op = VRTD_CLOCK_OP_SET,
        .rate_hz = rate_hz_in,
    };
    struct vrtd_resp_clock_op resp = {0};

    int ret = vrtd_raw_request(fd, VRTD_REQ_CLOCK_OP,
                               &req, sizeof(req),
                               &resp, sizeof(resp),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    *rate_hz_out = resp.rate_hz;
    return VRTD_RET_OK;
}

enum vrtd_ret vrtd_open_bar_file(
    int fd,
    uint32_t dev,
    uint8_t bar,
    struct slash_bar_file *bar_file_out
) {
    if (bar_file_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    int bar_fd = -1;
    size_t len = 0;
    enum vrtd_ret ret = vrtd_get_bar_fd(fd, dev,bar,  &bar_file_out->fd, &bar_file_out->len);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    bar_file_out->map = mmap(NULL, bar_file_out->len, PROT_READ | PROT_WRITE, MAP_SHARED, bar_file_out->fd, 0);
    if (bar_file_out->map == MAP_FAILED) {
        bar_file_out->map = NULL;
        close(fd);
        return VRTD_RET_INTERNAL_ERROR;
    }

    return VRTD_RET_OK;
}

void vrtd_close_bar_file(struct slash_bar_file *bar_file)
{
    if (bar_file == NULL) {
        return;
    }

    if (bar_file->map != NULL) {
        munmap(bar_file->map, bar_file->len);
        close(bar_file->fd);

        bar_file->map = NULL;
    }
}

enum vrtd_ret vrtd_get_sensor_info(
    int fd,
    uint32_t dev,
    struct vrtd_sensor_entry *entries_out,
    uint32_t max_entries,
    uint32_t *num_entries_out
)
{
    if (entries_out == NULL || num_entries_out == NULL) {
        return VRTD_RET_BAD_LIB_CALL;
    }

    struct vrtd_req_get_sensor_info req = {
        .dev_number = dev,
    };

    /*
     * The response is variable-length: a uint32_t count followed by
     * sensor entries.  We receive into a stack buffer sized to the
     * maximum the protocol can carry.
     */
    uint8_t resp_buf[VRTD_MSG_MAX_SIZE - sizeof(struct vrtd_resp_header)];
    memset(resp_buf, 0, sizeof(resp_buf));

    int ret = vrtd_raw_request(fd, VRTD_REQ_GET_SENSOR_INFO,
                               &req, sizeof(req),
                               resp_buf, sizeof(resp_buf),
                               NULL, NULL);
    if (ret != VRTD_RET_OK) {
        return ret;
    }

    /* Parse the variable-length response. */
    struct vrtd_resp_get_sensor_info *resp = (struct vrtd_resp_get_sensor_info *)resp_buf;
    uint32_t count = resp->num_sensors;

    if (count > max_entries) {
        count = max_entries;
    }

    memcpy(entries_out, resp->sensors, count * sizeof(struct vrtd_sensor_entry));
    *num_entries_out = count;

    return VRTD_RET_OK;
}
