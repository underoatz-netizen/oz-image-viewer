// ozbench_tests - focused Stage 0 tests (no test framework).
//
// Usage: ozbench_tests <source_dir> <fixture_exe>
//   source_dir  - repository root (to locate plans/fixture.json)
//   fixture_exe - path to the built ozbench_fixture executable
//
// Registered with CTest as "ozbench_tests".
#include "oz/markers/markers.hpp"
#include "oz/runner/collect.hpp"
#include "oz/runner/import.hpp"
#include "oz/runner/json.hpp"
#include "oz/runner/plan.hpp"
#include "oz/runner/process.hpp"
#include "oz/runner/record.hpp"
#include "oz/runner/run.hpp"
#include "oz/runner/summarize.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace test {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL " << file << ":" << line << ": " << expr << "\n";
    }
}
}  // namespace test

#define CHECK(expr) ::test::check(!!(expr), #expr, __FILE__, __LINE__)

namespace {

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// --- marker library ---------------------------------------------------------

void test_marker_names() {
    CHECK(oz::markers::marker_count == 12);
    for (std::size_t i = 0; i < oz::markers::marker_count; ++i) {
        const auto id = static_cast<oz::markers::marker_id>(i);
        const auto name = oz::markers::marker_name(id);
        CHECK(!name.empty());
        CHECK(oz::markers::marker_from_name(name) == id);
    }
    CHECK(oz::markers::marker_from_name("BogusMarker") == std::nullopt);
    CHECK(oz::markers::marker_name(static_cast<oz::markers::marker_id>(99)).empty());
    CHECK(oz::markers::marker_name(oz::markers::marker_id::process_start) == "ProcessStart");
    CHECK(oz::markers::marker_name(oz::markers::marker_id::first_present) == "FirstPresent");
    CHECK(oz::markers::marker_name(oz::markers::marker_id::folder_enumeration_complete) ==
          "FolderEnumerationComplete");
}

void test_session_basic() {
    oz::markers::marker_session session;
    const auto order = {
        oz::markers::marker_id::renderer_ready,
        oz::markers::marker_id::file_opened,
        oz::markers::marker_id::codec_detected,
        oz::markers::marker_id::header_parsed,
        oz::markers::marker_id::decode_start,
        oz::markers::marker_id::decode_complete,
        oz::markers::marker_id::gpu_upload_start,
        oz::markers::marker_id::gpu_upload_complete,
        oz::markers::marker_id::first_present,
        oz::markers::marker_id::folder_enumeration_start,
        oz::markers::marker_id::folder_enumeration_complete,
    };
    for (const auto id : order) {
        session.record(id);
    }
CHECK(session.records().size() == 12);
    CHECK(session.records().front().id == oz::markers::marker_id::process_start);
    CHECK(session.records().front().relative_ns < 1'000'000);  // near session start
    CHECK(session.records().front().thread_id == GetCurrentThreadId());
    CHECK(session.records().front().raw_ticks != 0);
    CHECK(session.validate().empty());
    bool monotonic = true;
    for (std::size_t i = 1; i < session.records().size(); ++i) {
        if (session.records()[i].relative_ns < session.records()[i - 1].relative_ns) {
            monotonic = false;
        }
    }
    CHECK(monotonic);
CHECK(oz::markers::qpc_frequency() != 0);
    CHECK(oz::markers::ticks_to_ns(1'000'000, 1'000'000) == 1'000'000'000);  // 1 s @ 1 MHz
    CHECK(oz::markers::ticks_to_ns(0, 1'000'000) == 0);
}

void test_validation_defects() {
    {
        oz::markers::marker_session session;
        session.record(oz::markers::marker_id::decode_start);
        session.record(oz::markers::marker_id::decode_start);
        const auto failures = session.validate();
        bool found = false;
        for (const auto& f : failures) {
            if (f.kind == oz::markers::validation_kind::duplicate &&
                f.id == oz::markers::marker_id::decode_start) {
                found = true;
            }
        }
        CHECK(found);
    }
    {
        oz::markers::marker_session session;
        session.record(oz::markers::marker_id::first_present);
        session.record(oz::markers::marker_id::decode_complete);
        const auto failures = session.validate();
        bool found = false;
        for (const auto& f : failures) {
            if (f.kind == oz::markers::validation_kind::out_of_order &&
                f.id == oz::markers::marker_id::decode_complete) {
                found = true;
            }
        }
        CHECK(found);
    }
    {
        oz::markers::marker_session session;
        session.record(oz::markers::marker_id::decode_complete);
        const auto failures = session.validate();
        bool found = false;
        for (const auto& f : failures) {
            if (f.kind == oz::markers::validation_kind::missing &&
                f.id == oz::markers::marker_id::decode_start) {
                found = true;
            }
        }
        CHECK(found);
    }
    // A duplicated marker is reported once as duplicate and is NOT also
    // reported as out_of_order (non-duplicative multi-error behavior).
    {
        std::vector<oz::markers::marker_record> records;
        const auto mk = [](oz::markers::marker_id id) {
            oz::markers::marker_record r;
            r.id = id;
            r.raw_ticks = 0;
            r.relative_ns = 0;
            r.thread_id = 0;
            return r;
        };
        records.push_back(mk(oz::markers::marker_id::decode_start));
        records.push_back(mk(oz::markers::marker_id::first_present));
        records.push_back(mk(oz::markers::marker_id::decode_start));
        const auto failures = oz::markers::validate_records(records);
        bool found_dup = false;
        bool found_ooo = false;
        for (const auto& f : failures) {
            if (f.kind == oz::markers::validation_kind::duplicate &&
                f.id == oz::markers::marker_id::decode_start) {
                found_dup = true;
            }
            if (f.kind == oz::markers::validation_kind::out_of_order &&
                f.id == oz::markers::marker_id::decode_start) {
                found_ooo = true;
            }
        }
        CHECK(found_dup);
        CHECK(!found_ooo);
    }
}

// --- json -------------------------------------------------------------------

void test_json_roundtrip() {
    auto o = oz::json::make_object();
    oz::json::set_member(o, "name", oz::json::make_string("a\"b\\c\nd"));
    oz::json::set_member(o, "count", oz::json::make_int(42));
    oz::json::set_member(o, "ratio", oz::json::make_double(1.5));
    oz::json::set_member(o, "flag", oz::json::make_bool(true));
    oz::json::set_member(o, "nothing", oz::json::make_null());
    auto arr = oz::json::make_array();
    oz::json::push_back(arr, oz::json::make_int(1));
    oz::json::push_back(arr, oz::json::make_string("x"));
    oz::json::set_member(o, "list", std::move(arr));

    const std::string dumped = oz::json::dump(o);
    auto parsed = oz::json::parse(dumped);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(oz::json::is_object(*parsed));
        CHECK(oz::json::get_string(*parsed, "name") == "a\"b\\c\nd");
        CHECK(oz::json::get_int(*parsed, "count") == 42);
        CHECK(oz::json::get_bool(*parsed, "flag") == true);
        CHECK(oz::json::is_null(*oz::json::find_member(*parsed, "nothing")));
        const oz::json::value* list = oz::json::find_member(*parsed, "list");
        CHECK(list && oz::json::is_array(*list));
        if (list) {
            CHECK(oz::json::array_size(*list) == 2);
            CHECK(oz::json::as_int(oz::json::array_at(*list, 0)) == 1);
        }
    }
    // unicode escape round-trip
    auto u = oz::json::parse("\"\\u00e9\\ud83d\\ude00\"");
    CHECK(u.has_value());
    if (u) {
        CHECK(oz::json::as_string(*u) == "\xC3\xA9\xF0\x9F\x98\x80");
    }
    // errors
    CHECK(!oz::json::parse("{").has_value());
    CHECK(!oz::json::parse("{\"a\":1} trailing").has_value());
    CHECK(!oz::json::parse("").has_value());
}

// --- plan -------------------------------------------------------------------

void test_plan_parse(const std::wstring& source_dir) {
    const std::string text = read_file(source_dir + L"\\plans\\fixture.json");
    CHECK(!text.empty());
    auto pr = oz::runner::parse_plan(text);
    CHECK(pr.errors.empty());
    if (pr.value) {
        CHECK(pr.value->schema == "oz.plan.v1");
        CHECK(pr.value->subject.name == "fixture");
        CHECK(pr.value->subject.executable == "{{fixture_exe}}");
        CHECK(pr.value->collector == "markers");
        CHECK(pr.value->cache == "cold");
        CHECK(pr.value->repetitions == 5);
        CHECK(pr.value->warmups == 1);
        CHECK(pr.value->images.size() == 1);
        CHECK(pr.value->args.size() == 4);
        CHECK(pr.value->args[3] == "{{image_path}}");
        CHECK(oz::runner::scenario_key(*pr.value, pr.value->images[0]) ==
              "fixture|cold|sample.jpg");
    }
    // invalid plan
    auto bad = oz::runner::parse_plan("{}");
    CHECK(!bad.errors.empty());
    auto bad2 = oz::runner::parse_plan("not json");
    CHECK(!bad2.errors.empty());
// disabled template parses; availability is machine-dependent, so only
    // the disabled flag is asserted here (skip behavior is covered by the
    // ozbench_cli_validate_skip CTest)
const std::string iv = read_file(source_dir + L"\\plans\\irfanview.json");
    auto ivp = oz::runner::parse_plan(iv);
    CHECK(ivp.errors.empty());
    if (ivp.value) {
        CHECK(!ivp.value->subject.enabled);
    }
}

void test_plan_rejects_negative_and_huge_counts() {
    // Negative timeout_ms / repetitions / warmups must be rejected before
    // narrowing (a negative value would otherwise wrap to a huge uint32 and
    // cause a pathological run loop).
    auto pr = oz::runner::parse_plan(
        "{\"schema\":\"oz.plan.v1\",\"name\":\"neg\","
        "\"subject\":{\"name\":\"s\",\"executable\":\"x.exe\"},"
        "\"timeout_ms\":-1,\"repetitions\":-5,\"warmups\":-2,"
        "\"images\":[{\"path\":\"a.jpg\"}]}");
    CHECK(!pr.errors.empty());
    bool found_timeout = false, found_reps = false, found_warmups = false;
    for (const auto& e : pr.errors) {
        if (e.find("timeout_ms") != std::string::npos) found_timeout = true;
        if (e.find("repetitions") != std::string::npos) found_reps = true;
        if (e.find("warmups") != std::string::npos) found_warmups = true;
    }
    CHECK(found_timeout);
    CHECK(found_reps);
    CHECK(found_warmups);

    // Huge values beyond the documented caps are rejected too.
    auto pr2 = oz::runner::parse_plan(
        "{\"schema\":\"oz.plan.v1\",\"name\":\"huge\","
        "\"subject\":{\"name\":\"s\",\"executable\":\"x.exe\"},"
        "\"repetitions\":99999999999,\"warmups\":99999999999,"
        "\"images\":[{\"path\":\"a.jpg\"}]}");
    CHECK(!pr2.errors.empty());
    bool found_reps2 = false, found_warmups2 = false;
    for (const auto& e : pr2.errors) {
        if (e.find("repetitions") != std::string::npos) found_reps2 = true;
        if (e.find("warmups") != std::string::npos) found_warmups2 = true;
    }
    CHECK(found_reps2);
    CHECK(found_warmups2);

    // run_plan rejects out-of-range overrides instead of looping.
    oz::runner::plan p;
    p.schema = "oz.plan.v1";
    p.name = "bounds";
    p.subject.name = "s";
    p.subject.executable = "x.exe";
    p.repetitions = 1;
    p.warmups = 0;
    oz::runner::image_spec img;
    img.path = "a.jpg";
    p.images.push_back(img);
    oz::runner::run_options ro;
    ro.repetitions = oz::runner::kMaxRepetitions + 1;
    std::string error;
    const auto records = oz::runner::run_plan(p, ro, error);
    CHECK(records.empty());
    CHECK(error.find("repetitions") != std::string::npos);
}

// --- process + collect ------------------------------------------------------

void test_process_and_collect(const std::wstring& fixture_exe) {
    oz::runner::process_options po;
    po.executable = fixture_exe;
    po.args = {L"--no-markers", L"--exit-code", L"7"};
    po.timeout_ms = 30000;
    po.capture_stdout = true;
    const auto pr = oz::runner::launch_process(po);
    CHECK(pr.launched);
    CHECK(!pr.timed_out);
    CHECK(pr.exit_code == 7);
    CHECK(pr.stdout_text.empty());

    oz::runner::process_options po2;
    po2.executable = fixture_exe;
    po2.args = {L"--delay-ms", L"1"};
    po2.timeout_ms = 30000;
    po2.capture_stdout = true;
    const auto pr2 = oz::runner::launch_process(po2);
    CHECK(pr2.launched);
    CHECK(pr2.exit_code == 0);
    const auto cr = oz::runner::parse_marker_stream(pr2.stdout_text);
    CHECK(cr.markers.size() == 12);
    CHECK(cr.validation.empty());
    CHECK(cr.runner_issues.empty());
    CHECK(cr.lines_parsed == 12);

    // unknown marker
    oz::runner::process_options po3;
    po3.executable = fixture_exe;
    po3.args = {L"--delay-ms", L"1", L"--unknown-marker"};
    po3.timeout_ms = 30000;
    po3.capture_stdout = true;
    const auto pr3 = oz::runner::launch_process(po3);
    const auto cr3 = oz::runner::parse_marker_stream(pr3.stdout_text);
    CHECK(cr3.markers.size() == 12);
    bool found_unknown = false;
    for (const auto& s : cr3.runner_issues) {
        if (s.find("unknown marker 'BogusMarker'") != std::string::npos) {
            found_unknown = true;
        }
    }
    CHECK(found_unknown);

    // duplicate defect surfaces through validation
    oz::runner::process_options po4;
    po4.executable = fixture_exe;
    po4.args = {L"--delay-ms", L"1", L"--duplicate"};
    po4.timeout_ms = 30000;
    po4.capture_stdout = true;
    const auto pr4 = oz::runner::launch_process(po4);
    const auto cr4 = oz::runner::parse_marker_stream(pr4.stdout_text);
    CHECK(cr4.markers.size() == 13);
    bool found_dup = false;
    for (const auto& f : cr4.validation) {
        if (f.kind == oz::markers::validation_kind::duplicate) {
            found_dup = true;
        }
    }
    CHECK(found_dup);

    // out-of-order defect
    oz::runner::process_options po5;
    po5.executable = fixture_exe;
    po5.args = {L"--delay-ms", L"1", L"--out-of-order"};
    po5.timeout_ms = 30000;
    po5.capture_stdout = true;
    const auto pr5 = oz::runner::launch_process(po5);
    const auto cr5 = oz::runner::parse_marker_stream(pr5.stdout_text);
    bool found_ooo = false;
    for (const auto& f : cr5.validation) {
        if (f.kind == oz::markers::validation_kind::out_of_order) {
            found_ooo = true;
        }
    }
    CHECK(found_ooo);

    // missing defect
    oz::runner::process_options po6;
    po6.executable = fixture_exe;
    po6.args = {L"--delay-ms", L"1", L"--missing"};
    po6.timeout_ms = 30000;
    po6.capture_stdout = true;
    const auto pr6 = oz::runner::launch_process(po6);
    const auto cr6 = oz::runner::parse_marker_stream(pr6.stdout_text);
    CHECK(cr6.markers.size() == 11);
    bool found_missing = false;
    for (const auto& f : cr6.validation) {
        if (f.kind == oz::markers::validation_kind::missing &&
            f.id == oz::markers::marker_id::header_parsed) {
            found_missing = true;
        }
    }
CHECK(found_missing);
}

void test_process_timeout(const std::wstring& fixture_exe) {
    oz::runner::process_options po;
    po.executable = fixture_exe;
    po.args = {L"--sleep-ms", L"3000"};
    po.timeout_ms = 100;
    po.capture_stdout = true;
    const auto t0 = std::chrono::steady_clock::now();
    const auto pr = oz::runner::launch_process(po);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    CHECK(pr.launched);
    CHECK(pr.timed_out);
    CHECK(pr.exit_code == 1);  // TerminateProcess exit code
    CHECK(elapsed_ms < 2000);  // must not block for the full 3 s sleep
}

void test_process_unicode_path(const std::wstring& fixture_exe) {
    // Copy the fixture into a temp directory whose name contains spaces and
    // non-ASCII characters; the CommandLineToArgvW-compatible quoting must
    // still launch it directly via CreateProcessW.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"oz bench \u00fc\u4e2d\u6587";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring copy = dir + L"\\ozbench_fixture.exe";
    if (!CopyFileW(fixture_exe.c_str(), copy.c_str(), FALSE)) {
        CHECK(false);  // cannot set up the test
        return;
    }
    oz::runner::process_options po;
    po.executable = copy;
    po.args = {L"--delay-ms", L"1"};
    po.timeout_ms = 30000;
    po.capture_stdout = true;
    const auto pr = oz::runner::launch_process(po);
    CHECK(pr.launched);
    CHECK(!pr.timed_out);
    CHECK(pr.exit_code == 0);
    const auto cr = oz::runner::parse_marker_stream(pr.stdout_text);
    CHECK(cr.markers.size() == 12);
    DeleteFileW(copy.c_str());
    RemoveDirectoryW(dir.c_str());
}

void test_launch_failure() {
    // A file that exists but is not an executable: CreateProcessW must fail
    // and the failure must be reported (launched=false, error set).
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"ozbench_not_an_exe.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        CHECK(false);
        return;
    }
    DWORD written = 0;
    WriteFile(h, "not an executable", 17, &written, nullptr);
    CloseHandle(h);
    oz::runner::process_options po;
    po.executable = path;
    po.timeout_ms = 1000;
    po.capture_stdout = true;
    const auto pr = oz::runner::launch_process(po);
    CHECK(!pr.launched);
    CHECK(pr.error != 0);
    CHECK(!pr.error_message.empty());
    DeleteFileW(path.c_str());
}

