// SPDX-License-Identifier: GPL-3.0-or-later
// notebook - implementation of module daemon.services.notebook.
module;
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
module daemon.services.notebook;
import std;
import daemon.base.util;

namespace fs = std::filesystem;

namespace ide::nb {

// ================================================================================================ .ipynb

namespace {

std::string joinSource(const json::Value& v) {
    if (v.isString()) return v.str();
    std::string out;
    if (const auto* a = v.array()) for (const auto& s : *a) out += s.str();
    return out;
}

std::string stripAnsi(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
        } else out += s[i];
    }
    return out;
}

json::Value linesOf(std::string_view text) {   // nbformat stores text as a list of lines that keep their newline
    auto arr = json::array();
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
        arr.push(std::string(text.substr(pos, end - pos)));
        pos = end;
    }
    return arr;
}

void pretty(std::string& out, const json::Value& v, int level) {
    auto indent = [&](int n) { out.append(static_cast<std::size_t>(n), ' '); };
    if (const auto* a = v.array()) {
        if (a->empty()) { out += "[]"; return; }
        out += "[\n";
        for (std::size_t i = 0; i < a->size(); ++i) {
            indent(level + 1);
            pretty(out, (*a)[i], level + 1);
            out += i + 1 < a->size() ? ",\n" : "\n";
        }
        indent(level);
        out += ']';
    } else if (const auto* o = v.object()) {
        if (o->empty()) { out += "{}"; return; }
        out += "{\n";
        for (std::size_t i = 0; i < o->size(); ++i) {
            indent(level + 1);
            json::detail::dumpString(out, (*o)[i].first);
            out += ": ";
            pretty(out, (*o)[i].second, level + 1);
            out += i + 1 < o->size() ? ",\n" : "\n";
        }
        indent(level);
        out += '}';
    } else if (v.isNumber()) {
        const double d = v.num();
        if (d == std::floor(d) && std::abs(d) < 1e15) out += std::to_string(static_cast<long long>(d));
        else out += json::dump(v);
    } else out += json::dump(v);
}

}  // namespace

bool Notebook::isCpp() const {
    const auto& ks = metadata["kernelspec"];
    std::string probe = ks["language"].str() + " " + ks["name"].str() + " " + metadata["language_info"]["name"].str();
    if (probe.find_first_not_of(' ') == std::string::npos) return true;   // nothing said: a new notebook
    for (auto& c : probe) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return probe.find("c++") != std::string::npos || probe.find("cpp") != std::string::npos || probe.find("cling") != std::string::npos;
}

std::string newCellId() {
    static std::mt19937_64 rng{std::random_device{}()};
    return std::format("{:08x}", static_cast<std::uint32_t>(rng()));
}

std::expected<Notebook, std::string> parseNotebook(std::string_view text) {
    Notebook nb;
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {   // brand new
        auto ks = json::object();
        ks.set("display_name", "C++23 (Daemon)");
        ks.set("language", "c++");
        ks.set("name", "daemon-cpp");
        nb.metadata.set("kernelspec", std::move(ks));
        auto li = json::object();
        li.set("file_extension", ".cpp");
        li.set("mimetype", "text/x-c++src");
        li.set("name", "c++");
        li.set("version", "23");
        nb.metadata.set("language_info", std::move(li));
        Cell c;
        c.id = newCellId();
        nb.cells.push_back(std::move(c));
        return nb;
    }
    auto j = json::parse(text);
    if (!j || !j->isObject()) return std::unexpected("not a notebook file (invalid JSON)");
    const json::Value& root = *j;
    nb.nbformat = root["nbformat"].integer(4);
    nb.nbformatMinor = root["nbformat_minor"].integer(5);
    if (nb.nbformat != 4) return std::unexpected(std::format("unsupported notebook format {} (only nbformat 4)", nb.nbformat));
    if (root["metadata"].isObject()) nb.metadata = root["metadata"];
    if (const auto* cells = root["cells"].array()) {
        for (const auto& cj : *cells) {
            Cell c;
            const std::string type = cj["cell_type"].str("code");
            c.type = type == "markdown" ? Cell::Type::Markdown : type == "raw" ? Cell::Type::Raw : Cell::Type::Code;
            c.source = joinSource(cj["source"]);
            c.id = cj["id"].str();
            if (c.id.empty()) c.id = newCellId();
            c.execCount = cj["execution_count"].integer(0);
            if (cj["metadata"].isObject()) c.metadata = cj["metadata"];
            if (const auto* outs = cj["outputs"].array()) {
                for (const auto& oj : *outs) {
                    Output o;
                    const std::string ot = oj["output_type"].str();
                    if (ot == "stream") {
                        o.kind = Output::Kind::Stream;
                        o.name = oj["name"].str("stdout");
                        o.text = joinSource(oj["text"]);
                    } else if (ot == "error") {
                        o.kind = Output::Kind::Error;
                        o.name = oj["ename"].str();
                        o.text = oj["evalue"].str();
                        if (const auto* tb = oj["traceback"].array(); tb && !tb->empty()) {
                            o.text.clear();
                            for (const auto& l : *tb) o.text += stripAnsi(l.str()) + "\n";
                            while (!o.text.empty() && o.text.back() == '\n') o.text.pop_back();
                        }
                    } else {   // execute_result / display_data: keep the plain text
                        o.kind = Output::Kind::Result;
                        o.text = joinSource(oj["data"]["text/plain"]);
                        if (o.text.empty()) o.text = "[output of type " + (oj["data"].object() && !oj["data"].object()->empty() ? oj["data"].object()->front().first : std::string("unknown")) + " is not shown]";
                    }
                    c.outputs.push_back(std::move(o));
                }
            }
            nb.cells.push_back(std::move(c));
        }
    }
    if (nb.cells.empty()) { Cell c; c.id = newCellId(); nb.cells.push_back(std::move(c)); }
    return nb;
}

