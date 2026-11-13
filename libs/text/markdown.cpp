// SPDX-License-Identifier: GPL-3.0-or-later
// markdown - implementation of module daemon.text.markdown.
module daemon.text.markdown;
import std;

namespace ide::md {

namespace {

using Lines = std::vector<std::string>;

bool isBlank(std::string_view s) { return s.find_first_not_of(" \t\r") == std::string_view::npos; }

int indentOf(std::string_view s) {
    int n = 0;
    while (n < static_cast<int>(s.size()) && s[static_cast<std::size_t>(n)] == ' ') ++n;
    return n;
}

std::string_view ltrim(std::string_view s) {
    const auto p = s.find_first_not_of(' ');
    return p == std::string_view::npos ? std::string_view() : s.substr(p);
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

bool isPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// ---------------------------------------------------------------------------- entities

void appendCodepoint(std::string& out, char32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Decodes the entity starting at s[i] == '&'; returns the number of bytes consumed (0 when it is not an entity).
std::size_t entity(std::string_view s, std::size_t i, std::string& out) {
    const auto semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10 || semi - i < 2) return 0;
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    static const std::unordered_map<std::string_view, std::string_view> named = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}, {"copy", "\xC2\xA9"},
        {"reg", "\xC2\xAE"}, {"trade", "\xE2\x84\xA2"}, {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"},
        {"hellip", "\xE2\x80\xA6"}, {"laquo", "\xC2\xAB"}, {"raquo", "\xC2\xBB"}, {"middot", "\xC2\xB7"},
        {"bull", "\xE2\x80\xA2"}, {"rarr", "\xE2\x86\x92"}, {"larr", "\xE2\x86\x90"}, {"times", "\xC3\x97"},
        {"check", "\xE2\x9C\x93"}};
    if (name[0] == '#') {
        std::uint32_t cp = 0;
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty()) return 0;
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
        if (r.ec != std::errc() || r.ptr != digits.data() + digits.size() || cp == 0 || cp > 0x10FFFF) return 0;
        appendCodepoint(out, cp);
        return semi - i + 1;
    }
    auto it = named.find(name);
    if (it == named.end()) return 0;
    out += it->second;
    return semi - i + 1;
}

// ---------------------------------------------------------------------------- inline parsing

struct Token {
    Span span;
    bool delim = false;
    char ch = 0;
    int count = 0, orig = 0;
    bool canOpen = false, canClose = false;
};

class Inline {
public:
    explicit Inline(const std::unordered_map<std::string, std::string>& refs) : refs_(refs) {}

