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

#ifndef VRTD_ANCILLARY_H
#define VRTD_ANCILLARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRTD_ANCILLARY_MAX_FDS 2
#define VRTD_ANCILLARY_RECV_FDS (VRTD_ANCILLARY_MAX_FDS + 1)
#define VRTD_ANCILLARY_BUFSIZE \
    CMSG_SPACE(VRTD_ANCILLARY_RECV_FDS * sizeof(int))

enum vrtd_ancillary_result {
    VRTD_ANCILLARY_OK = 0,
    VRTD_ANCILLARY_BAD_COUNT = 1,
    VRTD_ANCILLARY_MALFORMED = -1,
};

/*
 * On failure, close descriptors from complete, bounded SCM_RIGHTS payloads.
 * Never interpret bytes outside such a payload as descriptor integers.
 */
enum vrtd_ancillary_result vrtd_ancillary_extract(
    struct msghdr *msg, int *fds, size_t expected_fds);

size_t vrtd_request_expected_fds(uint16_t opcode);

/*
 * owned reports whether the descriptors the response carries are the sender's
 * own, which it must close once the send has handed a copy over, rather than
 * borrowed from something that keeps them open.  May be NULL.
 */
size_t vrtd_response_expected_fds(uint16_t opcode, bool *owned);

bool vrtd_response_expected_size(uint16_t opcode, size_t *expected_size);

#ifdef __cplusplus
}
#endif

#endif /* VRTD_ANCILLARY_H */