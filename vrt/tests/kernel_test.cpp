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
#include <vrt/kernel.hpp>
#include <vrt/utils/platform.hpp>

#include <vector>

static vrt::FunctionalArg makeArg(uint32_t idx, const std::string& name, const std::string& type,
                                  uint32_t offset, uint32_t range = 32, bool readable = false,
                                  bool writable = true, const std::string& port = "") {
    vrt::FunctionalArg a;
    a.idx = idx;
    a.name = name;
    a.type = type;
    a.offset = offset;
    a.range = range;
    a.readable = readable;
    a.writable = writable;
    a.port = port;
    return a;
}

static vrt::Kernel makeTestKernel(const std::vector<vrt::FunctionalArg>& args = {},
                                  const std::string& name = "testKernel",
                                  uint64_t baseAddr = 0x1000, uint64_t range = 0x100) {
    std::vector<vrt::Register> regs;
    return vrt::Kernel(name, baseAddr, range, regs, args);
}

TEST(KernelConstructTest, FiveArgConstructor) {
    auto k = makeTestKernel();
    EXPECT_EQ(k.getName(), "testKernel");
    EXPECT_EQ(k.getPhysAddr(), 0x1000u);
}

TEST(KernelConstructTest, HasFunctionalArgsTrue) {
    auto k = makeTestKernel({makeArg(0, "a", "int", 0x10)});
    EXPECT_TRUE(k.hasFunctionalArgs());
}

TEST(KernelConstructTest, HasFunctionalArgsFalse) {
    auto k = makeTestKernel();
    EXPECT_FALSE(k.hasFunctionalArgs());
}

TEST(KernelConstructTest, FunctionalArgsSortedOnConstruction) {
    auto k = makeTestKernel(
        {makeArg(2, "c", "int", 0x20), makeArg(0, "a", "int", 0x10), makeArg(1, "b", "int", 0x18)});
    auto& args = k.getFunctionalArgs();
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].idx, 0u);
    EXPECT_EQ(args[1].idx, 1u);
    EXPECT_EQ(args[2].idx, 2u);
}

TEST(KernelConstructTest, SetAndGetFunctionalArgs) {
    auto k = makeTestKernel();
    std::vector<vrt::FunctionalArg> newArgs = {makeArg(0, "x", "int", 0x10)};
    k.setFunctionalArgs(newArgs);
    EXPECT_TRUE(k.hasFunctionalArgs());
    EXPECT_EQ(k.getFunctionalArgs().size(), 1u);
    EXPECT_EQ(k.getFunctionalArgs()[0].name, "x");
}

TEST(KernelArgLookupTest, SetArgByIdx) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10, 32)});
    EXPECT_NO_THROW(k.setArg(0, 42));
}

TEST(KernelArgLookupTest, SetArgByName) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10, 32)});
    EXPECT_NO_THROW(k.setArg("input", 42));
}

TEST(KernelArgLookupTest, SetArgByNameWithRSuffix) {
    auto k = makeTestKernel({makeArg(0, "input_r", "buffer", 0x10, 64)});
    EXPECT_NO_THROW(k.setArg("input", static_cast<uint64_t>(0xDEAD)));
}

TEST(KernelArgLookupTest, SetArgEmptyNameThrows) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10)});
    EXPECT_THROW(k.setArg("", 42), std::runtime_error);
}

TEST(KernelArgLookupTest, SetArgNameNotFoundThrows) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10)});
    EXPECT_THROW(k.setArg("nonexistent", 42), std::runtime_error);
}

TEST(KernelArgLookupTest, SetArgIdxNotFoundThrows) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10)});
    EXPECT_THROW(k.setArg(99, 42), std::runtime_error);
}

TEST(KernelArgLookupTest, SetArgNegativeIndexThrows) {
    auto k = makeTestKernel({makeArg(0, "input", "scalar", 0x10)});
    EXPECT_THROW(k.setArg(-1, 42), std::runtime_error);
}

TEST(KernelArgLookupTest, SetArgNoMetadataThrows) {
    auto k = makeTestKernel();
    EXPECT_THROW(k.setArg(0, 42), std::runtime_error);
}

TEST(KernelArgValidationTest, EnsureSetArgValuesComplete) {
    auto k = makeTestKernel(
        {makeArg(0, "a", "scalar", 0x10, 32), makeArg(1, "b", "scalar", 0x14, 32)});
    k.setArg(0, 1);
    k.setArg(1, 2);
    // With no platform set (UNKNOWN), call() skips all branches — validation is
    // only exercised inside platform-specific blocks, so this tests that setArg
    // itself succeeds for complete argument sets.
    EXPECT_NO_THROW(k.call());
}

