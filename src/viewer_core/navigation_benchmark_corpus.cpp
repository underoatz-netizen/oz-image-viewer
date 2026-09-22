#include "oz/viewer_core/navigation_benchmark_corpus.hpp"
#include "oz/viewer_core/jpeg.hpp"
#include "oz/collector/manifest.hpp"
#include "oz/runner/json.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if OZVIEWER_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace oz::viewer_core {
namespace {

std::string format_ordinal_alias(std::size_t ordinal) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "img_%04zu", ordinal);
    return std::string(buf);
}

std::string format_ordinal_filename(std::size_t ordinal) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "img_%04zu.jpg", ordinal);
    return std::string(buf);
}

// Deterministic image specification table:
// Ordinals 0..N:
// - 0..49: 24MP images (6000x4000) for heavy uncached navigation tests
// - 50..149: 12MP images (4240x2832)
// - 150..299: 6MP images (3000x2000)
// - 300..449: 2MP images (1920x1080)
// - 450..499: 0.5MP images (800x600)
void get_corpus_dimensions(std::size_t ordinal, int& w, int& h) {
    if (ordinal < 50) {
        w = 6000;
        h = 4000;
    } else if (ordinal < 150) {
        w = 4240;
        h = 2832;
    } else if (ordinal < 300) {
        w = 3000;
        h = 2000;
    } else if (ordinal < 450) {
        w = 1920;
        h = 1080;
    } else {
        w = 800;
        h = 600;
    }
}

// Simple LCG PRNG for repeatable deterministic pseudo-pixel patterns
struct simple_prng {
    std::uint32_t state;
    explicit simple_prng(std::uint32_t seed) : state(seed ? seed : 1u) {}
    std::uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

// Rejects EXIF/XMP/IPTC and comment segments: the synthetic corpus contract is
// a metadata-free JPEG. JFIF APP0 is the only permitted application segment.
// Returns true when the header region contains no metadata segment.
bool jpeg_header_is_metadata_free(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    std::size_t i = 2;  // after SOI
    while (i + 1 < size) {
        if (data[i] != 0xFF) {
            return false;
        }
        const std::uint8_t marker = data[i + 1];
        if (marker == 0xFF) {  // fill byte
            ++i;
            continue;
        }
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) {
            return true;  // entropy-coded data / end of image: metadata region is over
        }
        if (i + 4 > size) {
            return false;
        }
        const std::size_t segment_length =
            (static_cast<std::size_t>(data[i + 2]) << 8) | static_cast<std::size_t>(data[i + 3]);
        if (segment_length < 2) {
            return false;
        }
        if (marker == 0xE1 || (marker >= 0xE2 && marker <= 0xEF) || marker == 0xFE) {
            return false;
        }
        i += 2 + segment_length;
    }
    return true;
}

std::string read_file_text(const std::filesystem::path& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

}  // namespace