std::string serializeNotebook(const Notebook& nb) {
    auto root = json::object();
    auto cells = json::array();
    for (const auto& c : nb.cells) {
        auto cj = json::object();
        cj.set("cell_type", c.type == Cell::Type::Code ? "code" : c.type == Cell::Type::Markdown ? "markdown" : "raw");
        cj.set("id", c.id.empty() ? newCellId() : c.id);
        cj.set("metadata", c.metadata);
        if (c.type == Cell::Type::Code) {
            if (c.execCount > 0) cj.set("execution_count", c.execCount); else cj.set("execution_count", nullptr);
            auto outs = json::array();
            for (const auto& o : c.outputs) {
                auto oj = json::object();
                if (o.kind == Output::Kind::Stream) {
                    oj.set("name", o.name.empty() ? "stdout" : o.name);
                    oj.set("output_type", "stream");
                    oj.set("text", linesOf(o.text));
                } else if (o.kind == Output::Kind::Error) {
                    oj.set("ename", o.name.empty() ? "Error" : o.name);
                    oj.set("evalue", o.text.substr(0, o.text.find('\n')));
                    oj.set("output_type", "error");
                    oj.set("traceback", linesOf(o.text));
                } else {
                    auto data = json::object();
                    data.set("text/plain", linesOf(o.text));
                    oj.set("data", std::move(data));
                    oj.set("execution_count", c.execCount > 0 ? json::Value(c.execCount) : json::Value(nullptr));
                    oj.set("metadata", json::object());
                    oj.set("output_type", "execute_result");
                }
                outs.push(std::move(oj));
            }
            cj.set("outputs", std::move(outs));
        }
        cj.set("source", linesOf(c.source));
        cells.push(std::move(cj));
    }
    root.set("cells", std::move(cells));
    root.set("metadata", nb.metadata);
    root.set("nbformat", nb.nbformat);
    root.set("nbformat_minor", std::max(nb.nbformatMinor, 5));
    std::string out;
    pretty(out, root, 0);
    out += '\n';
    return out;
}

// ================================================================================================ chunking

namespace {

bool isIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool isIdChar(char c) { return isIdStart(c) || std::isdigit(static_cast<unsigned char>(c)); }

// If a comment, string or character literal starts at s[i], returns the index after it, otherwise i.
std::size_t skipLiteral(std::string_view s, std::size_t i) {
    if (i >= s.size()) return i;
    if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') ++i;
        return i;
    }
    if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
        const auto e = s.find("*/", i + 2);
        return e == std::string_view::npos ? s.size() : e + 2;
    }
    // raw string: R"delim( ... )delim" with an optional encoding prefix
    if (s[i] == '"' && i > 0 && s[i - 1] == 'R' && (i == 1 || !isIdChar(s[i - 2]) || s[i - 2] == '8' || s[i - 2] == 'u' || s[i - 2] == 'U' || s[i - 2] == 'L')) {
        const auto open = s.find('(', i);
        if (open != std::string_view::npos && open - i <= 17) {
            const std::string close = ")" + std::string(s.substr(i + 1, open - i - 1)) + "\"";
            const auto e = s.find(close, open);
            return e == std::string_view::npos ? s.size() : e + close.size();
        }
    }
    if (s[i] == '"' || s[i] == '\'') {
        if (s[i] == '\'' && i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])) && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])))
            return i;   // digit separator: 1'000'000
        const char q = s[i];
        ++i;
        while (i < s.size() && s[i] != q && s[i] != '\n') { if (s[i] == '\\') ++i; ++i; }
        return std::min(s.size(), i + 1);
    }
    return i;
}

std::vector<std::string> lex(std::string_view s, std::size_t maxTokens = 80) {
    std::vector<std::string> t;
    std::size_t i = 0;
    while (i < s.size() && t.size() < maxTokens) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        if (const auto j = skipLiteral(s, i); j != i && (s[i] == '/' )) { i = j; continue; }
        if (s[i] == '"' || s[i] == '\'') { i = skipLiteral(s, i); t.push_back("\""); continue; }
        if (isIdStart(s[i])) {
            std::size_t j = i;
            while (j < s.size() && isIdChar(s[j])) ++j;
            if (j < s.size() && (s[j] == '"') && (s.substr(i, j - i) == "R" || s.substr(i, j - i) == "u8R" || s.substr(i, j - i) == "LR")) { i = skipLiteral(s, j); t.push_back("\""); continue; }
            t.emplace_back(s.substr(i, j - i));
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            std::size_t j = i;
            while (j < s.size() && (isIdChar(s[j]) || s[j] == '.' || s[j] == '\'')) ++j;
            t.emplace_back(s.substr(i, j - i));
            i = j;
            continue;
        }
        static const std::array<std::string_view, 12> two = {"::", "->", "<<", ">>", "==", "!=", "<=", ">=", "&&", "||", "++", "--"};
        bool matched = false;
        for (auto op : two) if (s.substr(i, 2) == op) { t.emplace_back(op); i += 2; matched = true; break; }
        if (matched) continue;
        t.emplace_back(1, s[i]);
        ++i;
    }
    return t;
}

bool isKeyword(const std::string& w) {
    static const std::unordered_set<std::string> k = {"if", "for", "while", "do", "switch", "try", "return", "break", "continue", "goto", "throw", "co_return",
                                                       "co_await", "co_yield", "delete", "new", "case", "default", "else", "sizeof", "typeid", "catch"};
    return k.contains(w);
}

bool isSpecifier(const std::string& w) {
    static const std::unordered_set<std::string> k = {"const", "constexpr", "consteval", "constinit", "static", "inline", "thread_local", "volatile", "mutable",
                                                       "register", "extern", "typename", "explicit", "virtual", "friend"};
    return k.contains(w);
}

bool isBuiltinType(const std::string& w) {
    static const std::unordered_set<std::string> k = {"unsigned", "signed", "long", "short", "int", "char", "bool", "float", "double", "void", "wchar_t",
                                                       "char8_t", "char16_t", "char32_t", "auto"};
    return k.contains(w);
}

// Index after the balanced <...> that starts at t[i] == "<", or npos when it is not a template argument list.
std::size_t skipAngles(const std::vector<std::string>& t, std::size_t i) {
    int depth = 0;
    for (std::size_t j = i; j < t.size(); ++j) {
        if (t[j] == "<") ++depth;
        else if (t[j] == ">") { if (--depth == 0) return j + 1; }
        else if (t[j] == ">>") { depth -= 2; if (depth <= 0) return depth == 0 ? j + 1 : std::string::npos; }
        else if (t[j] == ";" || t[j] == "{" || t[j] == "}" || t[j] == "=") { if (!(t[j] == "=" && depth > 0)) return std::string::npos; }
    }
    return std::string::npos;
}