// --- summarize --------------------------------------------------------------

void test_summarize_percentiles() {
    std::vector<oz::runner::record> records;
    for (std::int64_t i = 1; i <= 100; ++i) {
        oz::runner::record r;
        r.kind = oz::runner::record_kind::marker;
        r.scenario = "s|cold|i";
        r.marker = "DecodeComplete";
        r.relative_ns = i;
        records.push_back(std::move(r));
    }
    const auto rows = oz::runner::summarize_records(records, false);
    CHECK(rows.size() == 1);
    if (!rows.empty()) {
        CHECK(rows[0].count == 100);
        CHECK(rows[0].median_ns == 50);
        CHECK(rows[0].p95_ns == 95);
        CHECK(rows[0].p99_ns == 99);
    }
    // nearest-rank on N=10: P95 -> ceil(9.5)=10 -> 10th sample
    std::vector<std::int64_t> small;
    for (std::int64_t i = 1; i <= 10; ++i) {
        small.push_back(i);
    }
    CHECK(oz::runner::nearest_rank_percentile(small, 95.0) == 10);
    CHECK(oz::runner::nearest_rank_percentile(small, 50.0) == 5);
    CHECK(oz::runner::nearest_rank_percentile({}, 95.0) == 0);

    // warmup exclusion
    std::vector<oz::runner::record> with_warmup;
    for (std::int64_t i = 1; i <= 3; ++i) {
        oz::runner::record r;
        r.kind = oz::runner::record_kind::marker;
        r.scenario = "s|cold|i";
        r.marker = "DecodeComplete";
        r.relative_ns = i;
        r.warmup = true;
        with_warmup.push_back(std::move(r));
    }
    const auto rows2 = oz::runner::summarize_records(with_warmup, false);
    CHECK(rows2.empty());
    const auto rows3 = oz::runner::summarize_records(with_warmup, true);
    CHECK(rows3.size() == 1);
}

