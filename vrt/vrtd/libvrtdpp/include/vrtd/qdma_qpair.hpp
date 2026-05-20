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
#include <functional>
#include <fstream>
#include <fcntl.h>

namespace vrtd {

/**
 * @brief RAII wrapper for a QDMA queue pair (qpair).
 *
 * A @c QdmaQpair owns a qpair created through a @c Session. It provides
 * convenience methods to start/stop the qpair and to obtain a read/write
 * file descriptor. On destruction, it requests deletion of the qpair.
 *
 * @warning A @c QdmaQpair becomes invalid if its originating @c Session
 *          is closed or moved; methods will throw in that case. The
 *          destructor never throws and will silently ignore errors when
 *          attempting to delete the qpair.
 */
class QdmaQpair {
public:
    ~QdmaQpair();

    QdmaQpair(const QdmaQpair&)            = delete;
    QdmaQpair& operator=(const QdmaQpair&) = delete;

    QdmaQpair(QdmaQpair&& other) noexcept;
    QdmaQpair& operator=(QdmaQpair&& other) noexcept;

    /**
     * @brief Device index owning this qpair.
     */
    uint32_t getDeviceNum() const noexcept { return devNum; }

    /**
     * @brief Qpair identifier as assigned by the kernel.
     */
    uint32_t getQid() const noexcept { return qid; }

    /**
     * @brief Start the qpair.
     *
     * @throws vrtd::Error on error.
     */
    void start();

    /**
     * @brief Stop the qpair.
     *
     * @throws vrtd::Error on error.
     */
    void stop();

    /**
     * @brief Obtain a read/write file descriptor for this qpair.
     *
     * @param flags OR of O_CLOEXEC and 0 (other flags may be rejected).
     * @return New file descriptor owned by the caller.
     * @throws vrtd::Error on error.
     */
    int fd(uint32_t flags = O_CLOEXEC);

    /**
     * @brief Obtain a std::fstream bound to this qpair.
     *
     * @param flags OR of O_CLOEXEC and 0 (other flags may be rejected).
     * @param mode  Standard iostream open mode (defaults to in|out|binary).
     * @return A @c std::fstream owning a new file descriptor for this qpair.
     *
     * @throws vrtd::Error or std::runtime_error on error.
     *
     * @note Implementation is Linux-specific and relies on @c /proc/self/fd.
     */
    std::fstream fstream(
        uint32_t flags = O_CLOEXEC,
        std::ios_base::openmode mode =
            std::ios_base::in | std::ios_base::out | std::ios_base::binary
    );

private:
    friend class Session;

    QdmaQpair(uint32_t devNum,
              uint32_t qid,
              std::function<void(const QdmaQpair&)> fStart,
              std::function<void(const QdmaQpair&)> fStop,
              std::function<void(const QdmaQpair&)> fDelete,
              std::function<int(const QdmaQpair&, uint32_t)> fOpenFd) noexcept;

    uint32_t devNum{};
    uint32_t qid{};
    bool owned{true};

    std::function<void(const QdmaQpair&)>        fStart;
    std::function<void(const QdmaQpair&)>        fStop;
    std::function<void(const QdmaQpair&)>        fDelete;
    std::function<int(const QdmaQpair&, uint32_t)> fOpenFd;
};

} // namespace vrtd

#endif // VRTD_QDMA_QPAIR_HPP