// Does the token sequence start with `type declarator-name` followed by one of = { ( [ ; , or the end?
bool looksLikeDeclaration(const std::vector<std::string>& t) {
    std::size_t i = 0;
    const std::size_t n = t.size();
    bool haveType = false;
    while (i < n) {   // specifiers and builtin type words
        if (isSpecifier(t[i])) { ++i; continue; }
        if (isBuiltinType(t[i])) { haveType = true; ++i; continue; }
        if (t[i] == "decltype" && i + 1 < n && t[i + 1] == "(") {
            int depth = 0;
            for (; i < n; ++i) { if (t[i] == "(") ++depth; else if (t[i] == ")" && --depth == 0) { ++i; break; } }
            haveType = true;
            continue;
        }
        break;
    }
    if (!haveType) {   // a (qualified, maybe templated) type name
        if (i < n && t[i] == "::") ++i;
        if (i >= n || !isIdStart(t[i][0]) || isKeyword(t[i])) return false;
        while (true) {
            ++i;
            if (i < n && t[i] == "<") {
                const auto e = skipAngles(t, i);
                if (e == std::string::npos) return false;
                i = e;
            }
            if (i + 1 < n && t[i] == "::" && isIdStart(t[i + 1][0])) { ++i; continue; }
            break;
        }
    }
    while (i < n && (t[i] == "*" || t[i] == "&" || t[i] == "&&" || t[i] == "const" || t[i] == "volatile" || t[i] == "...")) ++i;
    if (i < n && t[i] == "[") return false;   // structured binding: cannot be exported, stays a local
    if (i >= n || !isIdStart(t[i][0]) || isKeyword(t[i])) return false;
    while (i + 2 < n && t[i + 1] == "::" && isIdStart(t[i + 2][0])) i += 2;   // Foo::bar
    ++i;
    if (i >= n) return true;
    const std::string& a = t[i];
    return a == "=" || a == "{" || a == "(" || a == "[" || a == ";" || a == "," || a == ":";
}

void classify(Chunk& c, bool terminated) {
    const auto t = lex(c.text);
    c.exported = false;
    if (t.empty()) { c.kind = Chunk::Kind::Stmt; return; }
    std::size_t i = 0;
    while (i + 1 < t.size() && t[i] == "[" && t[i + 1] == "[") {   // skip [[attributes]]
        while (i + 1 < t.size() && !(t[i] == "]" && t[i + 1] == "]")) ++i;
        i += 2;
    }
    if (i >= t.size()) { c.kind = Chunk::Kind::Stmt; return; }
    const std::string& w = t[i];
    auto at = [&](std::size_t k) -> const std::string& { static const std::string e; return i + k < t.size() ? t[i + k] : e; };
    if (w == "import" || (w == "export" && at(1) == "import")) { c.kind = Chunk::Kind::Import; return; }
    if (w == "using" || w == "typedef") { c.kind = Chunk::Kind::Using; return; }
    if (w == "namespace") {
        if (at(2) == "=") { c.kind = Chunk::Kind::Using; return; }   // namespace alias
        c.kind = Chunk::Kind::Decl;
        c.exported = at(1) != "{";   // an anonymous namespace has internal linkage
        return;
    }
    if (isKeyword(w) || w == "static_assert" || w == "{") { c.kind = Chunk::Kind::Stmt; return; }
    if (w == "template" || w == "struct" || w == "class" || w == "union" || w == "enum" || w == "concept") {
        // `struct S x = ...;` and `enum E e;` are rare; treat the whole family as exported declarations
        c.kind = Chunk::Kind::Decl;
        c.exported = true;
        return;
    }
    if (w == "extern" || w == "export") { c.kind = Chunk::Kind::Decl; return; }
    if (looksLikeDeclaration(std::vector<std::string>(t.begin() + static_cast<std::ptrdiff_t>(i), t.end()))) {
        c.kind = Chunk::Kind::Decl;
        c.exported = true;
        // `main` stays a plain function: exporting it would make it unreachable for the linker's entry point anyway
        for (std::size_t k = i; k + 1 < t.size(); ++k) if (t[k] == "main" && t[k + 1] == "(") { c.exported = false; break; }
        return;
    }
    c.kind = terminated ? Chunk::Kind::Stmt : Chunk::Kind::Expr;
}

std::string_view trimView(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}  // namespace

std::vector<Chunk> splitChunks(std::string_view s) {
    std::vector<Chunk> out;
    std::vector<std::size_t> nls;
    for (std::size_t i = 0; i < s.size(); ++i) if (s[i] == '\n') nls.push_back(i);
    auto lineOf = [&](std::size_t pos) { return static_cast<int>(std::ranges::lower_bound(nls, pos) - nls.begin()) + 1; };

    std::size_t i = 0;
    while (i < s.size()) {
        // between chunks: whitespace and comments
        if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        if (const auto j = skipLiteral(s, i); j != i && s[i] == '/') { i = j; continue; }
        const std::size_t start = i;
        // a preprocessor line
        if (s[i] == '#') {
            std::size_t j = i;
            while (j < s.size() && s[j] != '\n') { if (s[j] == '\\' && j + 1 < s.size() && s[j + 1] == '\n') ++j; ++j; }
            Chunk c;
            c.kind = Chunk::Kind::Include;
            c.text = std::string(s.substr(i, j - i));
            c.line = lineOf(i);
            out.push_back(std::move(c));
            i = j;
            continue;
        }
        // a statement / declaration: up to the `;` or closing `}` at depth 0
        int depth = 0;
        std::string head;
        bool terminated = false;
        std::size_t j = i;
        while (j < s.size()) {
            if (const auto k = skipLiteral(s, j); k != j) { j = k; continue; }
            const char ch = s[j];
            if (head.empty() && isIdStart(ch)) { std::size_t e = j; while (e < s.size() && isIdChar(s[e])) ++e; head = std::string(s.substr(j, e - j)); }
            if (ch == '(' || ch == '[' || ch == '{') ++depth;
            else if (ch == ')' || ch == ']') depth = std::max(0, depth - 1);
            else if (ch == '}') {
                depth = std::max(0, depth - 1);
                if (depth == 0) {
                    std::size_t k = j + 1;   // what follows decides whether the chunk ends here
                    while (k < s.size()) {
                        if (std::isspace(static_cast<unsigned char>(s[k]))) { ++k; continue; }
                        if (const auto m = skipLiteral(s, k); m != k && s[k] == '/') { k = m; continue; }
                        break;
                    }
                    const char next = k < s.size() ? s[k] : '\0';
                    std::string word;
                    if (isIdStart(next)) { std::size_t e = k; while (e < s.size() && isIdChar(s[e])) ++e; word = std::string(s.substr(k, e - k)); }
                    const bool typeLike = head == "struct" || head == "class" || head == "union" || head == "enum";
                    if (next == ';') { j = k + 1; terminated = true; break; }
                    if (next == ',' || next == ')' || next == '=') { ++j; continue; }
                    if (typeLike) { ++j; continue; }   // `struct S { } s;` - read on to the `;`
                    if ((head == "if" || head == "try") && (word == "else" || word == "catch")) { ++j; continue; }
                    if (head == "do" && word == "while") { ++j; continue; }
                    ++j;
                    terminated = true;
                    break;
                }
            } else if (ch == ';' && depth == 0) { ++j; terminated = true; break; }
            ++j;
        }
        Chunk c;
        c.text = std::string(s.substr(start, j - start));
        c.line = lineOf(start);
        classify(c, terminated);
        if (!trimView(c.text).empty()) out.push_back(std::move(c));
        i = j;
    }
    return out;
}

