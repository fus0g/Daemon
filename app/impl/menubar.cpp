// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / menubar - the File / Edit / ... menu that the logo in the title bar slides out (part of the App implementation).
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.theme;
import daemon.text.document;
import daemon.services.explorer;
using Clock = std::chrono::steady_clock;

namespace ide {

namespace {

struct Entry {
    const char* label;   // "" = separator
    const char* hint;
    Key key = Key::None;
    char32_t ch = 0;
    bool ctrl = false, shift = false, alt = false;
};

struct Top {
    const char* name;
    std::vector<Entry> items;
};

constexpr Entry sep{""};
Entry K(const char* l, const char* h, Key k, bool c = false, bool s = false, bool a = false) { return {l, h, k, 0, c, s, a}; }
Entry C(const char* l, const char* h, char32_t ch, bool c, bool s = false, bool a = false) { return {l, h, Key::Char, ch, c, s, a}; }

// Every entry is a keyboard shortcut that already exists: choosing it sends the same key event.
const std::vector<Top>& tops() {
    static const std::vector<Top> t = {
        {"File", {C("New File", "Ctrl+N", 'n', true), C("Open File...", "Ctrl+O", 'o', true), C("Open Folder...", "Ctrl+Shift+O", 'o', true, true),
                  C("Open Project...", "Ctrl+Shift+P", 'p', true, true), sep, C("Save", "Ctrl+S", 's', true), C("Save As...", "Ctrl+Shift+S", 's', true, true), sep,
                  C("Settings...", "Ctrl+,", ',', true), sep, C("Close Tab", "Ctrl+W", 'w', true), C("Quit", "Ctrl+Q", 'q', true)}},
        {"Edit", {C("Undo", "Ctrl+Z", 'z', true), C("Redo", "Ctrl+Y", 'y', true), sep, C("Cut", "Ctrl+X", 'x', true), C("Copy", "Ctrl+C", 'c', true),
                  C("Paste", "Ctrl+V", 'v', true), sep, C("Find", "Ctrl+F", 'f', true), C("Replace", "Ctrl+H", 'h', true),
                  C("Find in Files", "Ctrl+Shift+F", 'f', true, true), C("Replace in Files", "Ctrl+Shift+H", 'h', true, true), sep,
                  C("Toggle Comment", "Ctrl+/", '/', true)}},
        {"Selection", {C("Select All", "Ctrl+A", 'a', true), C("Select Line", "Ctrl+L", 'l', true), sep, K("Move Line Up", "Alt+Up", Key::Up, false, false, true),
                       K("Move Line Down", "Alt+Down", Key::Down, false, false, true), C("Duplicate Line", "Ctrl+Shift+D", 'd', true, true),
                       C("Delete Line", "Ctrl+Shift+K", 'k', true, true), sep, C("Indent", "Ctrl+]", ']', true), C("Outdent", "Ctrl+[", '[', true)}},
        {"View", {C("Sidebar", "Ctrl+B", 'b', true), C("Explorer", "Ctrl+Shift+E", 'e', true, true), C("Source Control", "Ctrl+Shift+G", 'g', true, true),
                  C("Problems", "Ctrl+Shift+M", 'm', true, true), C("Terminal", "Ctrl+`", '`', true), sep, C("Markdown Preview", "Ctrl+Shift+V", 'v', true, true), sep,
                  C("Zoom In", "Ctrl+=", '=', true), C("Zoom Out", "Ctrl+-", '-', true), C("Reset Zoom", "Ctrl+0", '0', true)}},
        {"Go", {C("Go to Line...", "Ctrl+G", 'g', true), K("Go to Definition", "F12", Key::F12), K("Back", "Alt+Left", Key::Left, false, false, true),
                C("Header / Source", "Alt+O", 'o', false, false, true), sep, K("Next Tab", "Ctrl+Tab", Key::Tab, true)}},
        {"Run", {K("Build", "Ctrl+F9", Key::F9, true), K("Build and Run", "Shift+F10", Key::F10, false, true), K("Build and Debug", "Shift+F9", Key::F9, false, true), sep,
                 K("Start / Continue Debugging", "F5", Key::F5), K("Stop Debugging", "Shift+F5", Key::F5, false, true), K("Step Over", "F10", Key::F10),
                 K("Step Into", "F11", Key::F11), K("Step Out", "Shift+F11", Key::F11, false, true), K("Toggle Breakpoint", "F9", Key::F9), sep,
                 K("Stop Running Program", "Ctrl+F2", Key::F2, true), C("Reload CMake", "Ctrl+Shift+R", 'r', true, true)}},
        {"Terminal", {C("Open Terminal", "Ctrl+`", '`', true), C("Problems", "Ctrl+Shift+M", 'm', true, true)}},
        {"Help", {{"Check Toolchain..."}, {"About Daemon"}}},
    };
    return t;
}

}  // namespace

float App::barPad() const { return std::round(12 * S()); }

void App::barLayout() {
    bar_.rects.clear();
    if (!L_.csd) { bar_.w = 0; return; }
    const float s = S();
    bar_.x = std::round(46 * s);
    float x = bar_.x;
    const float h = std::round(L_.title.h - 8 * s), y = L_.title.y + std::round((L_.title.h - h) / 2);
    for (const auto& t : tops()) {
        const float w = uiW(t.name) + 2 * barPad();
        bar_.rects.push_back({x, y, w, h});
        x += w;
    }
    bar_.w = x - bar_.x;
}

float App::barShift() const { return L_.csd ? easeInOut(bar_.anim) * (bar_.w + std::round(6 * S())) : 0.0f; }

int App::barLabelAt(float x, float y) const {
    if (!bar_.shown || bar_.anim < 0.95f) return -1;
    for (std::size_t i = 0; i < bar_.rects.size(); ++i)
        if (bar_.rects[i].contains(x, y)) return static_cast<int>(i);
    return -1;
}

Rect App::barZone() const {
    const Rect lg = logoPill();
    return {lg.x, L_.title.y, bar_.x + bar_.w - lg.x + std::round(6 * S()), L_.title.h};
}

void App::barToggle() {
    if (bar_.shown) { if (menu_.open && bar_.active >= 0) closeMenu(); bar_.shown = false; }
    else { bar_.shown = true; bar_.lastUse = Clock::now(); }
    dirty_ = true;
}

void App::barHide() {
    if (menu_.open && bar_.active >= 0) closeMenu();
    bar_.shown = false;
    bar_.active = -1;
    dirty_ = true;
}

void App::barOpen(int i) {
    const auto& all = tops();
    if (i < 0 || i >= static_cast<int>(all.size()) || static_cast<std::size_t>(i) >= bar_.rects.size()) return;
    const Top& top = all[static_cast<std::size_t>(i)];
    menu_ = {};
    menu_.open = true;
    menuActions_.clear();
    auto add = [&](std::string label, std::string hint, std::function<void()> fn) {
        menu_.items.push_back({std::move(label), std::move(hint), Act::Custom, true, static_cast<int>(menuActions_.size())});
        menuActions_.push_back(std::move(fn));
    };
    for (const Entry& e : top.items) {
        if (!*e.label) { menu_.items.push_back({"", "", Act::Sep}); continue; }
        const std::string label = e.label;
        if (e.key == Key::None) {   // Help
            if (label == "About Daemon") add(label, "", [this] { setMessage(std::format("Daemon {} - GPL-3.0-or-later", versionFull()), false, 5); });
            else add(label, "", [this] { toolsCheck(true); });
            continue;
        }
        KeyEvent ev;
        ev.key = e.key; ev.ch = e.ch; ev.ctrl = e.ctrl; ev.shift = e.shift; ev.alt = e.alt;
        add(label, e.hint, [this, ev] { barHide(); onKey(ev); });
    }
    const Rect& r = bar_.rects[static_cast<std::size_t>(i)];
    layoutMenu(r.x, L_.title.bottom() + std::round(2 * S()));
    bar_.active = i;
    bar_.lastUse = Clock::now();
}

void App::barKey(int dir) { barOpen((bar_.active + dir + static_cast<int>(tops().size())) % static_cast<int>(tops().size())); }

void App::barTick(Clock::time_point now) {
    if (!bar_.shown) return;
    if (menu_.open || barZone().contains(mx_, my_)) { bar_.lastUse = now; return; }
    if (now - bar_.lastUse > std::chrono::milliseconds(2200)) barHide();
}

void App::drawMenuBar() {
    if (!L_.csd || bar_.anim < 0.01f) return;
    const float s = S(), a = easeInOut(bar_.anim);
    const float clipX = bar_.x + (bar_.w + 6 * s) * a;   // text is revealed as the toolbar slides away
    const auto& all = tops();
    for (std::size_t i = 0; i < bar_.rects.size(); ++i) {
        const Rect& r = bar_.rects[i];
        const bool hot = bar_.hover == static_cast<int>(i), open = bar_.active == static_cast<int>(i);
        if ((hot || open) && r.right() <= clipX) fill(r, open ? theme::selection : Color::rgb(0xffffff, 24), r.h / 2);
        uiLabel(r.x + barPad(), r.y, r.h, all[i].name, theme::text.alpha(static_cast<std::uint8_t>(255 * a)), clipX);
    }
}

}  // namespace ide