TEST(KernelArgValidationTest, EnsureSetArgValuesMissingThrows) {
    auto k = makeTestKernel(
        {makeArg(0, "a", "scalar", 0x10, 32), makeArg(1, "b", "scalar", 0x14, 32)});
    k.setPlatform(vrt::Platform::HARDWARE);
    k.setArg(0, 1);
    EXPECT_THROW(k.call(), std::runtime_error);
}

TEST(KernelArgValidationTest, ReadOnlyArgNotRequiredForLaunch) {
    auto k = makeTestKernel({makeArg(0, "a", "scalar", 0x10, 32, true, true),
                             makeArg(1, "status", "scalar", 0x14, 32, true, false)});
    k.setPlatform(vrt::Platform::HARDWARE);
    k.setArg(0, 1);
    // status is read-only (writable=false), so only "a" needs to be set.
    // call() should reach ensureSetArgValuesCompleteForLaunch, which skips
    // read-only args, then try writeBatch which throws because no BAR is set.
    // The key assertion: it does NOT throw about a missing "status" arg.
    EXPECT_THROW(k.call(), std::runtime_error);
    try {
        k.call();
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_EQ(msg.find("status"), std::string::npos) << "Should not require read-only arg";
        EXPECT_NE(msg.find("BAR"), std::string::npos) << "Should fail at BAR access, not arg validation";
    }
}

TEST(KernelMemoryConfigTest, PortMemoryConfigDDR) {
    auto k = makeTestKernel({makeArg(0, "in", "buffer", 0x10, 64, false, true, "m_axi_gmem0")});
    k.setConnections({{"m_axi_gmem0", "DDR"}});
    auto cfg = k.portMemoryConfig("m_axi_gmem0");
    EXPECT_EQ(cfg.type, vrt::MemoryRangeType::DDR);
    EXPECT_FALSE(cfg.hbmPort.has_value());
}

TEST(KernelMemoryConfigTest, PortMemoryConfigHBM) {
    auto k = makeTestKernel({makeArg(0, "in", "buffer", 0x10, 64, false, true, "m_axi_gmem0")});
    k.setConnections({{"m_axi_gmem0", "HBM3"}});
    auto cfg = k.portMemoryConfig("m_axi_gmem0");
    EXPECT_EQ(cfg.type, vrt::MemoryRangeType::HBM);
    ASSERT_TRUE(cfg.hbmPort.has_value());
    EXPECT_EQ(cfg.hbmPort.value(), 3u);
}

TEST(KernelMemoryConfigTest, PortMemoryConfigHBMVnoc) {
    auto k = makeTestKernel();
    k.setConnections({{"port0", "HBM"}});
    auto cfg = k.portMemoryConfig("port0");
    EXPECT_EQ(cfg.type, vrt::MemoryRangeType::HBM_VNOC);
}

TEST(KernelMemoryConfigTest, PortMemoryConfigMEM) {
    auto k = makeTestKernel();
    k.setConnections({{"port0", "MEM"}});
    auto cfg = k.portMemoryConfig("port0");
    EXPECT_EQ(cfg.type, vrt::MemoryRangeType::HBM_VNOC);
}

TEST(KernelMemoryConfigTest, PortMemoryConfigUnknownTargetThrows) {
    auto k = makeTestKernel();
    k.setConnections({{"port0", "INVALID"}});
    EXPECT_THROW(k.portMemoryConfig("port0"), std::runtime_error);
}

TEST(KernelMemoryConfigTest, PortMemoryConfigNoConnectionThrows) {
    auto k = makeTestKernel();
    k.setConnections({{"port0", "DDR"}});
    EXPECT_THROW(k.portMemoryConfig("nonexistent"), std::runtime_error);
}

TEST(KernelMemoryConfigTest, ArgMemoryConfigByName) {
    auto k = makeTestKernel({makeArg(0, "input", "buffer", 0x10, 64, false, true, "m_axi_gmem0")});
    k.setConnections({{"m_axi_gmem0", "DDR"}});
    auto cfg = k.argMemoryConfig("input");
    EXPECT_EQ(cfg.type, vrt::MemoryRangeType::DDR);
}

TEST(KernelMemoryConfigTest, ArgMemoryConfigNoPortThrows) {
    auto k = makeTestKernel({makeArg(0, "scalar_arg", "scalar", 0x10, 32, false, true, "")});
    k.setConnections({});
    EXPECT_THROW(k.argMemoryConfig("scalar_arg"), std::runtime_error);
}
