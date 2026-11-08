// daemon.explorer — the file-tree model behind the sidebar (no rendering, no windowing).
module;
#include <sys/inotify.h>
#include <unistd.h>
#include <cerrno>
export module daemon.services.explorer;
import std;

namespace fs = std::filesystem;

export namespace ide {

struct FileNode {
    std::string name;
    fs::path path;
    bool dir = false;
    bool expanded = false;
    bool loaded = false;  // children listed from disk
    std::vector<std::unique_ptr<FileNode>> kids;
};

class Explorer {
public:
    struct Row {
        FileNode* node;
        int depth;
    };

    bool open(const fs::path& dir, std::string& err);

    bool isOpen() const { return root_ != nullptr; }
    const fs::path& rootPath() const { return root_->path; }
    const std::string& rootName() const { return root_->name; }

    // Visible rows (the root itself is shown as a header, not as a row).
    const std::vector<Row>& rows();

    void toggle(FileNode& n) {
        if (!n.dir) return;
        n.expanded = !n.expanded;
        if (n.expanded && !n.loaded) load(n);
        dirty_ = true;
    }
    void setExpanded(FileNode& n, bool v) { if (n.dir && n.expanded != v) toggle(n); }

    // Re-reads every loaded directory, keeping expansion state.
    void refresh() {
        if (!root_) return;
        reload(*root_);
        dirty_ = true;
    }

    // Expands the ancestors of `file` and returns its node (nullptr if it is outside the root / missing).
    FileNode* reveal(const fs::path& file);

    const std::string& selected() const { return selected_; }
    void select(std::string path) { selected_ = std::move(path); }

    int indexOfSelected() {
        const auto& r = rows();
        for (std::size_t i = 0; i < r.size(); ++i)
            if (r[i].node->path.string() == selected_) return static_cast<int>(i);
        return -1;
    }

    // Finds the parent directory node of `n` among the visible rows (for Left-arrow navigation).
    FileNode* parentOf(const FileNode& n) {
        if (n.path.parent_path() == root_->path) return nullptr;
        for (const auto& r : rows())
            if (r.node->path == n.path.parent_path()) return r.node;
        return nullptr;
    }

    void markDirty() { dirty_ = true; }

    // Every directory whose listing is currently cached (these are the ones that can go stale).
    std::vector<fs::path> loadedDirs() const {
        std::vector<fs::path> out;
        if (root_) gather(*root_, out);
        return out;
    }

private:
    static void gather(const FileNode& n, std::vector<fs::path>& out) {
        if (!n.dir || !n.loaded) return;
        out.push_back(n.path);
        for (const auto& k : n.kids) gather(*k, out);
    }

    static std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    static void sortKids(FileNode& n) {
        std::ranges::sort(n.kids, [](const auto& a, const auto& b) {
            if (a->dir != b->dir) return a->dir;  // folders first
            return lower(a->name) < lower(b->name);
        });
    }

    // Lists `n` from disk, reusing nodes (and their state) from `old` when names match.
    void list(FileNode& n, std::unordered_map<std::string, std::unique_ptr<FileNode>>& old);

    void load(FileNode& n) {
        std::unordered_map<std::string, std::unique_ptr<FileNode>> none;
        n.kids.clear();
        list(n, none);
    }

    void reload(FileNode& n);

    void collect(FileNode& n, int depth) {
        for (auto& k : n.kids) {
            rows_.push_back({k.get(), depth});
            if (k->dir && k->expanded) collect(*k, depth + 1);
        }
    }

    std::unique_ptr<FileNode> root_;
    std::vector<Row> rows_;
    std::string selected_;
    bool dirty_ = true;
};


// Watches a set of directories with inotify so the tree and open documents follow changes made by other programs.
class DirWatcher {
public:
    DirWatcher() : fd_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {}
    ~DirWatcher() { if (fd_ >= 0) ::close(fd_); }
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    int fd() const { return fd_; }

    // Makes the watched set equal to `dirs` (adds new ones, drops the others).
    void sync(const std::set<std::string>& dirs);

    // Reads pending events; true when anything happened. Watches that died with their directory are forgotten.
    bool drain();

private:
    int fd_ = -1;
    std::map<std::string, int> byPath_;
};

// ---------------------------------------------------------------------------------------------
// File operations used by the explorer. Plain functions so they can be unit-tested headless.

namespace fsops {

using Result = std::expected<fs::path, std::string>;
using Status = std::expected<void, std::string>;

inline std::string errText(const std::error_code& ec) { return ec.message(); }

inline bool validName(std::string_view n) {
    return !n.empty() && n != "." && n != ".." && n.find('/') == std::string_view::npos &&
           n.find('\0') == std::string_view::npos;
}

// True when `child` is `parent` or lies below it.
inline bool isInside(const fs::path& parent, const fs::path& child) {
    auto rel = child.lexically_normal().lexically_relative(parent.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

// "name", then "name copy", "name copy 2", ... (extension preserved) until the name is free.
std::string uniqueName(const fs::path& dir, const std::string& name);

// Creates a file (or nested path "a/b/c.txt") inside `dir`.
Result createFile(const fs::path& dir, std::string_view rel);

Result createFolder(const fs::path& dir, std::string_view rel);

Result rename(const fs::path& from, std::string_view newName);

Result copyInto(const fs::path& src, const fs::path& dir);

Result moveInto(const fs::path& src, const fs::path& dir);

inline Status removeForever(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) return std::unexpected("delete failed: " + errText(ec));
    return {};
}

std::string percentEncode(const std::string& s);

// Moves a file or folder to the freedesktop.org Trash (~/.local/share/Trash).
Status trash(const fs::path& path);

}  // namespace fsops

}  // namespace ide
