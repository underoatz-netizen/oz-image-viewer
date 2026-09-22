#include "oz/runner/collect.hpp"

#include "oz/runner/json.hpp"

namespace oz::runner {

collected_run parse_marker_stream(std::string_view text) {
    collected_run out;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        ++out.lines_parsed;
        auto parsed = json::parse(line);
        if (!parsed) {
            out.runner_issues.push_back("malformed JSON line: " + parsed.error());
            continue;
        }
        const json::value& v = *parsed;
        if (!json::is_object(v)) {
            out.runner_issues.push_back("marker line is not a JSON object");
            continue;
        }
        const auto name = json::get_string(v, "marker");
        if (!name) {
            out.runner_issues.push_back("marker line missing 'marker'");
            continue;
        }
        const auto id = markers::marker_from_name(*name);
        if (!id) {
            out.runner_issues.push_back("unknown marker '" + *name + "'");
            continue;
        }
        markers::marker_record r;
        r.id = *id;
        r.raw_ticks = static_cast<std::uint64_t>(json::get_int(v, "raw_ticks").value_or(0));
        r.relative_ns = json::get_int(v, "relative_ns").value_or(0);
        r.thread_id = static_cast<std::uint32_t>(json::get_int(v, "thread_id").value_or(0));
        out.markers.push_back(r);
    }
    out.validation = markers::validate_records(out.markers);
    if (out.markers.empty()) {
        out.runner_issues.push_back("no markers collected");
    }
    return out;
}

}  // namespace oz::runner