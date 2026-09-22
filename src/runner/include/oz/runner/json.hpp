// oz/runner/json.hpp - JSON accessor layer for the runner.
//
// Two interchangeable providers:
//   * internal minimal JSON module (default; dependency-free, tested)
//   * pinned nlohmann-json from vcpkg (when OZBENCH_HAVE_NLOHMANN is defined)
//
// Runner code must only use the free functions below, never provider
// internals, so both providers stay interchangeable.
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#if defined(OZBENCH_HAVE_NLOHMANN)
#include <nlohmann/json.hpp>
#endif

namespace oz::json {

#if defined(OZBENCH_HAVE_NLOHMANN)
using value = nlohmann::json;
using object = nlohmann::json::object_t;
using array = nlohmann::json::array_t;
#else
struct value;
using object = std::map<std::string, value, std::less<>>;
using array = std::vector<value>;

struct value {
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, array, object> data;

    value() : data(nullptr) {}
    value(std::nullptr_t) : data(nullptr) {}
    value(bool b) : data(b) {}
    value(std::int64_t i) : data(i) {}
    value(double d) : data(d) {}
    value(std::string s) : data(std::move(s)) {}
    value(const char* s) : data(std::string(s)) {}
    value(array a) : data(std::move(a)) {}
    value(object o) : data(std::move(o)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_int() const { return std::holds_alternative<std::int64_t>(data); }
    bool is_double() const { return std::holds_alternative<double>(data); }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<array>(data); }
    bool is_object() const { return std::holds_alternative<object>(data); }

    bool as_bool() const { return std::get<bool>(data); }
    std::int64_t as_int() const { return std::get<std::int64_t>(data); }
    double as_double() const {
        return is_int() ? static_cast<double>(as_int()) : std::get<double>(data);
    }
    const std::string& as_string() const { return std::get<std::string>(data); }
    const array& as_array() const { return std::get<array>(data); }
    const object& as_object() const { return std::get<object>(data); }
};
#endif

// --- construction -----------------------------------------------------------
value make_object();
value make_array();
value make_string(std::string s);
value make_int(std::int64_t i);
value make_double(double d);
value make_bool(bool b);
value make_null();
void set_member(value& obj, std::string_view key, value v);
void push_back(value& arr, value v);

// --- inspection -------------------------------------------------------------
bool is_null(const value& v);
bool is_bool(const value& v);
bool is_int(const value& v);
bool is_double(const value& v);
bool is_number(const value& v);
bool is_string(const value& v);
bool is_array(const value& v);
bool is_object(const value& v);

bool as_bool(const value& v);
std::int64_t as_int(const value& v);
double as_double(const value& v);
const std::string& as_string(const value& v);
const array& as_array(const value& v);
const object& as_object(const value& v);
std::size_t array_size(const value& v);
const value& array_at(const value& v, std::size_t i);

// --- member access ----------------------------------------------------------
const value* find_member(const value& obj, std::string_view key);
std::optional<std::string> get_string(const value& obj, std::string_view key);
std::optional<std::int64_t> get_int(const value& obj, std::string_view key);
std::optional<double> get_double(const value& obj, std::string_view key);
std::optional<bool> get_bool(const value& obj, std::string_view key);

// --- parse / dump -----------------------------------------------------------
std::expected<value, std::string> parse(std::string_view text);
std::string dump(const value& v);

}  // namespace oz::json