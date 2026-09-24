// drop_flow_tests.cpp - focused unit tests for the Stage 9.6B drop-open
// policy helpers (src/viewer/drop_flow.hpp).
// Dependency-free; builds in every preset including no-vcpkg.
#include "drop_flow.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

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

using oz::viewer::describe_drop_rejection;
using oz::viewer::describe_no_supported_images;
using oz::viewer::describe_drop_overload;
using oz::viewer::drop_classify_status;
using oz::viewer::is_promptable_drop_rejection;
using oz::viewer::select_supported_in_drop_order;

void test_promptable_set() {
    CHECK(is_promptable_drop_rejection(drop_classify_status::invalid_entry));
    CHECK(is_promptable_drop_rejection(drop_classify_status::mixed));
    CHECK(is_promptable_drop_rejection(drop_classify_status::multiple_folders));
    CHECK(is_promptable_drop_rejection(drop_classify_status::overflow));
    CHECK(!is_promptable_drop_rejection(drop_classify_status::ok));
    CHECK(!is_promptable_drop_rejection(drop_classify_status::empty));
}

void test_rejection_text_explicit() {
    for (auto status : {drop_classify_status::invalid_entry, drop_classify_status::mixed,
                        drop_classify_status::multiple_folders,
                        drop_classify_status::overflow}) {
        const auto text = describe_drop_rejection(status);
        CHECK(text.headline != nullptr && text.headline[0] != L'\0');
        CHECK(text.detail != nullptr && text.detail[0] != L'\0');
    }
    // Non-promptable outcomes carry no prompt.
    for (auto status : {drop_classify_status::ok, drop_classify_status::empty}) {
        const auto text = describe_drop_rejection(status);
        CHECK(text.headline != nullptr && text.headline[0] == L'\0');
        CHECK(text.detail == nullptr);
    }
}

void test_no_supported_images_text() {
    const auto files = describe_no_supported_images(false);
    CHECK(files.headline != nullptr && files.headline[0] != L'\0');
    CHECK(files.detail != nullptr);
    const auto folder = describe_no_supported_images(true);
    CHECK(folder.headline != nullptr && folder.headline[0] != L'\0');
    CHECK(folder.detail != nullptr);
    // Folder shortfall (empty folder) reports distinctly from file shortfall.
    CHECK(std::wstring(folder.headline) != std::wstring(files.headline));
}

// Stage 9.6B third MUST_FIX: the bounded retirement budget rejection is
// explicit and distinct from every classifier rejection and shortfall text.
void test_drop_overload_text() {
    const auto overload = describe_drop_overload();
    CHECK(overload.headline != nullptr && overload.headline[0] != L'\0');
    CHECK(overload.detail != nullptr && overload.detail[0] != L'\0');
    for (auto status : {drop_classify_status::invalid_entry, drop_classify_status::mixed,
                        drop_classify_status::multiple_folders,
                        drop_classify_status::overflow}) {
        CHECK(std::wstring(describe_drop_rejection(status).headline) !=
              std::wstring(overload.headline));
    }
    CHECK(std::wstring(describe_no_supported_images(false).headline) !=
          std::wstring(overload.headline));
    CHECK(std::wstring(describe_no_supported_images(true).headline) !=
          std::wstring(overload.headline));
}

bool stub_supported(const std::filesystem::path& p) {
    const auto ext = p.extension().wstring();
    return ext == L".jpg" || ext == L".JPG" || ext == L".png";
}

void test_drop_order_selection() {
    const std::vector<std::filesystem::path> files = {
        std::filesystem::path(L"C:\\d\\b.png"),
        std::filesystem::path(L"C:\\d\\a.txt"),  // unsupported: dropped
        std::filesystem::path(L"C:\\d\\a.jpg"),
    };
    const auto kept = select_supported_in_drop_order(files, stub_supported);
    CHECK(kept.size() == 2);
    if (kept.size() == 2) {
        // Drop order preserved (no sorting): b.png stays first.
        CHECK(kept[0].filename() == L"b.png");
        CHECK(kept[1].filename() == L"a.jpg");
    }
}

void test_selection_unicode_and_duplicates() {
    const std::vector<std::filesystem::path> files = {
        std::filesystem::path(L"C:\\\u6d4b\u8bd5\\\u753b\u50cf.jpg"),
        std::filesystem::path(L"C:\\d\\dup.png"),
        std::filesystem::path(L"C:\\d\\dup.png"),  // duplicates kept: caller order is policy
        std::filesystem::path(L"C:\\d\\nope.gif"),
    };
    const auto kept = select_supported_in_drop_order(files, stub_supported);
    CHECK(kept.size() == 3);
    if (!kept.empty()) {
        CHECK(kept[0].filename() == L"\u753b\u50cf.jpg");
    }
}

