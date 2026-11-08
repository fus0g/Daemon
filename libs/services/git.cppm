// daemon.git — thin git integration: repository status, line-level diff against HEAD, blame, staging and commits.
// Everything shells out to the `git` binary; calls are blocking and meant to run on a worker thread.
export module daemon.services.git;
import std;
import daemon.base.process;
import daemon.base.util;

namespace fs = std::filesystem;

export namespace ide::git {

struct Result {
    int code = -1;
    std::string out;
    bool ok() const { return code == 0; }
};

inline Result run(const fs::path& cwd, std::vector<std::string> args, int timeoutMs = 20000, bool mergeStderr = false) {
    ProcessOptions o;
    o.argv = {"git", "--no-optional-locks"};
    for (auto& a : args) o.argv.push_back(std::move(a));
    o.cwd = cwd.string();
    o.mergeStderr = mergeStderr;
    auto r = Process::runCapture(o, timeoutMs);
    if (!r.error.empty()) return {-1, r.error};
    return {r.exitCode, std::move(r.output)};
}

inline std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

inline bool available() { return findExecutable("git").has_value(); }

// Top-level directory of the repository containing `dir`, if any.
inline std::optional<fs::path> repoRoot(const fs::path& dir) {
    if (!available()) return std::nullopt;
    auto r = run(dir, {"rev-parse", "--show-toplevel"});
    if (!r.ok()) return std::nullopt;
    const std::string p = trim(r.out);
    if (p.empty()) return std::nullopt;
    return fs::path(p);
}

// ---------------------------------------------------------------------------------------------- status

struct Entry {
    char x = ' ', y = ' ';   // index / work-tree status (porcelain v1 letters; '?' for untracked)
    std::string path;        // relative to the repository root
    std::string orig;        // renames: the old path
    bool staged() const { return x != ' ' && x != '?' && x != '!'; }
    bool unstaged() const { return y != ' ' || x == '?'; }
    bool conflicted() const { return x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'); }
};

struct Status {
    std::string branch;   // "main", or "HEAD" when detached
    std::string head;     // current commit (full hash), empty on an unborn branch
    std::string upstream;
    int ahead = 0, behind = 0;
    std::vector<Entry> entries;
};

inline std::optional<Status> status(const fs::path& root) {
    auto r = run(root, {"status", "--porcelain=v1", "-b", "-z", "--untracked-files=normal"});
    if (!r.ok()) return std::nullopt;
    Status st;
    std::vector<std::string> recs;
    for (std::size_t i = 0; i < r.out.size();) {
        auto e = r.out.find('\0', i);
        if (e == std::string::npos) e = r.out.size();
        recs.push_back(r.out.substr(i, e - i));
        i = e + 1;
    }
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const std::string& rec = recs[i];
        if (rec.starts_with("## ")) {
            std::string b = rec.substr(3);
            if (b.starts_with("No commits yet on ")) { st.branch = b.substr(18); continue; }
            if (b.starts_with("HEAD (no branch)")) { st.branch = "HEAD"; continue; }
            if (auto br = b.find(" ["); br != std::string::npos) {
                const std::string tracking = b.substr(br + 2);
                if (auto a = tracking.find("ahead "); a != std::string::npos) st.ahead = std::atoi(tracking.c_str() + a + 6);
                if (auto be = tracking.find("behind "); be != std::string::npos) st.behind = std::atoi(tracking.c_str() + be + 7);
                b.resize(br);
            }
            if (auto dots = b.find("..."); dots != std::string::npos) { st.upstream = b.substr(dots + 3); b.resize(dots); }
            st.branch = b;
            continue;
        }
        if (rec.size() < 4) continue;
        Entry en;
        en.x = rec[0];
        en.y = rec[1];
        en.path = rec.substr(3);
        if ((en.x == 'R' || en.x == 'C' || en.y == 'R' || en.y == 'C') && i + 1 < recs.size()) en.orig = recs[++i];
        st.entries.push_back(std::move(en));
    }
    auto h = run(root, {"rev-parse", "HEAD"});
    if (h.ok()) st.head = trim(h.out);
    return st;
}

// ---------------------------------------------------------------------------------------------- diff

struct Hunk {
    int newStart = 0, newCount = 0;   // 0-based line range in the new text (newCount == 0: pure deletion before newStart)
    int oldStart = 0, oldCount = 0;
    bool added() const { return oldCount == 0 && newCount > 0; }
    bool removed() const { return newCount == 0 && oldCount > 0; }
};

// Myers O(ND) line diff. Returns the changed ranges between `a` (old) and `b` (new).
inline std::vector<Hunk> diffLines(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b) {
    int pre = 0;
    const int n0 = static_cast<int>(a.size()), m0 = static_cast<int>(b.size());
    while (pre < n0 && pre < m0 && a[static_cast<std::size_t>(pre)] == b[static_cast<std::size_t>(pre)]) ++pre;
    int suf = 0;
    while (suf < n0 - pre && suf < m0 - pre && a[static_cast<std::size_t>(n0 - 1 - suf)] == b[static_cast<std::size_t>(m0 - 1 - suf)]) ++suf;
    const int n = n0 - pre - suf, m = m0 - pre - suf;
    std::vector<Hunk> out;
    if (n == 0 && m == 0) return out;
    if (n == 0 || m == 0) { out.push_back({pre, m, pre, n}); return out; }

    const int maxD = n + m;
    const int cap = std::min(maxD, 3000);
    std::vector<int> v(static_cast<std::size_t>(2 * maxD + 3), 0);
    const int off = maxD + 1;
    std::vector<std::vector<int>> trace;
    int foundD = -1;
    for (int d = 0; d <= cap && foundD < 0; ++d) {
        trace.emplace_back(v.begin() + off - d - 1, v.begin() + off + d + 2);
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[static_cast<std::size_t>(off + k - 1)] < v[static_cast<std::size_t>(off + k + 1)])) ? v[static_cast<std::size_t>(off + k + 1)]
                                                                                                                                  : v[static_cast<std::size_t>(off + k - 1)] + 1;
            int y = x - k;
            while (x < n && y < m && a[static_cast<std::size_t>(pre + x)] == b[static_cast<std::size_t>(pre + y)]) { ++x; ++y; }
            v[static_cast<std::size_t>(off + k)] = x;
            if (x >= n && y >= m) { foundD = d; break; }
        }
    }
    if (foundD < 0) { out.push_back({pre, m, pre, n}); return out; }   // too different: one big hunk

    // Backtrack into per-line operations: 0 equal, 1 delete (old), 2 insert (new), collected in reverse.
    std::vector<char> ops;
    int x = n, y = m;
    for (int d = foundD; d > 0; --d) {
        const auto& tv = trace[static_cast<std::size_t>(d)];   // v as it was after round d - 1, for k in [-d-1, d+1]
        auto at = [&](int k) { return tv[static_cast<std::size_t>(k + d + 1)]; };
        const int k = x - y;
        const bool down = (k == -d || (k != d && at(k - 1) < at(k + 1)));
        const int pk = down ? k + 1 : k - 1;
        const int px = at(pk), py = px - pk;
        const int ex = down ? px : px + 1, ey = down ? py + 1 : py;   // position right after the edit
        while (x > ex && y > ey) { ops.push_back(0); --x; --y; }
        ops.push_back(down ? 2 : 1);
        x = px;
        y = py;
    }
    while (x > 0 && y > 0) { ops.push_back(0); --x; --y; }
    std::ranges::reverse(ops);

    int i = pre, j = pre;
    for (std::size_t p = 0; p < ops.size();) {
        if (ops[p] == 0) { ++i; ++j; ++p; continue; }
        Hunk h{j, 0, i, 0};
        while (p < ops.size() && ops[p] != 0) { if (ops[p] == 1) { ++h.oldCount; ++i; } else { ++h.newCount; ++j; } ++p; }
        out.push_back(h);
    }
    return out;
}

