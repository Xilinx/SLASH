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
 * @file qdma_qpair.cpp
 *
 * Implementation of the vrtd::QdmaQpair C++ wrapper.
 *
 * QdmaQpair manages the lifecycle of a QDMA queue pair obtained from
 * the vrtd daemon.  It uses the **callback injection** pattern: the
 * Session that creates the QdmaQpair provides start/stop/delete/openFd
 * callbacks that issue the appropriate wire protocol requests.  This
 * keeps QdmaQpair decoupled from Session while still enabling RAII
 * cleanup (stop + delete on destruction).
 */

#include <vrtd/qdma_qpair.hpp>

#include <utility>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace vrtd {

QdmaQpair::QdmaQpair(uint32_t devNum,
                     uint32_t qid,
                     std::function<void(const QdmaQpair&)> fStart,
                     std::function<void(const QdmaQpair&)> fStop,
                     std::function<void(const QdmaQpair&)> fDelete,
                     std::function<int(const QdmaQpair&, uint32_t)> fOpenFd) noexcept
    : devNum(devNum)
    , qid(qid)
    , owned(true)
    , fStart(std::move(fStart))
    , fStop(std::move(fStop))
    , fDelete(std::move(fDelete))
    , fOpenFd(std::move(fOpenFd))
{
}

QdmaQpair::~QdmaQpair()
{
    if (!owned) {
        return;
    }

    if (!fDelete || qid == 0) {
        return;
    }

    try {
        fDelete(*this);
    } catch (...) {
        // Destructors must not throw; ignore errors on best-effort delete.
    }
}

QdmaQpair::QdmaQpair(QdmaQpair&& other) noexcept
    : devNum(other.devNum)
    , qid(other.qid)
    , owned(other.owned)
    , fStart(std::move(other.fStart))
    , fStop(std::move(other.fStop))
    , fDelete(std::move(other.fDelete))
    , fOpenFd(std::move(other.fOpenFd))
{
    other.owned = false;
    other.qid   = 0;
}

QdmaQpair& QdmaQpair::operator=(QdmaQpair&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    // Drop current ownership (best-effort delete in destructor semantics)
    if (owned && fDelete && qid != 0) {
        try {
            fDelete(*this);
        } catch (...) {
            // ignore
        }
    }

    devNum = other.devNum;
    qid    = other.qid;
    owned  = other.owned;
    fStart = std::move(other.fStart);
    fStop  = std::move(other.fStop);
    fDelete= std::move(other.fDelete);
    fOpenFd= std::move(other.fOpenFd);

    other.owned = false;
    other.qid   = 0;

    return *this;
}

void QdmaQpair::start()
{
    if (!fStart) {
        throw std::runtime_error("QDMA qpair start not available");
    }

    fStart(*this);
}

void QdmaQpair::stop()
{
    if (!fStop) {
        throw std::runtime_error("QDMA qpair stop not available");
    }

    fStop(*this);
}

int QdmaQpair::fd(uint32_t flags)
{
    if (!fOpenFd) {
        throw std::runtime_error("QDMA qpair fd() not available");
    }

    return fOpenFd(*this, flags);
}

std::fstream QdmaQpair::fstream(uint32_t flags, std::ios_base::openmode mode)
{
    int qfd = fd(flags);

    try {
        std::string path = "/proc/self/fd/" + std::to_string(qfd);

        std::fstream stream;
        stream.open(path, mode);

        ::close(qfd);

        if (!stream.is_open()) {
            throw std::runtime_error("Failed to open fstream for QDMA qpair");
        }

        return stream;
    } catch (...) {
        ::close(qfd);
        throw;
    }
}

} // namespace vrtd
