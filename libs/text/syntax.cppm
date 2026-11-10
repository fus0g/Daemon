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

enum : std::uint8_t { StNormal = 0, StBlock = 1, StTripleD = 2, StTripleS = 3, StBacktick = 4 };

inline bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
inline bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

// Tokenises `s` starting in state `st`; returns the state at the end of the line.
// When `out` is non-null it receives one token class per byte.
std::uint8_t scanLine(const Lang* L, std::string_view s, std::uint8_t st, std::vector<Tok>* out);

}  // namespace detail

class Highlighter {
public:
    void setLang(const Lang* l) {
        lang_ = l;
        states_.assign(1, detail::StNormal);
    }
    const Lang* lang() const { return lang_; }

    // Fills `out` with one token class per byte of the given line.
    void highlight(Buffer& buf, int line, std::vector<Tok>& out);

private:
    void sync(Buffer& buf);

    const Lang* lang_ = nullptr;
    std::vector<std::uint8_t> states_{detail::StNormal};  // state at the start of each known line
    std::uint64_t seenVersion_ = 0;
};

}  // namespace ide
