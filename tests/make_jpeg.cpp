// ozviewer_make_jpeg - writes a small valid JPEG to the given path using
// TurboJPEG encode. Used by the Stage 1 integration test to produce a real
// JPEG fixture.
//
// Usage: ozviewer_make_jpeg <output.jpg> [width] [height]
#include <turbojpeg.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ozviewer_make_jpeg <output.jpg> [width] [height]\n");
        return 2;
    }
    int w = 64;
    int h = 48;
    if (argc >= 3) w = _wtoi(argv[2]);
    if (argc >= 4) h = _wtoi(argv[3]);
    if (w <= 0 || h <= 0) {
        std::fprintf(stderr, "invalid dimensions\n");
        return 2;
    }

    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            rgba[i + 0] = static_cast<unsigned char>((x * 255) / w);
            rgba[i + 1] = static_cast<unsigned char>((y * 255) / h);
            rgba[i + 2] = 128;
            rgba[i + 3] = 255;
        }
    }

    unsigned char* buf = nullptr;
    unsigned long out_size = 0;
    tjhandle enc = tjInitCompress();
    if (enc == nullptr) {
        std::fprintf(stderr, "tjInitCompress failed\n");
        return 1;
    }
    const int rc = tjCompress2(enc, rgba.data(), w, 0, h, TJPF_RGBA, &buf, &out_size,
                               TJSAMP_420, 90, TJFLAG_FASTDCT);
    tjDestroy(enc);
    if (rc != 0 || buf == nullptr) {
        std::fprintf(stderr, "encode failed\n");
        return 1;
    }

    FILE* f = _wfopen(argv[1], L"wb");
    if (!f) {
        tjFree(buf);
        std::fprintf(stderr, "cannot open output\n");
        return 1;
    }
    fwrite(buf, 1, out_size, f);
    fclose(f);
    tjFree(buf);
    return 0;
}