// oz/runner/import.hpp - import external first-present samples into canonical
// JSONL (bridge for the pending external collection protocol).
#pragma once

#include "oz/runner/record.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oz::runner {

struct import_options {
    std::string format = "csv";  // "csv" | "jsonl"
    std::optional<std::string> subject;
    std::optional<std::string> cache;
    std::optional<std::string> scenario;
    std::string marker = "FirstPresent";
};

// CSV: optional header "scenario,relative_ns" (or "sample_ns"); rows are
// "scenario,ns" or a single "ns" column when --scenario is given.
// JSONL: one object per line with "scenario" (optional) and
// "relative_ns"/"sample_ns".
std::vector<record> import_samples(std::string_view text, const import_options& opts,
                                   std::vector<std::string>& errors);

}  // namespace oz::runner