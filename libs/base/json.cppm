// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.json — a small JSON value type, parser and serialiser (used for LSP and the CMake File API).
export module daemon.base.json;
import std;
import daemon.base.util;

export namespace ide::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // insertion-ordered

struct Value {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v = nullptr;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v(b) {}
    Value(int n) : v(static_cast<double>(n)) {}
    Value(long n) : v(static_cast<double>(n)) {}
    Value(long long n) : v(static_cast<double>(n)) {}
    Value(unsigned n) : v(static_cast<double>(n)) {}
    Value(double n) : v(n) {}
    Value(const char* s) : v(std::string(s)) {}
    Value(std::string s) : v(std::move(s)) {}
    Value(std::string_view s) : v(std::string(s)) {}
    Value(Array a) : v(std::move(a)) {}
    Value(Object o) : v(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool isString() const { return std::holds_alternative<std::string>(v); }
    bool isNumber() const { return std::holds_alternative<double>(v); }
    bool isArray() const { return std::holds_alternative<Array>(v); }
    bool isObject() const { return std::holds_alternative<Object>(v); }

    const Array* array() const { return std::get_if<Array>(&v); }
    const Object* object() const { return std::get_if<Object>(&v); }

    std::string str(std::string def = {}) const {
        if (auto* s = std::get_if<std::string>(&v)) return *s;
        return def;
    }
    double num(double def = 0) const {
        if (auto* d = std::get_if<double>(&v)) return *d;
        return def;
    }
    int integer(int def = 0) const {
        if (auto* d = std::get_if<double>(&v)) return static_cast<int>(*d);
        return def;
    }
    bool boolean(bool def = false) const {
        if (auto* b = std::get_if<bool>(&v)) return *b;
        return def;
    }

    const Value* find(std::string_view key) const {
        if (auto* o = std::get_if<Object>(&v))
            for (const auto& [k, val] : *o)
                if (k == key) return &val;
        return nullptr;
    }
    // Missing keys / wrong types yield null, so lookups can be chained safely: v["a"]["b"].
    const Value& operator[](std::string_view key) const {
        static const Value null;
        const Value* f = find(key);
        return f ? *f : null;
    }
    const Value& operator[](std::size_t i) const {
        static const Value null;
        if (auto* a = std::get_if<Array>(&v); a && i < a->size()) return (*a)[i];
        return null;
    }
    std::size_t size() const {
        if (auto* a = std::get_if<Array>(&v)) return a->size();
        if (auto* o = std::get_if<Object>(&v)) return o->size();
        return 0;
    }

    // Builders.
    Value& set(std::string key, Value val);
    Value& push(Value val) {
        if (!isArray()) v = Array{};
        std::get<Array>(v).push_back(std::move(val));
        return *this;
    }
};

inline Value object() { return Value(Object{}); }
inline Value array() { return Value(Array{}); }

namespace detail {

struct Parser {
    std::string_view s;
    std::size_t i = 0;
    int depth = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    std::optional<Value> value();

    std::optional<Value> number();

    static void appendCodepoint(std::string& out, char32_t cp) { appendUtf8(out, cp); }

    std::optional<unsigned> hex4();

    std::optional<std::string> string();

    std::optional<Value> array();

    std::optional<Value> object();
};

void dumpString(std::string& out, std::string_view s);

void dumpValue(std::string& out, const Value& val);

}  // namespace detail

std::optional<Value> parse(std::string_view text);

inline std::string dump(const Value& v) {
    std::string out;
    detail::dumpValue(out, v);
    return out;
}

}  // namespace ide::json
