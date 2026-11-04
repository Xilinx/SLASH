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

/**
 * @file latency_stats.hpp
 * @brief Small, dependency-free summaries for latency samples.
 */

#ifndef RP1_LATENCY_STATS_HPP
#define RP1_LATENCY_STATS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rp1_bench {

/**
 * @brief Summary of one non-empty set of unsigned latency samples.
 */
struct Summary {
    /// Number of samples represented by this summary.
    std::size_t count = 0;
    /// Smallest observed sample.
    std::uint64_t minimum = 0;
    /// Nearest-rank 50th percentile.
    std::uint64_t p50 = 0;
    /// Nearest-rank 95th percentile.
    std::uint64_t p95 = 0;
    /// Nearest-rank 99th percentile.
    std::uint64_t p99 = 0;
    /// Largest observed sample.
    std::uint64_t maximum = 0;
    /// Arithmetic mean, retained as a floating-point value.
    long double mean = 0;
};

/**
 * @brief Summarize latency samples without modifying the caller's ordering.
 *
 * Percentiles use the nearest-rank definition. The function throws
 * @c std::invalid_argument when @p samples is empty.
 */
inline Summary summarize(const std::vector<std::uint64_t>& samples) {
    if (samples.empty()) {
        throw std::invalid_argument("summarize: samples must not be empty");
    }

    std::vector<std::uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    /*
     * Nearest rank is stable for small microbenchmark sets and always returns
     * an observed value. Integer arithmetic avoids rounding drift.
     */
    const auto percentile = [&sorted](std::size_t percentage) {
        const std::size_t rank =
            (percentage * sorted.size() + 99u) / 100u;
        return sorted[rank == 0 ? 0 : rank - 1u];
    };

    long double total = 0;
    for (std::uint64_t sample : samples) {
        total += static_cast<long double>(sample);
    }

    return Summary{
        samples.size(),
        sorted.front(),
        percentile(50),
        percentile(95),
        percentile(99),
        sorted.back(),
        total / static_cast<long double>(samples.size()),
    };
}

/**
 * @brief Count DDR records produced from normal events and flush markers.
 *
 * A staging page flushes after normal events consume all but its final slot.
 * FLUSH_START occupies that slot and FLUSH_END occupies the first slot of the
 * new page, so later pages accept one fewer normal event. The function throws
 * for staging pages smaller than three entries or if the result overflows.
 */
inline std::size_t traceEntriesWithFlushMarkers(
    std::size_t normalEntries, std::size_t stagingEntries = 256) {
    if (stagingEntries < 3) {
        throw std::invalid_argument(
            "traceEntriesWithFlushMarkers: staging page is too small");
    }
    if (normalEntries < stagingEntries - 1) return normalEntries;

    const std::size_t flushes =
        1 + (normalEntries - (stagingEntries - 1)) /
                (stagingEntries - 2);
    if (flushes >
        (std::numeric_limits<std::size_t>::max() - normalEntries) / 2) {
        throw std::overflow_error(
            "traceEntriesWithFlushMarkers: result overflows size_t");
    }
    return normalEntries + 2 * flushes;
}

}  // namespace rp1_bench

#endif  // RP1_LATENCY_STATS_HPP