Plan planCell(std::string_view source) {
    Plan plan;
    // `%flags ...` / `%link ...` magic lines at the top (blanked out so the line numbers stay right)
    std::string text(source);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        const std::size_t end = nl == std::string::npos ? text.size() : nl;
        std::string_view line = trimView(std::string_view(text).substr(pos, end - pos));
        if (line.empty()) { pos = end + 1; continue; }
        if (line[0] != '%') break;
        std::istringstream in{std::string(line.substr(1))};
        std::string cmd, arg;
        in >> cmd;
        if (cmd == "flags" || cmd == "link") {
            while (in >> arg) (cmd == "flags" ? plan.flags : plan.libs).push_back(arg);
        } else plan.error = "unknown magic '%" + cmd + "' (known: %flags, %link)";
        for (std::size_t k = pos; k < end; ++k) text[k] = ' ';
        pos = end + 1;
    }
    if (!plan.error.empty()) return plan;

    // a cell that is a module unit of its own is passed through
    {
        const auto t = lex(text, 8);
        std::size_t k = 0;
        if (k < t.size() && t[k] == "export") ++k;
        const bool gmf = t.size() >= 2 && t[0] == "module" && t[1] == ";";
        if (gmf || (k < t.size() && t[k] == "module" && k + 1 < t.size() && t[k + 1] != ";")) {
            Segment seg;
            seg.kind = Segment::Kind::Module;
            seg.text = text;
            // the module name: the tokens after `module` up to `;` (also in a cell that starts with a global module fragment)
            const auto all = lex(text, 4000);
            for (std::size_t m = 0; m + 1 < all.size(); ++m) {
                if (all[m] == "module" && all[m + 1] != ";" && (m == 0 || all[m - 1] == "export" || all[m - 1] == ";")) {
                    for (std::size_t q = m + 1; q < all.size() && all[q] != ";"; ++q) seg.moduleName += all[q];
                    break;
                }
            }
            plan.segments.push_back(std::move(seg));
            return plan;
        }
    }

    std::vector<Chunk> chunks = splitChunks(text);
    std::vector<Chunk> context;   // includes / imports / usings waiting for the next real chunk
    Segment cur;
    bool have = false;
    auto flush = [&] { if (have) { plan.segments.push_back(std::move(cur)); cur = {}; have = false; } };
    for (auto& c : chunks) {
        const bool real = c.kind == Chunk::Kind::Decl || c.kind == Chunk::Kind::Stmt || c.kind == Chunk::Kind::Expr;
        if (!real) { context.push_back(std::move(c)); continue; }
        const auto want = c.kind == Chunk::Kind::Decl ? Segment::Kind::Decls : Segment::Kind::Stmts;
        if (have && cur.kind != want) flush();
        if (!have) { cur.kind = want; have = true; }
        for (auto& x : context) cur.chunks.push_back(std::move(x));
        context.clear();
        cur.chunks.push_back(std::move(c));
    }
    if (!context.empty()) {
        if (!have) { cur.kind = Segment::Kind::Decls; have = true; }
        for (auto& x : context) cur.chunks.push_back(std::move(x));
    }
    flush();
    return plan;
}

// ================================================================================================ code generation

namespace {

constexpr std::string_view kSupportSource = R"CPP(module;
#include <cstdio>
#include <cxxabi.h>
export module nb_support;
import std;

namespace nb_detail {
template <class T>
std::string repr(const T& v) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) return std::format("{:?}", std::string_view(v));
    else if constexpr (std::is_same_v<T, char>) return std::format("{:?}", v);
    else if constexpr (std::formattable<T, char>) return std::format("{}", v);
    else if constexpr (requires(std::ostringstream& o) { o << v; }) { std::ostringstream o; o << v; return o.str(); }
    else {
        int status = 0;
        char* name = abi::__cxa_demangle(typeid(T).name(), nullptr, nullptr, &status);
        std::string s = std::string("<") + (status == 0 && name ? name : typeid(T).name()) + " object>";
        std::free(name);
        return s;
    }
}
}  // namespace nb_detail

export template <class T>
void nb_show(const T& v) {
    const std::string s = nb_detail::repr(v);
    std::cout.flush();
    std::fflush(stdout);
    std::fputs("\x1b]nb;res;", stdout);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputs("\x07", stdout);
    std::fflush(stdout);
}

export template <class F>
void nb_eval(F&& f) {
    if constexpr (std::is_void_v<decltype(f())>) f();
    else nb_show(f());
}
)CPP";

constexpr std::string_view kKernelSource = R"CPP(// Daemon notebook runner: loads cell modules (shared objects) on request and reports when each is done.
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

