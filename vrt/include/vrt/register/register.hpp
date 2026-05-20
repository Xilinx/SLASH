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

#ifndef VRT_REGISTER_HPP
#define VRT_REGISTER_HPP
#include <cstdint>
#include <string>

namespace vrt {

/**
 * @brief Class representing a hardware register.
 */
class Register {
    std::string registerName;  ///< Name of the register
    uint32_t offset;           ///< Offset of the register
    uint32_t width;            ///< Width of the register
    std::string rw;            ///< Read/Write permissions of the register
    std::string description;   ///< Description of the register

   public:
    /**
     * @brief Constructor for Register.
     * @param registerName The name of the register.
     * @param offset The offset of the register.
     * @param width The width of the register.
     * @param rw The read/write permissions of the register.
     * @param description The description of the register.
     */
    Register(std::string registerName, uint32_t offset, uint32_t width, std::string rw,
             std::string description);

    /**
     * @brief Default constructor for Register.
     */
    Register() = default;

    /**
     * @brief Gets the name of the register.
     * @return The name of the register.
     */
    std::string getRegisterName();

    /**
     * @brief Gets the offset of the register.
     * @return The offset of the register.
     */
    uint32_t getOffset();

    /**
     * @brief Gets the width of the register.
     * @return The width of the register.
     */
    uint32_t getWidth();

    /**
     * @brief Gets the read/write permissions of the register.
     * @return The read/write permissions of the register.
     */
    std::string getRW();

    /**
     * @brief Gets the description of the register.
     * @return The description of the register.
     */
    std::string getDescription();

    /**
     * @brief Sets the name of the register.
     * @param registerName The name of the register.
     */
    void setRegisterName(std::string registerName);

    /**
     * @brief Sets the offset of the register.
     * @param offset The offset of the register.
     */
    void setOffset(uint32_t offset);

    /**
     * @brief Sets the width of the register.
     * @param width The width of the register.
     */
    void setWidth(uint32_t width);

    /**
     * @brief Sets the read/write permissions of the register.
     * @param rw The read/write permissions of the register.
     */
    void setRW(std::string rw);

    /**
     * @brief Sets the description of the register.
     * @param description The description of the register.
     */
    void setDescription(std::string description);
};

}  // namespace vrt

#endif  // VRT_REGISTER_HPP