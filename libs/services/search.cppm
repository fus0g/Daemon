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
inline bool globMatch(std::string_view pat, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

inline std::vector<std::string> splitGlobs(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= s.size()) {
        auto j = s.find(',', i);
        std::string_view part = s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) part.remove_prefix(1);
        while (!part.empty() && std::isspace(static_cast<unsigned char>(part.back()))) part.remove_suffix(1);
        if (!part.empty()) out.emplace_back(part);
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

// Does the relative path match one glob? Patterns without '/' match any path component (so "*.cpp" and "build" work).
inline bool pathMatches(std::string pat, std::string_view rel) {
    if (pat.starts_with("./")) pat.erase(0, 2);
    while (pat.size() > 1 && pat.back() == '/') pat.pop_back();
    if (pat.find('/') == std::string::npos) {
        std::size_t i = 0;
        while (i <= rel.size()) {
            auto j = rel.find('/', i);
            if (globMatch(pat, rel.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i))) return true;
            if (j == std::string_view::npos) break;
            i = j + 1;
        }
        return false;
    }
    return globMatch(pat, rel) || globMatch(pat + "/*", rel);
}

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
    void findAll(std::string_view line, std::vector<Hit>& out, bool wantRepl = false) const {
        out.clear();
        if (!ok()) return;
        if (opt_.regex) {
            const std::string s(line);
            for (auto it = std::sregex_iterator(s.begin(), s.end(), re_), end = std::sregex_iterator(); it != end; ++it) {
                if (it->length() == 0) continue;
                Hit h{static_cast<int>(it->position()), static_cast<int>(it->length()), {}};
                if (wantRepl) h.repl = it->format(with_);
                out.push_back(std::move(h));
                if (out.size() >= 200) break;
            }
            return;
        }
        const std::string hay = opt_.matchCase ? std::string() : lower(line);
        const std::string_view h = opt_.matchCase ? line : std::string_view(hay);
        std::size_t at = 0;
        while ((at = h.find(needle_, at)) != std::string_view::npos) {
            const std::size_t e = at + needle_.size();
            if (opt_.wholeWord && ((at > 0 && isWordChar(static_cast<unsigned char>(h[at - 1]))) ||
                                   (e < h.size() && isWordChar(static_cast<unsigned char>(h[e]))))) { ++at; continue; }
            out.push_back({static_cast<int>(at), static_cast<int>(needle_.size()), wantRepl ? with_ : std::string()});
            at = e;
            if (out.size() >= 200) break;
        }
    }

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
inline std::vector<std::string> listFiles(const fs::path& root, bool* usedGit = nullptr) {
    std::vector<std::string> out;
    std::error_code ec;
    if (usedGit) *usedGit = false;
    if (fs::exists(root / ".git", ec) && findExecutable("git")) {
        ProcessOptions o;
        o.argv = {"git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"};
        o.cwd = root.string();
        auto r = Process::runCapture(o, 30000);
        if (r.exitCode == 0) {
            if (usedGit) *usedGit = true;
            std::size_t i = 0;
            while (i < r.output.size()) {
                auto j = r.output.find('\0', i);
                if (j == std::string::npos) j = r.output.size();
                if (j > i) out.push_back(r.output.substr(i, j - i));
                i = j + 1;
            }
            std::erase_if(out, [&](const std::string& rel) { return !fs::is_regular_file(root / rel, ec); });
            return out;
        }
    }
    static const std::set<std::string> skip = {".git", ".hg", ".svn", "node_modules", ".cache", ".daemon", "build", "target", "dist", ".idea", ".vscode"};
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (skip.contains(name) || name.starts_with("cmake-build-")) it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec)) out.push_back(fs::relative(it->path(), root, ec).generic_string());
    }
    return out;
}

inline bool looksBinary(std::string_view data) { return data.substr(0, 8192).find('\0') != std::string_view::npos; }

