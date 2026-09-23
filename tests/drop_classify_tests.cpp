// drop_classify_tests.cpp - focused unit tests for the Stage 9.6A
// multi-drop classification seam (src/viewer/drop_classify.hpp).
// Dependency-free; builds in every preset including no-vcpkg.
#include "drop_classify.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_failures; \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    } \
} while (0)

using oz::viewer::classify_drop_paths;
using oz::viewer::drop_classify_status;
using oz::viewer::drop_entry;
using oz::viewer::drop_entry_kind;
using oz::viewer::drop_intent;
using oz::viewer::kMaxDropEntries;

drop_entry make_file(const wchar_t* p) {
    return drop_entry{std::filesystem::path(p), drop_entry_kind::file};
}

drop_entry make_dir(const wchar_t* p) {
    return drop_entry{std::filesystem::path(p), drop_entry_kind::directory};
}

void test_empty() {
    const auto r = classify_drop_paths({});
    CHECK(!r.ok());
    CHECK(r.status == drop_classify_status::empty);
    CHECK(r.intent == drop_intent::none);
    CHECK(r.files.empty());
    CHECK(r.folder.empty());
}

void test_single_file_preserved() {
    const std::filesystem::path p(L"C:\\pics\\photo 01.JPG");
    const auto r = classify_drop_paths({{p, drop_entry_kind::file}});
    CHECK(r.ok());
    CHECK(r.status == drop_classify_status::ok);
    CHECK(r.intent == drop_intent::single_file);
    CHECK(r.files.size() == 1);
    CHECK(r.files[0] == p);
    CHECK(r.folder.empty());
}

void test_pure_no_filesystem_queries() {
    // Nonexistent drive paths still classify: the seam never inspects disk.
    const auto r = classify_drop_paths({make_file(L"Z:\\does\\not\\exist\\x.png")});
    CHECK(r.ok());
    CHECK(r.intent == drop_intent::single_file);
    CHECK(r.files.size() == 1);
    CHECK(r.files[0] == std::filesystem::path(L"Z:\\does\\not\\exist\\x.png"));
}

void test_multiple_files_preserve_drop_order() {
    // Deliberately non-sorted input: output must keep drop order, not sort.
    const std::vector<drop_entry> in = {
        make_file(L"C:\\pics\\c.png"),
        make_file(L"C:\\pics\\a.png"),
        make_file(L"C:\\pics\\b.png"),
    };
    const auto r = classify_drop_paths(in);
    CHECK(r.ok());
    CHECK(r.intent == drop_intent::file_list);
    CHECK(r.files.size() == 3);
    CHECK(r.files[0].filename() == L"c.png");
    CHECK(r.files[1].filename() == L"a.png");
    CHECK(r.files[2].filename() == L"b.png");
    CHECK(r.folder.empty());
}

void test_single_folder_intent() {
    const std::filesystem::path p(L"D:\\albums\\trip");
    const auto r = classify_drop_paths({{p, drop_entry_kind::directory}});
    CHECK(r.ok());
    CHECK(r.status == drop_classify_status::ok);
    CHECK(r.intent == drop_intent::folder);
    CHECK(r.folder == p);
    CHECK(r.files.empty());
}

void test_mixed_rejected() {
    const auto r = classify_drop_paths({
        make_file(L"C:\\pics\\a.png"),
        make_dir(L"C:\\pics\\sub"),
    });
    CHECK(!r.ok());
    CHECK(r.status == drop_classify_status::mixed);
    CHECK(r.intent == drop_intent::none);
    CHECK(r.files.empty());
    CHECK(r.folder.empty());

    // Order-independent: folder first still rejects.
    const auto r2 = classify_drop_paths({
        make_dir(L"C:\\pics\\sub"),
        make_file(L"C:\\pics\\a.png"),
        make_file(L"C:\\pics\\b.png"),
    });
    CHECK(!r2.ok());
    CHECK(r2.status == drop_classify_status::mixed);
}

void test_multiple_folders_rejected() {
    const auto r = classify_drop_paths({
        make_dir(L"C:\\a"),
        make_dir(L"C:\\b"),
    });
    CHECK(!r.ok());
    CHECK(r.status == drop_classify_status::multiple_folders);
    CHECK(r.intent == drop_intent::none);
}

