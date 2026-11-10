// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <stdlib.h>
#include <sys/utsname.h>
#include <unistd.h>
module daemon.services.toolchain;
import std;
import daemon.base.util;
import daemon.base.process;
import daemon.base.json;

namespace fs = std::filesystem;

namespace ide::toolchain {

const std::vector<Tool>& tools() {
    static const std::vector<Tool> t = {
        {"clangd", "clangd", "code completion, diagnostics, go to definition (18+ for C++20 modules)", 16, 0, Need::Recommended},
        {"cmake", "CMake", "configure and build CMake projects", 3, 20, Need::Recommended},
        {"ninja", "Ninja", "fast builds (CMake falls back to Make without it)", 0, 0, Need::Recommended},
        {"gdb", "GDB", "the debugger", 10, 0, Need::Recommended},
        {"git", "Git", "source control, blame, project search", 2, 20, Need::Recommended},
        {"clang++", "Clang", "IntelliSense for C++20 modules projects built with GCC", 16, 0, Need::Optional},
        {"zenity", "Zenity", "the native folder chooser (a built-in one is used without it)", 0, 0, Need::Optional},
    };
    return t;
}

std::vector<std::string> toolDirs() {
    std::vector<std::string> out;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) out.push_back(p.string());
    };
    if (const char* d = std::getenv("DAEMON_TOOLS_DIR"); d && *d) add(fs::path(d) / "bin");
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.has_parent_path()) {
        add(exe.parent_path().parent_path() / "libexec/daemon/bin");
        add(exe.parent_path() / "tools/bin");
    }
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) add(fs::path(x) / "daemon/tools/bin");
    else if (const char* h = std::getenv("HOME"); h && *h) add(fs::path(h) / ".local/share/daemon/tools/bin");
    return out;
}

void init() {
    const auto dirs = toolDirs();
    if (dirs.empty()) return;
    std::string path;
    const char* old = std::getenv("PATH");
    const std::string cur = old ? old : "";
    for (const auto& d : dirs)
        if ((":" + cur + ":").find(":" + d + ":") == std::string::npos) path += d + ":";   // idempotent
    if (path.empty()) return;
    path += old && *old ? old : "/usr/local/bin:/usr/bin:/bin";
    ::setenv("PATH", path.c_str(), 1);
}

std::string parseVersion(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i])) || (i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '.'))) continue;
        std::size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
        if (j + 1 < s.size() && s[j] == '.' && std::isdigit(static_cast<unsigned char>(s[j + 1]))) {
            while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
            std::string v(s.substr(i, j - i));
            while (!v.empty() && v.back() == '.') v.pop_back();
            return v;
        }
        i = j;
    }
    return {};
}

bool versionAtLeast(std::string_view version, int major, int minor) {
    if (major == 0 && minor == 0) return true;
    int parts[2] = {0, 0};
    int idx = 0;
    for (char c : version) {
        if (c == '.') { if (++idx > 1) break; }
        else if (std::isdigit(static_cast<unsigned char>(c))) parts[idx] = parts[idx] * 10 + (c - '0');
        else break;
    }
    return parts[0] > major || (parts[0] == major && parts[1] >= minor);
}

std::vector<Status> detect() {
    const auto dirs = toolDirs();
    std::vector<Status> out;
    for (const Tool& t : tools()) {
        Status s;
        s.tool = t;
        if (auto p = findExecutable(t.id)) {
            s.found = true;
            s.path = *p;
            s.bundled = std::ranges::any_of(dirs, [&](const std::string& d) { return s.path.starts_with(d + "/"); });
            ProcessOptions o;
            o.argv = {s.path, "--version"};
            auto r = Process::runCapture(o, 4000);
            s.version = parseVersion(r.output.substr(0, r.output.find('\n') == std::string::npos ? r.output.size() : r.output.find('\n')));
            if (s.version.empty()) s.version = parseVersion(r.output);
            s.tooOld = !s.version.empty() && !versionAtLeast(s.version, t.minMajor, t.minMinor);
        }
        out.push_back(std::move(s));
    }
    return out;
}

Distro parseOsRelease(std::string_view text) {
    Distro d;
    std::string like;
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(line.substr(0, eq)), val(line.substr(eq + 1));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'')) val = val.substr(1, val.size() - 2);
        if (key == "ID") d.id = val;
        else if (key == "ID_LIKE") like = val;
        else if (key == "PRETTY_NAME") d.name = val;
    }
    auto has = [&](std::string_view w) {
        std::string all = d.id + " " + like;
        for (std::size_t i = all.find(w); i != std::string::npos; i = all.find(w, i + 1)) {
            const bool l = i == 0 || all[i - 1] == ' ', r = i + w.size() >= all.size() || all[i + w.size()] == ' ';
            if (l && r) return true;
        }
        return false;
    };
    if (has("fedora") || has("rhel") || has("centos") || has("nobara") || has("rocky") || has("almalinux")) d.manager = "dnf";
    else if (has("debian") || has("ubuntu") || has("linuxmint") || has("pop")) d.manager = "apt";
    else if (has("arch") || has("manjaro") || has("endeavouros") || has("cachyos")) d.manager = "pacman";
    else if (has("suse") || has("opensuse") || has("sles")) d.manager = "zypper";
    else if (has("alpine")) d.manager = "apk";
    if (d.name.empty()) d.name = d.id.empty() ? "Linux" : d.id;
    return d;
}