    std::vector<Span> parse(std::string_view s, std::string linkUrl = {}, int depth = 0) {
        std::vector<Token> toks;
        std::string buf;
        std::string htmlLink = linkUrl;
        std::uint8_t htmlStyle = 0;
        int styleCount[4] = {0, 0, 0, 0};

        auto flush = [&] {
            if (buf.empty()) return;
            Token t;
            t.span.text = std::move(buf);
            t.span.style = htmlStyle;
            t.span.url = htmlLink;
            toks.push_back(std::move(t));
            buf.clear();
        };
        auto pushSpan = [&](Span sp) {
            flush();
            if (sp.url.empty()) sp.url = htmlLink;
            sp.style |= htmlStyle;
            Token t;
            t.span = std::move(sp);
            toks.push_back(std::move(t));
        };

        for (std::size_t i = 0; i < s.size();) {
            const char c = s[i];
            if (c == '\\' && i + 1 < s.size()) {
                if (s[i + 1] == '\n') { Span b; b.brk = true; pushSpan(std::move(b)); i += 2; continue; }
                if (isPunct(s[i + 1])) { buf += s[i + 1]; i += 2; continue; }
            }
            if (c == '`') {
                std::size_t n = 0;
                while (i + n < s.size() && s[i + n] == '`') ++n;
                std::size_t j = i + n;
                std::size_t close = std::string_view::npos;
                while (j < s.size()) {
                    const auto p = s.find('`', j);
                    if (p == std::string_view::npos) break;
                    std::size_t m = 0;
                    while (p + m < s.size() && s[p + m] == '`') ++m;
                    if (m == n) { close = p; break; }
                    j = p + m;
                }
                if (close != std::string_view::npos) {
                    std::string code(s.substr(i + n, close - i - n));
                    for (auto& ch : code) if (ch == '\n') ch = ' ';
                    if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && code.find_first_not_of(' ') != std::string::npos)
                        code = code.substr(1, code.size() - 2);
                    Span sp;
                    sp.text = std::move(code);
                    sp.style = Code;
                    pushSpan(std::move(sp));
                    i = close + n;
                    continue;
                }
                buf.append(n, '`');
                i += n;
                continue;
            }
            if (c == '*' || c == '_' || c == '~') {
                std::size_t n = 0;
                while (i + n < s.size() && s[i + n] == c) ++n;
                if (c == '~' && n != 2) { buf.append(n, c); i += n; continue; }
                const char prev = i == 0 ? ' ' : s[i - 1];
                const char next = i + n >= s.size() ? ' ' : s[i + n];
                const bool left = !isSpace(next) && (!isPunct(next) || isSpace(prev) || isPunct(prev));
                const bool right = !isSpace(prev) && (!isPunct(prev) || isSpace(next) || isPunct(next));
                flush();
                Token t;
                t.delim = true;
                t.ch = c;
                t.count = t.orig = static_cast<int>(n);
                if (c == '_') {
                    t.canOpen = left && (!right || isPunct(prev));
                    t.canClose = right && (!left || isPunct(next));
                } else {
                    t.canOpen = left;
                    t.canClose = right;
                }
                toks.push_back(std::move(t));
                i += n;
                continue;
            }
            if ((c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '[')) && depth < 6) {
                if (std::size_t used = link(s, i, depth, pushSpan, htmlLink)) { i += used; continue; }
            }
            if (c == '<') {
                if (std::size_t used = html(s, i, buf, pushSpan, htmlLink, htmlStyle, styleCount, linkUrl)) { i += used; continue; }
            }
            if (c == '&') {
                if (std::size_t used = entity(s, i, buf)) { i += used; continue; }
            }
            if (c == '\n') {
                std::size_t trailing = 0;
                while (!buf.empty() && buf.back() == ' ') { buf.pop_back(); ++trailing; }
                if (trailing >= 2) { Span b; b.brk = true; pushSpan(std::move(b)); }
                else buf += ' ';
                ++i;
                while (i < s.size() && s[i] == ' ') ++i;
                continue;
            }
            if (c == 'h' && htmlLink.empty() && (i == 0 || isSpace(s[i - 1]) || s[i - 1] == '(')) {
                if (std::size_t used = bareUrl(s, i, pushSpan)) { i += used; continue; }
            }
            buf += c;
            ++i;
        }
        flush();
        resolveEmphasis(toks);

        std::vector<Span> out;
        for (auto& t : toks) {
            if (t.delim) {
                if (t.count <= 0) continue;
                Span sp;
                sp.text.assign(static_cast<std::size_t>(t.count), t.ch);
                sp.url = linkUrl;
                out.push_back(std::move(sp));
                continue;
            }
            if (!out.empty()) {   // merge neighbours that look the same
                Span& p = out.back();
                if (!p.brk && !t.span.brk && p.src.empty() && t.span.src.empty() && p.style == t.span.style && p.url == t.span.url) {
                    p.text += t.span.text;
                    continue;
                }
            }
            out.push_back(std::move(t.span));
        }
        return out;
    }

