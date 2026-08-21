// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

// Guards the CMake-built default VBIN: it must be a valid Simulation VBIN that
// unpacks to a `vpp_sim` executable + a parseable system map.  SLASH_SYSEMU_DEFAULT_VBIN_BUILD
// is the build-tree path to the artifact, injected by tests/CMakeLists.txt.

#include "vbin.h"

#include <filesystem>

#include <gtest/gtest.h>

#ifndef SLASH_SYSEMU_DEFAULT_VBIN_BUILD
#error "SLASH_SYSEMU_DEFAULT_VBIN_BUILD must be defined by the build system"
#endif

namespace {

TEST(DefaultVbinArtifactTest, ExistsUnpacksAndIsSimulation) {
    const std::string path = SLASH_SYSEMU_DEFAULT_VBIN_BUILD;
    ASSERT_TRUE(std::filesystem::exists(path)) << "default VBIN not built at " << path;

    auto result = slash_sysemu::unpack_vbin(path);
    ASSERT_TRUE(result.has_value())
        << "unpack failed: " << result.error().message;

    const slash_sysemu::Vbin& vbin = result.value();
    EXPECT_EQ(slash_sysemu::Platform::Simulation, vbin.map.platform);
    EXPECT_EQ("vpp_sim", vbin.executable.filename().string());
    EXPECT_TRUE(std::filesystem::exists(vbin.executable));
    EXPECT_EQ("system_map.xml", vbin.system_map.filename().string());
}

} // namespace