Distro distro() {
    auto text = readFile("/etc/os-release");
    if (!text) text = readFile("/usr/lib/os-release");
    return parseOsRelease(text.value_or(""));
}

std::string packageName(std::string_view id, std::string_view m) {
    if (id == "clangd") return m == "dnf" ? "clang-tools-extra" : m == "apt" ? "clangd" : m == "pacman" ? "clang" : m == "zypper" ? "clang-tools" : m == "apk" ? "clang-extra-tools" : "";
    if (id == "clang++") return m.empty() ? "" : "clang";
    if (id == "ninja") return m == "dnf" || m == "apt" ? "ninja-build" : m.empty() ? "" : "ninja";
    if (id == "cmake" || id == "gdb" || id == "git" || id == "zenity") return m.empty() ? "" : std::string(id);
    return "";
}

std::string installCommand(const std::vector<Status>& statuses, const Distro& d) {
    if (d.manager.empty()) return {};
    std::string pkgs;
    for (const auto& s : statuses) {
        if (s.ok()) continue;
        const std::string p = packageName(s.tool.id, d.manager);
        if (!p.empty() && (" " + pkgs + " ").find(" " + p + " ") == std::string::npos) pkgs += (pkgs.empty() ? "" : " ") + p;
    }
    if (pkgs.empty()) return {};
    if (d.manager == "dnf") return "sudo dnf install -y " + pkgs;
    if (d.manager == "apt") return "sudo apt install -y " + pkgs;
    if (d.manager == "pacman") return "sudo pacman -S --needed " + pkgs;
    if (d.manager == "zypper") return "sudo zypper install -y " + pkgs;
    return "sudo apk add " + pkgs;
}

// ---------------------------------------------------------------- managed downloads

std::string toolsRoot() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return (fs::path(x) / "daemon/tools").string();
    if (const char* h = std::getenv("HOME"); h && *h) return (fs::path(h) / ".local/share/daemon/tools").string();
    return {};
}

static std::string machineArch() {
    utsname u{};
    if (::uname(&u) != 0) return "x86_64";
    return u.machine;
}

static const char* releaseApi(std::string_view id) {
    if (id == "clangd") return "https://api.github.com/repos/clangd/clangd/releases/latest";
    if (id == "cmake") return "https://api.github.com/repos/Kitware/CMake/releases/latest";
    if (id == "ninja") return "https://api.github.com/repos/ninja-build/ninja/releases/latest";
    return nullptr;
}

bool canDownload(std::string_view id) {
    const std::string arch = machineArch();
    if (!releaseApi(id) || toolsRoot().empty()) return false;
    if (id == "clangd") return arch == "x86_64";   // upstream publishes Linux x86_64 only
    return arch == "x86_64" || arch == "aarch64";
}

std::optional<Asset> pickAsset(std::string_view id, std::string_view releaseJson, std::string_view arch) {
    auto j = json::parse(std::string(releaseJson));
    if (!j) return std::nullopt;
    const auto* assets = (*j)["assets"].array();
    if (!assets) return std::nullopt;
    auto wanted = [&](const std::string& n) {
        if (id == "clangd") return arch == "x86_64" && n.starts_with("clangd-linux-") && n.ends_with(".zip");
        if (id == "cmake") return n.starts_with("cmake-") && n.ends_with(std::format("-linux-{}.tar.gz", arch));
        if (id == "ninja") return n == (arch == "aarch64" ? "ninja-linux-aarch64.zip" : "ninja-linux.zip") && (arch == "x86_64" || arch == "aarch64");
        return false;
    };
    for (const auto& a : *assets) {
        const std::string name = a["name"].str();
        if (!wanted(name)) continue;
        Asset out;
        out.name = name;
        out.url = a["browser_download_url"].str();
        out.size = static_cast<long long>(a["size"].num());
        out.tag = (*j)["tag_name"].str();
        std::string digest = a["digest"].str();
        if (digest.starts_with("sha256:")) out.sha256 = digest.substr(7);
        for (char& c : out.sha256) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (out.url.empty()) return std::nullopt;
        return out;
    }
    return std::nullopt;
}