private:
    const std::unordered_map<std::string, std::string>& refs_;

    static void resolveEmphasis(std::vector<Token>& toks) {
        for (std::size_t c = 0; c < toks.size(); ++c) {
            if (!toks[c].delim || !toks[c].canClose || toks[c].count == 0) continue;
            std::size_t o = c;
            bool found = false;
            while (o-- > 0) {
                Token& op = toks[o];
                if (!op.delim || !op.canOpen || op.ch != toks[c].ch || op.count == 0) continue;
                if (op.ch == '~' && (op.count < 2 || toks[c].count < 2)) continue;
                if (op.ch != '~' && (op.canClose || toks[c].canOpen) && (op.orig + toks[c].orig) % 3 == 0 &&
                    !(op.orig % 3 == 0 && toks[c].orig % 3 == 0))
                    continue;
                found = true;
                break;
            }
            if (!found) continue;
            const bool tilde = toks[c].ch == '~';
            const int use = tilde ? 2 : (toks[o].count >= 2 && toks[c].count >= 2 ? 2 : 1);
            const std::uint8_t bit = tilde ? Strike : (use == 2 ? Bold : Italic);
            for (std::size_t k = o + 1; k < c; ++k) {
                if (toks[k].delim) { toks[k].canOpen = toks[k].canClose = false; continue; }   // left as literal text
                toks[k].span.style |= bit;
            }
            toks[o].count -= use;
            toks[c].count -= use;
            if (toks[c].count > 0) --c;   // the closer may match another opener
        }
    }

    template <typename Push>
    std::size_t link(std::string_view s, std::size_t i, int depth, Push& pushSpan, const std::string& htmlLink) {
        const bool image = s[i] == '!';
        const std::size_t open = i + (image ? 1 : 0);
        // find the matching ']'
        int level = 0;
        std::size_t close = std::string_view::npos;
        for (std::size_t j = open; j < s.size(); ++j) {
            if (s[j] == '\\') { ++j; continue; }
            if (s[j] == '`') {   // skip code spans
                std::size_t n = 0;
                while (j + n < s.size() && s[j + n] == '`') ++n;
                const auto p = s.find(std::string(n, '`'), j + n);
                if (p != std::string_view::npos) { j = p + n - 1; continue; }
                j += n - 1;
                continue;
            }
            if (s[j] == '[') ++level;
            else if (s[j] == ']' && --level == 0) { close = j; break; }
        }
        if (close == std::string_view::npos) return 0;
        const std::string_view label = s.substr(open + 1, close - open - 1);
        std::string dest;
        std::size_t end = close + 1;
        if (end < s.size() && s[end] == '(') {
            std::size_t j = end + 1;
            while (j < s.size() && isSpace(s[j])) ++j;
            if (j < s.size() && s[j] == '<') {
                const auto gt = s.find('>', j);
                if (gt == std::string_view::npos) return 0;
                dest = std::string(s.substr(j + 1, gt - j - 1));
                j = gt + 1;
            } else {
                int paren = 0;
                const std::size_t b = j;
                while (j < s.size() && !isSpace(s[j])) {
                    if (s[j] == '\\') ++j;
                    else if (s[j] == '(') ++paren;
                    else if (s[j] == ')') { if (paren == 0) break; --paren; }
                    ++j;
                }
                dest = std::string(s.substr(b, std::min(j, s.size()) - b));
            }
            while (j < s.size() && isSpace(s[j])) ++j;
            if (j < s.size() && (s[j] == '"' || s[j] == '\'' || s[j] == '(')) {   // optional title
                const char q = s[j] == '(' ? ')' : s[j];
                const auto e = s.find(q, j + 1);
                if (e == std::string_view::npos) return 0;
                j = e + 1;
                while (j < s.size() && isSpace(s[j])) ++j;
            }
            if (j >= s.size() || s[j] != ')') return 0;
            end = j + 1;
        } else {
            std::string key;
            if (end < s.size() && s[end] == '[') {
                const auto e = s.find(']', end);
                if (e == std::string_view::npos) return 0;
                key = std::string(s.substr(end + 1, e - end - 1));
                if (key.empty()) key = std::string(label);
                end = e + 1;
            } else key = std::string(label);
            auto it = refs_.find(lower(trim(key)));
            if (it == refs_.end()) return 0;
            dest = it->second;
        }
        if (image) {
            Span sp;
            sp.text = plainText(Inline(refs_).parse(label, {}, depth + 1));
            sp.src = dest.empty() ? "?" : dest;
            sp.url = htmlLink;
            pushSpan(std::move(sp));
        } else {
            for (auto& sp : parse(label, dest, depth + 1)) pushSpan(std::move(sp));
        }
        return end - i;
    }

    template <typename Push>
    std::size_t bareUrl(std::string_view s, std::size_t i, Push& pushSpan) {
        const std::string_view rest = s.substr(i);
        if (!rest.starts_with("http://") && !rest.starts_with("https://")) return 0;
        std::size_t n = 0;
        int paren = 0;
        while (n < rest.size() && !isSpace(rest[n]) && rest[n] != '<') {
            if (rest[n] == '(') ++paren;
            if (rest[n] == ')' && --paren < 0) break;
            ++n;
        }
        while (n > 0 && std::strchr(".,;:!?'\"*_~", rest[n - 1])) --n;
        if (n < 9) return 0;
        Span sp;
        sp.text = std::string(rest.substr(0, n));
        sp.url = sp.text;
        pushSpan(std::move(sp));
        return n;
    }

    static std::string attr(std::string_view tag, std::string_view name) {
        std::size_t p = 0;
        while ((p = tag.find(name, p)) != std::string_view::npos) {
            const bool boundary = p > 0 && isSpace(tag[p - 1]);
            std::size_t q = p + name.size();
            while (q < tag.size() && isSpace(tag[q])) ++q;
            if (boundary && q < tag.size() && tag[q] == '=') {
                ++q;
                while (q < tag.size() && isSpace(tag[q])) ++q;
                if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                    const auto e = tag.find(tag[q], q + 1);
                    if (e != std::string_view::npos) return std::string(tag.substr(q + 1, e - q - 1));
                } else {
                    const auto e = tag.find_first_of(" \t/>", q);
                    return std::string(tag.substr(q, e == std::string_view::npos ? std::string_view::npos : e - q));
                }
            }
            p += name.size();
        }
        return {};
    }

    template <typename Push>
    std::size_t html(std::string_view s, std::size_t i, std::string& buf, Push& pushSpan, std::string& htmlLink,
                     std::uint8_t& htmlStyle, int* styleCount, const std::string& baseLink) {
        const auto gt = s.find('>', i);
        if (gt == std::string_view::npos || gt - i > 600) return 0;
        const std::string_view tag = s.substr(i + 1, gt - i - 1);
        if (tag.empty()) return 0;
        const std::size_t used = gt - i + 1;
        if (tag.starts_with("!--")) {   // comment
            const auto e = s.find("-->", i);
            return e == std::string_view::npos ? 0 : e + 3 - i;
        }
        // <https://example.com> and <me@example.com>
        if (tag.find(' ') == std::string_view::npos && (tag.starts_with("http://") || tag.starts_with("https://") ||
                                                        (tag.find('@') != std::string_view::npos && tag.find('/') == std::string_view::npos))) {
            Span sp;
            sp.text = std::string(tag);
            sp.url = tag.find('@') != std::string_view::npos && tag.find(':') == std::string_view::npos ? "mailto:" + sp.text : sp.text;
            pushSpan(std::move(sp));
            return used;
        }
        const bool closing = tag[0] == '/';
        std::string_view body = closing ? tag.substr(1) : tag;
        std::size_t nameEnd = 0;
        while (nameEnd < body.size() && (std::isalnum(static_cast<unsigned char>(body[nameEnd])) || body[nameEnd] == '-')) ++nameEnd;
        if (nameEnd == 0 || !std::isalpha(static_cast<unsigned char>(body[0]))) return 0;
        const std::string name = lower(body.substr(0, nameEnd));
        auto styleBit = [&](std::uint8_t bit, int idx) {
            std::string tmp;
            if (!buf.empty()) { Span sp; sp.text = std::move(buf); sp.style = htmlStyle; pushSpan(std::move(sp)); buf.clear(); }
            if (closing) { if (styleCount[idx] > 0 && --styleCount[idx] == 0) htmlStyle &= static_cast<std::uint8_t>(~bit); }
            else { ++styleCount[idx]; htmlStyle |= bit; }
        };
        if (name == "br") { Span b; b.brk = true; pushSpan(std::move(b)); return used; }
        if (name == "img" && !closing) {
            Span sp;
            sp.src = attr(body, "src");
            if (sp.src.empty()) sp.src = "?";
            sp.text = attr(body, "alt");
            if (sp.text.empty()) sp.text = attr(body, "title");
            auto px = [&](std::string_view name) {   // plain pixel counts only; percentages and units are ignored
                const std::string v = attr(body, name);
                int n = 0;
                const auto r = std::from_chars(v.data(), v.data() + v.size(), n);
                return (r.ec == std::errc() && (r.ptr == v.data() + v.size() || std::string_view(r.ptr) == "px") && n > 0 && n < 20000) ? n : 0;
            };
            sp.imgW = px("width");
            sp.imgH = px("height");
            pushSpan(std::move(sp));
            return used;
        }
        if (name == "a") {
            if (!buf.empty()) { Span sp; sp.text = std::move(buf); sp.style = htmlStyle; pushSpan(std::move(sp)); buf.clear(); }
            htmlLink = closing ? baseLink : attr(body, "href");
            return used;
        }
        if (name == "b" || name == "strong") { styleBit(Bold, 0); return used; }
        if (name == "i" || name == "em") { styleBit(Italic, 1); return used; }
        if (name == "code" || name == "kbd" || name == "tt") { styleBit(Code, 2); return used; }
        if (name == "del" || name == "s" || name == "strike") { styleBit(Strike, 3); return used; }
        static const std::unordered_set<std::string> known = {
            "p", "div", "span", "center", "details", "summary", "table", "thead", "tbody", "tr", "td", "th", "ul", "ol", "li",
            "h1", "h2", "h3", "h4", "h5", "h6", "hr", "sub", "sup", "u", "picture", "source", "small", "font", "section", "kbd",
            "blockquote", "pre", "figure", "figcaption", "br", "nobr", "mark", "abbr"};
        if (!known.contains(name)) return 0;   // a stray '<': keep it as text
        return used;   // unknown / structural tags vanish, their text stays
    }
};

