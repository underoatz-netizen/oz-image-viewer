// oz/runner/collect.hpp - parse a subject's marker JSONL stream.
#pragma once

#include "oz/markers/markers.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oz::runner {

struct collected_run {
    std::vector<markers::marker_record> markers;
    std::vector<markers::validation_failure> validation;  // duplicate/missing/out-of-order
    std::vector<std::string> runner_issues;  // unknown marker / malformed line / no markers
    std::size_t lines_parsed = 0;
};

// Parses lines of the form
//   {"marker":"ProcessStart","raw_ticks":123,"relative_ns":0,"thread_id":1234}
// Unknown marker names and malformed lines are reported as runner issues.
collected_run parse_marker_stream(std::string_view text);

}  // namespace oz::runner