namespace {

InstallResult fail(std::string msg) { InstallResult r; r.error = std::move(msg); return r; }

// Runs a command to completion; success means exit code 0.
bool run(std::vector<std::string> argv, std::string* out = nullptr, int timeoutMs = 120000) {
    ProcessOptions o;
    o.argv = std::move(argv);
    auto r = Process::runCapture(o, timeoutMs);
    if (out) *out = r.output;
    return r.error.empty() && !r.timedOut && r.exitCode == 0;
}

bool fetchText(const std::string& url, std::string& out) {
    return run({"curl", "-fsSL", "--retry", "2", "-H", "Accept: application/vnd.github+json", url}, &out, 60000) ||
           run({"wget", "-qO-", "--header=Accept: application/vnd.github+json", url}, &out, 60000);
}

bool fetchFile(const std::string& url, const fs::path& dest, Progress& p, long long expected) {
    std::atomic<bool> stop{false};
    std::thread poller([&] {   // the transfer blocks in the child process; watch the growing file for progress
        while (!stop) {
            std::error_code ec;
            const auto sz = fs::exists(dest, ec) ? fs::file_size(dest, ec) : 0;
            if (!ec) p.done = static_cast<long long>(sz);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });
    p.total = expected;
    const bool ok = run({"curl", "-fL", "--retry", "3", "-sS", "-o", dest.string(), url}, nullptr, 3'600'000) ||
                    run({"wget", "-q", "-O", dest.string(), url}, nullptr, 3'600'000);
    stop = true;
    poller.join();
    return ok;
}

bool extractZip(const fs::path& zip, const fs::path& dir) {
    return run({"unzip", "-q", "-o", zip.string(), "-d", dir.string()}, nullptr, 600000) ||
           run({"bsdtar", "-xf", zip.string(), "-C", dir.string()}, nullptr, 600000) ||
           run({"python3", "-m", "zipfile", "-e", zip.string(), dir.string()}, nullptr, 600000);
}

std::optional<fs::path> findFile(const fs::path& root, const std::string& name, bool inBin) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().filename() != name) continue;
        if (!inBin || it->path().parent_path().filename() == "bin") return it->path();
    }
    return std::nullopt;
}

}  // namespace

InstallResult install(std::string_view id, Progress& p) {
    if (!canDownload(id)) return fail("Downloads are not available for " + std::string(id) + " on this machine");
    const std::string arch = machineArch();
    const fs::path root = toolsRoot();
    std::error_code ec;
    fs::create_directories(root / "bin", ec);
    if (ec) return fail("Cannot create " + root.string() + ": " + ec.message());

    p.setPhase("Looking up the latest release");
    std::string text;
    if (!fetchText(releaseApi(id), text)) return fail("Could not reach github.com (is curl or wget installed and the network up?)");
    auto asset = pickAsset(id, text, arch);
    if (!asset) return fail("No matching download found in the latest release");
    if (asset->sha256.size() != 64) return fail("The release publishes no SHA-256 checksum; refusing to install an unverified download");

    const fs::path work = root / ".download";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path archive = work / asset->name;

    p.setPhase("Downloading " + asset->name);
    if (!fetchFile(asset->url, archive, p, asset->size)) { fs::remove_all(work, ec); return fail("Download failed"); }

    p.setPhase("Verifying checksum");
    std::string sum;
    if (!run({"sha256sum", archive.string()}, &sum) || sum.size() < 64) { fs::remove_all(work, ec); return fail("sha256sum is not available"); }
    if (sum.substr(0, 64) != asset->sha256) { fs::remove_all(work, ec); return fail("Checksum mismatch - the download was discarded"); }

    p.setPhase("Extracting");
    const std::string version = asset->tag.starts_with("v") ? asset->tag.substr(1) : asset->tag;
    const fs::path dest = root / std::string(id) / version;
    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    const bool tarball = asset->name.ends_with(".tar.gz");
    if (!(tarball ? run({"tar", "-xzf", archive.string(), "-C", dest.string()}, nullptr, 600000) : extractZip(archive, dest))) {
        fs::remove_all(dest, ec); fs::remove_all(work, ec);
        return fail(tarball ? "Could not extract the archive (is tar installed?)" : "Could not extract the archive (install unzip)");
    }
    fs::remove_all(work, ec);

    p.setPhase("Installing");
    std::vector<std::string> names = id == "cmake" ? std::vector<std::string>{"cmake", "ctest", "cpack"} : std::vector<std::string>{std::string(id)};
    for (const auto& n : names) {
        auto bin = findFile(dest, n, id != "ninja");
        if (!bin) { fs::remove_all(dest, ec); return fail("The archive did not contain " + n); }
        fs::permissions(*bin, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        const fs::path link = root / "bin" / n;
        fs::remove(link, ec);
        fs::create_symlink(fs::absolute(*bin), link, ec);
        if (ec) return fail("Cannot link " + n + ": " + ec.message());
    }

    std::string banner;
    const fs::path main = root / "bin" / std::string(id);
    if (!run({main.string(), "--version"}, &banner, 10000)) return fail("The downloaded program does not run on this system");
    for (const auto& e : fs::directory_iterator(root / std::string(id), ec))   // keep only the newest version
        if (e.path().filename() != version) fs::remove_all(e.path(), ec);
    InstallResult r;
    r.ok = true;
    r.version = parseVersion(banner);
    return r;
}

}  // namespace ide::toolchain
