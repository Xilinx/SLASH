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

#include "latency_stats.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

/**
 * @brief Exercise ordering, nearest-rank percentiles, mean, and empty input.
 */
int main() {
    const rp1_bench::Summary summary =
        rp1_bench::summarize({100, 10, 40, 20, 30});

    if (summary.count != 5 ||
        summary.minimum != 10 ||
        summary.p50 != 30 ||
        summary.p95 != 100 ||
        summary.p99 != 100 ||
        summary.maximum != 100 ||
        std::fabs(summary.mean - 40.0L) >= 0.0001L) {
        return 1;
    }

    bool rejectedEmpty = false;
    try {
        (void)rp1_bench::summarize({});
    } catch (const std::invalid_argument&) {
        rejectedEmpty = true;
    }
    if (!rejectedEmpty) return 1;

    if (rp1_bench::traceEntriesWithFlushMarkers(254) != 254 ||
        rp1_bench::traceEntriesWithFlushMarkers(255) != 257 ||
        rp1_bench::traceEntriesWithFlushMarkers(508) != 510 ||
        rp1_bench::traceEntriesWithFlushMarkers(509) != 513) {
        return 1;
    }
    try {
        (void)rp1_bench::traceEntriesWithFlushMarkers(2, 2);
    } catch (const std::invalid_argument&) {
        return 0;
    }
    return 1;
}