corpus_validation_result validate_navigation_corpus(
    const std::filesystem::path& dir, std::size_t expected_count) {
    corpus_validation_result res;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        res.error = "Corpus path is not a valid directory";
        return res;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            res.error = "Failed to iterate corpus directory: " + ec.message();
            return res;
        }
        if (entry.is_regular_file()) {
            const auto filename = entry.path().filename().string();
            if (filename == "manifest.json" || filename == "manifest.json.sha256") {
                continue;
            }
            const auto ext = entry.path().extension().string();
            std::string ext_lower;
            ext_lower.reserve(ext.size());
            for (char c : ext) ext_lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (ext_lower != ".jpg" && ext_lower != ".jpeg") {
                res.error = "Corpus directory contains non-JPEG file: " + filename;
                return res;
            }
            files.push_back(entry.path());
        }
    }

    if (files.size() != expected_count) {
        res.error = "Corpus directory must contain exactly " + std::to_string(expected_count) +
                    " JPEG files, found: " + std::to_string(files.size());
        return res;
    }

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.filename().wstring() < b.filename().wstring();
    });

    res.entries.reserve(expected_count);
    std::size_t heavy_24mp_count = 0;

    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& p = files[i];
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            res.error = "Cannot open corpus file: " + p.filename().string();
            return res;
        }
        f.seekg(0, std::ios::end);
        const auto sz = static_cast<std::uint64_t>(f.tellg());
        f.seekg(0, std::ios::beg);

        if (sz < 4) {
            res.error = "Corpus file too small: " + p.filename().string();
            return res;
        }

        std::vector<std::uint8_t> data(static_cast<std::size_t>(sz));
        f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));

        if (!has_jpeg_magic(data.data(), data.size())) {
            res.error = "Corpus file missing JPEG SOI magic: " + p.filename().string();
            return res;
        }

        if (!jpeg_header_is_metadata_free(data.data(), data.size())) {
            res.error = "Corpus file carries EXIF/XMP/comment metadata: " + p.filename().string();
            return res;
        }

        jpeg_header h{};
        const auto herr = parse_jpeg_header(data.data(), data.size(), h);
        if (herr != jpeg_error_kind::none) {
            res.error = "Corpus file header parse failed: " + p.filename().string();
            return res;
        }

        if (h.width >= 6000 && h.height >= 4000) {
            ++heavy_24mp_count;
        }

        corpus_manifest_entry me;
        me.alias = format_ordinal_alias(i);
        me.ordinal = i;
        me.rel_filename = p.filename().string();
        me.width = h.width;
        me.height = h.height;
        me.file_bytes = sz;
        me.sha256 = oz::collector::sha256_file_hex(p.wstring());
        res.entries.push_back(std::move(me));
    }

    const std::size_t required_24mp = (std::min)(std::size_t{10}, expected_count);
    if (heavy_24mp_count < required_24mp) {
        res.error = "Corpus must include sufficient 24MP files for uncached-next tests (found " +
                    std::to_string(heavy_24mp_count) + ", required " +
                    std::to_string(required_24mp) + ")";
        return res;
    }

    // Manifest verification: when manifest.json is present it must describe
    // exactly these files, and its .sha256 seal must match the manifest bytes.
    const auto manifest_path = dir / "manifest.json";
    if (std::filesystem::exists(manifest_path, ec)) {
        bool read_ok = false;
        const std::string manifest_text = read_file_text(manifest_path, read_ok);
        if (!read_ok) {
            res.error = "Cannot read corpus manifest.json";
            return res;
        }
        auto parsed = json::parse(manifest_text);
        if (!parsed) {
            res.error = "Corpus manifest.json is malformed";
            return res;
        }
        const auto total_images = json::get_int(*parsed, "total_images");
        if (!total_images || *total_images != static_cast<std::int64_t>(expected_count)) {
            res.error = "Corpus manifest.json total_images does not match " +
                        std::to_string(expected_count);
            return res;
        }
        const json::value* images = json::find_member(*parsed, "images");
        if (images == nullptr || !json::is_array(*images) ||
            json::array_size(*images) != expected_count) {
            res.error = "Corpus manifest.json images array does not match " +
                        std::to_string(expected_count);
            return res;
        }

        std::unordered_map<std::string, const json::value*> by_filename;
        by_filename.reserve(expected_count * 2);
        for (const auto& image : json::as_array(*images)) {
            const auto filename = json::get_string(image, "filename");
            if (!filename) {
                res.error = "Corpus manifest.json entry missing filename";
                return res;
            }
            by_filename.emplace(*filename, &image);
        }

        for (const auto& computed : res.entries) {
            const auto it = by_filename.find(computed.rel_filename);
            if (it == by_filename.end()) {
                res.error = "Corpus manifest.json is missing " + computed.rel_filename;
                return res;
            }
            const json::value& entry = *it->second;
            const auto sha = json::get_string(entry, "sha256");
            const auto width = json::get_int(entry, "width");
            const auto height = json::get_int(entry, "height");
            const auto bytes = json::get_int(entry, "file_bytes");
            if (!sha || *sha != computed.sha256 || !width ||
                *width != static_cast<std::int64_t>(computed.width) || !height ||
                *height != static_cast<std::int64_t>(computed.height) || !bytes ||
                *bytes != static_cast<std::int64_t>(computed.file_bytes)) {
                res.error = "Corpus manifest.json entry mismatch for " + computed.rel_filename;
                return res;
            }
        }

        res.manifest_sha256 = oz::collector::sha256_file_hex(manifest_path.wstring());

        const auto seal_path = dir / "manifest.json.sha256";
        if (!std::filesystem::exists(seal_path, ec)) {
            res.error = "Corpus manifest.json.sha256 seal is missing";
            return res;
        }
        bool seal_ok = false;
        std::string seal = read_file_text(seal_path, seal_ok);
        while (!seal.empty() && (seal.back() == '\n' || seal.back() == '\r' || seal.back() == ' ')) {
            seal.pop_back();
        }
        if (!seal_ok || seal != res.manifest_sha256) {
            res.error = "Corpus manifest.json.sha256 seal mismatch";
            return res;
        }
    } else {
        // Deterministic hash of entry hashes for hand-built corpora without a
        // manifest file.
        std::string concat;
        for (const auto& e : res.entries) {
            concat += e.sha256;
        }
        res.manifest_sha256 = oz::collector::sha256_hex(concat.data(), concat.size());
    }

    res.ok = true;
    res.count = expected_count;
    return res;
}