// `shouldCancel` is polled between files; `overrides` maps absolute paths to unsaved editor contents.
inline Result run(const fs::path& root, const Options& opt, const std::map<std::string, std::string>& overrides,
                  const std::function<bool()>& shouldCancel, int maxMatches = 20000) {
    Result res;
    Matcher m(opt);
    if (!m.ok()) { res.error = m.error(); return res; }
    const auto inc = splitGlobs(opt.include), exc = splitGlobs(opt.exclude);
    std::vector<std::string> files = listFiles(root);
    std::ranges::sort(files);
    std::vector<Hit> hits;
    for (const auto& rel : files) {
        if (shouldCancel && shouldCancel()) return res;
        if (!passesFilters(inc, exc, rel)) continue;
        const fs::path abs = root / rel;
        std::string data;
        if (auto ov = overrides.find(abs.string()); ov != overrides.end()) data = ov->second;
        else {
            std::error_code ec;
            if (fs::file_size(abs, ec) > (2u << 20) || ec) continue;
            auto d = readFile(abs);
            if (!d) continue;
            data = std::move(*d);
        }
        if (looksBinary(data)) continue;
        FileResult fr;
        fr.path = abs.string();
        fr.rel = rel;
        int lineNo = 0;
        for (std::size_t pos = 0; pos <= data.size(); ++lineNo) {
            auto nl = data.find('\n', pos);
            std::size_t end = nl == std::string::npos ? data.size() : nl;
            std::string_view line(data.data() + pos, end - pos);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            m.findAll(line, hits);
            for (const auto& h : hits) {
                Match mt;
                mt.line = lineNo; mt.col = h.col; mt.len = h.len;
                // Trim leading blanks; when the match is far to the right start the preview just before it.
                std::size_t from = 0;
                while (from < line.size() && (line[from] == ' ' || line[from] == '\t')) ++from;
                std::string prefix;
                if (static_cast<std::size_t>(h.col) > from + 36) {
                    from = static_cast<std::size_t>(h.col) - 24;
                    while (from < line.size() && (static_cast<unsigned char>(line[from]) & 0xC0) == 0x80) ++from;   // stay on a character boundary
                    prefix = "\xE2\x80\xA6";
                }
                mt.preview = prefix + std::string(line.substr(from, 240));
                mt.pcol = static_cast<int>(prefix.size()) + h.col - static_cast<int>(from);
                fr.matches.push_back(std::move(mt));
                if (++res.total >= maxMatches) { res.truncated = true; break; }
            }
            if (res.truncated || nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (!fr.matches.empty()) res.files.push_back(std::move(fr));
        if (res.truncated) break;
    }
    return res;
}

// Replaces matches in `text`. `only` limits it to the given (line, column) pairs (0-based, original positions); null = all.
inline std::string replaceInText(std::string_view text, const Options& opt, const std::string& with,
                                 const std::set<std::pair<int, int>>* only, int& count) {
    Matcher m(opt, with);
    std::string out;
    if (!m.ok()) { out.assign(text); return out; }
    std::vector<Hit> hits;
    int lineNo = 0;
    for (std::size_t pos = 0; pos <= text.size(); ++lineNo) {
        auto nl = text.find('\n', pos);
        const std::size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        std::string_view eol;
        if (!line.empty() && line.back() == '\r') { line.remove_suffix(1); eol = "\r"; }
        m.findAll(line, hits, true);
        std::size_t last = 0;
        for (const auto& h : hits) {
            if (only && !only->contains({lineNo, h.col})) continue;
            out.append(line.substr(last, static_cast<std::size_t>(h.col) - last));
            out += h.repl;
            last = static_cast<std::size_t>(h.col + h.len);
            ++count;
        }
        out.append(line.substr(last));
        out.append(eol);
        if (nl == std::string_view::npos) break;
        out += '\n';
        pos = nl + 1;
    }
    return out;
}

// Rewrites a file on disk; returns how many replacements were made (-1 on I/O failure).
inline int replaceInFile(const fs::path& file, const Options& opt, const std::string& with, const std::set<std::pair<int, int>>* only) {
    auto data = readFile(file);
    if (!data || looksBinary(*data)) return -1;
    int count = 0;
    std::string out = replaceInText(*data, opt, with, only, count);
    if (count == 0) return 0;
    return writeFile(file, out) ? count : -1;
}

}  // namespace ide::search
