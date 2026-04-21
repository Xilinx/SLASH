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
#include "array.h"
}

// --- Value array (int_array) ---

TEST(IntArrayTest, InitIsEmpty) {
    struct int_array arr = int_array_init();
    EXPECT_EQ(arr.len, 0u);
    EXPECT_EQ(arr.cap, 0u);
    EXPECT_EQ(arr.d, nullptr);
    int_array_free(&arr);
}

TEST(IntArrayTest, PushAndAccess) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_push(&arr, 42), 0);
    ASSERT_EQ(int_array_push(&arr, 99), 0);
    EXPECT_EQ(arr.len, 2u);
    EXPECT_EQ(arr.d[0], 42);
    EXPECT_EQ(arr.d[1], 99);
    int_array_free(&arr);
}

TEST(IntArrayTest, CapacityGrowsPowerOfTwo) {
    struct int_array arr = int_array_init();
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(int_array_push(&arr, i), 0);
    }
    EXPECT_EQ(arr.len, 5u);
    EXPECT_GE(arr.cap, 5u);
    EXPECT_EQ(arr.cap & (arr.cap - 1), 0u);
    int_array_free(&arr);
}

TEST(IntArrayTest, PopLifo) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_push(&arr, 10), 0);
    ASSERT_EQ(int_array_push(&arr, 20), 0);
    ASSERT_EQ(int_array_push(&arr, 30), 0);

    int val;
    EXPECT_EQ(int_array_pop(&arr, &val), 0);
    EXPECT_EQ(val, 30);
    EXPECT_EQ(int_array_pop(&arr, &val), 0);
    EXPECT_EQ(val, 20);
    EXPECT_EQ(int_array_pop(&arr, &val), 0);
    EXPECT_EQ(val, 10);
    int_array_free(&arr);
}

TEST(IntArrayTest, PopEmptyReturnsError) {
    struct int_array arr = int_array_init();
    int val;
    EXPECT_EQ(int_array_pop(&arr, &val), -1);
    int_array_free(&arr);
}

TEST(IntArrayTest, PopSafeEmptyIsNoop) {
    struct int_array arr = int_array_init();
    int val = -1;
    int_array_pop_safe(&arr, &val);
    EXPECT_EQ(val, -1);
    int_array_free(&arr);
}

TEST(IntArrayTest, RmByValue) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_push(&arr, 1), 0);
    ASSERT_EQ(int_array_push(&arr, 2), 0);
    ASSERT_EQ(int_array_push(&arr, 3), 0);
    ASSERT_EQ(int_array_push(&arr, 2), 0);

    int_array_rm_by_value(&arr, 2);
    EXPECT_EQ(arr.len, 2u);
    EXPECT_EQ(arr.d[0], 1);
    EXPECT_EQ(arr.d[1], 3);
    int_array_free(&arr);
}

TEST(IntArrayTest, ShrinkToFit) {
    struct int_array arr = int_array_init();
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(int_array_push(&arr, i), 0);
    }
    // pop_safe doesn't call resize, so capacity stays at 16
    int val;
    for (int i = 0; i < 8; i++) {
        int_array_pop_safe(&arr, &val);
    }
    EXPECT_EQ(arr.len, 8u);
    EXPECT_EQ(arr.cap, 16u);

    ASSERT_EQ(int_array_shrink_to_fit(&arr), 0);
    EXPECT_EQ(arr.cap, 8u);
    int_array_free(&arr);
}

TEST(IntArrayTest, Zero) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_push(&arr, 42), 0);
    ASSERT_EQ(int_array_push(&arr, 99), 0);

    int_array_zero(&arr);
    EXPECT_EQ(arr.d[0], 0);
    EXPECT_EQ(arr.d[1], 0);
    EXPECT_EQ(arr.len, 2u);
    int_array_free(&arr);
}

TEST(IntArrayTest, Resize) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_resize(&arr, 10), 0);
    EXPECT_GE(arr.cap, 10u);
    int_array_free(&arr);
}

TEST(IntArrayTest, FreeResetsState) {
    struct int_array arr = int_array_init();
    ASSERT_EQ(int_array_push(&arr, 1), 0);
    int_array_free(&arr);
    EXPECT_EQ(arr.len, 0u);
    EXPECT_EQ(arr.cap, 0u);
    EXPECT_EQ(arr.d, nullptr);
}

// --- Owning pointer array (str_array) ---

static int g_cleanup_count = 0;

struct dummy {
    int value;
};

static void cleanup_dummy(struct dummy *d) {
    g_cleanup_count++;
    free(d);
}

DECLARE_OWNING_PTR_ARRAY(dummy_ptr_array, struct dummy *, cleanup_dummy)

TEST(OwningArrayTest, PushMoveNullifiesSource) {
    struct dummy_ptr_array arr = dummy_ptr_array_init();
    auto *d = static_cast<struct dummy *>(calloc(1, sizeof(struct dummy)));
    d->value = 42;

    struct dummy *src = d;
    ASSERT_EQ(dummy_ptr_array_push_move(&arr, &src), 0);
    EXPECT_EQ(src, nullptr);
    EXPECT_EQ(arr.d[0]->value, 42);
    dummy_ptr_array_free(&arr);
}

TEST(OwningArrayTest, FreeCallsCleanup) {
    struct dummy_ptr_array arr = dummy_ptr_array_init();
    for (int i = 0; i < 3; i++) {
        auto *d = static_cast<struct dummy *>(calloc(1, sizeof(struct dummy)));
        d->value = i;
        struct dummy *src = d;
        ASSERT_EQ(dummy_ptr_array_push_move(&arr, &src), 0);
    }

    g_cleanup_count = 0;
    dummy_ptr_array_free(&arr);
    EXPECT_EQ(g_cleanup_count, 3);
}

TEST(OwningArrayTest, RmByReferenceCallsCleanup) {
    struct dummy_ptr_array arr = dummy_ptr_array_init();
    auto *d1 = static_cast<struct dummy *>(calloc(1, sizeof(struct dummy)));
    auto *d2 = static_cast<struct dummy *>(calloc(1, sizeof(struct dummy)));
    d1->value = 1;
    d2->value = 2;

    struct dummy *src = d1;
    ASSERT_EQ(dummy_ptr_array_push_move(&arr, &src), 0);
    src = d2;
    ASSERT_EQ(dummy_ptr_array_push_move(&arr, &src), 0);

    g_cleanup_count = 0;
    dummy_ptr_array_rm_by_reference(&arr, d1);
    EXPECT_EQ(g_cleanup_count, 1);
    EXPECT_EQ(arr.len, 1u);
    EXPECT_EQ(arr.d[0]->value, 2);
    dummy_ptr_array_free(&arr);
}

TEST(StrArrayTest, PushAndFree) {
    struct str_array arr = str_array_init();
    char *s1 = strdup("hello");
    char *s2 = strdup("world");
    ASSERT_EQ(str_array_push_move(&arr, &s1), 0);
    ASSERT_EQ(str_array_push_move(&arr, &s2), 0);
    EXPECT_EQ(s1, nullptr);
    EXPECT_EQ(s2, nullptr);
    EXPECT_EQ(arr.len, 2u);
    EXPECT_STREQ(arr.d[0], "hello");
    EXPECT_STREQ(arr.d[1], "world");
    str_array_free(&arr);
}