// --- end-to-end fixture plan ------------------------------------------------

void test_e2e_fixture_plan(const std::wstring& source_dir, const std::wstring& fixture_exe) {
    const std::string text = read_file(source_dir + L"\\plans\\fixture.json");
    auto pr = oz::runner::parse_plan(text);
    CHECK(pr.value.has_value());
    if (!pr.value) {
        return;
    }
    oz::runner::run_options ro;
    ro.defines["fixture_exe"] = oz::runner::wide_to_utf8(fixture_exe);
    std::string error;
    const auto records = oz::runner::run_plan(*pr.value, ro, error);
    CHECK(error.empty());
    CHECK(!records.empty());

    std::size_t run_starts = 0, run_ends = 0, markers = 0;
    for (const auto& r : records) {
        if (r.kind == oz::runner::record_kind::run_start) ++run_starts;
        if (r.kind == oz::runner::record_kind::run_end) ++run_ends;
        if (r.kind == oz::runner::record_kind::marker) ++markers;
    }
    // 1 warmup + 5 repetitions = 6 runs, 12 markers each
    CHECK(run_starts == 6);
    CHECK(run_ends == 6);
    CHECK(markers == 72);

// every run_end must be clean
    for (const auto& r : records) {
        if (r.kind == oz::runner::record_kind::run_end) {
            CHECK(r.validation.empty());
            CHECK(r.issues.empty());
            CHECK(r.exit_code == 0u);
            CHECK(!r.timed_out);
            CHECK(r.launched);
        }
    }

    // provenance: subject_path must be the resolved executable (not the
    // {{fixture_exe}} placeholder) and the SHA-256 must be captured.
    for (const auto& r : records) {
        if (r.kind == oz::runner::record_kind::run_start) {
            CHECK(r.subject_path == oz::runner::wide_to_utf8(fixture_exe));
            CHECK(r.subject_path.find("{{") == std::string::npos);
            CHECK(r.subject_sha256.size() == 64);
        }
    }

    // JSONL round-trip: serialize then re-parse for summarize
    std::string jsonl;
    for (const auto& r : records) {
        jsonl += oz::runner::to_jsonl(r);
    }
std::vector<oz::runner::record> reparsed;
    const std::string_view jsonl_view = jsonl;
    std::size_t start = 0;
    while (start <= jsonl_view.size()) {
        std::size_t end = jsonl_view.find('\n', start);
        if (end == std::string_view::npos) end = jsonl_view.size();
        const std::string_view line = jsonl_view.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;
        auto parsed = oz::json::parse(line);
        CHECK(parsed.has_value());
        if (parsed) {
            auto rec = oz::runner::from_json(*parsed);
            CHECK(rec.has_value());
            if (rec) reparsed.push_back(std::move(*rec));
        }
    }
    CHECK(reparsed.size() == records.size());

    const auto rows = oz::runner::summarize_records(reparsed, false);
    CHECK(rows.size() == 12);  // one row per marker
    bool found_decode = false;
    for (const auto& row : rows) {
        if (row.marker == "DecodeComplete") {
            found_decode = true;
            CHECK(row.count == 5);  // warmup excluded
            CHECK(row.median_ns >= 0 && row.median_ns < 200'000'000);
        }
    }
    CHECK(found_decode);

// CSV and JSON output are non-empty
    CHECK(!oz::runner::summary_to_csv(rows).empty());
    CHECK(oz::json::is_array(oz::runner::summary_to_json(rows)));
}

