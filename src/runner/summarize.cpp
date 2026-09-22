#include "oz/runner/summarize.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace oz::runner {
namespace {

// RFC 4180-style field quoting: fields containing a comma, double quote, CR
// or LF are wrapped in double quotes with internal quotes doubled.
std::string csv_field(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

}  // namespace

std::int64_t nearest_rank_percentile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    std::size_t rank = static_cast<std::size_t>(
        std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    if (rank < 1) {
        rank = 1;
    }
    if (rank > sorted.size()) {
        rank = sorted.size();
    }
    return sorted[rank - 1];
}

std::vector<summary_row> summarize_records(const std::vector<record>& records,
                                           bool include_warmups) {
    std::map<std::pair<std::string, std::string>, std::vector<std::int64_t>> groups;
    for (const auto& r : records) {
        if (r.warmup && !include_warmups) {
            continue;
        }
        if (r.kind == record_kind::marker) {
            groups[{r.scenario, r.marker}].push_back(r.relative_ns);
        } else if (r.kind == record_kind::external_visible && r.status == "matched") {
            // ExternalFirstImageVisible samples; non-matched captures are
            // recorded explicitly but never summarized as measurements.
            const std::string endpoint =
                r.endpoint.empty() ? "ExternalFirstImageVisible" : r.endpoint;
            groups[{r.scenario, endpoint}].push_back(r.relative_ns);
        }
    }

    std::vector<summary_row> rows;
    rows.reserve(groups.size());
    for (auto& [key, samples] : groups) {
        std::sort(samples.begin(), samples.end());
        summary_row row;
        row.scenario = key.first;
        row.marker = key.second;
        row.count = samples.size();
        row.median_ns = nearest_rank_percentile(samples, 50.0);
        row.p95_ns = nearest_rank_percentile(samples, 95.0);
        row.p99_ns = nearest_rank_percentile(samples, 99.0);
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const summary_row& a, const summary_row& b) {
        if (a.scenario != b.scenario) {
            return a.scenario < b.scenario;
        }
        const auto ma = markers::marker_from_name(a.marker);
        const auto mb = markers::marker_from_name(b.marker);
        if (ma && mb) {
            return *ma < *mb;
        }
        return a.marker < b.marker;
    });
    return rows;
}

std::string summary_to_csv(const std::vector<summary_row>& rows) {
    std::string out = "scenario,marker,count,median_ns,p95_ns,p99_ns,convention\n";
    for (const auto& r : rows) {
        out += csv_field(r.scenario) + "," + csv_field(r.marker) + "," +
               std::to_string(r.count) + "," + std::to_string(r.median_ns) + "," +
               std::to_string(r.p95_ns) + "," + std::to_string(r.p99_ns) +
               ",nearest-rank\n";
    }
    return out;
}

json::value summary_to_json(const std::vector<summary_row>& rows) {
    auto arr = json::make_array();
    for (const auto& r : rows) {
        auto o = json::make_object();
        json::set_member(o, "scenario", json::make_string(r.scenario));
        json::set_member(o, "marker", json::make_string(r.marker));
        json::set_member(o, "count", json::make_int(static_cast<std::int64_t>(r.count)));
        json::set_member(o, "median_ns", json::make_int(r.median_ns));
        json::set_member(o, "p95_ns", json::make_int(r.p95_ns));
        json::set_member(o, "p99_ns", json::make_int(r.p99_ns));
        json::set_member(o, "convention", json::make_string("nearest-rank"));
        json::push_back(arr, std::move(o));
    }
    return arr;
}

std::vector<record> summary_records_to_jsonl(const std::vector<summary_row>& rows) {
    std::vector<record> out;
    out.reserve(rows.size() * 3);
    for (const auto& r : rows) {
        for (const auto& [metric, value] :
             {std::pair<const char*, std::int64_t>{"median", r.median_ns},
              {"p95", r.p95_ns}, {"p99", r.p99_ns}}) {
            record rec;
            rec.kind = record_kind::summary;
            rec.scenario = r.scenario;
            rec.marker = r.marker;
            rec.count = r.count;
            rec.convention = "nearest-rank";
            rec.metric = metric;
            rec.value_ns = value;
            out.push_back(std::move(rec));
        }
    }
    return out;
}

}  // namespace oz::runner