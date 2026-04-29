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
#include <vrt/parser/xml_parser.hpp>
#include <vrt/utils/platform.hpp>

#include <filesystem>

#include "test_helpers.hpp"

class XMLParserTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;

    void SetUp() override { tmpDir = makeTempDir("xml-parser-test"); }
    void TearDown() override { std::filesystem::remove_all(tmpDir); }

    std::string writeXml(const std::string& content) {
        return writeTempFile(tmpDir, "system_map.xml", content);
    }
};

TEST_F(XMLParserTest, ParseSingleKernel) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>vadd</Name>
    <BaseAddress>0x1000</BaseAddress>
    <Range>0x100</Range>
  </Kernel>
  <ClockFrequency>300000000</ClockFrequency>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    ASSERT_EQ(kernels.count("vadd"), 1u);
    EXPECT_EQ(kernels["vadd"].getName(), "vadd");
    EXPECT_EQ(kernels["vadd"].getPhysAddr(), 0x1000u);
}

TEST_F(XMLParserTest, ParseKernelRegisters) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>k</Name>
    <BaseAddress>0x0</BaseAddress>
    <Range>0x100</Range>
    <register offset="0x10" name="CTRL" access="RW" description="Control" range="32"/>
  </Kernel>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    ASSERT_EQ(kernels.count("k"), 1u);
}

TEST_F(XMLParserTest, ParseKernelFunctionalArgs) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>k</Name>
    <BaseAddress>0x0</BaseAddress>
    <Range>0x100</Range>
    <functional_args>
      <arg idx="0" name="input" type="buffer" offset="0x10" range="64" r="0" w="1" port="m_axi_gmem0"/>
    </functional_args>
  </Kernel>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    ASSERT_EQ(kernels.count("k"), 1u);
    auto& args = kernels["k"].getFunctionalArgs();
    ASSERT_EQ(args.size(), 1u);
    EXPECT_EQ(args[0].idx, 0u);
    EXPECT_EQ(args[0].name, "input");
    EXPECT_EQ(args[0].type, "buffer");
    EXPECT_EQ(args[0].offset, 0x10u);
    EXPECT_EQ(args[0].range, 64u);
    EXPECT_FALSE(args[0].readable);
    EXPECT_TRUE(args[0].writable);
    EXPECT_EQ(args[0].port, "m_axi_gmem0");
}

TEST_F(XMLParserTest, FunctionalArgsSortedByIdx) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>k</Name>
    <BaseAddress>0x0</BaseAddress>
    <Range>0x100</Range>
    <functional_args>
      <arg idx="2" name="c" type="int" offset="0x20" r="0" w="1"/>
      <arg idx="0" name="a" type="int" offset="0x10" r="0" w="1"/>
      <arg idx="1" name="b" type="int" offset="0x18" r="0" w="1"/>
    </functional_args>
  </Kernel>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    auto& args = kernels["k"].getFunctionalArgs();
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].idx, 0u);
    EXPECT_EQ(args[1].idx, 1u);
    EXPECT_EQ(args[2].idx, 2u);
}

TEST_F(XMLParserTest, ParseClockFrequency) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <ClockFrequency>250000000</ClockFrequency>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    EXPECT_EQ(parser.getClockFrequency(), 250000000u);
}

TEST_F(XMLParserTest, ParsePlatformHardware) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap><Platform>Hardware</Platform></SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    EXPECT_EQ(parser.getPlatform(), vrt::Platform::HARDWARE);
}

TEST_F(XMLParserTest, ParsePlatformEmulation) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap><Platform>Emulation</Platform></SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    EXPECT_EQ(parser.getPlatform(), vrt::Platform::EMULATION);
}

TEST_F(XMLParserTest, ParsePlatformSimulation) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap><Platform>Simulation</Platform></SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    EXPECT_EQ(parser.getPlatform(), vrt::Platform::SIMULATION);
}

TEST_F(XMLParserTest, ParsePlatformUnknownThrows) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap><Platform>SomethingWeird</Platform></SystemMap>)");
    vrt::XMLParser parser(path);
    EXPECT_THROW(parser.parseXML(), std::runtime_error);
}

TEST_F(XMLParserTest, ParseQdmaConnections) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Platform>Hardware</Platform>
  <Qdma>
    <kernel>myKernel</kernel>
    <interface>axis_port</interface>
    <direction>HostToDevice</direction>
    <qid>3</qid>
  </Qdma>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto conns = parser.getQdmaConnections();
    ASSERT_EQ(conns.size(), 1u);
    EXPECT_EQ(conns[0].getKernel(), "myKernel");
    EXPECT_EQ(conns[0].getInterface(), "axis_port");
    EXPECT_EQ(conns[0].getDirection(), vrt::StreamDirection::HOST_TO_DEVICE);
    EXPECT_EQ(conns[0].getQid(), 3u);
}

TEST_F(XMLParserTest, ParseMultipleKernels) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>k1</Name>
    <BaseAddress>0x1000</BaseAddress>
    <Range>0x100</Range>
  </Kernel>
  <Kernel>
    <Name>k2</Name>
    <BaseAddress>0x2000</BaseAddress>
    <Range>0x200</Range>
  </Kernel>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    EXPECT_EQ(kernels.size(), 2u);
    EXPECT_EQ(kernels.count("k1"), 1u);
    EXPECT_EQ(kernels.count("k2"), 1u);
}

TEST_F(XMLParserTest, InvalidXmlFileThrows) {
    auto path = writeTempFile(tmpDir, "bad.xml", "not valid xml <<<<");
    EXPECT_THROW(vrt::XMLParser parser(path), std::runtime_error);
}

TEST_F(XMLParserTest, EmptySystemMap) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap></SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    EXPECT_TRUE(parser.getKernels().empty());
    EXPECT_TRUE(parser.getQdmaConnections().empty());
}

TEST_F(XMLParserTest, ParseKernelConnections) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<SystemMap>
  <Kernel>
    <Name>k</Name>
    <BaseAddress>0x0</BaseAddress>
    <Range>0x100</Range>
    <connection port="m_axi_gmem0" target="DDR[0]"/>
    <connection port="m_axi_gmem1" target="HBM[3]"/>
  </Kernel>
  <Platform>Hardware</Platform>
</SystemMap>)");
    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    ASSERT_EQ(kernels.count("k"), 1u);
}
