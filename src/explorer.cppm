// ide.explorer — the file-tree model behind the sidebar (no rendering, no windowing).
module;
#include <sys/inotify.h>
#include <unistd.h>
#include <cerrno>
export module ide.explorer;
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

    bool open(const fs::path& dir, std::string& err) {
        std::error_code ec;
        fs::path abs = fs::absolute(dir, ec).lexically_normal();
        while (abs.filename().empty() && abs.has_parent_path() && abs != abs.root_path()) abs = abs.parent_path();
        if (!fs::is_directory(abs, ec)) { err = "not a directory: " + dir.string(); return false; }
        root_ = std::make_unique<FileNode>();
        root_->path = abs;
        root_->name = abs.filename().string().empty() ? abs.string() : abs.filename().string();
        root_->dir = true;
        root_->expanded = true;
        load(*root_);
        selected_.clear();
        dirty_ = true;
        return true;
    }

    bool isOpen() const { return root_ != nullptr; }
    const fs::path& rootPath() const { return root_->path; }
    const std::string& rootName() const { return root_->name; }

    // Visible rows (the root itself is shown as a header, not as a row).
    const std::vector<Row>& rows() {
        if (dirty_) {
            rows_.clear();
            if (root_) collect(*root_, 0);
            dirty_ = false;
        }
        return rows_;
    }

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
    FileNode* reveal(const fs::path& file) {
        if (!root_) return nullptr;
        std::error_code ec;
        fs::path rel = fs::path(file).lexically_relative(root_->path);
        if (rel.empty() || *rel.begin() == "..") return nullptr;
        FileNode* cur = root_.get();
        for (const auto& part : rel) {
            if (!cur->dir) return nullptr;
            if (!cur->loaded) load(*cur);
            cur->expanded = true;
            FileNode* next = nullptr;
            for (auto& k : cur->kids)
                if (k->name == part.string()) { next = k.get(); break; }
            if (!next) return nullptr;
            cur = next;
        }
        dirty_ = true;
        select(cur->path.string());
        return cur;
    }

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
    void list(FileNode& n, std::unordered_map<std::string, std::unique_ptr<FileNode>>& old) {
        std::error_code ec;
        std::size_t count = 0;
        for (fs::directory_iterator it(n.path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (name == ".git" || ++count > 20000) continue;
            std::error_code e2;
            bool isDir = it->is_directory(e2);
            if (auto o = old.find(name); o != old.end() && o->second->dir == isDir) {
                n.kids.push_back(std::move(o->second));
            } else {
                auto k = std::make_unique<FileNode>();
                k->name = name;
                k->path = it->path();
                k->dir = isDir;
                n.kids.push_back(std::move(k));
            }
        }
        sortKids(n);
        n.loaded = true;
    }

    void load(FileNode& n) {
        std::unordered_map<std::string, std::unique_ptr<FileNode>> none;
        n.kids.clear();
        list(n, none);
    }

    void reload(FileNode& n) {
        if (!n.loaded) return;
        std::unordered_map<std::string, std::unique_ptr<FileNode>> old;
        for (auto& k : n.kids) old[k->name] = std::move(k);
        n.kids.clear();
        list(n, old);
        for (auto& k : n.kids)
            if (k->dir && k->loaded) reload(*k);
    }

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
    void sync(const std::set<std::string>& dirs) {
        if (fd_ < 0) return;
        for (auto it = byPath_.begin(); it != byPath_.end();) {
            if (dirs.contains(it->first)) { ++it; continue; }
            ::inotify_rm_watch(fd_, it->second);
            it = byPath_.erase(it);
        }
        for (const auto& d : dirs) {
            if (byPath_.contains(d) || byPath_.size() >= 4096) continue;
            const int wd = ::inotify_add_watch(fd_, d.c_str(), IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO |
                                                               IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
            if (wd >= 0) byPath_[d] = wd;
        }
    }

    // Reads pending events; true when anything happened. Watches that died with their directory are forgotten.
    bool drain() {
        if (fd_ < 0) return false;
        alignas(inotify_event) char buf[16384];
        bool any = false;
        while (true) {
            const ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n <= 0) break;
            for (ssize_t off = 0; off < n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
                off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
                if (ev->mask & IN_IGNORED) {
                    for (auto it = byPath_.begin(); it != byPath_.end(); ++it)
                        if (it->second == ev->wd) { byPath_.erase(it); break; }
                }
                any = true;
            }
        }
        return any;
    }

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
inline std::string uniqueName(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    if (!fs::exists(dir / name, ec) && !fs::is_symlink(dir / name, ec)) return name;
    fs::path p(name);
    std::string stem = p.stem().string(), ext = p.extension().string();
    for (int i = 1; i < 10000; ++i) {
        std::string cand = stem + (i == 1 ? " copy" : " copy " + std::to_string(i)) + ext;
        if (!fs::exists(dir / cand, ec) && !fs::is_symlink(dir / cand, ec)) return cand;
    }
    return name;
}

// Creates a file (or nested path "a/b/c.txt") inside `dir`.
inline Result createFile(const fs::path& dir, std::string_view rel) {
    if (rel.empty() || rel.front() == '/') return std::unexpected("invalid file name");
    for (const auto& part : fs::path(rel))
        if (part == ".." || part == ".") return std::unexpected("invalid file name");
    fs::path full = dir / std::string(rel);
    std::error_code ec;
    if (fs::exists(full, ec)) return std::unexpected("'" + full.filename().string() + "' already exists");
    fs::create_directories(full.parent_path(), ec);
    if (ec) return std::unexpected("cannot create folder: " + errText(ec));
    std::ofstream f(full, std::ios::binary);
    if (!f) return std::unexpected("cannot create " + full.string());
    return full;
}

inline Result createFolder(const fs::path& dir, std::string_view rel) {
    if (rel.empty() || rel.front() == '/') return std::unexpected("invalid folder name");
    for (const auto& part : fs::path(rel))
        if (part == ".." || part == ".") return std::unexpected("invalid folder name");
    fs::path full = dir / std::string(rel);
    std::error_code ec;
    if (fs::exists(full, ec)) return std::unexpected("'" + full.filename().string() + "' already exists");
    fs::create_directories(full, ec);
    if (ec) return std::unexpected("cannot create folder: " + errText(ec));
    return full;
}

inline Result rename(const fs::path& from, std::string_view newName) {
    if (!validName(newName)) return std::unexpected("invalid name");
    fs::path to = from.parent_path() / std::string(newName);
    if (to == from) return from;
    std::error_code ec;
    if (fs::exists(to, ec)) return std::unexpected("'" + std::string(newName) + "' already exists");
    fs::rename(from, to, ec);
    if (ec) return std::unexpected("rename failed: " + errText(ec));
    return to;
}

inline Result copyInto(const fs::path& src, const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(src, ec) && isInside(src, dir)) return std::unexpected("cannot copy a folder into itself");
    fs::path dest = dir / uniqueName(dir, src.filename().string());
    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) return std::unexpected("copy failed: " + errText(ec));
    return dest;
}

