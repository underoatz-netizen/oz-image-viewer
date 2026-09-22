#include "oz/runner/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oz::json {

#if defined(OZBENCH_HAVE_NLOHMANN)

// ---------------------------------------------------------------------------
// nlohmann-json provider (pinned via vcpkg; requires OZBENCH_USE_NLOHMANN=ON)
// ---------------------------------------------------------------------------
value make_object() { return nlohmann::json::object(); }
value make_array() { return nlohmann::json::array(); }
value make_string(std::string s) { return nlohmann::json(std::move(s)); }
value make_int(std::int64_t i) { return nlohmann::json(i); }
value make_double(double d) { return nlohmann::json(d); }
value make_bool(bool b) { return nlohmann::json(b); }
value make_null() { return nlohmann::json(nullptr); }

void set_member(value& obj, std::string_view key, value v) {
    obj[std::string(key)] = std::move(v);
}
void push_back(value& arr, value v) { arr.push_back(std::move(v)); }

bool is_null(const value& v) { return v.is_null(); }
bool is_bool(const value& v) { return v.is_boolean(); }
bool is_int(const value& v) { return v.is_number_integer(); }
bool is_double(const value& v) { return v.is_number_float(); }
bool is_number(const value& v) { return v.is_number(); }
bool is_string(const value& v) { return v.is_string(); }
bool is_array(const value& v) { return v.is_array(); }
bool is_object(const value& v) { return v.is_object(); }

bool as_bool(const value& v) { return v.get<bool>(); }
std::int64_t as_int(const value& v) { return v.get<std::int64_t>(); }
double as_double(const value& v) { return v.get<double>(); }
const std::string& as_string(const value& v) { return v.get_ref<const std::string&>(); }
const array& as_array(const value& v) { return v.get_ref<const array&>(); }
const object& as_object(const value& v) { return v.get_ref<const object&>(); }
std::size_t array_size(const value& v) { return v.size(); }
const value& array_at(const value& v, std::size_t i) { return v.at(i); }

const value* find_member(const value& obj, std::string_view key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    auto it = obj.find(std::string(key));
    return it == obj.end() ? nullptr : &*it;
}
std::optional<std::string> get_string(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_string()) {
        return std::nullopt;
    }
    return v->get<std::string>();
}
std::optional<std::int64_t> get_int(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_number_integer()) {
        return std::nullopt;
    }
    return v->get<std::int64_t>();
}
std::optional<double> get_double(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_number()) {
        return std::nullopt;
    }
    return v->get<double>();
}
std::optional<bool> get_bool(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_boolean()) {
        return std::nullopt;
    }
    return v->get<bool>();
}

std::expected<value, std::string> parse(std::string_view text) {
    try {
        return nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(std::string(e.what()));
    }
}
std::string dump(const value& v) { return v.dump(); }

#else  // !OZBENCH_HAVE_NLOHMANN - internal minimal JSON module

