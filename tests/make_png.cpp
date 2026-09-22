// ozviewer_make_png - writes a small valid PNG to the given path using the
// decoder-independent test-only mini writer (stored deflate, no codec libs).
// Used by the Stage 3.1 PNG integration test to produce real PNG fixtures.
//
// Usage: ozviewer_make_png <output.png> [width] [height] [mode] [exif]
//   mode: rgb | rgba | gray | palette | interlaced   (default rgba)
//   exif: 0 (none) or 1-8 orientation value (default 0)
#include "png_mini_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ozviewer_make_png <output.png> [width] [height] [mode] [exif]\n");
        return 2;
    }
    int w = 64;
    int h = 48;
    if (argc >= 3) w = _wtoi(argv[2]);
    if (argc >= 4) h = _wtoi(argv[3]);
    std::wstring mode = (argc >= 5) ? argv[4] : L"rgba";
    int exif = (argc >= 6) ? _wtoi(argv[5]) : 0;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        std::fprintf(stderr, "invalid dimensions\n");
        return 2;
    }

    const auto uw = static_cast<std::uint32_t>(w);
    const auto uh = static_cast<std::uint32_t>(h);
    std::vector<std::uint8_t> png;
    if (mode == L"rgb") {
        png = oz::test_png::make_rgb_png(uw, uh, oz::test_png::gradient(uw, uh));
    } else if (mode == L"gray") {
        std::vector<std::uint8_t> gray(static_cast<std::size_t>(uw) * uh);
        for (std::size_t i = 0; i < gray.size(); ++i) {
            gray[i] = static_cast<std::uint8_t>((i * 255) / (gray.size() > 1 ? gray.size() - 1 : 1));
        }
        png = oz::test_png::make_gray_png(uw, uh, gray);
    } else if (mode == L"palette") {
        std::vector<std::uint8_t> idx(static_cast<std::size_t>(uw) * uh);
        for (std::size_t i = 0; i < idx.size(); ++i) {
            idx[i] = static_cast<std::uint8_t>(i % 2);
        }
        const std::vector<oz::test_png::rgba> pal = {{255, 0, 0, 255}, {0, 0, 255, 128}};
        const std::vector<std::uint8_t> trns = {255, 128};
        png = oz::test_png::make_palette_png(uw, uh, idx, pal, trns);
    } else if (mode == L"interlaced") {
        png = oz::test_png::make_rgba_png(uw, uh, oz::test_png::gradient(uw, uh),
                                          true, exif);
    } else {
        png = oz::test_png::make_rgba_png(uw, uh, oz::test_png::gradient(uw, uh),
                                          false, exif);
    }

    FILE* f = _wfopen(argv[1], L"wb");
    if (!f) {
        std::fprintf(stderr, "cannot open output\n");
        return 1;
    }
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return 0;
}