inline std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        auto e = text.find('\n', i);
        if (e == std::string_view::npos) { out.push_back(text.substr(i)); break; }
        std::size_t end = e;
        if (end > i && text[end - 1] == '\r') --end;
        out.push_back(text.substr(i, end - i));
        i = e + 1;
    }
    return out;
}

// The file's content at HEAD (nullopt when it is not tracked / there is no commit yet).
inline std::optional<std::string> headContent(const fs::path& root, const std::string& rel) {
    auto r = run(root, {"show", "HEAD:" + rel});
    if (!r.ok()) return std::nullopt;
    return std::move(r.out);
}

// ---------------------------------------------------------------------------------------------- blame

struct Commit {
    std::string hash, author, email, summary;
    std::int64_t time = 0;
    bool uncommitted = false;
};

struct Blame {
    std::vector<Commit> commits;
    std::vector<int> lineCommit;   // per line (0-based): index into commits
};

// `contentsFile` holds the text to blame (the editor buffer, so unsaved edits show as "not committed yet").
inline std::optional<Blame> blame(const fs::path& root, const std::string& rel, const fs::path& contentsFile) {
    auto r = run(root, {"blame", "--porcelain", "--contents", contentsFile.string(), "--", rel}, 30000);
    if (!r.ok()) return std::nullopt;
    Blame out;
    std::unordered_map<std::string, int> index;
    const std::string& s = r.out;
    int current = -1;
    for (std::size_t i = 0; i < s.size();) {
        auto e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        const std::string_view line(s.data() + i, e - i);
        i = e + 1;
        if (line.empty()) continue;
        if (line[0] == '\t') { out.lineCommit.push_back(current); continue; }   // the source line: one per blamed line
        const bool header = line.size() >= 42 && std::ranges::all_of(line.substr(0, 40), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }) && line[40] == ' ';
        if (header) {
            const std::string hash(line.substr(0, 40));
            auto it = index.find(hash);
            if (it == index.end()) {
                Commit c;
                c.hash = hash;
                c.uncommitted = std::ranges::all_of(hash, [](char ch) { return ch == '0'; });
                index[hash] = static_cast<int>(out.commits.size());
                out.commits.push_back(std::move(c));
                current = static_cast<int>(out.commits.size()) - 1;
            } else {
                current = it->second;
            }
            continue;
        }
        if (current < 0) continue;
        Commit& c = out.commits[static_cast<std::size_t>(current)];
        auto val = [&](std::string_view key) { return std::string(line.substr(key.size() + 1)); };
        if (line.starts_with("author ")) c.author = val("author");
        else if (line.starts_with("author-mail ")) { c.email = val("author-mail"); if (c.email.size() >= 2 && c.email.front() == '<') c.email = c.email.substr(1, c.email.size() - 2); }
        else if (line.starts_with("author-time ")) c.time = std::atoll(val("author-time").c_str());
        else if (line.starts_with("summary ")) c.summary = val("summary");
    }
    return out;
}

