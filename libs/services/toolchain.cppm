// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.services.toolchain — finds the external tools Daemon drives (clangd, cmake, ninja, gdb, git, ...), checks
// their versions, and works out the install command for the user's distribution.
//
// Tool lookup order: $DAEMON_TOOLS_DIR/bin, a copy shipped next to the executable (libexec/daemon/bin or tools/bin),
// ~/.local/share/daemon/tools/bin, then $PATH. init() prepends the existing directories to $PATH so every child process
// (including cmake running ninja) uses the same tools.
export module daemon.services.toolchain;
import std;
import daemon.base.util;
import daemon.base.process;
import daemon.base.json;

export namespace ide::toolchain {

enum class Need { Recommended, Optional };

struct Tool {
    std::string id;                       // also the binary name
    std::string name;                     // display name
    std::string purpose;                  // what it is used for
    int minMajor = 0, minMinor = 0;       // older versions are reported as "too old"
    Need need = Need::Recommended;
};

struct Status {
    Tool tool;
    std::string path;                     // empty when not found
    std::string version;                  // "18.1.8" (empty if it could not be read)
    bool found = false;
    bool tooOld = false;
    bool bundled = false;                 // found in a Daemon tools directory rather than on the system PATH
    bool ok() const { return found && !tooOld; }
};

struct Distro {
    std::string id;                       // "fedora", "ubuntu", "arch", ...
    std::string name;                     // pretty name
    std::string manager;                  // "dnf", "apt", "pacman", "zypper", "apk" or "" when unknown
};

const std::vector<Tool>& tools();

// Directories Daemon looks in before $PATH (only existing ones).
std::vector<std::string> toolDirs();
// Prepends toolDirs() to $PATH. Call once at startup.
void init();

// Blocking: runs `<tool> --version` for every tool (a few tens of milliseconds in total).
std::vector<Status> detect();

// "18.1.8" out of any `--version` banner; empty if there is no version number.
std::string parseVersion(std::string_view banner);
bool versionAtLeast(std::string_view version, int major, int minor);

Distro parseOsRelease(std::string_view text);
Distro distro();

// Package names for a tool on a package manager ("" if unknown).
std::string packageName(std::string_view toolId, std::string_view manager);
// e.g. "sudo dnf install -y clang-tools-extra cmake". Empty when nothing needs installing or the distro is unknown.
std::string installCommand(const std::vector<Status>& statuses, const Distro& d);

// ---------------------------------------------------------------- managed downloads
//
// clangd, CMake and Ninja publish official Linux binaries on GitHub. Daemon can download the latest release into
// <toolsRoot>/<tool>/<version>/ (verified against the SHA-256 digest GitHub publishes for the asset), and link the
// executables into <toolsRoot>/bin, which comes first in the tool lookup order. Nothing is installed system-wide.

struct Asset {
    std::string name, url, sha256, tag;   // sha256 is lower-case hex
    long long size = 0;
};

// The user-writable tools directory (~/.local/share/daemon/tools).
std::string toolsRoot();
// True for tools Daemon can download on this machine's architecture.
bool canDownload(std::string_view toolId);
// Chooses the release asset for `toolId` out of a GitHub `releases/latest` JSON document ("x86_64" / "aarch64").
std::optional<Asset> pickAsset(std::string_view toolId, std::string_view releaseJson, std::string_view arch);

// Progress shared with the UI thread.
struct Progress {
    std::atomic<long long> done{0}, total{0};
    std::mutex mutex;
    std::string phase;                    // "Looking up the latest release", "Downloading", ...
    void setPhase(std::string s) { std::lock_guard l(mutex); phase = std::move(s); }
    std::string getPhase() { std::lock_guard l(mutex); return phase; }
};

struct InstallResult {
    bool ok = false;
    std::string error, version;
};

// Blocking (run it on a worker thread). Downloads, verifies, extracts and links `toolId`.
InstallResult install(std::string_view toolId, Progress& progress);

}  // namespace ide::toolchain
