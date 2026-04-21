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
#include <vrt/qdma/qdma_connection.hpp>

TEST(QdmaConnectionTest, HostToDeviceDirection) {
    vrt::QdmaConnection conn("myKernel", 0, "axis_port", "HostToDevice");
    EXPECT_EQ(conn.getDirection(), vrt::StreamDirection::HOST_TO_DEVICE);
}

TEST(QdmaConnectionTest, DeviceToHostDirection) {
    vrt::QdmaConnection conn("myKernel", 1, "axis_port", "DeviceToHost");
    EXPECT_EQ(conn.getDirection(), vrt::StreamDirection::DEVICE_TO_HOST);
}

TEST(QdmaConnectionTest, GetKernel) {
    vrt::QdmaConnection conn("testKernel", 3, "iface0", "HostToDevice");
    EXPECT_EQ(conn.getKernel(), "testKernel");
}

TEST(QdmaConnectionTest, GetQid) {
    vrt::QdmaConnection conn("k", 42, "iface0", "HostToDevice");
    EXPECT_EQ(conn.getQid(), 42u);
}

TEST(QdmaConnectionTest, GetInterface) {
    vrt::QdmaConnection conn("k", 0, "my_interface", "DeviceToHost");
    EXPECT_EQ(conn.getInterface(), "my_interface");
}
