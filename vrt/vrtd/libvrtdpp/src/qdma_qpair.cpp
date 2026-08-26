/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <vrtd/qdma_qpair.hpp>

#include "connection.hpp"

#include <vrtd/error.hpp>

#include <stdexcept>
#include <string>
#include <utility>

#include <unistd.h>

namespace vrtd {

/*
 * Device creates this wrapper only after the daemon allocates the queue. The
 * owned flag, not the numeric QID, controls exactly-once deletion because zero
 * is a valid kernel-assigned identifier.
 */
QdmaQpair::QdmaQpair(std::shared_ptr<detail::Connection> connection,
                     uint32_t devNum,
                     uint32_t qid) noexcept
    : connection(std::move(connection))
    , devNum(devNum)
    , qid(qid)
    , owned(true)
{
}

QdmaQpair::~QdmaQpair()
{
    if (!owned || !connection) {
        return;
    }

    try {
        connection->deleteQdmaQpair(devNum, qid);
    } catch (...) {
        // Destructors must not throw; daemon disconnect also releases qpairs.
    }
}

QdmaQpair::QdmaQpair(QdmaQpair&& other) noexcept
    : connection(std::move(other.connection))
    , devNum(other.devNum)
    , qid(other.qid)
    , owned(std::exchange(other.owned, false))
{
    /*
     * Transfer deletion responsibility through owned rather than inferring it
     * from qid: queue identifier zero is valid.
     */
    other.qid = 0;
}

QdmaQpair& QdmaQpair::operator=(QdmaQpair&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    if (owned && connection) {
        try {
            connection->deleteQdmaQpair(devNum, qid);
        } catch (...) {
            /*
             * Preserve noexcept move assignment. If deletion failed because
             * the connection closed, vrtd releases the queue on disconnect.
             */
        }
    }

    connection = std::move(other.connection);
    devNum = other.devNum;
    qid = other.qid;
    owned = std::exchange(other.owned, false);
    other.qid = 0;

    return *this;
}

void QdmaQpair::start()
{
    if (!owned || !connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->startQdmaQpair(devNum, qid);
}

void QdmaQpair::stop()
{
    if (!owned || !connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    connection->stopQdmaQpair(devNum, qid);
}

int QdmaQpair::fd(uint32_t flags)
{
    if (!owned || !connection) {
        throw Error(VRTD_RET_BAD_LIB_CALL);
    }

    return connection->openQdmaQpairFd(devNum, qid, flags);
}

std::fstream QdmaQpair::fstream(
    uint32_t flags,
    std::ios_base::openmode mode
)
{
    int qpairFd = fd(flags);

    try {
        /*
         * Open the procfs descriptor path while the received FD is still live.
         * The stream opens its own file description, after which the temporary
         * daemon-provided descriptor can close.
         */
        std::string path = "/proc/self/fd/" + std::to_string(qpairFd);
        std::fstream stream(path, mode);
        (void) ::close(qpairFd);
        qpairFd = -1;

        if (!stream.is_open()) {
            throw std::runtime_error(
                "Failed to open fstream for QDMA qpair"
            );
        }

        return stream;
    } catch (...) {
        /* Close the temporary exactly once if ownership was not transferred. */
        if (qpairFd >= 0) {
            (void) ::close(qpairFd);
        }

        throw;
    }
}

} // namespace vrtd
