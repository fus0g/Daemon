// SPDX-License-Identifier: GPL-3.0-or-later
module;
#include <stdlib.h>
#include <unistd.h>
module daemon.services.toolchain;
import std;
import daemon.base.util;
import daemon.base.process;

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
    for (const auto& d : dirs) path += d + ":";
    const char* old = std::getenv("PATH");
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

}  // namespace ide::toolchain