void test_unsupported_inputs() {
    // Unknown kind rejects.
    const auto r = classify_drop_paths({
        make_file(L"C:\\pics\\a.png"),
        drop_entry{std::filesystem::path(L"C:\\pics\\odd.lnk"), drop_entry_kind::unknown},
    });
    CHECK(!r.ok());
    CHECK(r.status == drop_classify_status::invalid_entry);

    // Lone unknown rejects.
    const auto r2 = classify_drop_paths(
        {drop_entry{std::filesystem::path(L"C:\\pics\\x.png"), drop_entry_kind::unknown}});
    CHECK(!r2.ok());
    CHECK(r2.status == drop_classify_status::invalid_entry);

    // Empty path rejects even with a file kind.
    const auto r3 = classify_drop_paths({drop_entry{std::filesystem::path{}, drop_entry_kind::file}});
    CHECK(!r3.ok());
    CHECK(r3.status == drop_classify_status::invalid_entry);

    // Structural validation wins over shape: unknown present alongside a
    // folder reports invalid_entry, not mixed.
    const auto r4 = classify_drop_paths({
        make_dir(L"C:\\pics\\sub"),
        drop_entry{std::filesystem::path(L"C:\\pics\\x.png"), drop_entry_kind::unknown},
    });
    CHECK(!r4.ok());
    CHECK(r4.status == drop_classify_status::invalid_entry);
}

void test_unicode_paths() {
    const std::vector<drop_entry> in = {
        make_file(L"C:\\pics\\\u753B\u50CF_\U0001F3A8.png"),
        make_file(L"C:\\pics\\\u0444\u043E\u0442\u043E_\u00E9.JPG"),
        make_file(L"C:\\pics\\caf\u00E9 menu.bmp"),
    };
    const auto r = classify_drop_paths(in);
    CHECK(r.ok());
    CHECK(r.intent == drop_intent::file_list);
    CHECK(r.files.size() == 3);
    CHECK(r.files[0] == std::filesystem::path(L"C:\\pics\\\u753B\u50CF_\U0001F3A8.png"));
    CHECK(r.files[1] == std::filesystem::path(L"C:\\pics\\\u0444\u043E\u0442\u043E_\u00E9.JPG"));
    CHECK(r.files[2] == std::filesystem::path(L"C:\\pics\\caf\u00E9 menu.bmp"));

    // Unicode folder intent preserves the exact path too.
    const std::filesystem::path dir(L"D:\\\u65C5\u884C\u5199\u771F");
    const auto rf = classify_drop_paths({{dir, drop_entry_kind::directory}});
    CHECK(rf.ok());
    CHECK(rf.intent == drop_intent::folder);
    CHECK(rf.folder == dir);
}

void test_duplicates_deduped_first_wins() {
    const auto r = classify_drop_paths({
        make_file(L"C:\\pics\\b.png"),
        make_file(L"C:\\pics\\a.png"),
        make_file(L"C:\\pics\\b.png"),    // exact repeat
        make_file(L"C:\\PICS\\A.PNG"),    // case-variant repeat of a.png
        make_file(L"C:\\pics\\c.png"),
    });
    CHECK(r.ok());
    CHECK(r.intent == drop_intent::file_list);
    CHECK(r.files.size() == 3);
    // First occurrences survive, in original drop order.
    CHECK(r.files[0] == std::filesystem::path(L"C:\\pics\\b.png"));
    CHECK(r.files[1] == std::filesystem::path(L"C:\\pics\\a.png"));
    CHECK(r.files[2] == std::filesystem::path(L"C:\\pics\\c.png"));
}

void test_bounds() {
    // Exactly at the bound: accepted.
    std::vector<drop_entry> at_bound;
    at_bound.reserve(kMaxDropEntries);
    for (std::size_t i = 0; i < kMaxDropEntries; ++i) {
        wchar_t buf[64]{};
        std::swprintf(buf, 64, L"C:\\pics\\img_%05zu.png", i);
        at_bound.push_back(make_file(buf));
    }
    const auto rok = classify_drop_paths(at_bound);
    CHECK(rok.ok());
    CHECK(rok.intent == drop_intent::file_list);
    CHECK(rok.files.size() == kMaxDropEntries);

    // One over the bound: rejected with no partial list.
    at_bound.push_back(make_file(L"C:\\pics\\one_too_many.png"));
    const auto r = classify_drop_paths(at_bound);
    CHECK(!r.ok());
    CHECK(r.status == drop_classify_status::overflow);
    CHECK(r.intent == drop_intent::none);
    CHECK(r.files.empty());
    CHECK(r.folder.empty());
}

}  // namespace

int main() {
    test_empty();
    test_single_file_preserved();
    test_pure_no_filesystem_queries();
    test_multiple_files_preserve_drop_order();
    test_single_folder_intent();
    test_mixed_rejected();
    test_multiple_folders_rejected();
    test_unsupported_inputs();
    test_unicode_paths();
    test_duplicates_deduped_first_wins();
    test_bounds();

    std::printf("drop_classify_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