void test_csv_escaping() {
    std::vector<oz::runner::summary_row> rows;
    oz::runner::summary_row r1;
    r1.scenario = "s|cold|a,b.jpg";
    r1.marker = "Decode\"Complete";
    r1.count = 1;
    r1.median_ns = 10;
    r1.p95_ns = 11;
    r1.p99_ns = 12;
    rows.push_back(r1);
    oz::runner::summary_row r2;
    r2.scenario = "s|cold|line1\nline2.jpg";
    r2.marker = "FirstPresent";
    r2.count = 2;
    r2.median_ns = 20;
    r2.p95_ns = 21;
    r2.p99_ns = 22;
    rows.push_back(r2);
    const std::string csv = oz::runner::summary_to_csv(rows);
    CHECK(csv.find("\"s|cold|a,b.jpg\"") != std::string::npos);
    CHECK(csv.find("\"Decode\"\"Complete\"") != std::string::npos);
    CHECK(csv.find("\"s|cold|line1\nline2.jpg\"") != std::string::npos);
    // unquoted fields stay unquoted
    oz::runner::summary_row r3;
    r3.scenario = "s|cold|plain.jpg";
    r3.marker = "DecodeComplete";
    r3.count = 3;
    r3.median_ns = 30;
    r3.p95_ns = 31;
    r3.p99_ns = 32;
    std::vector<oz::runner::summary_row> rows2{r3};
    const std::string csv2 = oz::runner::summary_to_csv(rows2);
    CHECK(csv2.find("s|cold|plain.jpg,DecodeComplete,3,30,31,32,nearest-rank") !=
          std::string::npos);
}

