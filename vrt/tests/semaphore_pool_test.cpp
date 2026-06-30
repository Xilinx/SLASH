/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
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

#include <vrt/graph/crossdevice/semaphore_pool.hpp>

using vrt::graph::SemaphorePool;

TEST(SemaphorePoolTest, TryAwaitReturnsFalseBeforeSignal) {
    SemaphorePool pool;
    auto sem = pool.allocate();
    EXPECT_FALSE(pool.tryAwait(sem));
    EXPECT_FALSE(pool.tryAwait(sem));
}

TEST(SemaphorePoolTest, TryAwaitConsumesSignalExactlyOnce) {
    SemaphorePool pool;
    auto sem = pool.allocate();
    pool.signal(sem);
    EXPECT_TRUE(pool.tryAwait(sem));
    EXPECT_FALSE(pool.tryAwait(sem));
}

TEST(SemaphorePoolTest, RepeatedSignalAndTryAwait) {
    SemaphorePool pool;
    auto sem = pool.allocate();
    for (int i = 0; i < 5; ++i) {
        pool.signal(sem);
        EXPECT_TRUE(pool.tryAwait(sem));
        EXPECT_FALSE(pool.tryAwait(sem));
    }
}

TEST(SemaphorePoolTest, IndependentHandles) {
    SemaphorePool pool;
    auto a = pool.allocate();
    auto b = pool.allocate();
    pool.signal(a);
    EXPECT_FALSE(pool.tryAwait(b));
    EXPECT_TRUE(pool.tryAwait(a));
    EXPECT_FALSE(pool.tryAwait(a));
}
