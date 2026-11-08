// explorer - implementation of module daemon.services.explorer.
module;
#include <sys/inotify.h>
#include <unistd.h>
#include <cerrno>
module daemon.services.explorer;
import std;
namespace fs = std::filesystem;

namespace ide {

bool Explorer::open(const fs::path& dir, std::string& err) {
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

auto Explorer::rows() -> const std::vector<Row>& {
        if (dirty_) {
            rows_.clear();
            if (root_) collect(*root_, 0);
            dirty_ = false;
        }
        return rows_;
    }

FileNode* Explorer::reveal(const fs::path& file) {
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

void Explorer::list(FileNode& n, std::unordered_map<std::string, std::unique_ptr<FileNode>>& old) {
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

void Explorer::reload(FileNode& n) {
        if (!n.loaded) return;
        std::unordered_map<std::string, std::unique_ptr<FileNode>> old;
        for (auto& k : n.kids) old[k->name] = std::move(k);
        n.kids.clear();
        list(n, old);
        for (auto& k : n.kids)
            if (k->dir && k->loaded) reload(*k);
    }

void DirWatcher::sync(const std::set<std::string>& dirs) {
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

bool DirWatcher::drain() {
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

}  // namespace ide

namespace ide::fsops {

std::string uniqueName(const fs::path& dir, const std::string& name) {
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

Result createFile(const fs::path& dir, std::string_view rel) {
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

Result createFolder(const fs::path& dir, std::string_view rel) {
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

Result rename(const fs::path& from, std::string_view newName) {
    if (!validName(newName)) return std::unexpected("invalid name");
    fs::path to = from.parent_path() / std::string(newName);
    if (to == from) return from;
    std::error_code ec;
    if (fs::exists(to, ec)) return std::unexpected("'" + std::string(newName) + "' already exists");
    fs::rename(from, to, ec);
    if (ec) return std::unexpected("rename failed: " + errText(ec));
    return to;
}

Result copyInto(const fs::path& src, const fs::path& dir) {
    std::error_code ec;
    if (fs::is_directory(src, ec) && isInside(src, dir)) return std::unexpected("cannot copy a folder into itself");
    fs::path dest = dir / uniqueName(dir, src.filename().string());
    fs::copy(src, dest, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) return std::unexpected("copy failed: " + errText(ec));
    return dest;
}

Result moveInto(const fs::path& src, const fs::path& dir) {
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

std::string percentEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

Status trash(const fs::path& path) {
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

}  // namespace ide::fsops
