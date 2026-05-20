/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <vrt/qdma/qdma_connection.hpp>

namespace vrt {
QdmaConnection::QdmaConnection(const std::string& kernel, uint32_t qid,
                               const std::string& interface, const std::string& direction)
    : kernel(kernel), interface(interface), qid(qid) {
    if (direction == "HostToDevice") {
        this->direction = StreamDirection::HOST_TO_DEVICE;
    } else if (direction == "DeviceToHost") {
        this->direction = StreamDirection::DEVICE_TO_HOST;
    }
}

std::string QdmaConnection::getKernel() const { return kernel; }

uint32_t QdmaConnection::getQid() const { return qid; }

std::string QdmaConnection::getInterface() const { return interface; }

StreamDirection QdmaConnection::getDirection() const { return direction; }

}  // namespace vrt