inline Result moveInto(const fs::path& src, const fs::path& dir) {
    std::error_code ec;
    fs::path dest = dir / src.filename();
    if (dest == src) return src;
    if (fs::is_directory(src, ec) && isInside(src, dir)) return std::unexpected("cannot move a folder into itself");
    if (fs::exists(dest, ec)) return std::unexpected("'" + src.filename().string() + "' already exists in the target folder");
    fs::rename(src, dest, ec);
    if (ec == std::errc::cross_device_link) {  // different file system: copy, then remove
        ec.clear();
        fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (!ec) fs::remove_all(src, ec);
    }
    if (ec) return std::unexpected("move failed: " + errText(ec));
    return dest;
}

inline Status removeForever(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (ec) return std::unexpected("delete failed: " + errText(ec));
    return {};
}

inline std::string percentEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

// Moves a file or folder to the freedesktop.org Trash (~/.local/share/Trash).
inline Status trash(const fs::path& path) {
    std::error_code ec;
    fs::path data;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) data = x;
    else if (const char* h = std::getenv("HOME"); h && *h) data = fs::path(h) / ".local/share";
    else return std::unexpected("no home directory for the Trash");
    fs::path files = data / "Trash/files", info = data / "Trash/info";
    fs::create_directories(files, ec);
    fs::create_directories(info, ec);
    if (ec) return std::unexpected("cannot create Trash: " + errText(ec));

    fs::path abs = fs::absolute(path, ec).lexically_normal();
    std::string name = uniqueName(files, abs.filename().string());
    std::string date;
    try {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        date = std::format("{:%Y-%m-%dT%H:%M:%S}", std::chrono::zoned_time{std::chrono::current_zone(), now});
    } catch (...) {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        date = std::format("{:%Y-%m-%dT%H:%M:%S}", now);
    }
    {
        std::ofstream f(info / (name + ".trashinfo"));
        if (!f) return std::unexpected("cannot write Trash info");
        f << "[Trash Info]\nPath=" << percentEncode(abs.string()) << "\nDeletionDate=" << date << "\n";
    }
    fs::rename(abs, files / name, ec);
    if (ec) {
        fs::remove(info / (name + ".trashinfo"), ec);
        return std::unexpected("cannot move to Trash (different file system?) - use Shift+Delete to delete permanently");
    }
    return {};
}

}  // namespace fsops

}  // namespace ide
