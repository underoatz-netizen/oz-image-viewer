// oz_navigation_corpus_tool - deterministic generator and validator for the
// 500-image synthetic JPEG navigation benchmark corpus.
//
// Usage:
//   oz_navigation_corpus_tool generate <out_dir> [count]
//   oz_navigation_corpus_tool validate <corpus_dir> [expected_count]
//
// `count`/`expected_count` default to 500 (the real navigation benchmark
// corpus). Smaller counts exist only for isolated smoke checks of this tool and
// are never final performance data.
#include "oz/viewer_core/navigation_benchmark_corpus.hpp"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

bool parse_count(const wchar_t* text, std::size_t& out_count) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const long long v = std::wcstoll(text, &end, 10);
    if (end == text || *end != L'\0' || v < 1) {
        return false;
    }
    out_count = static_cast<std::size_t>(v);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: oz_navigation_corpus_tool generate <out_dir> [count]\n"
                             "       oz_navigation_corpus_tool validate <corpus_dir> [expected_count]\n");
        return 2;
    }

    const std::wstring cmd = argv[1];
    const std::filesystem::path dir = argv[2];
    std::size_t count = 500;
    if (argc >= 4 && !parse_count(argv[3], count)) {
        std::fprintf(stderr, "oz_navigation_corpus_tool: count must be a positive integer\n");
        return 2;
    }

    if (cmd == L"generate") {
        std::printf("Generating deterministic %zu-JPEG corpus in: %ls\n", count, dir.c_str());
        if (count != 500) {
            std::printf("  NOTE: smoke corpus size %zu; not final benchmark data.\n", count);
        }
        const std::string err = oz::viewer_core::generate_deterministic_corpus(
            dir,
            [](std::size_t done, std::size_t total) {
                if (done % 50 == 0 || done == total) {
                    std::printf("  progress: %zu/%zu images\n", done, total);
                }
            },
            count);
        if (!err.empty()) {
            std::fprintf(stderr, "oz_navigation_corpus_tool: generation failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("Corpus generation complete.\n");
        return 0;
    } else if (cmd == L"validate") {
        std::printf("Validating corpus directory: %ls (expected %zu images)\n",
                    dir.c_str(), count);
        const auto val = oz::viewer_core::validate_navigation_corpus(dir, count);
        if (!val.ok) {
            std::fprintf(stderr, "oz_navigation_corpus_tool: validation failed: %s\n", val.error.c_str());
            return 1;
        }
        std::printf("Corpus valid: exactly %zu JPEGs, manifest hash: %s\n",
                    val.count, val.manifest_sha256.c_str());
        return 0;
    } else {
        std::fprintf(stderr, "oz_navigation_corpus_tool: unknown command\n");
        return 2;
    }
}