// Full commit message and header for a hover card: lines of plain text.
inline std::vector<std::string> commitCard(const fs::path& root, const std::string& hash) {
    std::vector<std::string> out;
    auto r = run(root, {"show", "-s", "--format=%an <%ae>%x1f%ad%x1f%B", "--date=format:%Y-%m-%d %H:%M", hash});
    if (!r.ok()) return out;
    auto p1 = r.out.find('\x1f');
    auto p2 = p1 == std::string::npos ? p1 : r.out.find('\x1f', p1 + 1);
    if (p2 == std::string::npos) return out;
    out.push_back(r.out.substr(0, p1));
    out.push_back(r.out.substr(p1 + 1, p2 - p1 - 1));
    out.push_back("");
    std::string msg = trim(r.out.substr(p2 + 1));
    for (std::size_t i = 0; i <= msg.size();) {
        auto e = msg.find('\n', i);
        if (e == std::string::npos) e = msg.size();
        std::string l = msg.substr(i, e - i);
        while (l.size() > 90) {   // hard wrap long lines
            auto cut = l.rfind(' ', 90);
            if (cut == std::string::npos || cut < 30) cut = 90;
            out.push_back(l.substr(0, cut));
            l.erase(0, cut + (l[cut] == ' ' ? 1 : 0));
        }
        out.push_back(l);
        i = e + 1;
    }
    out.push_back("");
    out.push_back("commit " + hash);
    return out;
}

inline std::string relativeTime(std::int64_t then, std::int64_t now) {
    const std::int64_t d = std::max<std::int64_t>(0, now - then);
    auto plural = [](std::int64_t n, const char* unit) { return std::format("{} {}{} ago", n, unit, n == 1 ? "" : "s"); };
    if (d < 60) return "just now";
    if (d < 3600) return plural(d / 60, "minute");
    if (d < 86400) return plural(d / 3600, "hour");
    if (d < 86400 * 14) return plural(d / 86400, "day");
    if (d < 86400 * 60) return plural(d / (86400 * 7), "week");
    if (d < 86400 * 365) return plural(d / (86400 * 30), "month");
    return plural(d / (86400 * 365), "year");
}


// ---------------------------------------------------------------------------------------------- history & branches

struct LogEntry {
    std::string hash, shortHash, author, refs, subject;
    std::vector<std::string> parents;
    std::int64_t time = 0;
};