namespace {

constexpr int kMaxDepth = 256;

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct parser {
    std::string_view s;
    std::size_t pos = 0;
    int depth = 0;

    bool eof() const { return pos >= s.size(); }
    char peek() const { return eof() ? '\0' : s[pos]; }

    void skip_ws() {
        while (!eof()) {
            const char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    std::string err(std::string msg) const {
        return msg + " at offset " + std::to_string(pos);
    }

    bool consume(char c) {
        if (peek() == c) {
            ++pos;
            return true;
        }
        return false;
    }

    std::expected<std::uint32_t, std::string> parse_hex4() {
        if (pos + 4 > s.size()) {
            return std::unexpected(err("truncated \\u escape"));
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::unexpected(err("invalid \\u escape"));
            }
        }
        return v;
    }

    std::expected<value, std::string> parse_string() {
        if (!consume('"')) {
            return std::unexpected(err("expected '\"'"));
        }
        std::string out;
        while (!eof()) {
            const char c = s[pos++];
            if (c == '"') {
                return value(std::move(out));
            }
            if (c == '\\') {
                if (eof()) {
                    return std::unexpected(err("unterminated escape"));
                }
                const char e = s[pos++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        auto cp = parse_hex4();
                        if (!cp) {
                            return std::unexpected(cp.error());
                        }
                        std::uint32_t code = *cp;
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (pos + 1 < s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                pos += 2;
                                auto lo = parse_hex4();
                                if (!lo) {
                                    return std::unexpected(lo.error());
                                }
                                if (*lo >= 0xDC00 && *lo <= 0xDFFF) {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (*lo - 0xDC00);
                                } else {
                                    return std::unexpected(err("invalid low surrogate"));
                                }
                            } else {
                                return std::unexpected(err("missing low surrogate"));
                            }
                        } else if (code >= 0xDC00 && code <= 0xDFFF) {
                            return std::unexpected(err("unexpected low surrogate"));
                        }
                        append_utf8(out, code);
                        break;
                    }
                    default:
                        return std::unexpected(err("invalid escape"));
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return std::unexpected(err("unescaped control character in string"));
            } else {
                out += c;
            }
        }
        return std::unexpected(err("unterminated string"));
    }

    std::expected<value, std::string> parse_number() {
        const std::size_t start = pos;
        consume('-');
        bool is_double = false;
        while (!eof() && s[pos] >= '0' && s[pos] <= '9') {
            ++pos;
        }
        if (!eof() && s[pos] == '.') {
            is_double = true;
            ++pos;
            while (!eof() && s[pos] >= '0' && s[pos] <= '9') {
                ++pos;
            }
        }
        if (!eof() && (s[pos] == 'e' || s[pos] == 'E')) {
            is_double = true;
            ++pos;
            if (!eof() && (s[pos] == '+' || s[pos] == '-')) {
                ++pos;
            }
            while (!eof() && s[pos] >= '0' && s[pos] <= '9') {
                ++pos;
            }
        }
        const std::string_view num = s.substr(start, pos - start);
        if (num.empty() || num == "-") {
            return std::unexpected(err("invalid number"));
        }
        if (!is_double) {
            std::int64_t v = 0;
            const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), v);
            if (ec == std::errc()) {
                return value(v);
            }
            // integer overflow: fall back to double
            return value(std::strtod(std::string(num).c_str(), nullptr));
        }
        return value(std::strtod(std::string(num).c_str(), nullptr));
    }

    std::expected<value, std::string> parse_bool() {
        if (s.substr(pos, 4) == "true") {
            pos += 4;
            return value(true);
        }
        if (s.substr(pos, 5) == "false") {
            pos += 5;
            return value(false);
        }
        return std::unexpected(err("invalid literal"));
    }

    std::expected<value, std::string> parse_null() {
        if (s.substr(pos, 4) == "null") {
            pos += 4;
            return value(nullptr);
        }
        return std::unexpected(err("invalid literal"));
    }

    std::expected<value, std::string> parse_array() {
        if (!consume('[')) {
            return std::unexpected(err("expected '['"));
        }
        array a;
        skip_ws();
        if (consume(']')) {
            return value(std::move(a));
        }
        while (true) {
            skip_ws();
            auto v = parse_value();
            if (!v) {
                return std::unexpected(v.error());
            }
            a.push_back(std::move(*v));
            skip_ws();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return std::unexpected(err("expected ',' or ']'"));
            }
        }
        return value(std::move(a));
    }

    std::expected<value, std::string> parse_object() {
        if (!consume('{')) {
            return std::unexpected(err("expected '{'"));
        }
        object o;
        skip_ws();
        if (consume('}')) {
            return value(std::move(o));
        }
        while (true) {
            skip_ws();
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_ws();
            if (!consume(':')) {
                return std::unexpected(err("expected ':'"));
            }
            auto v = parse_value();
            if (!v) {
                return std::unexpected(v.error());
            }
            o.emplace(std::get<std::string>(key->data), std::move(*v));
            skip_ws();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return std::unexpected(err("expected ',' or '}'"));
            }
        }
        return value(std::move(o));
    }

    std::expected<value, std::string> parse_value() {
        if (++depth > kMaxDepth) {
            --depth;
            return std::unexpected(err("nesting too deep"));
        }
        skip_ws();
        std::expected<value, std::string> r;
        const char c = peek();
        if (c == '{') {
            r = parse_object();
        } else if (c == '[') {
            r = parse_array();
        } else if (c == '"') {
            r = parse_string();
        } else if (c == 't' || c == 'f') {
            r = parse_bool();
        } else if (c == 'n') {
            r = parse_null();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            r = parse_number();
        } else {
            r = std::unexpected(err("unexpected character"));
        }
        --depth;
        return r;
    }
};

