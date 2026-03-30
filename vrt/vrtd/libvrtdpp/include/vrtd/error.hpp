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

#ifndef VRTD_ERROR_H
#define VRTD_ERROR_H

#include <stdexcept>

#include <vrtd/vrtd.h>

namespace vrtd {

/**
 * @brief Exception type for libvrtd/libvrtd++ operations.
 *
 * Wraps a @c vrtd_ret code and exposes a human-readable, static message via
 * @c what(). Use @c getErrorCode() to branch on a specific error.
 *
 * @note Transport/socket issues in the C++ layer are mapped to
 *       @c VRTD_RET_BAD_CONN.
 * @note The message returned by @c what() is a static string mapped from the
 *       code (e.g., "Authentication error") and does not allocate.
 */
class Error : public std::exception {
private:
    vrtd_ret errorCode;

public:
    /**
     * @brief Construct an Error with the given code.
     * @param errorCode A value from @c vrtd_ret.
     */
    explicit Error(vrtd_ret errorCode) noexcept;

    ~Error() = default;

    Error(const Error&)                = default;
    Error& operator=(const Error&)     = default;
    Error(Error&&) noexcept            = default;
    Error& operator=(Error&&) noexcept = default;

    /**
     * @brief Retrieve the underlying error code.
     */
    vrtd_ret getErrorCode() const noexcept;

    /**
     * @brief Human-readable description corresponding to @c errorCode.
     *
     * @return Pointer to a static, null-terminated string. The storage is
     *         valid for the lifetime of the program.
     */
    const char *what() const noexcept override;
};

}

#endif //VRTD_ERROR_H
