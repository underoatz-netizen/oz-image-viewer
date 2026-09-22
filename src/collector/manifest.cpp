#include "oz/collector/manifest.hpp"

#include "oz/runner/json.hpp"
#include "oz/runner/utf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cctype>
#include <fstream>
#include <iterator>

namespace oz::collector {
namespace {

std::string trim(std::string s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

bool read_file_bytes(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool iequals_hex(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string sha256_hex(const void* data, std::size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    struct alg_guard {
        BCRYPT_ALG_HANDLE a;
        ~alg_guard() {
            if (a) {
                BCryptCloseAlgorithmProvider(a, 0);
            }
        }
    } ag{alg};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        return {};
    }
    struct hash_guard {
        BCRYPT_HASH_HANDLE hh;
        ~hash_guard() {
            if (hh) {
                BCryptDestroyHash(hh);
            }
        }
    } hg{hash};

    if (size > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(size), 0) != 0) {
        return {};
    }
    BYTE digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const BYTE b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::string sha256_file_hex(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    struct file_guard {
        HANDLE h;
        ~file_guard() {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    } fg{h};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    struct alg_guard {
        BCRYPT_ALG_HANDLE a;
        ~alg_guard() {
            if (a) {
                BCryptCloseAlgorithmProvider(a, 0);
            }
        }
    } ag{alg};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        return {};
    }
    struct hash_guard {
        BCRYPT_HASH_HANDLE hh;
        ~hash_guard() {
            if (hh) {
                BCryptDestroyHash(hh);
            }
        }
    } hg{hash};

    char buf[64 * 1024];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf), n, 0) != 0) {
            return {};
        }
    }
    BYTE digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const BYTE b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::optional<manifest> load_manifest(const std::wstring& path, std::string& error) {
    std::string raw;
    if (!read_file_bytes(path, raw)) {
        error = "cannot open manifest: " + oz::runner::wide_to_utf8(path);
        return std::nullopt;
    }
    // The seal is computed over the exact file bytes (including any BOM the
    // inventory script wrote), so hash before stripping anything.
    const std::string actual = sha256_hex(raw.data(), raw.size());
    if (actual.empty()) {
        error = "SHA-256 unavailable";
        return std::nullopt;
    }

    std::string seal;
    const std::wstring seal_path = path + L".sha256";
    if (!read_file_bytes(seal_path, seal)) {
        error = "missing seal file (tamper protection): " +
                oz::runner::wide_to_utf8(seal_path);
        return std::nullopt;
    }
    seal = trim(seal);
    if (seal.size() != 64 || !iequals_hex(seal, actual)) {
        error = "manifest seal mismatch (tamper detected): " +
                oz::runner::wide_to_utf8(path);
        return std::nullopt;
    }

    std::string text = raw;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);  // strip UTF-8 BOM for parsing
    }

    auto parsed = oz::json::parse(text);
    if (!parsed) {
        error = "manifest is not valid JSON: " + parsed.error();
        return std::nullopt;
    }
    const oz::json::value& root = *parsed;
    if (!oz::json::is_object(root)) {
        error = "manifest root must be a JSON object";
        return std::nullopt;
    }

    manifest m;
    m.schema = oz::json::get_string(root, "schema").value_or("");
    if (m.schema != "oz.manifest.v1") {
        error = "manifest.schema: unsupported '" + m.schema + "' (expected oz.manifest.v1)";
        return std::nullopt;
    }
    m.id = oz::json::get_string(root, "id").value_or("");
    if (m.id.empty()) {
        error = "manifest.id: missing or empty";
        return std::nullopt;
    }

    const oz::json::value* imgs = oz::json::find_member(root, "images");
    if (!imgs || !oz::json::is_array(*imgs) || oz::json::array_size(*imgs) == 0) {
        error = "manifest.images: must be a non-empty array";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < oz::json::array_size(*imgs); ++i) {
        const oz::json::value& iv = oz::json::array_at(*imgs, i);
        if (!oz::json::is_object(iv)) {
            error = "manifest.images[" + std::to_string(i) + "]: must be an object";
            return std::nullopt;
        }
        manifest_image img;
        img.alias = oz::json::get_string(iv, "alias").value_or("");
        img.path = oz::json::get_string(iv, "path").value_or("");
        img.sha256 = oz::json::get_string(iv, "sha256").value_or("");
        if (img.alias.empty() || img.path.empty()) {
            error = "manifest.images[" + std::to_string(i) +
                    "]: alias and path are required";
            return std::nullopt;
        }
        img.width = oz::json::get_int(iv, "width").value_or(0);
        img.height = oz::json::get_int(iv, "height").value_or(0);
        if (const auto sb = oz::json::get_int(iv, "size_bytes")) {
            img.size_bytes = static_cast<std::uint64_t>(*sb);
        }
        if (const auto px = oz::json::get_int(iv, "pixels")) {
            img.pixels = static_cast<std::uint64_t>(*px);
        }
        img.target_mp = oz::json::get_double(iv, "target_mp").value_or(0);
        img.deviation_mp = oz::json::get_double(iv, "deviation_mp").value_or(0);
        m.images.push_back(std::move(img));
    }
    return m;
}

}  // namespace oz::collector