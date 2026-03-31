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
 * @file error.cpp
 *
 * Implementation of the vrtd::Error exception class.
 *
 * Error wraps a @c vrtd_ret error code (from the C wire protocol) into
 * a C++ @c std::exception.  The @c what() override translates the
 * numeric code into a human-readable string for diagnostics.
 */

#include <vrtd/error.hpp>

namespace vrtd {

Error::Error(vrtd_ret errorCode) noexcept {
    this->errorCode = errorCode;
}

vrtd_ret Error::getErrorCode() const noexcept {
    return errorCode;
}

const char *Error::what() const noexcept {
    switch (errorCode) {
    case VRTD_RET_BAD_LIB_CALL:
        return "Bad library call";

    case VRTD_RET_BAD_CONN:
        return "Bad connection to daemon";

    case VRTD_RET_BAD_REQUEST:
        return "Bad request";

    case VRTD_RET_INVALID_ARGUMENT:
        return "Invalid argument";

    case VRTD_RET_NOEXIST:
        return "Requested resouce doesn't exist";

    case VRTD_RET_INTERNAL_ERROR:
        return "Internal error in vrtd daemon or local libvrtd";

    case VRTD_RET_AUTH_ERROR:
        return "Missing permission";

    default:
        return "Unknown error";
    }
}

}
