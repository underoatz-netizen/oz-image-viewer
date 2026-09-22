// oz/runner/run.hpp - plan execution (used by the CLI and by tests).
#pragma once

#include "oz/runner/plan.hpp"
#include "oz/runner/record.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oz::runner {

struct run_options {
    std::map<std::string, std::string> defines;  // {{key}} placeholder values
    std::optional<std::string> scenario_filter;  // substring filter on scenario key
    std::optional<std::uint32_t> repetitions;
    std::optional<std::uint32_t> warmups;
};

// Runs every image in the plan (warmups first, then repetitions) and returns
// canonical records (run_start / marker / run_end). On a plan-level failure
// (subject disabled or executable missing) returns an empty vector and sets
// error. Placeholders: {{image_path}} per image, {{fixture_exe}} from
// --set or the OZBENCH_FIXTURE_EXE environment variable, plus any --set keys.
std::vector<record> run_plan(const plan& p, const run_options& opts, std::string& error);

}  // namespace oz::runner