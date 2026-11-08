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
    Value& set(std::string key, Value val) {
        if (!isObject()) v = Object{};
        auto& o = std::get<Object>(v);
        for (auto& [k, existing] : o)
            if (k == key) { existing = std::move(val); return *this; }
        o.emplace_back(std::move(key), std::move(val));
        return *this;
    }
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

    std::optional<Value> value() {
        ws();
        if (i >= s.size() || ++depth > 200) return std::nullopt;
        struct Guard { int& d; ~Guard() { --d; } } guard{depth};
        char c = s[i];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') { auto str = string(); if (!str) return std::nullopt; return Value(std::move(*str)); }
        if (s.compare(i, 4, "true") == 0) { i += 4; return Value(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Value(false); }
        if (s.compare(i, 4, "null") == 0) { i += 4; return Value(nullptr); }
        return number();
    }

    std::optional<Value> number() {
        std::size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                s[i] == '-' || s[i] == '+'))
            ++i;
        if (i == start) return std::nullopt;
        double d = 0;
        auto [p, ec] = std::from_chars(s.data() + start, s.data() + i, d);
        if (ec != std::errc() || p != s.data() + i) return std::nullopt;
        return Value(d);
    }

    static void appendCodepoint(std::string& out, char32_t cp) { appendUtf8(out, cp); }

    std::optional<unsigned> hex4() {
        if (i + 4 > s.size()) return std::nullopt;
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i + static_cast<std::size_t>(k)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
        }
        i += 4;
        return v;
    }

    std::optional<std::string> string() {
        if (i >= s.size() || s[i] != '"') return std::nullopt;
        ++i;
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return std::nullopt;
            char e = s[i++];
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
                    auto hi = hex4();
                    if (!hi) return std::nullopt;
                    char32_t cp = *hi;
                    if (cp >= 0xD800 && cp < 0xDC00 && s.compare(i, 2, "\\u") == 0) {  // surrogate pair
                        i += 2;
                        auto lo = hex4();
                        if (!lo) return std::nullopt;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (*lo - 0xDC00);
                    }
                    appendCodepoint(out, cp);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> array() {
        ++i;  // [
        Array out;
        if (eat(']')) return Value(std::move(out));
        while (true) {
            auto v = value();
            if (!v) return std::nullopt;
            out.push_back(std::move(*v));
            if (eat(',')) continue;
            if (eat(']')) return Value(std::move(out));
            return std::nullopt;
        }
    }

    std::optional<Value> object() {
        ++i;  // {
        Object out;
        if (eat('}')) return Value(std::move(out));
        while (true) {
            ws();
            auto key = string();
            if (!key || !eat(':')) return std::nullopt;
            auto v = value();
            if (!v) return std::nullopt;
            out.emplace_back(std::move(*key), std::move(*v));
            if (eat(',')) continue;
            if (eat('}')) return Value(std::move(out));
            return std::nullopt;
        }
    }
};

inline void dumpString(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) out += std::format("\\u{:04x}", c);
                else out += static_cast<char>(c);
        }
    }
    out += '"';
}

inline void dumpValue(std::string& out, const Value& val) {
    struct Visitor {
        std::string& out;
        void operator()(std::nullptr_t) const { out += "null"; }
        void operator()(bool b) const { out += b ? "true" : "false"; }
        void operator()(double d) const {
            if (d == std::floor(d) && std::abs(d) < 1e15) out += std::format("{}", static_cast<long long>(d));
            else out += std::format("{}", d);
        }
        void operator()(const std::string& s) const { dumpString(out, s); }
        void operator()(const Array& a) const {
            out += '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                dumpValue(out, a[i]);
            }
            out += ']';
        }
        void operator()(const Object& o) const {
            out += '{';
            for (std::size_t i = 0; i < o.size(); ++i) {
                if (i) out += ',';
                dumpString(out, o[i].first);
                out += ':';
                dumpValue(out, o[i].second);
            }
            out += '}';
        }
    };
    std::visit(Visitor{out}, val.v);
}

}  // namespace detail

inline std::optional<Value> parse(std::string_view text) {
    detail::Parser p{text};
    auto v = p.value();
    if (!v) return std::nullopt;
    p.ws();
    if (p.i != text.size()) return std::nullopt;
    return v;
}

inline std::string dump(const Value& v) {
    std::string out;
    detail::dumpValue(out, v);
    return out;
}

}  // namespace ide::json
