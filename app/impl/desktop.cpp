// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / desktop - registers the launcher entry and icon so every desktop shows the right name and logo.
//
// Wayland shells (GNOME, KDE, ...) pick the taskbar / overview icon from a .desktop file whose name matches the
// window's app id. Daemon therefore writes ~/.local/share/applications/daemon.desktop and the icon itself on start
// (idempotent, user-level, no installer needed). The icon pixels come from the built-in vector logo.
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.icons;

namespace ide {
namespace fs = std::filesystem;

void App::installDesktopEntry() {
    if (!opt_.screenshot.empty() || std::getenv("DAEMON_NO_INTEGRATION")) return;
    std::error_code ec;
    // An installed package (deb / rpm) already provides the launcher entry and icons: do not shadow it.
    for (const char* sys : {"/usr/share/applications/daemon.desktop", "/usr/local/share/applications/daemon.desktop"})
        if (fs::exists(sys, ec)) return;
    // Inside an AppImage /proc/self/exe is a temporary mount point; the launcher must point at the .AppImage file itself.
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (const char* ai = std::getenv("APPIMAGE"); ai && *ai) exe = ai;
    if (ec || exe.empty()) return;

    fs::path data;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) data = x;
    else if (const char* h = std::getenv("HOME"); h && *h) data = fs::path(h) / ".local/share";
    else return;

    const std::string entry =
        "[Desktop Entry]\nType=Application\nName=Daemon\nGenericName=C++ IDE\nComment=Wayland + Vulkan C++ IDE\n"
        "Exec=\"" + exe.string() + "\" %F\nIcon=daemon\nTerminal=false\nCategories=Development;IDE;TextEditor;\n"
        "StartupWMClass=daemon\nStartupNotify=true\n";
    const fs::path desktop = data / "applications/daemon.desktop";
    bool changed = false;

    auto current = readFile(desktop.string());
    if (!current || *current != entry) {
        fs::create_directories(desktop.parent_path(), ec);
        if (writeFile(desktop.string(), entry)) changed = true;
    }

    // Bump when the logo changes so installed icons are regenerated.
    constexpr std::string_view kIconRev = "helm-2";
    const fs::path stamp = data / "icons/hicolor/.daemon-icon-rev";
    const bool iconsFresh = readFile(stamp).value_or("") == kIconRev;
    for (int size : {48, 128, 256}) {
        const fs::path png = data / "icons/hicolor" / std::format("{}x{}", size, size) / "apps/daemon.png";
        if (iconsFresh && fs::exists(png, ec)) continue;
        fs::create_directories(png.parent_path(), ec);
        const auto argb = appIconPixels(size);
        std::vector<std::uint8_t> rgba(argb.size() * 4);
        for (std::size_t i = 0; i < argb.size(); ++i) {   // premultiplied ARGB -> straight RGBA
            const std::uint32_t p = argb[i];
            const std::uint32_t a = p >> 24;
            auto un = [&](std::uint32_t c) { return static_cast<std::uint8_t>(a ? std::min<std::uint32_t>(255, c * 255 / a) : 0); };
            rgba[i * 4] = un((p >> 16) & 255); rgba[i * 4 + 1] = un((p >> 8) & 255); rgba[i * 4 + 2] = un(p & 255);
            rgba[i * 4 + 3] = static_cast<std::uint8_t>(a);
        }
        if (writePng(png, static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size), rgba)) changed = true;
    }

    if (!iconsFresh) writeFile(stamp, kIconRev);
    if (changed) {   // a stale icon cache would hide the new icon from GTK / GNOME Shell until the next login
        fs::remove(data / "icons/hicolor/icon-theme.cache", ec);
        std::error_code ec2;
        fs::last_write_time(data / "icons/hicolor", fs::file_time_type::clock::now(), ec2);
    }
}

}  // namespace ide
