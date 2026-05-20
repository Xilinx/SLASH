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
#include <vrt/parser/utilization_parser.hpp>

#include <filesystem>

#include "test_helpers.hpp"

class UtilizationParserTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;

    void SetUp() override { tmpDir = makeTempDir("util-parser-test"); }
    void TearDown() override { std::filesystem::remove_all(tmpDir); }

    std::string writeXml(const std::string& content) {
        return writeTempFile(tmpDir, "utilization.xml", content);
    }
};

TEST_F(UtilizationParserTest, ParseSlashBlock) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="1000" ff="500" dsp="10" ramb36="5" ramb18="3" uram="2"/>
  </slash>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    auto& report = parser.getReport();
    EXPECT_EQ(report.slash.name, "slash");
    EXPECT_EQ(report.slash.totals.totalLuts, 1000u);
    EXPECT_EQ(report.slash.totals.ff, 500u);
    EXPECT_EQ(report.slash.totals.dsp, 10u);
    EXPECT_EQ(report.slash.totals.ramb36, 5u);
    EXPECT_EQ(report.slash.totals.ramb18, 3u);
    EXPECT_EQ(report.slash.totals.uram, 2u);
}

TEST_F(UtilizationParserTest, ParseSlashBlockWithKernels) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="1000"/>
    <kernels>
      <kernel instance="k0" module="myKernel">
        <totals total_luts="400"/>
      </kernel>
    </kernels>
    <kernel_sum total_luts="400"/>
  </slash>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    auto& report = parser.getReport();
    ASSERT_TRUE(report.slash.subhierarchy.has_value());
    ASSERT_EQ(report.slash.subhierarchy->cells.size(), 1u);
    EXPECT_EQ(report.slash.subhierarchy->cells[0].instance, "k0");
    EXPECT_EQ(report.slash.subhierarchy->cells[0].module, "myKernel");
    EXPECT_EQ(report.slash.subhierarchy->cells[0].metrics.totalLuts, 400u);
    EXPECT_EQ(report.slash.subhierarchy->subhierarchySum.totalLuts, 400u);
}

TEST_F(UtilizationParserTest, ParseSlashBlockWithSlashLogic) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="1000"/>
    <slash_logic>
      <cell instance="sl0" module="slashCell">
        <totals total_luts="100"/>
      </cell>
    </slash_logic>
    <slash_logic_sum total_luts="100"/>
  </slash>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    auto& sub = parser.getReport().slash.subhierarchy;
    ASSERT_TRUE(sub.has_value());
    ASSERT_EQ(sub->slashLogic.size(), 1u);
    EXPECT_EQ(sub->slashLogic[0].instance, "sl0");
    EXPECT_EQ(sub->slashLogicSum.totalLuts, 100u);
}

TEST_F(UtilizationParserTest, ParseServiceLayer) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="1000"/>
  </slash>
  <service_layer>
    <totals total_luts="200" ff="150"/>
  </service_layer>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    auto& report = parser.getReport();
    ASSERT_TRUE(report.serviceLayer.has_value());
    EXPECT_EQ(report.serviceLayer->name, "service_layer");
    EXPECT_EQ(report.serviceLayer->totals.totalLuts, 200u);
    EXPECT_EQ(report.serviceLayer->totals.ff, 150u);
}

TEST_F(UtilizationParserTest, ParseResourceMetricsPercentages) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="1000" total_luts_pct="5.2" ff_pct="3.1"/>
  </slash>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    auto& m = parser.getReport().slash.totals;
    ASSERT_TRUE(m.totalLutsPct.has_value());
    EXPECT_FLOAT_EQ(m.totalLutsPct.value(), 5.2f);
    ASSERT_TRUE(m.ffPct.has_value());
    EXPECT_FLOAT_EQ(m.ffPct.value(), 3.1f);
    EXPECT_FALSE(m.dspPct.has_value());
}

TEST_F(UtilizationParserTest, MissingSlashBlockThrows) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization></utilization>)");
    vrt::UtilizationParser parser(path);
    EXPECT_THROW(parser.parse(), std::runtime_error);
}

TEST_F(UtilizationParserTest, InvalidXmlThrows) {
    auto path = writeTempFile(tmpDir, "bad.xml", "not valid xml <<<<");
    EXPECT_THROW(vrt::UtilizationParser parser(path), std::runtime_error);
}

TEST_F(UtilizationParserTest, SlashBlockWithoutServiceLayer) {
    auto path = writeXml(R"(<?xml version="1.0"?>
<utilization>
  <slash>
    <totals total_luts="500"/>
  </slash>
</utilization>)");
    vrt::UtilizationParser parser(path);
    parser.parse();
    EXPECT_FALSE(parser.getReport().serviceLayer.has_value());
}