// ---------------------------------------------------------------------------- blocks

struct FenceOpen { char ch = 0; int len = 0; std::string info; };

std::optional<FenceOpen> fenceOpen(std::string_view t) {
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~')) return std::nullopt;
    std::size_t n = 0;
    while (n < t.size() && t[n] == t[0]) ++n;
    if (n < 3) return std::nullopt;
    std::string info(trim(t.substr(n)));
    if (t[0] == '`' && info.find('`') != std::string::npos) return std::nullopt;
    return FenceOpen{t[0], static_cast<int>(n), std::move(info)};
}

bool isRule(std::string_view t) {
    if (t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_')) return false;
    int n = 0;
    for (char c : t) {
        if (c == t[0]) ++n;
        else if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return n >= 3;
}

int atxLevel(std::string_view t) {
    std::size_t n = 0;
    while (n < t.size() && t[n] == '#') ++n;
    if (n == 0 || n > 6) return 0;
    if (n < t.size() && t[n] != ' ' && t[n] != '\t') return 0;
    return static_cast<int>(n);
}

int setextLevel(std::string_view t) {
    t = trim(t);
    if (t.empty()) return 0;
    const char c = t[0];
    if (c != '=' && c != '-') return 0;
    for (char x : t) if (x != c) return 0;
    return c == '=' ? 1 : 2;
}

struct Marker { bool ordered = false; int num = 1; char ch = 0; int width = 0; };   // width: marker + following spaces

std::optional<Marker> listMarker(std::string_view t) {
    Marker m;
    if (!t.empty() && (t[0] == '-' || t[0] == '*' || t[0] == '+') && (t.size() == 1 || t[1] == ' ' || t[1] == '\t')) {
        m.ch = t[0];
        std::size_t sp = 1;
        while (sp < t.size() && t[sp] == ' ') ++sp;
        m.width = (sp - 1 >= 1 && sp - 1 <= 4 && sp < t.size()) ? static_cast<int>(sp) : 2;
        return m;
    }
    std::size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    if (d >= 1 && d <= 9 && d < t.size() && (t[d] == '.' || t[d] == ')') && (d + 1 == t.size() || t[d + 1] == ' ' || t[d + 1] == '\t')) {
        m.ordered = true;
        m.ch = t[d];
        std::from_chars(t.data(), t.data() + d, m.num);
        std::size_t sp = d + 1;
        while (sp < t.size() && t[sp] == ' ') ++sp;
        m.width = (sp - d - 1 >= 1 && sp - d - 1 <= 4 && sp < t.size()) ? static_cast<int>(sp) : static_cast<int>(d) + 2;
        return m;
    }
    return std::nullopt;
}

bool htmlBlockStart(std::string_view t) {
    if (t.size() < 2 || t[0] != '<') return false;
    if (t.starts_with("<!--")) return true;
    std::size_t p = t[1] == '/' ? 2 : 1;
    std::size_t e = p;
    while (e < t.size() && std::isalnum(static_cast<unsigned char>(t[e]))) ++e;
    const std::string name = lower(t.substr(p, e - p));
    static const std::unordered_set<std::string> blockTags = {
        "div", "p", "table", "details", "summary", "h1", "h2", "h3", "h4", "h5", "h6", "hr", "ul", "ol", "li", "pre",
        "center", "section", "blockquote", "picture", "figure", "img", "a", "br", "thead", "tbody", "tr", "td", "th", "kbd"};
    return blockTags.contains(name);
}

// Does `t` begin something that interrupts a paragraph?
bool interrupts(std::string_view line) {
    if (indentOf(line) >= 4) return false;
    const std::string_view t = ltrim(line);
    if (fenceOpen(t) || atxLevel(t) || isRule(t) || (!t.empty() && t[0] == '>')) return true;
    if (auto m = listMarker(t)) {
        if (t.size() <= static_cast<std::size_t>(m->width) && !m->ordered) return false;   // an empty bullet cannot interrupt
        return !m->ordered || m->num == 1;
    }
    return htmlBlockStart(t) && t[1] != 'a' && !t.starts_with("<img") && !t.starts_with("<kbd");
}

// Splits a table row at unescaped pipes.
std::vector<std::string> splitRow(std::string_view row) {
    row = trim(row);
    if (!row.empty() && row.front() == '|') row.remove_prefix(1);
    if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\')) row.remove_suffix(1);
    std::vector<std::string> cells;
    std::string cur;
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (row[i] == '\\' && i + 1 < row.size() && row[i + 1] == '|') { cur += '|'; ++i; }
        else if (row[i] == '|') { cells.emplace_back(trim(cur)); cur.clear(); }
        else cur += row[i];
    }
    cells.emplace_back(trim(cur));
    return cells;
}

// `| --- | :-: |` → alignments, or nullopt when the line is not a delimiter row.
std::optional<std::vector<int>> delimiterRow(std::string_view line) {
    if (line.find('-') == std::string_view::npos) return std::nullopt;
    std::vector<int> align;
    for (const auto& cell : splitRow(line)) {
        std::string_view c = cell;
        if (c.empty()) return std::nullopt;
        const bool l = c.front() == ':', r = c.back() == ':';
        if (l) c.remove_prefix(1);
        if (r && !c.empty()) c.remove_suffix(1);
        if (c.empty() || c.find_first_not_of('-') != std::string_view::npos) return std::nullopt;
        align.push_back(l && r ? 1 : r ? 2 : 0);
    }
    return align;
}

class Parser {
public:
    std::vector<Block> parse(std::string_view src) {
        Lines lines;
        std::size_t pos = 0;
        while (pos <= src.size()) {
            auto nl = src.find('\n', pos);
            std::string_view l = src.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            lines.push_back(expandTabs(l));
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        collectReferences(lines);
        return blocks(lines);
    }

private:
    std::unordered_map<std::string, std::string> refs_;

    static std::string expandTabs(std::string_view l) {
        if (l.find('\t') == std::string_view::npos) return std::string(l);
        std::string out;
        for (char c : l) {
            if (c == '\t') out.append(4 - out.size() % 4, ' ');
            else out += c;
        }
        return out;
    }

    // `[label]: url "title"` lines (outside code fences) become reference definitions and disappear from the text.
    void collectReferences(Lines& lines) {
        std::optional<FenceOpen> fence;
        for (auto& l : lines) {
            const std::string_view t = ltrim(l);
            if (fence) {
                if (auto f = fenceOpen(t); f && f->ch == fence->ch && f->len >= fence->len && f->info.empty()) fence.reset();
                continue;
            }
            if (indentOf(l) < 4) if (auto f = fenceOpen(t)) { fence = f; continue; }
            if (indentOf(l) >= 4 || t.size() < 5 || t[0] != '[') continue;
            const auto close = t.find("]:");
            if (close == std::string_view::npos || close < 2 || t[1] == '^') continue;
            std::string_view rest = trim(t.substr(close + 2));
            if (rest.empty()) continue;
            std::string url;
            if (rest[0] == '<') {
                const auto gt = rest.find('>');
                if (gt == std::string_view::npos) continue;
                url = std::string(rest.substr(1, gt - 1));
            } else url = std::string(rest.substr(0, rest.find(' ')));
            refs_.emplace(lower(trim(t.substr(1, close - 1))), std::move(url));
            l.clear();
        }
    }

    std::vector<Span> inlines(std::string_view s) { return Inline(refs_).parse(trim(s)); }


    std::vector<Block> blocks(const Lines& L) {
        std::vector<Block> out;
        const std::size_t n = L.size();
        std::size_t i = 0;
        while (i < n) {
            const std::string& line = L[i];
            if (isBlank(line)) { ++i; continue; }
            const int ind = indentOf(line);
            const std::string_view t = ltrim(line);

            if (ind >= 4) {   // indented code
                Block b;
                b.kind = Kind::Code;
                std::size_t j = i;
                std::string text;
                std::size_t lastCode = i;
                while (j < n && (isBlank(L[j]) || indentOf(L[j]) >= 4)) {
                    if (!isBlank(L[j])) lastCode = j;
                    ++j;
                }
                for (std::size_t k = i; k <= lastCode; ++k) {
                    if (k > i) text += '\n';
                    if (L[k].size() > 4) text += L[k].substr(4);
                }
                b.text = std::move(text);
                out.push_back(std::move(b));
                i = lastCode + 1;
                continue;
            }
            if (auto f = fenceOpen(t)) {
                Block b;
                b.kind = Kind::Code;
                b.lang = f->info.substr(0, f->info.find_first_of(" \t{"));
                std::size_t j = i + 1;
                std::string text;
                bool first = true;
                for (; j < n; ++j) {
                    const std::string_view lt = ltrim(L[j]);
                    if (indentOf(L[j]) < 4)
                        if (auto c = fenceOpen(lt); c && c->ch == f->ch && c->len >= f->len && c->info.empty()) break;
                    if (!first) text += '\n';
                    first = false;
                    text += L[j].size() >= static_cast<std::size_t>(ind) && indentOf(L[j]) >= ind ? L[j].substr(static_cast<std::size_t>(ind)) : std::string(ltrim(L[j]));
                }
                b.text = std::move(text);
                out.push_back(std::move(b));
                i = std::min(n, j + 1);
                continue;
            }
            if (int lvl = atxLevel(t)) {
                Block b;
                b.kind = Kind::Heading;
                b.level = lvl;
                std::string_view body = trim(t.substr(static_cast<std::size_t>(lvl)));
                std::size_t e = body.size();
                while (e > 0 && body[e - 1] == '#') --e;
                if (e == 0) body = {};
                else if (e < body.size() && body[e - 1] == ' ') body = trim(body.substr(0, e));   // closing "##" sequence
                b.spans = inlines(trim(body));
                out.push_back(std::move(b));
                ++i;
                continue;
            }
            if (isRule(t)) { Block b; b.kind = Kind::Rule; out.push_back(std::move(b)); ++i; continue; }
            if (t[0] == '>') {
                Lines inner;
                std::size_t j = i;
                bool lastNonBlank = false;
                while (j < n) {
                    const std::string_view lt = ltrim(L[j]);
                    if (indentOf(L[j]) < 4 && !lt.empty() && lt[0] == '>') {
                        std::string_view body = lt.substr(1);
                        if (!body.empty() && body[0] == ' ') body.remove_prefix(1);
                        inner.emplace_back(body);
                        lastNonBlank = !isBlank(body);
                    } else if (!isBlank(L[j]) && lastNonBlank && !interrupts(L[j])) {   // lazy continuation
                        inner.emplace_back(L[j]);
                    } else break;
                    ++j;
                }
                Block b;
                b.kind = Kind::Quote;
                b.kids = blocks(inner);
                out.push_back(std::move(b));
                i = j;
                continue;
            }
            if (auto m = listMarker(t)) { i = list(L, i, out); continue; }
            if (t[0] == '<' && htmlBlockStart(t)) {
                if (t.starts_with("<!--")) {
                    std::size_t j = i;
                    while (j < n && L[j].find("-->") == std::string::npos) ++j;
                    i = std::min(n, j + 1);
                    continue;
                }
                std::size_t j = i;
                std::string text;
                while (j < n && !isBlank(L[j])) { text += L[j]; text += '\n'; ++j; }
                Block b;
                b.kind = Kind::Paragraph;
                b.spans = inlines(text);
                const bool visible = std::ranges::any_of(b.spans, [](const Span& s) { return !s.src.empty() || s.text.find_first_not_of(' ') != std::string::npos; });
                if (visible) out.push_back(std::move(b));
                i = j;
                continue;
            }
            // table: header row, delimiter row
            if (line.find('|') != std::string::npos && i + 1 < n) {
                if (auto align = delimiterRow(L[i + 1]); align && splitRow(line).size() == align->size()) {
                    Block b;
                    b.kind = Kind::Table;
                    b.align = *align;
                    auto addRow = [&](std::string_view row) {
                        std::vector<std::vector<Span>> cells;
                        auto parts = splitRow(row);
                        parts.resize(align->size());
                        for (const auto& c : parts) cells.push_back(inlines(c));
                        b.rows.push_back(std::move(cells));
                    };
                    addRow(line);
                    std::size_t j = i + 2;
                    while (j < n && !isBlank(L[j]) && (L[j].find('|') != std::string::npos) && !interrupts(L[j])) addRow(L[j++]);
                    out.push_back(std::move(b));
                    i = j;
                    continue;
                }
            }
            // paragraph (possibly a setext heading)
            std::string text;
            std::size_t j = i;
            int setext = 0;
            for (; j < n; ++j) {
                if (isBlank(L[j])) break;
                if (j > i) {
                    if (int lv = setextLevel(L[j]); lv && indentOf(L[j]) < 4) { setext = lv; break; }
                    if (interrupts(L[j])) break;
                }
                if (j > i) text += '\n';
                text += ltrim(L[j]);
            }
            Block b;
            b.spans = inlines(text);
            if (setext) { b.kind = Kind::Heading; b.level = setext; j += 1; }
            else b.kind = Kind::Paragraph;
            if (!b.spans.empty()) out.push_back(std::move(b));
            i = std::max(j, i + 1);
        }
        return out;
    }

    // Parses the list starting at L[i]; returns the index after it.
    std::size_t list(const Lines& L, std::size_t i, std::vector<Block>& out) {
        const std::size_t n = L.size();
        Block lst;
        lst.kind = Kind::List;
        const int baseIndent = indentOf(L[i]);
        const Marker first = *listMarker(ltrim(L[i]));
        lst.ordered = first.ordered;
        lst.start = first.num;
        while (i < n) {
            const int ind = indentOf(L[i]);
            const auto mk = listMarker(ltrim(L[i]));
            if (!mk || mk->ordered != first.ordered || mk->ch != first.ch || ind >= baseIndent + 4 || ind + 4 <= baseIndent || isRule(ltrim(L[i]))) break;
            const int contentIndent = ind + mk->width;
            Lines itemLines;
            std::string_view firstText = ltrim(L[i]).substr(std::min<std::size_t>(static_cast<std::size_t>(mk->width), ltrim(L[i]).size()));
            itemLines.emplace_back(firstText);
            std::size_t j = i + 1;
            bool prevBlank = isBlank(firstText);
            while (j < n) {
                if (isBlank(L[j])) {
                    std::size_t k = j;
                    while (k < n && isBlank(L[k])) ++k;
                    if (k < n && indentOf(L[k]) >= contentIndent && !(prevBlank && isBlank(firstText) && itemLines.size() == 1)) {
                        for (; j < k; ++j) itemLines.emplace_back();
                        prevBlank = true;
                        continue;
                    }
                    break;
                }
                if (indentOf(L[j]) >= contentIndent) {
                    itemLines.push_back(L[j].substr(static_cast<std::size_t>(contentIndent)));
                    prevBlank = false;
                    ++j;
                    continue;
                }
                if (!prevBlank && !interrupts(L[j]) && !listMarker(ltrim(L[j]))) {   // lazy continuation of the item's paragraph
                    itemLines.emplace_back(ltrim(L[j]));
                    ++j;
                    continue;
                }
                break;
            }
            Block item;
            item.kind = Kind::Item;
            if (!itemLines.empty()) {
                std::string_view f = itemLines[0];
                if (f.size() >= 3 && f[0] == '[' && (f[1] == ' ' || f[1] == 'x' || f[1] == 'X') && f[2] == ']' && (f.size() == 3 || f[3] == ' ')) {
                    item.checked = f[1] == ' ' ? 0 : 1;
                    itemLines[0] = std::string(f.size() > 4 ? f.substr(4) : std::string_view());
                }
            }
            item.kids = blocks(itemLines);
            lst.kids.push_back(std::move(item));
            i = j;
            while (i < n && isBlank(L[i])) ++i;   // blank lines between items
            if (i < n) {
                const auto nm = listMarker(ltrim(L[i]));
                if (!nm || indentOf(L[i]) > baseIndent + 3 || nm->ordered != first.ordered || nm->ch != first.ch) break;
            }
        }
        out.push_back(std::move(lst));
        return i;
    }
};

}  // namespace

std::vector<Block> parse(std::string_view source) { return Parser().parse(source); }

std::string plainText(const std::vector<Span>& spans) {
    std::string out;
    for (const auto& s : spans) {
        if (s.brk) out += ' ';
        else out += s.text;
    }
    return out;
}

std::string slug(std::string_view heading) {
    std::string out;
    for (std::size_t i = 0; i < heading.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(heading[i]);
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        else if (c == ' ' || c == '-') out += '-';
        else if (c == '_') out += '_';
        else if (c >= 0x80) out += static_cast<char>(c);   // keep non-ASCII letters
    }
    return out;
}

}  // namespace ide::md
