// oz/runner/summarize.hpp - median/P95/P99 by identical scenario.
#pragma once

#include "oz/runner/record.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oz::runner {

struct summary_row {
    std::string scenario;
    std::string marker;
    std::size_t count = 0;
    std::int64_t median_ns = 0;
    std::int64_t p95_ns = 0;
    std::int64_t p99_ns = 0;
};

// Nearest-rank percentile: for N sorted samples, P(p) = sorted[ceil(p*N/100)-1].
// Median is P50. See docs/benchmark.md.
std::int64_t nearest_rank_percentile(const std::vector<std::int64_t>& sorted, double p);

// Groups marker records by (scenario, marker). Warmup repetitions are
// excluded unless include_warmups is true.
std::vector<summary_row> summarize_records(const std::vector<record>& records,
                                           bool include_warmups);

std::string summary_to_csv(const std::vector<summary_row>& rows);
json::value summary_to_json(const std::vector<summary_row>& rows);

// One canonical kind=summary record per metric (median/p95/p99).
std::vector<record> summary_records_to_jsonl(const std::vector<summary_row>& rows);

}  // namespace oz::runner