// SPDX-License-Identifier: GPL-3.0-or-later
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

Result run(const fs::path& cwd, std::vector<std::string> args, int timeoutMs = 20000, bool mergeStderr = false);

inline std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

inline bool available() { return findExecutable("git").has_value(); }

// Top-level directory of the repository containing `dir`, if any.
std::optional<fs::path> repoRoot(const fs::path& dir);

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

std::optional<Status> status(const fs::path& root);

// ---------------------------------------------------------------------------------------------- diff

struct Hunk {
    int newStart = 0, newCount = 0;   // 0-based line range in the new text (newCount == 0: pure deletion before newStart)
    int oldStart = 0, oldCount = 0;
    bool added() const { return oldCount == 0 && newCount > 0; }
    bool removed() const { return newCount == 0 && oldCount > 0; }
};

// Myers O(ND) line diff. Returns the changed ranges between `a` (old) and `b` (new).
std::vector<Hunk> diffLines(const std::vector<std::string_view>& a, const std::vector<std::string_view>& b);

std::vector<std::string_view> splitLines(std::string_view text);

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
std::optional<Blame> blame(const fs::path& root, const std::string& rel, const fs::path& contentsFile);

// Full commit message and header for a hover card: lines of plain text.
std::vector<std::string> commitCard(const fs::path& root, const std::string& hash);

std::string relativeTime(std::int64_t then, std::int64_t now);


// ---------------------------------------------------------------------------------------------- history & branches

struct LogEntry {
    std::string hash, shortHash, author, refs, subject;
    std::vector<std::string> parents;
    std::int64_t time = 0;
};

// Commits of all branches, newest first in topological order (so a graph can be laid out from it).
std::vector<LogEntry> log(const fs::path& root, int maxCount = 400);

struct FileChange { char status = 'M'; std::string path; };

std::vector<FileChange> commitFiles(const fs::path& root, const std::string& hash);

struct Branch {
    std::string name;       // short name, e.g. "main" or "origin/main"
    bool remote = false, current = false;
    std::string hash, subject, upstream;
};

std::vector<Branch> branches(const fs::path& root);

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
Result unstage(const fs::path& root, const std::vector<std::string>& paths);
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
