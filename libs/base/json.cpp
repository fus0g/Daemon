// json - implementation of module daemon.base.json.
module daemon.base.json;
import std;
import daemon.base.util;

namespace ide::json {

Value& Value::set(std::string key, Value val) {
    if (!isObject()) v = Object{};
    auto& o = std::get<Object>(v);
    for (auto& [k, existing] : o)
        if (k == key) { existing = std::move(val); return *this; }
    o.emplace_back(std::move(key), std::move(val));
    return *this;
}

}  // namespace ide::json

namespace ide::json::detail {

std::optional<Value> Parser::value() {
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

std::optional<Value> Parser::number() {
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

std::optional<unsigned> Parser::hex4() {
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

std::optional<std::string> Parser::string() {
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

std::optional<Value> Parser::array() {
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

std::optional<Value> Parser::object() {
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

void dumpString(std::string& out, std::string_view s) {
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

void dumpValue(std::string& out, const Value& val) {
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

}  // namespace ide::json::detail

namespace ide::json {

std::optional<Value> parse(std::string_view text) {
    detail::Parser p{text};
    auto v = p.value();
    if (!v) return std::nullopt;
    p.ws();
    if (p.i != text.size()) return std::nullopt;
    return v;
}

}  // namespace ide::json