// --- external_first_present semantics ---------------------------------------

oz::runner::plan make_external_plan(const std::wstring& executable) {
    oz::runner::plan p;
    p.schema = "oz.plan.v1";
    p.name = "ext";
    p.subject.name = "ext-subject";
    p.subject.executable = oz::runner::wide_to_utf8(executable);
    p.collector = "external_first_present";
    p.adapter = "ext-adapter";
    p.cache = "cold";
    p.timeout_ms = 30000;
    p.repetitions = 1;
    p.warmups = 0;
    oz::runner::image_spec img;
    img.path = "a.jpg";
    img.format = "jpg";
    p.images.push_back(img);
    return p;
}

void test_run_plan_external_first_present(const std::wstring& fixture_exe) {
    oz::runner::run_options ro;
    std::string error;
    const auto records = oz::runner::run_plan(make_external_plan(fixture_exe), ro, error);
    CHECK(error.empty());
    CHECK(records.size() == 2);  // run_start + run_end
    bool found_end = false;
    for (const auto& r : records) {
        if (r.kind == oz::runner::record_kind::run_end) {
            found_end = true;
            CHECK(r.launched);
            CHECK(r.exit_code == 0u);
            CHECK(!r.timed_out);
            bool found_ext_issue = false;
            for (const auto& s : r.issues) {
                if (s.find("external_first_present") != std::string::npos) {
                    found_ext_issue = true;
                }
            }
            CHECK(found_ext_issue);  // documented: samples come via ozbench import
        }
    }
    CHECK(found_end);
}

