// recycle_delete_tests.cpp - focused unit tests for the Stage 9.7
// post-recycle navigation policy (src/viewer/recycle_delete.hpp).
// Dependency-free; builds in every preset including no-vcpkg.
#include "recycle_delete.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

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

using oz::viewer::decide_recycle_successor;
using oz::viewer::find_recycled_index;
using oz::viewer::prune_recycled_path;

void test_next_available() {
    const std::vector<std::filesystem::path> items = {L"a.png", L"b.png", L"c.png"};
    const auto first = decide_recycle_successor(items, 0);
    CHECK(first.has_successor);
    CHECK(first.path == std::filesystem::path(L"b.png"));
    const auto middle = decide_recycle_successor(items, 1);
    CHECK(middle.has_successor);
    CHECK(middle.path == std::filesystem::path(L"c.png"));
}

void test_previous_at_end() {
    const std::vector<std::filesystem::path> items = {L"a.png", L"b.png", L"c.png"};
    const auto last = decide_recycle_successor(items, 2);
    CHECK(last.has_successor);
    CHECK(last.path == std::filesystem::path(L"b.png"));

    // Two-entry list deleting the tail falls back to the head.
    const std::vector<std::filesystem::path> pair = {L"a.png", L"b.png"};
    const auto tail = decide_recycle_successor(pair, 1);
    CHECK(tail.has_successor);
    CHECK(tail.path == std::filesystem::path(L"a.png"));
}

void test_blank_when_alone_or_empty() {
    const std::vector<std::filesystem::path> single = {L"only.png"};
    const auto lone = decide_recycle_successor(single, 0);
    CHECK(!lone.has_successor);
    CHECK(lone.path.empty());

    const std::vector<std::filesystem::path> empty;
    CHECK(!decide_recycle_successor(empty, 0).has_successor);
    // Out-of-range deleted positions can never yield a successor.
    const std::vector<std::filesystem::path> items = {L"a.png", L"b.png"};
    CHECK(!decide_recycle_successor(items, 2).has_successor);
    CHECK(!decide_recycle_successor(items, 99).has_successor);
}

void test_unicode_paths_preserved() {
    const std::vector<std::filesystem::path> items = {
        L"\u65e5\u672c\u8a9e.png", L"\U0001f600.png", L"b.png"};
    const auto next = decide_recycle_successor(items, 0);
    CHECK(next.has_successor);
    CHECK(next.path == std::filesystem::path(L"\U0001f600.png"));
    const auto prev = decide_recycle_successor(items, 2);
    CHECK(prev.has_successor);
    CHECK(prev.path == std::filesystem::path(L"\U0001f600.png"));

    const auto found = find_recycled_index(items, L"\U0001f600.png");
    CHECK(found.has_value() && *found == 1);
    CHECK(!find_recycled_index(items, L"missing.png").has_value());

    const auto pruned = prune_recycled_path(items, L"\U0001f600.png");
    CHECK(pruned.size() == 2);
    CHECK(pruned[0] == std::filesystem::path(L"\u65e5\u672c\u8a9e.png"));
    CHECK(pruned[1] == std::filesystem::path(L"b.png"));
}

void test_prune_preserves_order_and_dedupes_once() {
    const std::vector<std::filesystem::path> items = {L"b.png", L"a.png", L"b.png", L"c.png"};
    const auto pruned = prune_recycled_path(items, L"b.png");
    CHECK(pruned.size() == 3);
    CHECK(pruned[0] == std::filesystem::path(L"a.png"));
    CHECK(pruned[1] == std::filesystem::path(L"b.png"));
    CHECK(pruned[2] == std::filesystem::path(L"c.png"));

    // Absent path yields the input unchanged.
    const auto same = prune_recycled_path(items, L"z.png");
    CHECK(same.size() == items.size());
}

}  // namespace

int main() {
    test_next_available();
    test_previous_at_end();
    test_blank_when_alone_or_empty();
    test_unicode_paths_preserved();
    test_prune_preserves_order_and_dedupes_once();

    std::printf("recycle_delete_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