static void done(int token, int status, std::string msg) {
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    for (auto& c : msg) if (c == '\n' || c == '\x07' || c == '\x1b') c = ' ';
    std::printf("\x1b]nb;done;%d;%d;%s\x07", token, status, msg.c_str());
    std::fflush(stdout);
}

int main() {
    const int cmd = dup(0);
    const int nul = open("/dev/null", O_RDONLY);
    dup2(nul, 0);   // cells that read stdin see end of file, they must not eat the commands
    setvbuf(stdout, nullptr, _IOLBF, 0);
    FILE* in = fdopen(cmd, "r");
    char* line = nullptr;
    size_t cap = 0;
    while (getline(&line, &cap, in) > 0) {
        char kind = 0;
        int token = 0;
        char entry[512] = {0};
        char path[4096] = {0};
        if (std::sscanf(line, "%c %d %511s %4095[^\n]", &kind, &token, entry, path) < 3) continue;
        if (kind == 'Q') break;
        void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!h) { done(token, 2, dlerror()); continue; }
        if (std::strcmp(entry, "-") != 0) {
            auto fn = reinterpret_cast<void (*)()>(dlsym(h, entry));
            if (!fn) { done(token, 2, dlerror()); continue; }
            try {
                fn();
            } catch (const std::exception& e) {
                done(token, 1, std::string("uncaught exception: ") + e.what());
                continue;
            } catch (...) {
                done(token, 1, "uncaught exception of unknown type");
                continue;
            }
        }
        done(token, 0, "");
    }
    return 0;
}
)CPP";

std::string expandStatic(std::string text) {   // `static int x = 1;` cannot be exported: drop the internal linkage
    std::size_t p = 0;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    for (int round = 0; round < 3; ++round) {
        for (std::string_view kw : {"static", "extern"}) {
            if (text.compare(p, kw.size(), kw) == 0 && p + kw.size() < text.size() && !isIdChar(text[p + kw.size()])) {
                text.erase(p, kw.size());
                while (p < text.size() && text[p] == ' ') text.erase(p, 1);
            }
        }
    }
    return text;
}

struct CellState {
    int id = -1;
    std::vector<std::string> gmf, imports, usings, modules;
};

}  // namespace

namespace {

void addUnique(std::vector<std::string>& v, const std::string& s) {
    if (std::ranges::find(v, s) == v.end()) v.push_back(s);
}

// The text of one module unit for a segment.
std::string assemble(const Segment& seg, const std::vector<CellState>& states, const CellState& cur, int cellId, const std::string& modName,
                     const std::string& entry) {
    std::string gmf, imports, usings;
    auto take = [&](const CellState& st) {
        for (const auto& g : st.gmf) gmf += g + "\n";
        for (const auto& m : st.modules) imports += "import " + m + ";\n";
        for (const auto& u : st.imports) imports += u + "\n";
        for (const auto& u : st.usings) usings += u + "\n";
    };
    for (const auto& st : states) if (st.id != cellId) take(st);
    take(cur);
    std::string body;
    std::string ownImports;
    for (const auto& c : seg.chunks) {
        switch (c.kind) {
            case Chunk::Kind::Include: gmf += c.text + "\n"; break;
            case Chunk::Kind::Import: ownImports += c.text + "\n"; break;
            default: break;
        }
    }
    const bool stmts = seg.kind == Segment::Kind::Stmts;
    if (stmts) body += "extern \"C\" void " + entry + "() {\n";
    for (const auto& c : seg.chunks) {
        if (c.kind == Chunk::Kind::Include || c.kind == Chunk::Kind::Import) continue;
        body += "#line " + std::to_string(c.line) + " \"cell\"\n";
        if (c.kind == Chunk::Kind::Expr) body += "nb_eval([&]() -> decltype(auto) { return (" + std::string(trimView(c.text)) + "); });\n";
        else if (c.kind == Chunk::Kind::Decl && c.exported && !stmts) body += "export " + expandStatic(c.text) + "\n";
        else body += c.text + "\n";
    }
    if (stmts) body += "\n}\n";
    std::string out = "module;\n#include <cstdio>\n" + gmf + "export module " + modName + ";\nimport std;\nimport nb_support;\n" + imports + ownImports;
    if (!stmts) out += usings;
    else out += "";
    out += body;
    if (stmts && !usings.empty()) {   // using-directives of earlier cells must be visible inside the function too
        const auto p = out.find("extern \"C\" void " + entry + "() {\n");
        out.insert(p + std::string("extern \"C\" void " + entry + "() {\n").size(), usings);
    }
    return out;
}

}  // namespace

// ================================================================================================ the session

struct Session::Impl {
    struct Request { int cell; std::string source; };

    std::string notebookPath, dir, sharedCache;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Request> queue;
    std::vector<Event> events;
    State state = State::Idle;
    std::string status = "not started";
    bool stop = false, busy = false;
    std::atomic<bool> interrupted{false};
    std::atomic<int> epoch{0};   // bumped by restart / interrupt: the worker drops what it was doing

    // --- worker-thread state
    bool prepared = false;
    std::string cxx, cxxVersion;
    std::vector<std::string> flags, libs;
    std::vector<CellState> states;
    std::unordered_map<std::string, std::string> userModules;   // module name -> source, for this kernel only
    int generation = 0, unit = 0, token = 0, execCounter = 0;
    std::unique_ptr<Process> kernel;
    std::string pending;   // output read but not parsed yet
    std::jthread worker;

    void emit(Event e) {
        std::lock_guard lock(mu);
        events.push_back(std::move(e));
    }
    void setState(State s, std::string text = {}) {
        std::lock_guard lock(mu);
        state = s;
        if (!text.empty()) status = std::move(text);
    }
    void note(int cell, const std::string& text) { Event e; e.type = Event::Type::Status; e.cell = cell; e.text = text; emit(std::move(e)); }

    void loop(std::stop_token st);
    bool prepare(int cell, std::string& error);
    bool compile(const std::string& unitName, const std::string& source, bool shared, std::string& diagnostics);
    bool startKernel(std::string& error);
    void killKernel();
    bool load(int cell, const std::string& so, const std::string& entry, bool& died);
    void runRequest(const Request& r);
    std::vector<std::string> baseFlags() const {
        std::vector<std::string> f = {"-std=c++23", "-fmodules", "-fno-diagnostics-color", "-fdiagnostics-plain-output"};
        for (const auto& x : flags) f.push_back(x);
        return f;
    }
};

