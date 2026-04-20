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