void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // UTF-8 bytes pass through
                }
        }
    }
    out += '"';
}

void dump_value(const value& v, std::string& out, int depth) {
    if (depth > kMaxDepth) {
        out += "null";
        return;
    }
    if (v.is_null()) {
        out += "null";
        return;
    }
    if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
        return;
    }
    if (v.is_int()) {
        out += std::to_string(v.as_int());
        return;
    }
    if (v.is_double()) {
        const double d = v.as_double();
        if (std::isfinite(d)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", d);
            out += buf;
        } else {
            out += "null";
        }
        return;
    }
    if (v.is_string()) {
        dump_string(v.as_string(), out);
        return;
    }
    if (v.is_array()) {
        out += '[';
        const array& a = v.as_array();
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) {
                out += ',';
            }
            dump_value(a[i], out, depth + 1);
        }
        out += ']';
        return;
    }
    out += '{';
    const object& o = v.as_object();
    bool first = true;
    for (const auto& [k, mv] : o) {
        if (!first) {
            out += ',';
        }
        first = false;
        dump_string(k, out);
        out += ':';
        dump_value(mv, out, depth + 1);
    }
    out += '}';
}

}  // namespace

value make_object() { return value(object{}); }
value make_array() { return value(array{}); }
value make_string(std::string s) { return value(std::move(s)); }
value make_int(std::int64_t i) { return value(i); }
value make_double(double d) { return value(d); }
value make_bool(bool b) { return value(b); }
value make_null() { return value(nullptr); }

void set_member(value& obj, std::string_view key, value v) {
    std::get<object>(obj.data).emplace(std::string(key), std::move(v));
}
void push_back(value& arr, value v) {
    std::get<array>(arr.data).push_back(std::move(v));
}

bool is_null(const value& v) { return v.is_null(); }
bool is_bool(const value& v) { return v.is_bool(); }
bool is_int(const value& v) { return v.is_int(); }
bool is_double(const value& v) { return v.is_double(); }
bool is_number(const value& v) { return v.is_number(); }
bool is_string(const value& v) { return v.is_string(); }
bool is_array(const value& v) { return v.is_array(); }
bool is_object(const value& v) { return v.is_object(); }

bool as_bool(const value& v) { return v.as_bool(); }
std::int64_t as_int(const value& v) { return v.as_int(); }
double as_double(const value& v) { return v.as_double(); }
const std::string& as_string(const value& v) { return v.as_string(); }
const array& as_array(const value& v) { return std::get<array>(v.data); }
const object& as_object(const value& v) { return std::get<object>(v.data); }
std::size_t array_size(const value& v) { return v.as_array().size(); }
const value& array_at(const value& v, std::size_t i) { return v.as_array()[i]; }

const value* find_member(const value& obj, std::string_view key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.as_object().find(key);
    return it == obj.as_object().end() ? nullptr : &it->second;
}
std::optional<std::string> get_string(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_string()) {
        return std::nullopt;
    }
    return v->as_string();
}
std::optional<std::int64_t> get_int(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_int()) {
        return std::nullopt;
    }
    return v->as_int();
}
std::optional<double> get_double(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_number()) {
        return std::nullopt;
    }
    return v->as_double();
}
std::optional<bool> get_bool(const value& obj, std::string_view key) {
    const value* v = find_member(obj, key);
    if (!v || !v->is_bool()) {
        return std::nullopt;
    }
    return v->as_bool();
}

std::expected<value, std::string> parse(std::string_view text) {
    parser p{text};
    auto v = p.parse_value();
    if (!v) {
        return v;
    }
    p.skip_ws();
    if (!p.eof()) {
        return std::unexpected(p.err("trailing characters after JSON value"));
    }
    return v;
}

std::string dump(const value& v) {
    std::string out;
    dump_value(v, out, 0);
    return out;
}

#endif  // OZBENCH_HAVE_NLOHMANN

}  // namespace oz::json