std::string Session::compilerName() {
    if (const char* c = std::getenv("DAEMON_NB_CXX"); c && *c) return c;
    return "g++";
}

Session::Session(std::string notebookPath) : d_(std::make_unique<Impl>()) {
    d_->notebookPath = std::move(notebookPath);
    fs::path base;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = fs::path(h) / ".cache";
    else base = fs::temp_directory_path();
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : d_->notebookPath) { hash ^= c; hash *= 1099511628211ull; }
    hash ^= static_cast<std::uint64_t>(::getpid()) * 0x9E3779B97F4A7C15ull;   // two Daemons may open the same notebook
    d_->dir = (base / "daemon" / "notebook" / std::format("s{:016x}", hash)).string();
    d_->sharedCache = (base / "daemon" / "notebook" / "shared").string();
    std::error_code ec;
    fs::remove_all(d_->dir, ec);
    {   // directories of sessions that were never cleaned up (a crash): forget the ones untouched for a day
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
        for (const auto& e : fs::directory_iterator(fs::path(d_->dir).parent_path(), ec))
            if (e.is_directory(ec) && e.path().filename().string().starts_with('s') && fs::last_write_time(e.path(), ec) < cutoff) fs::remove_all(e.path(), ec);
    }
    fs::create_directories(d_->dir, ec);
    d_->worker = std::jthread([this](std::stop_token st) { d_->loop(st); });
}

Session::~Session() {
    {
        std::lock_guard lock(d_->mu);
        d_->stop = true;
    }
    d_->interrupted = true;
    d_->killKernel();
    d_->cv.notify_all();
    d_->worker.request_stop();
    if (d_->worker.joinable()) d_->worker.join();
    std::error_code ec;
    fs::remove_all(d_->dir, ec);
}

void Session::request(int index, std::string source) {
    {
        std::lock_guard lock(d_->mu);
        d_->queue.push_back({index, std::move(source)});
        if (d_->state == State::Idle || d_->state == State::Dead) d_->state = State::Busy;
    }
    d_->cv.notify_all();
}

std::vector<Event> Session::poll() {
    std::lock_guard lock(d_->mu);
    std::vector<Event> out;
    out.swap(d_->events);
    return out;
}

void Session::interrupt() {
    std::vector<int> dropped;
    {
        std::lock_guard lock(d_->mu);
        for (const auto& r : d_->queue) dropped.push_back(r.cell);
        d_->queue.clear();
    }
    d_->interrupted = true;
    ++d_->epoch;
    d_->killKernel();
    for (int c : dropped) { Event e; e.type = Event::Type::Finished; e.cell = c; e.ok = false; d_->emit(std::move(e)); }
}

void Session::restart() { interrupt(); }

Session::State Session::state() const {
    std::lock_guard lock(d_->mu);
    return d_->state;
}

std::string Session::statusLine() const {
    std::lock_guard lock(d_->mu);
    const char* s = d_->state == State::Idle ? "idle" : d_->state == State::Busy ? "busy" : d_->state == State::Starting ? "starting" : "stopped";
    return d_->status + " - " + s;
}

bool Session::pending() const {
    std::lock_guard lock(d_->mu);
    return !d_->queue.empty() || d_->busy;
}

void Session::Impl::killKernel() {
    if (kernel && kernel->started()) {
        ::kill(-kernel->pid(), SIGKILL);   // never wait for a cell that may be stuck in a loop
    }
}

bool Session::Impl::compile(const std::string& unitName, const std::string& source, bool shared, std::string& diagnostics) {
    const fs::path src = fs::path(dir) / (unitName + ".cpp");
    if (!writeFile(src, source)) { diagnostics = "cannot write " + src.string(); return false; }
    ProcessOptions o;
    o.cwd = dir;
    o.argv = {cxx};
    for (const auto& f : baseFlags()) o.argv.push_back(f);
    if (shared) {
        for (const char* f : {"-fPIC", "-shared", "-O0", "-w"}) o.argv.push_back(f);
        for (const auto& i : {std::string("-I") + fs::path(notebookPath).parent_path().string()}) o.argv.push_back(i);
        o.argv.push_back("-o");
        o.argv.push_back(unitName + ".so");
        o.argv.push_back(unitName + ".cpp");
        for (const auto& l : libs) o.argv.push_back(l);
    } else {
        o.argv.push_back("-fmodule-only");
        o.argv.push_back("-c");
        o.argv.push_back(unitName + ".cpp");
    }
    auto r = Process::runCapture(o, 180000);
    if (!r.error.empty()) { diagnostics = r.error; return false; }
    diagnostics = r.output;
    if (r.timedOut) { diagnostics += "\ncompilation timed out"; return false; }
    return r.exitCode == 0;
}

