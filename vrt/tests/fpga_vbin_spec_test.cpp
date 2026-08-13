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

#include <vrt/graph/device/fpga/vbin_spec.hpp>
#include <vrt/parser/xml_parser.hpp>

#include "test_helpers.hpp"

using namespace vrt::graph;

TEST(FpgaVbinSpecTest, ConvertsFunctionalArgsToIoTypeMap) {
    std::vector<vrt::FunctionalArg> args;
    args.push_back(vrt::FunctionalArg{0, "n", "scalar", 0x10, 32, false, true, ""});
    args.push_back(vrt::FunctionalArg{1, "in", "buffer", 0x18, 64, false, true, "m_axi_gmem0"});
    args.push_back(vrt::FunctionalArg{2, "result", "uint64_t", 0x20, 64, true, false, ""});
    args.push_back(vrt::FunctionalArg{3, "level", "int*", 0x28, 32, true, false, ""});

    IOTypeMap io = fpga::ioTypeMapFromFunctionalArgs(args);
    ASSERT_EQ(io.inputScalars.size(), 1u);
    EXPECT_EQ(io.inputScalars[0].name, "n");
    EXPECT_EQ(io.inputScalars[0].type, ScalarType::U32);
    ASSERT_EQ(io.inputs.size(), 1u);
    EXPECT_EQ(io.inputs[0].name, "in");
    ASSERT_EQ(io.outputScalars.size(), 2u);
    EXPECT_EQ(io.outputScalars[0].name, "result");
    EXPECT_EQ(io.outputScalars[0].type, ScalarType::U64);
    EXPECT_EQ(io.outputScalars[1].name, "level");
    EXPECT_EQ(io.outputScalars[1].type, ScalarType::I32);
}

TEST(FpgaVbinSpecTest, ComputesR5AddressFromSystemMapBase) {
    const auto tmpDir = makeTempDir("fpga-vbin-spec-test");
    const std::string path = writeTempFile(tmpDir, "system_map.xml", R"(<?xml version="1.0"?>
<SystemMap>
  <Platform>Hardware</Platform>
  <Kernel>
    <Name>k0</Name>
    <BaseAddress>0x020200010000</BaseAddress>
    <Range>0x1000</Range>
    <functional_args>
      <arg idx="0" name="size" type="scalar" offset="0x10" range="32" r="0" w="1"/>
    </functional_args>
  </Kernel>
</SystemMap>)");

    vrt::XMLParser parser(path);
    parser.parseXML();
    auto kernels = parser.getKernels();
    ASSERT_EQ(kernels.count("k0"), 1u);

    fpga::FpgaKernelSpec spec = fpga::fpgaKernelSpecFromKernel(kernels["k0"]);
    EXPECT_EQ(spec.name, "k0");
    EXPECT_EQ(spec.system_map_base_addr, 0x020200010000ULL);
    EXPECT_EQ(spec.r5_base_addr, 0x88010000u);
    ASSERT_EQ(spec.ioType.inputScalars.size(), 1u);
    EXPECT_EQ(spec.ioType.inputScalars[0].name, "size");

    std::filesystem::remove_all(tmpDir);
}

TEST(FpgaVbinSpecTest, CatalogLooksUpImagesAndKernels) {
    fpga::FpgaImageSpec image;
    image.id = "imageA";
    image.vbinPath = "a.vbin";
    image.pdiPath = "a.pdi";
    fpga::FpgaKernelSpec kernel;
    kernel.name = "k0";
    kernel.r5_base_addr = 0x88000000u;
    image.kernels.emplace(kernel.name, kernel);

    fpga::FpgaVbinSpec catalog;
    catalog.addImage(std::move(image));

    EXPECT_TRUE(catalog.hasImage("imageA"));
    EXPECT_EQ(catalog.defaultImageId(), "imageA");
    EXPECT_EQ(catalog.kernel("imageA", "k0").r5_base_addr, 0x88000000u);
}
