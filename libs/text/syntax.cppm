// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.syntax — table-driven, line-oriented syntax highlighter.
//
// The highlighter tokenises one line at a time, carrying a tiny state (inside a block
// comment / multi-line string) from line to line. States are cached per line and
// invalidated from the first edited line, so only visible lines are ever scanned.
export module daemon.text.syntax;
import std;
import daemon.text.buffer;

export namespace ide {

enum class Tok : std::uint8_t {
    Normal, Keyword, Type, String, Number, Comment, Preproc, Function, Operator, Constant, Key,
};

struct Lang {
    std::string name;
    std::vector<std::string> exts;       // lower-case, without dot
    std::vector<std::string> filenames;  // exact file names (CMakeLists.txt, Makefile, ...)
    std::unordered_set<std::string> keywords, types, constants;
    std::string lineComment, blockOpen, blockClose;
    std::string quotes = "\"'";
    bool tripleQuotes = false;   // Python-style """ strings
    bool backtickMulti = false;  // JS template literals
    bool preproc = false;        // C-style '#' directives
    bool lifetimes = false;      // Rust 'a
    bool capsHeuristics = false; // Capitalised identifiers are types, ALL_CAPS constants
    bool jsonKeys = false;       // "string": -> Key
    bool rawStrings = false;     // C++ R"delim( ... )delim" (multi-line, and GLSL inside is highlighted as GLSL)
};

namespace detail {

std::unordered_set<std::string> words(std::string_view list);

inline std::vector<std::string> split(std::string_view list) {
    auto set = words(list);
    return {set.begin(), set.end()};
}

std::vector<Lang> buildLanguages();

}  // namespace detail

inline const std::vector<Lang>& languages() {
    static const std::vector<Lang> langs = detail::buildLanguages();
    return langs;
}

const Lang* langForPath(std::string_view path);

namespace detail {

enum : std::uint8_t { StNormal = 0, StBlock = 1, StTripleD = 2, StTripleS = 3, StBacktick = 4, StRaw = 5 };

// A raw string literal in progress. The line state is `StRaw | inner << 8 | ctx << 16`: `inner` is the state of the embedded
// language (a GLSL block comment), `ctx` indexes the table of raw strings a Highlighter keeps.
struct RawCtx {
    std::string delim, hint;        // hint: the text before the R" on its line, lower case (names like fragSrc give the language away)
    bool glsl = false;
    int startLine = 0, startCol = 0;   // first character of the content
};
struct ScanEnv {
    std::vector<RawCtx>* ctxs = nullptr;
    int line = 0;
    const Lang* glslLang = nullptr;
    std::function<bool(const std::string& delim, const std::string& hint, int line, int col)> isGlsl;
};

inline bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
inline bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

// Tokenises `s` starting in state `st`; returns the state at the end of the line.
// When `out` is non-null it receives one token class per byte.
std::uint32_t scanLine(const Lang* L, std::string_view s, std::uint32_t st, std::vector<Tok>* out, ScanEnv* env = nullptr);

}  // namespace detail

class Highlighter {
public:
    void setLang(const Lang* l) {
        lang_ = l;
        states_.assign(1, detail::StNormal);
        ctxs_.clear();
    }
    const Lang* lang() const { return lang_; }

    // Fills `out` with one token class per byte of the given line.
    void highlight(Buffer& buf, int line, std::vector<Tok>& out);

    // The content of a raw string literal (between the opening ( and the closing )delim") that contains the position, if any.
    struct RawRegion { Pos begin, end; bool glsl = false; std::string hint; };
    std::optional<RawRegion> rawRegionAt(Buffer& buf, int line, int col);
    // Every multi-line raw string in the buffer that holds GLSL.
    std::vector<RawRegion> glslRegions(Buffer& buf);

private:
    void sync(Buffer& buf);
    detail::ScanEnv env(Buffer& buf, int line);
    void ensure(Buffer& buf, int line);   // line states known up to and including `line`

    const Lang* lang_ = nullptr;
    std::vector<std::uint32_t> states_{detail::StNormal};  // state at the start of each known line
    std::vector<detail::RawCtx> ctxs_;
    std::uint64_t seenVersion_ = 0;
};

}  // namespace ide
