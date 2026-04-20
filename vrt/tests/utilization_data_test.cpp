#include <gtest/gtest.h>
#include <vrt/parser/utilization_data.hpp>

TEST(UtilizationDataTest, ResourceMetricsDefaults) {
    vrt::ResourceMetrics m{};
    EXPECT_EQ(m.totalPplocs, 0u);
    EXPECT_EQ(m.totalLuts, 0u);
    EXPECT_EQ(m.lutram, 0u);
    EXPECT_EQ(m.srl, 0u);
    EXPECT_EQ(m.ff, 0u);
    EXPECT_EQ(m.ramb36, 0u);
    EXPECT_EQ(m.ramb18, 0u);
    EXPECT_EQ(m.ramb, 0u);
    EXPECT_EQ(m.uram, 0u);
    EXPECT_EQ(m.dsp, 0u);
}

TEST(UtilizationDataTest, OptionalFieldsDefaultToNullopt) {
    vrt::ResourceMetrics m{};
    EXPECT_FALSE(m.totalLutsPct.has_value());
    EXPECT_FALSE(m.lutramPct.has_value());
    EXPECT_FALSE(m.srlPct.has_value());
    EXPECT_FALSE(m.ffPct.has_value());
    EXPECT_FALSE(m.ramb36Pct.has_value());
    EXPECT_FALSE(m.ramb18Pct.has_value());
    EXPECT_FALSE(m.uramPct.has_value());
    EXPECT_FALSE(m.dspPct.has_value());
}

TEST(UtilizationDataTest, ResourceMetricsAssignment) {
    vrt::ResourceMetrics m{};
    m.totalLuts = 1000;
    m.totalLutsPct = 5.2f;
    EXPECT_EQ(m.totalLuts, 1000u);
    ASSERT_TRUE(m.totalLutsPct.has_value());
    EXPECT_FLOAT_EQ(m.totalLutsPct.value(), 5.2f);
}

TEST(UtilizationDataTest, UtilizationCellConstruction) {
    vrt::UtilizationCell cell;
    cell.instance = "k0";
    cell.module = "myKernel";
    cell.pr = "pblock_0";
    cell.metrics.totalLuts = 400;
    EXPECT_EQ(cell.instance, "k0");
    EXPECT_EQ(cell.module, "myKernel");
    EXPECT_EQ(cell.metrics.totalLuts, 400u);
}

TEST(UtilizationDataTest, UtilizationReportSlashPresent) {
    vrt::UtilizationReport report;
    report.slash.name = "slash";
    report.slash.totals.totalLuts = 500;
    EXPECT_EQ(report.slash.name, "slash");
    EXPECT_EQ(report.slash.totals.totalLuts, 500u);
    EXPECT_FALSE(report.serviceLayer.has_value());
}