bool Session::Impl::prepare(int cell, std::string& error) {
    cxx = Session::compilerName();
    auto exe = findExecutable(cxx);
    if (!exe) { error = "compiler '" + cxx + "' not found (install GCC 15+, or set DAEMON_NB_CXX)"; return false; }
    cxx = *exe;
    {
        ProcessOptions o;
        o.argv = {cxx, "-dumpfullversion"};
        auto r = Process::runCapture(o, 10000);
        cxxVersion = std::string(trimView(r.output));
        ProcessOptions o2;
        o2.argv = {cxx, "--version"};
        auto r2 = Process::runCapture(o2, 10000);
        const bool gcc = r2.output.find("Free Software Foundation") != std::string::npos || r2.output.find("g++") == 0 || r2.output.find("GCC") != std::string::npos;
        int major = 0;
        std::from_chars(cxxVersion.data(), cxxVersion.data() + cxxVersion.size(), major);
        if (!gcc) { error = "notebooks need GCC (C++20 modules with import std); " + cxx + " is not GCC"; return false; }
        if (major < 15) { error = std::format("notebooks need GCC 15 or newer (import std, C++20 modules); found {}", cxxVersion); return false; }
    }
    setState(State::Starting, "g++ " + cxxVersion);
    if (const char* f = std::getenv("DAEMON_NB_FLAGS"); f && *f) {
        std::istringstream in(f);
        std::string a;
        while (in >> a) flags.push_back(a);
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir) / "gcm.cache", ec);
    writeFile(fs::path(dir) / "nb_support.cpp", kSupportSource);
    writeFile(fs::path(dir) / "nb_kernel.cpp", kKernelSource);

    // the std module and the support module only depend on the compiler: build them once and share them between notebooks
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : cxx + cxxVersion + std::string(kSupportSource)) { h ^= c; h *= 1099511628211ull; }
    for (const auto& f : flags) for (unsigned char c : f) { h ^= c; h *= 1099511628211ull; }
    const fs::path shared = fs::path(sharedCache) / std::format("{:016x}", h);
    const fs::path stdGcm = shared / "std.gcm", supGcm = shared / "nb_support.gcm", kernelBin = shared / "nb_kernel";
    if (fs::exists(stdGcm, ec) && fs::exists(supGcm, ec) && fs::exists(kernelBin, ec)) {
        // hard links (the files are only read), copies when the cache is on another file system
        auto link = [&](const fs::path& from, const fs::path& to) {
            std::error_code e2;
            fs::create_hard_link(from, to, e2);
            if (e2) fs::copy_file(from, to, fs::copy_options::overwrite_existing, e2);
        };
        link(stdGcm, fs::path(dir) / "gcm.cache" / "std.gcm");
        link(supGcm, fs::path(dir) / "gcm.cache" / "nb_support.gcm");
        link(kernelBin, fs::path(dir) / "nb_kernel");
    } else {
        note(cell, "Building the std module (first run, about 5 s)...");
        ProcessOptions o;
        o.cwd = dir;
        o.argv = {cxx};
        for (const auto& f : baseFlags()) o.argv.push_back(f);
        for (const char* f : {"-fsearch-include-path", "-fmodule-only", "-c", "bits/std.cc"}) o.argv.push_back(f);
        auto r = Process::runCapture(o, 180000);
        if (r.exitCode != 0) { error = "could not build the std module:\n" + r.output; return false; }
        std::string diag;
        if (!compile("nb_support", std::string(kSupportSource), false, diag)) { error = "could not build the notebook support module:\n" + diag; return false; }
        ProcessOptions k;
        k.cwd = dir;
        k.argv = {cxx, "-O1", "-std=c++17", "-o", "nb_kernel", "nb_kernel.cpp", "-ldl"};
        auto kr = Process::runCapture(k, 120000);
        if (kr.exitCode != 0) { error = "could not build the notebook runner:\n" + kr.output; return false; }
        fs::create_directories(shared, ec);
        fs::copy_file(fs::path(dir) / "gcm.cache" / "std.gcm", stdGcm, fs::copy_options::overwrite_existing, ec);
        fs::copy_file(fs::path(dir) / "gcm.cache" / "nb_support.gcm", supGcm, fs::copy_options::overwrite_existing, ec);
        fs::copy_file(fs::path(dir) / "nb_kernel", kernelBin, fs::copy_options::overwrite_existing, ec);
    }
    prepared = true;
    return true;
}

bool Session::Impl::startKernel(std::string& error) {
    kernel = std::make_unique<Process>();
    ProcessOptions o;
    o.argv = {(fs::path(dir) / "nb_kernel").string()};
    o.cwd = dir;
    o.pipeStdin = true;
    o.mergeStderr = true;
    if (auto r = kernel->start(o); !r) { error = "cannot start the notebook runner: " + r.error(); kernel.reset(); return false; }
    pending.clear();
    ++generation;
    unit = 0;
    states.clear();
    userModules.clear();
    return true;
}

// Sends one load command and relays the output until the runner says it is done. Returns false when the cell failed.
bool Session::Impl::load(int cell, const std::string& so, const std::string& entry, bool& died) {
    died = false;
    const int myToken = ++token;
    if (!kernel->write(std::format("L {} {} {}\n", myToken, entry.empty() ? "-" : entry, so))) { died = true; return false; }
    const int fd = kernel->outFd();
    bool ok = true;
    bool finished = false;
    while (!finished) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 200);
        if (interrupted) { died = true; return false; }
        std::string chunk;
        long n = 0;
        if (r > 0) n = kernel->read(chunk);
        if (r > 0 && n == 0) { died = true; break; }                         // end of stream: the runner is gone
        if (r < 0 || (r == 0 && !kernel->running())) { if (!kernel->running()) { died = true; break; } }
        pending += chunk;
        while (!pending.empty()) {
            const auto esc = pending.find("\x1b]nb;");
            if (esc == std::string::npos) {   // plain output (keep a possible start of a marker for the next read)
                std::size_t keep = 0;
                for (std::size_t k = 1; k <= std::min<std::size_t>(4, pending.size()); ++k)
                    if (std::string_view("\x1b]nb;").starts_with(std::string_view(pending).substr(pending.size() - k))) keep = k;
                if (pending.size() > keep) {
                    Event e; e.cell = cell; e.name = "stdout"; e.text = pending.substr(0, pending.size() - keep);
                    emit(std::move(e));
                    pending.erase(0, pending.size() - keep);
                }
                break;
            }
            if (esc > 0) { Event e; e.cell = cell; e.name = "stdout"; e.text = pending.substr(0, esc); emit(std::move(e)); pending.erase(0, esc); }
            const auto end = pending.find('\x07');
            if (end == std::string::npos) break;   // marker not complete yet
            const std::string body = pending.substr(5, end - 5);
            pending.erase(0, end + 1);
            if (body.starts_with("res;")) {
                Event e; e.type = Event::Type::Result; e.cell = cell; e.text = body.substr(4);
                emit(std::move(e));
            } else if (body.starts_with("done;")) {
                int tk = 0, status = 0;
                char msg[4096] = {0};
                std::sscanf(body.c_str() + 5, "%d;%d;%4095[^\n]", &tk, &status, msg);
                if (tk == myToken) {
                    finished = true;
                    if (status != 0) {
                        ok = false;
                        Event e; e.type = Event::Type::Error; e.cell = cell; e.name = status == 1 ? "Exception" : "LoadError"; e.text = msg;
                        emit(std::move(e));
                    }
                }
            }
        }
    }
    return ok && !died;
}

