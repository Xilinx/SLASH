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

#ifndef VRT_QDMA_LOGIC_HPP
#define VRT_QDMA_LOGIC_HPP
#include <vrt/kernel.hpp>

namespace vrt {

/**
 * @brief Class for managing QDMA logic operations.
 *
 * The QdmaLogic class extends the Kernel class to provide functionality specific
 * to QDMA operations, allowing for control of QDMA queues and data transfers in streaming mode.
 * 
 * @note Not used now. Might be needed for C2H stream future development
 */
class QdmaLogic : public Kernel {
   public:
    /**
     * @brief Constructor for QdmaLogic.
     *
     * @param name Name of the QDMA kernel.
     * @param baseAddr Base address of the QDMA kernel in device memory.
     * @param range Memory range allocated to the QDMA kernel.
     */
    QdmaLogic(const std::string& name, uint64_t baseAddr, uint64_t range);

    /**
     * @brief Sets QDMA queue parameters.
     *
     * @param qid Queue ID to configure.
     * @param length Length of the data transfer.
     *
     * This method configures the specified QDMA queue with the given parameters.
     */
    void setValues(uint16_t qid, uint32_t length);
};

}  // namespace vrt

#endif  // VRT_QDMA_LOGIC_HPP
