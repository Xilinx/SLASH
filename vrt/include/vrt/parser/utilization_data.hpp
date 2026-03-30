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

/**
 * @file utilization_data.hpp
 * @brief Data structures for FPGA resource utilization reports.
 */

#ifndef VRT_UTILIZATION_DATA_HPP
#define VRT_UTILIZATION_DATA_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vrt {

/**
 * @brief FPGA resource counts and optional utilization percentages.
 */
struct ResourceMetrics {
    uint32_t totalPplocs = 0;  ///< Total physical placement locations
    uint32_t totalLuts = 0;    ///< Total look-up tables used
    uint32_t lutram = 0;       ///< LUTs used as distributed RAM
    uint32_t srl = 0;          ///< LUTs used as shift registers
    uint32_t ff = 0;           ///< Flip-flops used
    uint32_t ramb36 = 0;       ///< 36 Kb block RAMs used
    uint32_t ramb18 = 0;       ///< 18 Kb block RAMs used
    uint32_t ramb = 0;         ///< Total block RAMs used
    uint32_t uram = 0;         ///< UltraRAMs used
    uint32_t dsp = 0;          ///< DSP slices used

    std::optional<float> totalLutsPct;  ///< LUT utilization percentage
    std::optional<float> lutramPct;     ///< LUTRAM utilization percentage
    std::optional<float> srlPct;        ///< SRL utilization percentage
    std::optional<float> ffPct;         ///< FF utilization percentage
    std::optional<float> ramb36Pct;     ///< RAMB36 utilization percentage
    std::optional<float> ramb18Pct;     ///< RAMB18 utilization percentage
    std::optional<float> uramPct;       ///< URAM utilization percentage
    std::optional<float> dspPct;        ///< DSP utilization percentage
};

/**
 * @brief Per-instance resource metrics for a single module.
 */
struct UtilizationCell {
    std::string instance;       ///< Module instance name
    std::string module;         ///< Module definition name
    std::string pr;             ///< Partial reconfiguration region
    ResourceMetrics metrics;    ///< Resource counts for this instance
};

/**
 * @brief Hierarchical grouping of user logic and framework overhead cells.
 */
struct Subhierarchy {
    std::vector<UtilizationCell> cells;       ///< User logic cells
    std::vector<UtilizationCell> slashLogic;  ///< SLASH framework overhead cells
    ResourceMetrics subhierarchySum;          ///< Aggregated metrics for user logic
    ResourceMetrics slashLogicSum;            ///< Aggregated metrics for framework logic
};

/**
 * @brief Top-level utilization block (e.g. SLASH framework or service layer).
 */
struct UtilizationBlock {
    std::string name;                           ///< Block name
    std::string instance;                       ///< Block instance name
    std::string pr;                             ///< Partial reconfiguration region
    ResourceMetrics totals;                     ///< Block-level resource totals
    std::optional<Subhierarchy> subhierarchy;   ///< Detailed breakdown (if available)
};

/**
 * @brief Complete FPGA design utilization report.
 */
struct UtilizationReport {
    UtilizationBlock slash;                          ///< SLASH framework block (always present)
    std::optional<UtilizationBlock> serviceLayer;    ///< Optional service layer block
};

}  // namespace vrt

#endif  // VRT_UTILIZATION_DATA_HPP