void Session::Impl::runRequest(const Request& r) {
    const int cell = r.cell;
    const int myEpoch = epoch.load();
    interrupted = false;
    auto fail = [&](const std::string& name, const std::string& text) {
        Event e; e.type = Event::Type::Error; e.cell = cell; e.name = name; e.text = text;
        emit(std::move(e));
    };
    auto finish = [&](bool ok) { Event e; e.type = Event::Type::Finished; e.cell = cell; e.ok = ok; emit(std::move(e)); };
    {
        Event e; e.type = Event::Type::Started; e.cell = cell; e.execCount = ++execCounter;
        emit(std::move(e));
    }
    std::string error;
    if (!prepared && !prepare(cell, error)) { fail("SetupError", error); finish(false); setState(State::Idle, "kernel unavailable"); return; }
    if (!kernel || !kernel->running()) {
        killKernel();
        if (!startKernel(error)) { fail("SetupError", error); finish(false); return; }
    }
    Plan plan = planCell(r.source);
    if (!plan.error.empty()) { fail("SyntaxError", plan.error); finish(false); return; }
    for (const auto& f : plan.flags) if (std::ranges::find(flags, f) == flags.end()) flags.push_back(f);
    for (const auto& l : plan.libs) if (std::ranges::find(libs, l) == libs.end()) libs.push_back(l);

    CellState cur;
    cur.id = cell;
    bool ok = true;
    for (const auto& seg : plan.segments) {
        if (epoch.load() != myEpoch) { ok = false; break; }
        const int n = ++unit;
        std::string diag;
        if (seg.kind == Segment::Kind::Module) {
            // A loaded module cannot be replaced (its symbols stay in the process): the same text is a no-op, a changed one needs a restart.
            if (!seg.moduleName.empty()) {
                auto known = userModules.find(seg.moduleName);
                if (known != userModules.end()) {
                    if (known->second == seg.text) continue;
                    fail("ModuleError", "module '" + seg.moduleName + "' was already built in this session with different code; press Restart to rebuild it, or give the new version another name");
                    ok = false;
                    break;
                }
            }
            const std::string name = std::format("nbm{}_{}", generation, n);
            if (!compile(name, seg.text, true, diag)) { fail("CompileError", diag); ok = false; break; }
            bool died = false;
            if (!load(cell, (fs::path(dir) / (name + ".so")).string(), "", died)) { if (died) { fail("KernelError", "the kernel died"); } ok = false; break; }
            if (!seg.moduleName.empty()) userModules[seg.moduleName] = seg.text;
            continue;
        }
        const std::string mod = std::format("nb{}_{}", generation, n), entry = std::format("nbrun{}_{}", generation, n);
        const std::string text = assemble(seg, states, cur, cell, mod, entry);
        if (!compile(mod, text, true, diag)) { fail("CompileError", diag); ok = false; break; }
        bool died = false;
        const bool ran = load(cell, (fs::path(dir) / (mod + ".so")).string(), seg.kind == Segment::Kind::Stmts ? entry : "", died);
        if (died) { fail("KernelError", interrupted ? "interrupted - the kernel was stopped, its state is gone" : "the kernel died (crash in the cell); its state is gone"); ok = false; break; }
        if (!ran) { ok = false; }
        // remember what later cells have to repeat, and the module they import
        for (const auto& c : seg.chunks) {
            const std::string t = std::string(trimView(c.text));
            if (c.kind == Chunk::Kind::Include) addUnique(cur.gmf, t);
            else if (c.kind == Chunk::Kind::Import) addUnique(cur.imports, t);
            else if (c.kind == Chunk::Kind::Using) addUnique(cur.usings, t);
        }
        if (seg.kind == Segment::Kind::Decls) cur.modules.push_back(mod);
        else if (std::ranges::any_of(seg.chunks, [](const Chunk& c) { return c.kind == Chunk::Kind::Stmt || c.kind == Chunk::Kind::Expr; })) {}
        if (!ran) break;
    }
    if (epoch.load() == myEpoch) {   // (after an interrupt the state was thrown away with the kernel)
        std::erase_if(states, [&](const CellState& s) { return s.id == cell; });
        states.push_back(std::move(cur));
    } else {
        states.clear();
        kernel.reset();
    }
    finish(ok);
}

void Session::Impl::loop(std::stop_token st) {
    while (!st.stop_requested()) {
        Request r;
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return stop || !queue.empty(); });
            if (stop) return;
            r = std::move(queue.front());
            queue.pop_front();
            busy = true;
            state = State::Busy;
        }
        runRequest(r);
        {
            std::lock_guard lock(mu);
            busy = false;
            if (queue.empty()) state = (kernel && kernel->started() && kernel->running()) || !prepared ? State::Idle : State::Dead;
        }
    }
}

// ================================================================================================ batch run

int runAll(Notebook& nb, const std::string& notebookPath, std::ostream* log) {
    Session session(notebookPath);
    int failed = 0;
    for (std::size_t i = 0; i < nb.cells.size(); ++i) {
        Cell& c = nb.cells[i];
        if (c.type != Cell::Type::Code || std::ranges::all_of(c.source, [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); })) continue;
        c.outputs.clear();
        session.request(static_cast<int>(i), c.source);
        bool done = false;
        while (!done) {
            for (auto& e : session.poll()) {
                if (e.cell != static_cast<int>(i) && e.type != Event::Type::Status) continue;
                switch (e.type) {
                    case Event::Type::Started: c.execCount = e.execCount; break;
                    case Event::Type::Stream:
                        if (!c.outputs.empty() && c.outputs.back().kind == Output::Kind::Stream && c.outputs.back().name == e.name) c.outputs.back().text += e.text;
                        else c.outputs.push_back({Output::Kind::Stream, e.name, e.text});
                        if (log) *log << e.text;
                        break;
                    case Event::Type::Result:
                        c.outputs.push_back({Output::Kind::Result, "", e.text});
                        if (log) *log << "=> " << e.text << "\n";
                        break;
                    case Event::Type::Error:
                        c.outputs.push_back({Output::Kind::Error, e.name, e.text});
                        if (log) *log << "[" << e.name << "] " << e.text << "\n";
                        break;
                    case Event::Type::Status: if (log) *log << "(" << e.text << ")\n"; break;
                    case Event::Type::Finished: done = true; if (!e.ok) ++failed; break;
                    default: break;
                }
            }
            if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return failed;
}

}  // namespace ide::nb
