// daemon.search — project-wide find and replace (no UI): matcher, file enumeration, search and replace passes.
export module daemon.services.search;
import std;
import daemon.base.util;
import daemon.base.process;

namespace fs = std::filesystem;

export namespace ide::search {

struct Options {
    std::string query;
    bool matchCase = false, wholeWord = false, regex = false;
    std::string include, exclude;   // comma separated globs ("*.cpp, src/**"); empty = everything / nothing
};

struct Match {
    int line = 0, col = 0, len = 0;   // 0-based line, byte column and length in the file
    std::string preview;              // the line (trimmed, maybe starting with an ellipsis)
    int pcol = 0;                     // where the match starts inside `preview`
};

struct FileResult {
    std::string path, rel;
    std::vector<Match> matches;
    bool collapsed = false;
};

struct Result {
    std::vector<FileResult> files;
    int total = 0;
    bool truncated = false;
    std::string error;
};

inline bool isWordChar(unsigned char c) { return std::isalnum(c) || c == '_'; }

// ---- globs

// '*' and '?' wildcards ('*' also crosses '/'), everything else literal.
bool globMatch(std::string_view pat, std::string_view text);

std::vector<std::string> splitGlobs(std::string_view s);

// Does the relative path match one glob? Patterns without '/' match any path component (so "*.cpp" and "build" work).
bool pathMatches(std::string pat, std::string_view rel);

inline bool passesFilters(const std::vector<std::string>& inc, const std::vector<std::string>& exc, std::string_view rel) {
    if (!inc.empty() && !std::ranges::any_of(inc, [&](const auto& g) { return pathMatches(g, rel); })) return false;
    return !std::ranges::any_of(exc, [&](const auto& g) { return pathMatches(g, rel); });
}

// ---- matcher

struct Hit {
    int col = 0, len = 0;
    std::string repl;
};

class Matcher {
public:
    explicit Matcher(const Options& o, std::string replacement = {}) : opt_(o), with_(std::move(replacement)) {
        if (o.query.empty()) { error_ = "empty"; return; }
        if (o.regex) {
            try {
                std::string pat = o.wholeWord ? "\\b(?:" + o.query + ")\\b" : o.query;
                auto flags = std::regex::ECMAScript;
                if (!o.matchCase) flags |= std::regex::icase;
                re_ = std::regex(pat, flags);
            } catch (const std::regex_error& e) {
                error_ = std::string("Invalid regular expression: ") + e.what();
            }
        } else {
            needle_ = o.matchCase ? o.query : lower(o.query);
        }
    }

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_ == "empty" ? empty_ : error_; }

    // Non-overlapping matches in one line (without its line terminator).
    void findAll(std::string_view line, std::vector<Hit>& out, bool wantRepl = false) const;

private:
    static std::string lower(std::string_view s) {
        std::string o(s);
        std::ranges::transform(o, o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return o;
    }
    Options opt_;
    std::string with_, needle_, error_;
    std::string empty_ = "Type to search";
    std::regex re_;
};

// ---- files

// The files a search covers: the git index + untracked (respecting .gitignore) when `root` is in a repository,
// otherwise a walk that skips the usual build / vendor folders.
std::vector<std::string> listFiles(const fs::path& root, bool* usedGit = nullptr);

inline bool looksBinary(std::string_view data) { return data.substr(0, 8192).find('\0') != std::string_view::npos; }

// `shouldCancel` is polled between files; `overrides` maps absolute paths to unsaved editor contents.
Result run(const fs::path& root, const Options& opt, const std::map<std::string, std::string>& overrides,
                  const std::function<bool()>& shouldCancel, int maxMatches = 20000);

// Replaces matches in `text`. `only` limits it to the given (line, column) pairs (0-based, original positions); null = all.
std::string replaceInText(std::string_view text, const Options& opt, const std::string& with,
                                 const std::set<std::pair<int, int>>* only, int& count);

// Rewrites a file on disk; returns how many replacements were made (-1 on I/O failure).
int replaceInFile(const fs::path& file, const Options& opt, const std::string& with, const std::set<std::pair<int, int>>* only);

}  // namespace ide::search