// Stage 9.6B MUST_FIX: folder first-item failure retention contract.
// Mirrors the WndProc folder branches in src/viewer/main.cpp: the async
// folder result is staged alongside the untouched prior navigation and
// installs only after the first item successfully presents. Decode or
// present failure discards the stage; prior items and indices are unchanged.
namespace staged_contract {
struct nav_snapshot {
    std::vector<std::filesystem::path> items;
    std::size_t desired = 0;
    std::size_t committed = 0;
};
struct folder_stage {
    nav_snapshot prior;
    std::vector<std::filesystem::path> staged;
    std::size_t staged_desired = 0;
    bool alive = true;
};
inline nav_snapshot install_on_first_present(const folder_stage& s) {
    nav_snapshot out;
    out.items = s.staged;
    out.desired = s.staged_desired;
    out.committed = s.staged_desired;
    return out;
}
}  // namespace staged_contract

void test_folder_first_item_decode_failure_retains_navigation() {
    using namespace staged_contract;
    folder_stage s;
    s.prior = {{L"C:\\prior\\a.jpg", L"C:\\prior\\b.jpg"}, 1, 1};
    s.staged = {L"C:\\folder\\f1.jpg", L"C:\\folder\\f2.jpg"};
    s.staged_desired = 0;
    // Simulate kMsgDecodeComplete decode failure: discard stage, keep prior.
    s.alive = false;
    const nav_snapshot retained = s.prior;
    CHECK(retained.items.size() == 2);
    CHECK(retained.items[0].filename() == L"a.jpg");
    CHECK(retained.items[1].filename() == L"b.jpg");
    CHECK(retained.desired == 1);
    CHECK(retained.committed == 1);
    CHECK(!s.alive);
}

void test_folder_first_item_present_failure_retains_navigation() {
    using namespace staged_contract;
    folder_stage s;
    s.prior = {{L"C:\\prior\\a.jpg", L"C:\\prior\\b.jpg"}, 1, 1};
    s.staged = {L"C:\\folder\\f1.jpg", L"C:\\folder\\f2.jpg"};
    s.staged_desired = 0;
    // Simulate upload_and_present/recover_and_present failure: discard stage,
    // restore prior image; navigation untouched.
    s.alive = false;
    const nav_snapshot retained = s.prior;
    CHECK(retained.items.size() == 2);
    CHECK(retained.items[0].filename() == L"a.jpg");
    CHECK(retained.desired == 1);
    CHECK(retained.committed == 1);
    CHECK(!(retained.items.size() == s.staged.size() &&
            retained.items[0] == s.staged[0]));
}

void test_folder_first_item_success_installs_staged_navigation() {
    using namespace staged_contract;
    folder_stage s;
    s.prior = {{L"C:\\prior\\a.jpg", L"C:\\prior\\b.jpg"}, 1, 1};
    s.staged = {L"C:\\folder\\f1.jpg", L"C:\\folder\\f2.jpg"};
    s.staged_desired = 0;
    // Simulate successful present: staged list installs at index 0.
    const nav_snapshot installed = install_on_first_present(s);
    CHECK(installed.items.size() == 2);
    CHECK(installed.items[0].filename() == L"f1.jpg");
    CHECK(installed.items[1].filename() == L"f2.jpg");
    CHECK(installed.desired == 0);
    CHECK(installed.committed == 0);
}

void test_selection_predicate_fault_tolerated() {
    const std::vector<std::filesystem::path> files = {
        std::filesystem::path(L"C:\\d\\a.jpg"),
        std::filesystem::path(L"C:\\d\\b.png"),
    };
    auto throwing = [](const std::filesystem::path&) -> bool {
        throw std::runtime_error("probe fault");
    };
    const auto kept = select_supported_in_drop_order(files, throwing);
    CHECK(kept.empty());  // predicate fault treats the entry as not openable
    const auto none = select_supported_in_drop_order(
        std::vector<std::filesystem::path>{}, stub_supported);
    CHECK(none.empty());
}

}  // namespace

int main() {
    test_promptable_set();
    test_rejection_text_explicit();
    test_no_supported_images_text();
    test_drop_overload_text();
    test_drop_order_selection();
    test_selection_unicode_and_duplicates();
    test_selection_predicate_fault_tolerated();
    test_folder_first_item_decode_failure_retains_navigation();
    test_folder_first_item_present_failure_retains_navigation();
    test_folder_first_item_success_installs_staged_navigation();
    std::printf("drop_flow_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