// Commits of all branches, newest first in topological order (so a graph can be laid out from it).
inline std::vector<LogEntry> log(const fs::path& root, int maxCount = 400) {
    std::vector<LogEntry> out;
    auto r = run(root, {"log", "--all", "--topo-order", "--decorate=short", std::format("--max-count={}", maxCount),
                        "--format=%H%x1f%P%x1f%h%x1f%an%x1f%at%x1f%D%x1f%s"}, 30000);
    if (!r.ok()) return out;
    for (std::size_t i = 0; i < r.out.size();) {
        auto e = r.out.find('\n', i);
        if (e == std::string::npos) e = r.out.size();
        const std::string line = r.out.substr(i, e - i);
        i = e + 1;
        std::vector<std::string> f;
        for (std::size_t p = 0;;) {
            auto q = line.find('\x1f', p);
            f.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (f.size() < 7) continue;
        LogEntry le;
        le.hash = f[0];
        for (std::size_t p = 0; p < f[1].size();) {
            auto q = f[1].find(' ', p);
            if (q == std::string::npos) q = f[1].size();
            if (q > p) le.parents.push_back(f[1].substr(p, q - p));
            p = q + 1;
        }
        le.shortHash = f[2];
        le.author = f[3];
        le.time = std::atoll(f[4].c_str());
        le.refs = f[5];
        le.subject = f[6];
        out.push_back(std::move(le));
    }
    return out;
}

struct FileChange { char status = 'M'; std::string path; };

inline std::vector<FileChange> commitFiles(const fs::path& root, const std::string& hash) {
    std::vector<FileChange> out;
    auto r = run(root, {"show", "--name-status", "--format=", "-M", hash});
    if (!r.ok()) return out;
    for (std::size_t i = 0; i < r.out.size();) {
        auto e = r.out.find('\n', i);
        if (e == std::string::npos) e = r.out.size();
        const std::string line = r.out.substr(i, e - i);
        i = e + 1;
        if (line.size() < 3) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string path = line.substr(tab + 1);
        if (auto t2 = path.find('\t'); t2 != std::string::npos) path = path.substr(t2 + 1);   // renames: show the new name
        out.push_back({line[0], std::move(path)});
    }
    return out;
}

struct Branch {
    std::string name;       // short name, e.g. "main" or "origin/main"
    bool remote = false, current = false;
    std::string hash, subject, upstream;
};

inline std::vector<Branch> branches(const fs::path& root) {
    std::vector<Branch> out;
    auto r = run(root, {"for-each-ref", "--sort=-committerdate", "--format=%(HEAD)%1f%(refname)%1f%(refname:short)%1f%(objectname:short)%1f%(upstream:short)%1f%(subject)",
                        "refs/heads", "refs/remotes"});
    if (!r.ok()) return out;
    for (std::size_t i = 0; i < r.out.size();) {
        auto e = r.out.find('\n', i);
        if (e == std::string::npos) e = r.out.size();
        const std::string line = r.out.substr(i, e - i);
        i = e + 1;
        std::vector<std::string> f;
        for (std::size_t p = 0;;) {
            auto q = line.find('\x1f', p);
            f.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (f.size() < 6) continue;
        if (f[1].ends_with("/HEAD")) continue;
        Branch b;
        b.current = f[0] == "*";
        b.remote = f[1].starts_with("refs/remotes/");
        b.name = f[2];
        b.hash = f[3];
        b.upstream = f[4];
        b.subject = f[5];
        out.push_back(std::move(b));
    }
    return out;
}

inline Result checkout(const fs::path& root, const std::string& name) { return run(root, {"switch", name}, 60000, true); }
inline Result fetch(const fs::path& root) { return run(root, {"fetch", "--all", "--prune"}, 120000, true); }
inline Result pull(const fs::path& root) { return run(root, {"pull", "--ff-only"}, 120000, true); }
inline Result push(const fs::path& root) { return run(root, {"push"}, 120000, true); }

// ---------------------------------------------------------------------------------------------- actions

inline Result stage(const fs::path& root, const std::vector<std::string>& paths) {
    std::vector<std::string> a = {"add", "--"};
    a.insert(a.end(), paths.begin(), paths.end());
    return run(root, std::move(a), 30000, true);
}
inline Result stageAll(const fs::path& root) { return run(root, {"add", "-A"}, 30000, true); }
inline Result unstage(const fs::path& root, const std::vector<std::string>& paths) {
    std::vector<std::string> a = {"restore", "--staged", "--"};
    a.insert(a.end(), paths.begin(), paths.end());
    auto r = run(root, std::move(a), 30000, true);
    if (!r.ok()) {   // an unborn branch has no HEAD to restore from
        std::vector<std::string> b = {"rm", "--cached", "-q", "--"};
        b.insert(b.end(), paths.begin(), paths.end());
        r = run(root, std::move(b), 30000, true);
    }
    return r;
}
inline Result unstageAll(const fs::path& root) {
    auto r = run(root, {"reset", "-q"}, 30000, true);
    return r;
}
// Throws away work-tree changes of tracked files; untracked files are deleted.
inline Result discard(const fs::path& root, const Entry& e) {
    if (e.x == '?') return run(root, {"clean", "-f", "-q", "--", e.path}, 30000, true);
    return run(root, {"restore", "--", e.path}, 30000, true);
}
inline Result commit(const fs::path& root, const std::string& message) {
    return run(root, {"commit", "-m", message}, 60000, true);
}

}  // namespace ide::git
