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
