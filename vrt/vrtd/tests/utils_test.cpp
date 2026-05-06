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

extern "C" {
#include "utils.h"
}

// --- string_to_bool ---

TEST(StringToBoolTest, TruthyValues) {
    EXPECT_TRUE(string_to_bool("1"));
    EXPECT_TRUE(string_to_bool("y"));
    EXPECT_TRUE(string_to_bool("Y"));
    EXPECT_TRUE(string_to_bool("yes"));
    EXPECT_TRUE(string_to_bool("YES"));
    EXPECT_TRUE(string_to_bool("Yes"));
    EXPECT_TRUE(string_to_bool("true"));
    EXPECT_TRUE(string_to_bool("TRUE"));
    EXPECT_TRUE(string_to_bool("True"));
}

TEST(StringToBoolTest, FalsyValues) {
    EXPECT_FALSE(string_to_bool("0"));
    EXPECT_FALSE(string_to_bool("n"));
    EXPECT_FALSE(string_to_bool("N"));
    EXPECT_FALSE(string_to_bool("no"));
    EXPECT_FALSE(string_to_bool("false"));
    EXPECT_FALSE(string_to_bool("FALSE"));
    EXPECT_FALSE(string_to_bool(""));
    EXPECT_FALSE(string_to_bool("2"));
    EXPECT_FALSE(string_to_bool("maybe"));
}

TEST(StringToBoolTest, NullReturnsFalse) {
    EXPECT_FALSE(string_to_bool(nullptr));
}

TEST(StringToBoolTest, WhitespaceTrimming) {
    EXPECT_TRUE(string_to_bool("  yes  "));
    EXPECT_TRUE(string_to_bool("\ttrue\n"));
    EXPECT_TRUE(string_to_bool(" 1 "));
    EXPECT_FALSE(string_to_bool("   "));
}

// --- bit_ceil_u32 ---

TEST(BitCeilU32Test, Zero) {
    EXPECT_EQ(bit_ceil_u32(0u), 1u);
}

TEST(BitCeilU32Test, One) {
    EXPECT_EQ(bit_ceil_u32(1u), 1u);
}

TEST(BitCeilU32Test, PowersOfTwo) {
    EXPECT_EQ(bit_ceil_u32(2u), 2u);
    EXPECT_EQ(bit_ceil_u32(4u), 4u);
    EXPECT_EQ(bit_ceil_u32(8u), 8u);
    EXPECT_EQ(bit_ceil_u32(0x80000000u), 0x80000000u);
}

TEST(BitCeilU32Test, NonPowersRoundUp) {
    EXPECT_EQ(bit_ceil_u32(3u), 4u);
    EXPECT_EQ(bit_ceil_u32(5u), 8u);
    EXPECT_EQ(bit_ceil_u32(7u), 8u);
    EXPECT_EQ(bit_ceil_u32(9u), 16u);
    EXPECT_EQ(bit_ceil_u32(100u), 128u);
}

TEST(BitCeilU32Test, Overflow) {
    EXPECT_EQ(bit_ceil_u32(0x80000001u), 0u);
    EXPECT_EQ(bit_ceil_u32(0xFFFFFFFFu), 0u);
}

// --- bit_ceil_u64 ---

TEST(BitCeilU64Test, Zero) {
    EXPECT_EQ(bit_ceil_u64(0ull), 1ull);
}

TEST(BitCeilU64Test, One) {
    EXPECT_EQ(bit_ceil_u64(1ull), 1ull);
}

TEST(BitCeilU64Test, PowersOfTwo) {
    EXPECT_EQ(bit_ceil_u64(2ull), 2ull);
    EXPECT_EQ(bit_ceil_u64(0x100000000ull), 0x100000000ull);
}

TEST(BitCeilU64Test, NonPowersRoundUp) {
    EXPECT_EQ(bit_ceil_u64(3ull), 4ull);
    EXPECT_EQ(bit_ceil_u64(5ull), 8ull);
    EXPECT_EQ(bit_ceil_u64(0x100000001ull), 0x200000000ull);
}

TEST(BitCeilU64Test, Overflow) {
    EXPECT_EQ(bit_ceil_u64(0x8000000000000001ull), 0ull);
}

// --- glob_err_to_string ---

TEST(GlobErrTest, AllCodes) {
    EXPECT_STREQ(glob_err_to_string(0), "OK");
    EXPECT_STREQ(glob_err_to_string(GLOB_NOSPACE), "out of memory");
    EXPECT_STREQ(glob_err_to_string(GLOB_ABORTED), "read error");
    EXPECT_STREQ(glob_err_to_string(GLOB_NOMATCH), "no matches found");
    EXPECT_STREQ(glob_err_to_string(999), "unknown glob(3) error");
}
