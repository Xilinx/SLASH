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
#include <gtest/gtest.h>
#include <vrt/register/register.hpp>

TEST(RegisterTest, ParameterizedConstructor) {
    vrt::Register reg("CTRL", 0x10, 32, "RW", "Control register");
    EXPECT_EQ(reg.getRegisterName(), "CTRL");
    EXPECT_EQ(reg.getOffset(), 0x10u);
    EXPECT_EQ(reg.getWidth(), 32u);
    EXPECT_EQ(reg.getRW(), "RW");
    EXPECT_EQ(reg.getDescription(), "Control register");
}

TEST(RegisterTest, DefaultConstructor) {
    vrt::Register reg;
    EXPECT_EQ(reg.getRegisterName(), "");
    EXPECT_EQ(reg.getRW(), "");
    EXPECT_EQ(reg.getDescription(), "");
}

TEST(RegisterTest, SetRegisterName) {
    vrt::Register reg;
    reg.setRegisterName("STATUS");
    EXPECT_EQ(reg.getRegisterName(), "STATUS");
}

TEST(RegisterTest, SetOffset) {
    vrt::Register reg;
    reg.setOffset(0x20);
    EXPECT_EQ(reg.getOffset(), 0x20u);
}

TEST(RegisterTest, SetWidth) {
    vrt::Register reg;
    reg.setWidth(64);
    EXPECT_EQ(reg.getWidth(), 64u);
}

TEST(RegisterTest, SetRW) {
    vrt::Register reg;
    reg.setRW("RO");
    EXPECT_EQ(reg.getRW(), "RO");
}

TEST(RegisterTest, SetDescription) {
    vrt::Register reg;
    reg.setDescription("Status register for monitoring");
    EXPECT_EQ(reg.getDescription(), "Status register for monitoring");
}
