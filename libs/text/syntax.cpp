// SPDX-License-Identifier: GPL-3.0-or-later
// syntax - implementation of module daemon.text.syntax.
module daemon.text.syntax;
import std;
import daemon.text.buffer;

namespace ide::detail {

std::unordered_set<std::string> words(std::string_view list) {
    std::unordered_set<std::string> out;
    std::size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace(static_cast<unsigned char>(list[i]))) ++i;
        std::size_t j = i;
        while (j < list.size() && !std::isspace(static_cast<unsigned char>(list[j]))) ++j;
        if (j > i) out.emplace(list.substr(i, j - i));
        i = j;
    }
    return out;
}

std::vector<Lang> buildLanguages() {
    std::vector<Lang> v;
    auto add = [&](std::string name, std::string_view exts, std::string_view files, std::string_view kw,
                   std::string_view ty, std::string_view cs, std::string lc, std::string bo,
                   std::string bc) -> Lang& {
        Lang l;
        l.name = std::move(name);
        l.exts = split(exts);
        l.filenames = split(files);
        l.keywords = words(kw);
        l.types = words(ty);
        l.constants = words(cs);
        l.lineComment = std::move(lc);
        l.blockOpen = std::move(bo);
        l.blockClose = std::move(bc);
        v.push_back(std::move(l));
        return v.back();
    };

    const std::string_view cKw =
        "auto break case const continue default do else enum extern for goto if inline register "
        "restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof "
        "_Atomic _Generic _Noreturn _Static_assert _Thread_local";
    const std::string_view cTypes =
        "void char short int long float double signed unsigned bool _Bool size_t ssize_t ptrdiff_t "
        "intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t FILE";

    {
        auto& l = add("C", "c h", "", cKw, cTypes, "NULL true false", "//", "/*", "*/");
        l.preproc = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("C++", "cc cpp cxx c++ hh hpp hxx h++ ipp inl tpp cppm ixx mpp cu cuh", "",
                      "alignas alignof and asm auto break case catch class concept const consteval "
                      "constexpr constinit const_cast continue co_await co_return co_yield decltype "
                      "default delete do dynamic_cast else enum explicit export extern for friend goto "
                      "if import inline module mutable namespace new noexcept not operator or private "
                      "protected public register reinterpret_cast requires return sizeof static "
                      "static_assert static_cast struct switch template this thread_local throw try "
                      "typedef typeid typename union using virtual volatile while override final xor",
                      "void bool char short int long float double signed unsigned wchar_t char8_t "
                      "char16_t char32_t size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t "
                      "int32_t int64_t uint8_t uint16_t uint32_t uint64_t string string_view vector "
                      "array span optional variant expected unique_ptr shared_ptr map unordered_map set",
                      "nullptr NULL true false", "//", "/*", "*/");
        l.preproc = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("GLSL", "glsl vert frag comp geom tesc tese", "",
                      "const uniform in out inout layout flat smooth centroid precision highp mediump "
                      "lowp if else for while do break continue return discard struct buffer shared "
                      "readonly writeonly coherent restrict",
                      "void bool int uint float double vec2 vec3 vec4 ivec2 ivec3 ivec4 uvec2 uvec3 "
                      "uvec4 mat2 mat3 mat4 sampler2D sampler3D samplerCube image2D",
                      "true false", "//", "/*", "*/");
        l.preproc = true;
    }
    {
        auto& l = add("Python", "py pyw pyi", "SConstruct SConscript",
                      "and as assert async await break class continue def del elif else except finally "
                      "for from global if import in is lambda nonlocal not or pass raise return try "
                      "while with yield match case",
                      "int float str bool bytes list dict set tuple object type range complex",
                      "True False None self cls", "#", "", "");
        l.tripleQuotes = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("Rust", "rs", "",
                      "as async await break const continue crate dyn else enum extern fn for if impl in "
                      "let loop match mod move mut pub ref return self Self static struct super trait "
                      "type unsafe use where while macro_rules",
                      "i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String "
                      "Vec Option Result Box Rc Arc",
                      "true false None Some Ok Err", "//", "/*", "*/");
        l.lifetimes = true;
        l.capsHeuristics = true;
    }
    const std::string_view jsKw =
        "async await break case catch class const continue debugger default delete do else enum export "
        "extends finally for from function if implements import in instanceof interface let new of "
        "package private protected public return static super switch this throw try typeof var void "
        "while with yield as type namespace declare readonly abstract";
    {
        auto& l = add("JavaScript", "js jsx mjs cjs", "", jsKw, "", "true false null undefined NaN Infinity", "//",
                      "/*", "*/");
        l.quotes = "\"'`";
        l.backtickMulti = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("TypeScript", "ts tsx mts cts", "", jsKw,
                      "string number boolean any unknown never object symbol bigint",
                      "true false null undefined NaN Infinity", "//", "/*", "*/");
        l.quotes = "\"'`";
        l.backtickMulti = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("Go", "go", "",
                      "break case chan const continue default defer else fallthrough for func go goto if "
                      "import interface map package range return select struct switch type var",
                      "bool byte rune string int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 "
                      "uintptr float32 float64 complex64 complex128 error any",
                      "true false nil iota", "//", "/*", "*/");
        l.quotes = "\"'`";
        l.backtickMulti = true;
        l.capsHeuristics = true;
    }
    {
        auto& l = add("Java", "java kt kts cs", "",
                      "abstract assert break case catch class const continue default do else enum "
                      "extends final finally for goto if implements import instanceof interface native "
                      "new package private protected public return static strictfp super switch "
                      "synchronized this throw throws transient try volatile while var val fun override "
                      "namespace using readonly sealed",
                      "void boolean byte char short int long float double String Integer Long Object List "
                      "Map Set string bool decimal",
                      "true false null", "//", "/*", "*/");
        l.capsHeuristics = true;
    }
    {
        add("Shell", "sh bash zsh fish ksh", ".bashrc .zshrc .profile PKGBUILD",
                      "if then else elif fi for while until do done case esac in function select time "
                      "return break continue exit export local readonly declare unset source alias echo "
                      "cd set shift trap eval exec",
                      "", "true false", "#", "", "");
    }
    {
        auto& l = add("JSON", "json jsonc json5 geojson", ".eslintrc .babelrc", "", "", "true false null", "", "", "");
        l.quotes = "\"";
        l.jsonKeys = true;
    }
    {
        auto& l = add("CMake", "cmake", "CMakeLists.txt",
                      "if elseif else endif foreach endforeach while endwhile function endfunction macro "
                      "endmacro return break continue block endblock",
                      "", "ON OFF TRUE FALSE", "#", "", "");
        l.quotes = "\"";
    }
    {
        auto& l = add("Config", "toml ini cfg conf yaml yml env", "Makefile makefile Dockerfile .gitignore",
                      "", "", "true false null yes no on off", "#", "", "");
        l.capsHeuristics = false;
    }
    {
        auto& l = add("Lua", "lua", "", "and break do else elseif end for function goto if in local not or "
                      "repeat return then until while", "", "true false nil", "--", "--[[", "]]");
        (void)l;
    }
    return v;
}

}  // namespace ide::detail

