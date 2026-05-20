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

#include <vrt/register/register.hpp>

namespace vrt {

Register::Register(std::string registerName, uint32_t offset, uint32_t width, std::string rw,
                   std::string description)
    : registerName(registerName), offset(offset), width(width), rw(rw), description(description) {}

std::string Register::getRegisterName() { return registerName; }

uint32_t Register::getOffset() { return offset; }

uint32_t Register::getWidth() { return width; }

std::string Register::getRW() { return rw; }

std::string Register::getDescription() { return description; }

void Register::setRegisterName(std::string registerName) { this->registerName = registerName; }

void Register::setOffset(uint32_t offset) { this->offset = offset; }

void Register::setWidth(uint32_t width) { this->width = width; }

void Register::setRW(std::string rw) { this->rw = rw; }

void Register::setDescription(std::string description) { this->description = description; }

}  // namespace vrt