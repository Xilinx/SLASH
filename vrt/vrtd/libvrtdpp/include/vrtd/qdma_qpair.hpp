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

#ifndef VRTD_QDMA_QPAIR_HPP
#define VRTD_QDMA_QPAIR_HPP

#include <cstdint>
#include <fstream>
#include <memory>
#include <fcntl.h>

namespace vrtd {

namespace detail {
class Connection;
}

/**
 * @brief RAII wrapper for a QDMA queue pair (qpair).
 *
 * A @c QdmaQpair owns a qpair created through a @c Session. It provides
 * lifecycle methods and access to the ioctl-only QDMA descriptor used for
 * buffer creation and transfer commands. On destruction, it requests deletion
 * of the qpair.
 *
 * Moving or destroying the originating @c Session does not invalidate a
 * QdmaQpair. Explicitly closing the shared connection does. The destructor
 * never throws and silently ignores errors during best-effort deletion.
 */
class QdmaQpair {
public:
    /**
     * @brief Best-effort delete the owned queue pair.
     *
     * Errors do not escape; daemon disconnect remains the cleanup backstop.
     */
    ~QdmaQpair();

    /** A daemon queue-pair identity has one RAII owner and cannot be copied. */
    QdmaQpair(const QdmaQpair&)            = delete;
    QdmaQpair& operator=(const QdmaQpair&) = delete;

    /** Transfer queue ownership and leave @p other unowned. */
    QdmaQpair(QdmaQpair&& other) noexcept;

    /**
     * @brief Best-effort delete the current queue, then adopt @p other.
     *
     * The moved-from object becomes unowned.
     */
    QdmaQpair& operator=(QdmaQpair&& other) noexcept;

    /**
     * @brief Return the daemon device index owning this qpair.
     */
    uint32_t getDeviceNum() const noexcept { return devNum; }

    /**
     * @brief Return the kernel-assigned queue identifier.
     *
     * Zero is valid for an owned queue and is also the moved-from sentinel.
     */
    uint32_t getQid() const noexcept { return qid; }

    /**
     * @brief Start the qpair.
     *
     * @throws vrtd::Error if this object is unowned, the shared connection is
     *         closed, or the daemon rejects the lifecycle transition.
     */
    void start();

    /**
     * @brief Stop the qpair.
     *
     * @throws vrtd::Error if this object is unowned, the shared connection is
     *         closed, or the daemon rejects the lifecycle transition.
     */
    void stop();

    /**
     * @brief Obtain an ioctl-only QDMA descriptor for this qpair.
     *
     * @param flags OR of O_CLOEXEC and 0 (other flags may be rejected).
     * @return New file descriptor owned by the caller.
     * @throws vrtd::Error if unowned, closed, or the request fails.
     */
    int fd(uint32_t flags = O_CLOEXEC);

    /**
     * @brief Obtain a compatibility stream object bound to this qpair.
     *
     * The underlying QDMA endpoint remains ioctl-only; ordinary stream
     * read/write operations are unsupported even if opening the stream succeeds.
     *
     * @param flags OR of O_CLOEXEC and 0 (other flags may be rejected).
     * @param mode  Standard iostream open mode (defaults to in|out|binary).
     * @return A @c std::fstream owning a new file descriptor for this qpair.
     *
     * @throws vrtd::Error or std::runtime_error on error.
     *
     * @note Linux-specific: opens @c /proc/self/fd/<fd> so the stream receives
     *       its own descriptor before the temporary daemon-provided FD closes.
     */
    std::fstream fstream(
        uint32_t flags = O_CLOEXEC,
        std::ios_base::openmode mode =
            std::ios_base::in | std::ios_base::out | std::ios_base::binary
    );

private:
    friend class Device;

    /**
     * @brief Adopt ownership of a newly created daemon queue pair.
     *
     * @param connection Shared connection used for all lifecycle operations.
     * @param devNum Zero-based owning device index.
     * @param qid Kernel-assigned queue identifier; zero is valid.
     */
    QdmaQpair(std::shared_ptr<detail::Connection> connection,
              uint32_t devNum,
              uint32_t qid) noexcept;

    /** Shared connection retained through best-effort deletion. */
    std::shared_ptr<detail::Connection> connection;
    uint32_t devNum{}; ///< Zero-based owning device index.
    uint32_t qid{}; ///< Kernel-assigned queue identifier.
    bool owned{true}; ///< Sole indicator that destruction must delete the queue.
};

} // namespace vrtd

#endif // VRTD_QDMA_QPAIR_HPP