std::string generate_deterministic_corpus(
    const std::filesystem::path& out_dir,
    const std::function<void(std::size_t, std::size_t)>& progress_cb,
    std::size_t count) {
#if !OZVIEWER_HAVE_TURBOJPEG
    (void)out_dir;
    (void)progress_cb;
    (void)count;
    return "libjpeg-turbo not available; cannot generate synthetic JPEG corpus";
#else
    if (count == 0) {
        return "count must be at least 1";
    }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        return "Failed to create directory: " + ec.message();
    }

    tjhandle enc = tjInitCompress();
    if (!enc) {
        return "tjInitCompress failed";
    }

    struct enc_guard {
        tjhandle h;
        ~enc_guard() { if (h) tjDestroy(h); }
    } eg{enc};

    std::vector<corpus_manifest_entry> entries;
    entries.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        int w = 0;
        int h = 0;
        get_corpus_dimensions(i, w, h);

        simple_prng prng(static_cast<std::uint32_t>(i + 12345u));
        std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * h * 4);

        const unsigned char base_r = static_cast<unsigned char>(prng.next() & 0xFF);
        const unsigned char base_g = static_cast<unsigned char>(prng.next() & 0xFF);
        const unsigned char base_b = static_cast<unsigned char>(prng.next() & 0xFF);

        // Fill buffer with deterministic pattern
        for (int y = 0; y < h; ++y) {
            const unsigned char y_mod = static_cast<unsigned char>((y * 255) / h);
            for (int x = 0; x < w; ++x) {
                const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
                const unsigned char x_mod = static_cast<unsigned char>((x * 255) / w);
                rgba[idx + 0] = static_cast<unsigned char>((base_r + x_mod) & 0xFF);
                rgba[idx + 1] = static_cast<unsigned char>((base_g + y_mod) & 0xFF);
                rgba[idx + 2] = static_cast<unsigned char>((base_b + ((x + y) & 0x7F)) & 0xFF);
                rgba[idx + 3] = 255;
            }
        }

        unsigned char* buf = nullptr;
        unsigned long out_size = 0;
        // Moderate quality (80) for compact disk footprint; the JFIF header
        // written by TurboJPEG is the only marker segment (metadata-free).
        const int rc = tjCompress2(enc, rgba.data(), w, 0, h, TJPF_RGBA, &buf, &out_size,
                                   TJSAMP_420, 80, TJFLAG_FASTDCT);
        if (rc != 0 || !buf) {
            return "tjCompress2 failed for image " + std::to_string(i);
        }

        const auto filename = format_ordinal_filename(i);
        const auto file_path = out_dir / filename;
        FILE* fp = _wfopen(file_path.c_str(), L"wb");
        if (!fp) {
            tjFree(buf);
            return "Cannot open file for writing: " + filename;
        }
        fwrite(buf, 1, out_size, fp);
        fclose(fp);
        tjFree(buf);

        corpus_manifest_entry me;
        me.alias = format_ordinal_alias(i);
        me.ordinal = i;
        me.rel_filename = filename;
        me.width = w;
        me.height = h;
        me.file_bytes = out_size;
        me.sha256 = oz::collector::sha256_file_hex(file_path.wstring());
        entries.push_back(std::move(me));

        if (progress_cb) {
            progress_cb(i + 1, count);
        }
    }

    // Write manifest.json
    auto manifest_obj = json::make_object();
    json::set_member(manifest_obj, "schema", json::make_string("oz.corpus-manifest.v1"));
    json::set_member(manifest_obj, "total_images", json::make_int(static_cast<std::int64_t>(count)));
    auto img_arr = json::make_array();
    for (const auto& e : entries) {
        auto io = json::make_object();
        json::set_member(io, "alias", json::make_string(e.alias));
        json::set_member(io, "ordinal", json::make_int(static_cast<std::int64_t>(e.ordinal)));
        json::set_member(io, "filename", json::make_string(e.rel_filename));
        json::set_member(io, "sha256", json::make_string(e.sha256));
        json::set_member(io, "width", json::make_int(e.width));
        json::set_member(io, "height", json::make_int(e.height));
        json::set_member(io, "file_bytes", json::make_int(static_cast<std::int64_t>(e.file_bytes)));
        json::push_back(img_arr, std::move(io));
    }
    json::set_member(manifest_obj, "images", std::move(img_arr));

    const std::string manifest_str = json::dump(manifest_obj);
    const auto manifest_file = out_dir / "manifest.json";
    std::ofstream mf(manifest_file, std::ios::binary);
    if (!mf) {
        return "Failed to write manifest.json";
    }
    mf.write(manifest_str.data(), static_cast<std::streamsize>(manifest_str.size()));
    mf.close();

    const std::string seal = oz::collector::sha256_file_hex(manifest_file.wstring());
    std::ofstream sf(out_dir / "manifest.json.sha256", std::ios::binary);
    if (sf) {
        sf.write(seal.data(), static_cast<std::streamsize>(seal.size()));
        sf.close();
    }

    return {};
#endif
}

}  // namespace oz::viewer_core
