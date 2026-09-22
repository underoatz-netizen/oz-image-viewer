#include "oz/runner/import.hpp"

#include "oz/runner/json.hpp"

#include <cstdlib>

namespace oz::runner {

std::vector<record> import_samples(std::string_view text, const import_options& opts,
                                   std::vector<std::string>& errors) {
    std::vector<record> out;
    const std::string imported_utc = utc_now_iso8601();

    if (opts.format == "jsonl") {
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
            auto parsed = json::parse(line);
            if (!parsed) {
                errors.push_back("import: malformed JSON line: " + parsed.error());
                continue;
            }
            const json::value& v = *parsed;
            if (!json::is_object(v)) {
                errors.push_back("import: line is not an object");
                continue;
            }
            auto ns = json::get_int(v, "relative_ns");
            if (!ns) {
                ns = json::get_int(v, "sample_ns");
            }
            if (!ns) {
                errors.push_back("import: line missing relative_ns/sample_ns");
                continue;
            }
            record r;
            r.kind = record_kind::import;
            r.scenario = json::get_string(v, "scenario").value_or(opts.scenario.value_or(""));
            r.cache = opts.cache.value_or("cold");
            r.subject = opts.subject.value_or("external");
            r.marker = opts.marker;
            r.relative_ns = *ns;
            r.source = "external";
            r.imported_utc = imported_utc;
            out.push_back(std::move(r));
        }
        return out;
    }

    // CSV (simple: no quoted-field support; documented limitation)
    std::size_t start = 0;
    bool first = true;
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
        if (first) {
            first = false;
            if (line.find("scenario") != std::string_view::npos &&
                (line.find("relative_ns") != std::string_view::npos ||
                 line.find("sample_ns") != std::string_view::npos)) {
                continue;  // header row
            }
        }
        const auto comma = line.find(',');
        std::string scenario;
        std::string ns_str;
        if (comma == std::string_view::npos) {
            ns_str = std::string(line);
            scenario = opts.scenario.value_or("");
        } else {
            scenario = std::string(line.substr(0, comma));
            ns_str = std::string(line.substr(comma + 1));
        }
        char* endp = nullptr;
        const long long ns = std::strtoll(ns_str.c_str(), &endp, 10);
        if (endp == ns_str.c_str()) {
            errors.push_back("import: invalid number in row: " + ns_str);
            continue;
        }
        record r;
        r.kind = record_kind::import;
        r.scenario = scenario;
        r.cache = opts.cache.value_or("cold");
        r.subject = opts.subject.value_or("external");
        r.marker = opts.marker;
        r.relative_ns = ns;
        r.source = "external";
        r.imported_utc = imported_utc;
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace oz::runner