namespace ide {

const Lang* langForPath(std::string_view path) {
    std::filesystem::path p{std::string(path)};
    std::string fname = p.filename().string();
    std::string ext = p.extension().string();
    if (!ext.empty()) ext.erase(0, 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const auto& l : languages()) {
        if (std::ranges::find(l.filenames, fname) != l.filenames.end()) return &l;
        if (!ext.empty() && std::ranges::find(l.exts, ext) != l.exts.end()) return &l;
    }
    return nullptr;
}

}  // namespace ide

namespace ide::detail {

std::uint8_t scanLine(const Lang* L, std::string_view s, std::uint8_t st, std::vector<Tok>* out) {
    const std::size_t n = s.size();
    if (out) out->assign(n, Tok::Normal);
    if (!L) return StNormal;
    auto paint = [&](std::size_t a, std::size_t b, Tok t) {
        if (!out) return;
        for (std::size_t k = a; k < b && k < n; ++k) (*out)[k] = t;
    };
    std::size_t i = 0;

    // Continue a construct that started on an earlier line.
    auto resume = [&](std::string_view term, Tok t, bool escapes) -> bool {
        std::size_t j = i;
        while (j < n) {
            if (escapes && s[j] == '\\') { j += 2; continue; }
            if (s.compare(j, term.size(), term) == 0) {
                j += term.size();
                paint(i, j, t);
                i = j;
                return true;
            }
            ++j;
        }
        paint(i, n, t);
        i = n;
        return false;
    };
    if (st == StBlock) {
        if (!resume(L->blockClose, Tok::Comment, false)) return StBlock;
    } else if (st == StTripleD) {
        if (!resume("\"\"\"", Tok::String, true)) return StTripleD;
    } else if (st == StTripleS) {
        if (!resume("'''", Tok::String, true)) return StTripleS;
    } else if (st == StBacktick) {
        if (!resume("`", Tok::String, true)) return StBacktick;
    }

    const std::size_t indentEnd = s.find_first_not_of(" \t");
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t') { ++i; continue; }

        if (L->preproc && c == '#' && i == indentEnd) {
            std::size_t cm = s.find("//", i);
            if (cm == std::string_view::npos) cm = n;
            paint(i, cm, Tok::Preproc);
            i = cm;
            continue;
        }
        if (!L->lineComment.empty() && s.compare(i, L->lineComment.size(), L->lineComment) == 0 &&
            !(L->blockOpen.size() > L->lineComment.size() && s.compare(i, L->blockOpen.size(), L->blockOpen) == 0)) {
            paint(i, n, Tok::Comment);
            return StNormal;
        }
        if (!L->blockOpen.empty() && s.compare(i, L->blockOpen.size(), L->blockOpen) == 0) {
            std::size_t start = i;
            i += L->blockOpen.size();
            auto e = s.find(L->blockClose, i);
            if (e == std::string_view::npos) { paint(start, n, Tok::Comment); return StBlock; }
            i = e + L->blockClose.size();
            paint(start, i, Tok::Comment);
            continue;
        }
        if (L->tripleQuotes && (c == '"' || c == '\'') && i + 2 < n && s[i + 1] == s[i] && s[i + 2] == s[i]) {
            std::size_t start = i;
            i += 3;
            paint(start, i, Tok::String);
            if (!resume(c == '"' ? "\"\"\"" : "'''", Tok::String, true)) return c == '"' ? StTripleD : StTripleS;
            continue;
        }
        if (L->lifetimes && c == '\'' && i + 1 < n && isIdentStart(static_cast<unsigned char>(s[i + 1])) &&
            !(i + 2 < n && s[i + 2] == '\'')) {
            std::size_t j = i + 1;
            while (j < n && isIdentChar(static_cast<unsigned char>(s[j]))) ++j;
            paint(i, j, Tok::Type);
            i = j;
            continue;
        }
        if (L->quotes.find(static_cast<char>(c)) != std::string::npos) {
            std::size_t start = i++;
            bool closed = false;
            while (i < n) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == static_cast<char>(c)) { ++i; closed = true; break; }
                ++i;
            }
            if (i > n) i = n;
            Tok t = Tok::String;
            if (L->jsonKeys && closed) {
                auto nx = s.find_first_not_of(" \t", i);
                if (nx != std::string_view::npos && s[nx] == ':') t = Tok::Key;
            }
            paint(start, i, t);
            if (!closed && c == '`' && L->backtickMulti) return StBacktick;
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            std::size_t j = i;
            const bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            while (j < n) {
                unsigned char d = static_cast<unsigned char>(s[j]);
                if (std::isalnum(d) || d == '_') { ++j; continue; }
                if (d == '.' && !(j + 1 < n && s[j + 1] == '.')) { ++j; continue; }
                if (d == '\'' && j + 1 < n && std::isalnum(static_cast<unsigned char>(s[j + 1]))) { ++j; continue; }
                if ((d == '+' || d == '-') && !hex && j > i && (s[j - 1] == 'e' || s[j - 1] == 'E')) { ++j; continue; }
                break;
            }
            paint(i, j, Tok::Number);
            i = j;
            continue;
        }
        if (isIdentStart(c)) {
            std::size_t j = i;
            while (j < n && isIdentChar(static_cast<unsigned char>(s[j]))) ++j;
            std::string word(s.substr(i, j - i));
            Tok t = Tok::Normal;
            if (L->keywords.contains(word)) t = Tok::Keyword;
            else if (L->types.contains(word)) t = Tok::Type;
            else if (L->constants.contains(word)) t = Tok::Constant;
            else {
                auto nx = s.find_first_not_of(" \t", j);
                if (nx != std::string_view::npos && s[nx] == '(') {
                    t = Tok::Function;
                } else if (L->capsHeuristics && word.size() > 1) {
                    bool upperFirst = std::isupper(static_cast<unsigned char>(word[0])) != 0;
                    bool anyLower = std::ranges::any_of(word, [](unsigned char ch) { return std::islower(ch) != 0; });
                    bool allCaps = std::ranges::all_of(word, [](unsigned char ch) {
                        return std::isupper(ch) || std::isdigit(ch) || ch == '_';
                    });
                    if (allCaps) t = Tok::Constant;
                    else if (upperFirst && anyLower) t = Tok::Type;
                }
            }
            paint(i, j, t);
            i = j;
            continue;
        }
        if (std::string_view("+-*/%=<>!&|^~?:").find(static_cast<char>(c)) != std::string_view::npos)
            paint(i, i + 1, Tok::Operator);
        ++i;
    }
    return StNormal;
}

}  // namespace ide::detail

namespace ide {

void Highlighter::highlight(Buffer& buf, int line, std::vector<Tok>& out) {
    sync(buf);
    if (!lang_) {
        out.assign(buf.line(line).size(), Tok::Normal);
        return;
    }
    while (static_cast<int>(states_.size()) <= line) {
        int prev = static_cast<int>(states_.size()) - 1;
        states_.push_back(detail::scanLine(lang_, buf.line(prev), states_.back(), nullptr));
    }
    detail::scanLine(lang_, buf.line(line), states_[static_cast<std::size_t>(line)], &out);
}

void Highlighter::sync(Buffer& buf) {
    if (buf.version() == seenVersion_) return;
    seenVersion_ = buf.version();
    int d = buf.takeDirtyLine();
    if (d != std::numeric_limits<int>::max() && static_cast<std::size_t>(d) + 1 < states_.size())
        states_.resize(static_cast<std::size_t>(d) + 1);
    if (states_.empty()) states_.push_back(detail::StNormal);
}

}  // namespace ide