void test_run_plan_launch_failure() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"ozbench_not_an_exe2.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        CHECK(false);
        return;
    }
    DWORD written = 0;
    WriteFile(h, "not an executable", 17, &written, nullptr);
    CloseHandle(h);

    oz::runner::run_options ro;
    std::string error;
    const auto records = oz::runner::run_plan(make_external_plan(path), ro, error);
    CHECK(error.empty());
    bool found_failed_end = false;
    for (const auto& r : records) {
        if (r.kind == oz::runner::record_kind::run_end) {
            if (!r.launched) {
                found_failed_end = true;
                CHECK(r.error != 0);
                CHECK(!r.error_message.empty());
                bool found_issue = false;
                for (const auto& s : r.issues) {
                    if (s.find("launch failed") != std::string::npos) {
                        found_issue = true;
                    }
                }
                CHECK(found_issue);
            }
        }
    }
    CHECK(found_failed_end);
    DeleteFileW(path.c_str());
}

// --- import -----------------------------------------------------------------

void test_import() {
    oz::runner::import_options opts;
    opts.format = "csv";
    opts.subject = "irfanview";
    opts.cache = "cold";
    std::vector<std::string> errors;
    const auto recs = oz::runner::import_samples(
        "scenario,relative_ns\nirfanview|cold|a.jpg,100\nirfanview|cold|a.jpg,200\n", opts,
        errors);
    CHECK(errors.empty());
    CHECK(recs.size() == 2);
    if (recs.size() == 2) {
        CHECK(recs[0].kind == oz::runner::record_kind::import);
        CHECK(recs[0].marker == "FirstPresent");
        CHECK(recs[0].relative_ns == 100);
        CHECK(recs[0].scenario == "irfanview|cold|a.jpg");
        CHECK(recs[0].subject == "irfanview");
        CHECK(recs[0].cache == "cold");
        CHECK(recs[1].relative_ns == 200);
    }

    oz::runner::import_options opts2;
    opts2.format = "jsonl";
    opts2.subject = "xnview";
    std::vector<std::string> errors2;
    const auto recs2 = oz::runner::import_samples(
        "{\"scenario\":\"xnview|cold|b.jpg\",\"sample_ns\":150}\n", opts2, errors2);
    CHECK(errors2.empty());
    CHECK(recs2.size() == 1);
    if (recs2.size() == 1) {
        CHECK(recs2[0].relative_ns == 150);
        CHECK(recs2[0].scenario == "xnview|cold|b.jpg");
    }

    // single-column CSV requires --scenario
    oz::runner::import_options opts3;
    opts3.format = "csv";
    opts3.scenario = "irfanview|cold|c.jpg";
    std::vector<std::string> errors3;
    const auto recs3 = oz::runner::import_samples("300\n400\n", opts3, errors3);
    CHECK(errors3.empty());
    CHECK(recs3.size() == 2);
    if (recs3.size() == 2) {
        CHECK(recs3[0].scenario == "irfanview|cold|c.jpg");
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cerr << "usage: ozbench_tests <source_dir> <fixture_exe>\n";
        return 2;
    }
    const std::wstring source_dir = argv[1];
    const std::wstring fixture_exe = argv[2];

test_marker_names();
    test_session_basic();
    test_validation_defects();
    test_json_roundtrip();
    test_plan_parse(source_dir);
    test_plan_rejects_negative_and_huge_counts();
    test_process_and_collect(fixture_exe);
    test_process_timeout(fixture_exe);
    test_process_unicode_path(fixture_exe);
    test_launch_failure();
    test_summarize_percentiles();
    test_csv_escaping();
    test_e2e_fixture_plan(source_dir, fixture_exe);
    test_run_plan_external_first_present(fixture_exe);
    test_run_plan_launch_failure();
    test_import();

    std::cout << "ozbench_tests: " << test::g_checks << " checks, " << test::g_failures
              << " failures\n";
    return test::g_failures == 0 ? 0 : 1;
}