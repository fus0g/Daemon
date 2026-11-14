// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / dialogs - part of the App implementation (see app.cppm for the class).
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.options;
import daemon.app.theme;
import daemon.ui.vulkan;
import daemon.ui.wayland;
import daemon.text.document;
import daemon.services.explorer;
import daemon.ui.icons;
import daemon.base.json;
import daemon.base.process;
import daemon.services.cmake;
import daemon.services.project;
import daemon.services.git;
import daemon.services.search;
import daemon.services.gdb;
import daemon.services.terminal;
import daemon.services.lsp;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

std::string App::uniqueProjectName(const std::string& parent, const std::string& base) {
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        const std::string n = i == 0 ? base : base + std::to_string(i);
        if (!fs::exists(fs::path(parent) / n, ec)) return n;
    }
    return base;
}

void App::newProjectOpen() {
    const std::string keepLoc = dlg_.location.text;
    dlg_ = {};
    dlg_.kind = DlgKind::NewProject;
    std::string loc = !state_.lastLocation.empty() ? state_.lastLocation : (fs::path(homeDir()) / "Projects").string();
    std::error_code ec;
    if (!fs::is_directory(loc, ec)) loc = fs::is_directory(fs::path(homeDir()) / "Projects", ec) ? (fs::path(homeDir()) / "Projects").string() : homeDir();
    (void)keepLoc;
    dlg_.location = {loc, loc.size()};
    const std::string n = uniqueProjectName(loc, "untitled");
    dlg_.name = {n, n.size()};
    dlg_.std_ = 1;   // C++20
    dlg_.focus = 0;
    dirty_ = true;
}

void App::browseRefresh() {
    dlg_.entries.clear();
    dlg_.sel = -1;
    dlg_.scroll = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dlg_.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        const std::string n = it->path().filename().string();
        if (!dlg_.showHidden && n.starts_with('.')) continue;
        dlg_.entries.push_back({n, fs::exists(it->path() / "CMakeLists.txt", e2)});
    }
    std::ranges::sort(dlg_.entries, [](const BrowseEntry& a, const BrowseEntry& b) {
        auto lower = [](std::string s) { std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; };
        return lower(a.name) < lower(b.name);
    });
    dlg_.path = {dlg_.dir, dlg_.dir.size()};
}

void App::browseOpen(bool forLocation) {
    if (pickerBusy_) return;
    if (!findExecutable("zenity")) { browseOpenBuiltin(forLocation); return; }
    std::error_code ec;
    std::string start = forLocation ? dlg_.location.text : (explorer_.isOpen() ? explorer_.rootPath().parent_path().string() : state_.lastLocation);
    start = expandPath(start);
    if (start.empty() || !fs::is_directory(start, ec)) start = homeDir();
    if (start.back() != '/') start += '/';
    const std::string title = forLocation ? "Choose Location" : "Open Folder";
    pickerBusy_ = true;
    setMessage("Choose a folder in the file dialog...", false, 4);
    spawn([this, start, title, forLocation]() -> std::function<void()> {
        ProcessOptions o;
        o.argv = {"zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + start};
        auto r = Process::runCapture(o, 30 * 60 * 1000);
        std::string chosen;
        std::istringstream in(r.output);
        for (std::string line; std::getline(in, line);) if (!line.empty() && line.front() == '/') chosen = line;
        const bool failed = !r.error.empty();
        return [this, chosen, forLocation, failed] {
            pickerBusy_ = false;
            if (failed) { browseOpenBuiltin(forLocation); return; }
            if (chosen.empty()) return;   // cancelled
            if (forLocation) {
                if (dlg_.kind != DlgKind::NewProject) newProjectOpen();
                dlg_.location = {chosen, chosen.size()};
                dlg_.error.clear();
                dlg_.focus = 0;
            } else {
                openProject(chosen);
            }
        };
    });
}

void App::browseOpenBuiltin(bool forLocation) {
    const DlgKind from = dlg_.kind;
    if (from != DlgKind::NewProject) dlg_ = {};
    dlg_.kind = DlgKind::Browse;
    dlg_.forLocation = forLocation;
    dlg_.pathFocus = true;
    std::error_code ec;
    std::string start = forLocation ? dlg_.location.text : (explorer_.isOpen() ? explorer_.rootPath().parent_path().string() : state_.lastLocation);
    if (start.empty() || !fs::is_directory(start, ec)) start = homeDir();
    dlg_.dir = fs::path(start).lexically_normal().string();
    while (dlg_.dir.size() > 1 && dlg_.dir.back() == '/') dlg_.dir.pop_back();
    browseRefresh();
    dirty_ = true;
}

void App::browseGo(const std::string& dir) {
    std::error_code ec;
    fs::path p = fs::path(expandPath(dir)).lexically_normal();
    if (!fs::is_directory(p, ec)) { setMessage("Not a folder: " + dir, true, 2); return; }
    dlg_.dir = p.string();
    while (dlg_.dir.size() > 1 && dlg_.dir.back() == '/') dlg_.dir.pop_back();
    browseRefresh();
}

void App::browseConfirm() {
    std::string target = dlg_.dir;
    if (dlg_.sel >= 0 && dlg_.sel < static_cast<int>(dlg_.entries.size()))
        target = (fs::path(dlg_.dir) / dlg_.entries[static_cast<std::size_t>(dlg_.sel)].name).string();
    if (dlg_.forLocation) {
        dlg_.location = {target, target.size()};
        dlg_.kind = DlgKind::NewProject;
        dlg_.error.clear();
        dlg_.focus = 0;
        dirty_ = true;
    } else {
        openProject(target);
    }
}

void App::openProject(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) { setMessage("Not a folder: " + path, true, 3); return; }
    dlgClose();
    openFolder(path);
}

void App::createProjectFromDialog() {
    NewProjectSpec spec;
    const auto& t = projectTemplates()[static_cast<std::size_t>(dlg_.tmpl)];
    spec.templateId = t.id;
    spec.name = dlg_.name.text;
    spec.parent = expandPath(dlg_.location.text);
    static const char* cxx[] = {"17", "20", "23"};
    static const char* cc[] = {"11", "17"};
    spec.standard = t.isC ? cc[std::clamp(dlg_.std_, 0, 1)] : cxx[std::clamp(dlg_.std_, 0, 2)];
    spec.initGit = dlg_.git;
    auto r = createProject(spec);
    if (!r) { dlg_.error = r.error(); dirty_ = true; return; }
    state_.lastLocation = spec.parent.string();
    saveState(state_);
    dlgClose();
    openFolder(r->dir.string());
    openFile(r->mainFile.string());
    if (!r->note.empty()) setMessage("Created " + r->dir.string() + " (" + r->note + ")", false, 4);
    else setMessage("Created " + r->dir.string(), false, 3);
}

bool App::editText(TextBox& t, const KeyEvent& k) {
    t.cur = std::min(t.cur, t.text.size());
    if (k.ctrl && k.key == Key::Char) {
        if (k.ch == 'v') {
            std::string c = win_.clipboard();
            std::erase(c, '\r');
            if (auto nl = c.find('\n'); nl != std::string::npos) c.resize(nl);
            t.text.insert(t.cur, c);
            t.cur += c.size();
            return true;
        }
        if (k.ch == 'a') { t.cur = t.text.size(); return true; }
        return false;
    }
    switch (k.key) {
        case Key::Left: if (t.cur > 0) t.cur = utf8Prev(t.text, t.cur); return true;
        case Key::Right: if (t.cur < t.text.size()) t.cur = utf8Next(t.text, t.cur); return true;
        case Key::Home: t.cur = 0; return true;
        case Key::End: t.cur = t.text.size(); return true;
        case Key::Backspace:
            if (t.cur > 0) { auto p = utf8Prev(t.text, t.cur); t.text.erase(p, t.cur - p); t.cur = p; }
            return true;
        case Key::Delete:
            if (t.cur < t.text.size()) { auto n = utf8Next(t.text, t.cur); t.text.erase(t.cur, n - t.cur); }
            return true;
        default:
            if (!k.text.empty() && !k.alt) { t.text.insert(t.cur, k.text); t.cur += k.text.size(); return true; }
            return false;
    }
}

std::size_t App::caretFromX(const std::string& t, float relX) {
    std::size_t best = 0;
    float bestD = std::abs(relX);
    for (std::size_t i = 0; i < t.size();) {
        i = utf8Next(t, i);
        const float d = std::abs(relX - uiW(std::string_view(t).substr(0, i)));
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void App::dlgKey(const KeyEvent& k) {
    Dialog& d = dlg_;
    if (d.kind == DlgKind::Settings && k.key == Key::Escape) { settingsKey(k); return; }
    if (k.key == Key::Escape) {
        if (d.kind == DlgKind::Browse && !d.forLocation) { dlgClose(); return; }
        if (d.kind == DlgKind::Browse) { d.kind = DlgKind::NewProject; return; }
        if (d.kind == DlgKind::NewProject) { welcomeOpen(); return; }
        dlgClose();
        return;
    }
    if (k.ctrl && k.key == Key::Char && k.ch == 'q') { requestQuit(); return; }
    if (d.kind == DlgKind::Settings) { settingsKey(k); return; }
    switch (d.kind) {
        case DlgKind::Welcome:
            if (k.key == Key::Enter) { newProjectOpen(); return; }
            if (k.ctrl && k.shift && k.key == Key::Char && k.ch == 'o') { browseOpen(false); return; }
            if (k.ctrl && k.shift && k.key == Key::Char && k.ch == 'n') { newProjectOpen(); return; }
            break;
        case DlgKind::NewProject: {
            if (k.key == Key::Tab) { d.focus = (d.focus + 1) % 2; return; }
            if (k.key == Key::Enter) { createProjectFromDialog(); return; }
            if (k.key == Key::Up || k.key == Key::Down) {
                const int n = static_cast<int>(projectTemplates().size());
                d.tmpl = (d.tmpl + (k.key == Key::Down ? 1 : n - 1)) % n;
                d.std_ = std::min(d.std_, projectTemplates()[static_cast<std::size_t>(d.tmpl)].isC ? 1 : 2);
                return;
            }
            TextBox& t = d.focus == 0 ? d.name : d.location;
            if (editText(t, k)) d.error.clear();
            break;
        }
        case DlgKind::Browse: {
            if (k.key == Key::Tab) { d.pathFocus = !d.pathFocus; return; }
            const int n = static_cast<int>(d.entries.size());
            if (d.pathFocus) {
                if (k.key == Key::Enter) { browseGo(d.path.text); d.pathFocus = false; return; }
                if (k.key == Key::Down && n > 0) { d.pathFocus = false; d.sel = 0; return; }
                editText(d.path, k);
                return;
            }
            switch (k.key) {
                case Key::Up: d.sel = std::max(0, d.sel - 1); dlgScrollToSel(); return;
                case Key::Down: d.sel = std::min(n - 1, d.sel + 1); dlgScrollToSel(); return;
                case Key::Right: if (d.sel >= 0 && d.sel < n) browseGo((fs::path(d.dir) / d.entries[static_cast<std::size_t>(d.sel)].name).string()); return;
                case Key::Left: case Key::Backspace: browseGo(fs::path(d.dir).parent_path().string()); return;
                case Key::Enter: browseConfirm(); return;
                default: break;
            }
            break;
        }
        default: break;
    }
}

void App::dlgScrollToSel() {
    const float rowH = std::round(34 * S());
    const float top = dlg_.sel * rowH;
    if (top < dlg_.scroll) dlg_.scroll = top;
    else if (top + rowH > dlg_.scroll + browseListH_) dlg_.scroll = top + rowH - browseListH_;
}

void App::dlgClick(Button b, float x, float y) {
    if (b != Button::Left) return;
    int id = -1;
    for (auto it = dlgHits_.rbegin(); it != dlgHits_.rend(); ++it)
        if (it->r.contains(x, y)) { id = it->id; break; }
    const auto now = Clock::now();
    const bool dbl = id >= 0 && id == dlgLastId_ && now - dlgLastClick_ < std::chrono::milliseconds(420);
    dlgLastClick_ = now;
    dlgLastId_ = id;
    Dialog& d = dlg_;
    if (id < 0) {
        if (!dlgPanel_.contains(x, y)) { if (d.kind == DlgKind::Welcome) dlgClose(); else if (d.kind == DlgKind::Settings) { settingsCommitEdit(); dlgClose(); } }
        else if (d.kind == DlgKind::Settings) settingsCommitEdit();
        return;
    }
    if (d.kind == DlgKind::Settings) { settingsClick(id, x); return; }
    auto hitRect = [&](int want) { for (const auto& h : dlgHits_) if (h.id == want) return h.r; return Rect{}; };
    if (id == kDlgNew) { newProjectOpen(); return; }
    if (id == kDlgOpen) { browseOpen(false); return; }
    if (id == kDlgSettings) { settingsOpen(); return; }
    if (id >= kDlgRecentDel && id < kDlgRecentDel + 100) {
        const auto idx = static_cast<std::size_t>(id - kDlgRecentDel);
        if (idx < dlgRecentShown_.size()) { std::erase(state_.recent, dlgRecentShown_[idx]); saveState(state_); }
        return;
    }
    if (id >= kDlgRecent && id < kDlgRecent + 100) {
        const auto idx = static_cast<std::size_t>(id - kDlgRecent);
        if (idx < dlgRecentShown_.size()) openProject(dlgRecentShown_[idx]);
        return;
    }
    if (id >= kDlgTmpl && id < kDlgTmpl + 50) {
        d.tmpl = id - kDlgTmpl;
        d.std_ = std::min(d.std_, projectTemplates()[static_cast<std::size_t>(d.tmpl)].isC ? 1 : 2);
        return;
    }
    if (id == kDlgName || id == kDlgLoc) {
        d.focus = id == kDlgName ? 0 : 1;
        TextBox& t = d.focus == 0 ? d.name : d.location;
        t.cur = caretFromX(t.text, x - (hitRect(id).x + 10 * S()));
        return;
    }
    if (id == kDlgBrowseBtn) { browseOpen(true); return; }
    if (id >= kDlgStd && id < kDlgStd + 4) { d.std_ = id - kDlgStd; return; }
    if (id == kDlgGit) { d.git = !d.git; return; }
    if (id == kDlgCreate) { createProjectFromDialog(); return; }
    if (id == kDlgCancel) { welcomeOpen(); return; }
    if (id >= kDlgBm && id < kDlgBm + 20) {
        if (static_cast<std::size_t>(id - kDlgBm) < dlgPlaces_.size()) browseGo(dlgPlaces_[static_cast<std::size_t>(id - kDlgBm)]);
        return;
    }
    if (id == kDlgPath) { d.pathFocus = true; d.path.cur = caretFromX(d.path.text, x - (hitRect(id).x + 10 * S())); return; }
    if (id == kDlgUp) { browseGo(fs::path(d.dir).parent_path().string()); return; }
    if (id == kDlgHidden) { d.showHidden = !d.showHidden; browseRefresh(); return; }
    if (id == kDlgOk) { browseConfirm(); return; }
    if (id == kDlgBrowseCancel) { if (d.forLocation) d.kind = DlgKind::NewProject; else dlgClose(); return; }
    if (id >= kDlgEntry && id < kDlgEntry + 400) {
        const int i = id - kDlgEntry;
        d.pathFocus = false;
        d.sel = i;
        if (dbl && i < static_cast<int>(d.entries.size())) browseGo((fs::path(d.dir) / d.entries[static_cast<std::size_t>(i)].name).string());
        return;
    }
}

void App::dlgScroll(float dy) {
    Dialog& d = dlg_;
    if (d.kind == DlgKind::Welcome) d.recentScroll = std::clamp(d.recentScroll + dy, 0.0f, std::max(0.0f, dlgRecentMax_));
    else if (d.kind == DlgKind::Settings) d.setScroll = std::clamp(d.setScroll + dy, 0.0f, std::max(0.0f, settingsMax_));
    else if (d.kind == DlgKind::Browse) d.scroll = std::clamp(d.scroll + dy, 0.0f, std::max(0.0f, static_cast<float>(d.entries.size()) * std::round(34 * S()) - browseListH_));
}

void App::dlgButton(const Rect& r, std::string_view label, int id, bool primary, char32_t icon) {
    const float s = S();
    dlgHit(r, id);
    const bool hot = dlgHot(id);
    if (primary) fill(r, hot ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : theme::accent, r.h / 2);
    else {
        fill(r, Color::rgb(0xffffff, hot ? 60 : 34), r.h / 2);
        fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, hot ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), r.h / 2 - 1);
    }
    float x = r.x + 16 * s;
    const Color tc = primary ? Color::rgb(0xffffff) : theme::text;
    if (icon) {
        const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.15f)));
        Icons::glyph(dl_, font_, x, r.y + (r.h - static_cast<float>(ip)) / 2, ip, icon, primary ? Color::rgb(0xffffff) : theme::dim);
        x += static_cast<float>(ip) + 10 * s;
        uiLabel(x, r.y, r.h, label, tc, r.right() - 8 * s);
    } else {
        uiLabelC(r.x + r.w / 2, r.y, r.h, label, tc);
    }
}

void App::dlgField(const Rect& r, const TextBox& t, bool focused, int id, std::string_view placeholder) {
    const float s = S();
    dlgHit(r, id);
    fill(r, focused ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 56 : 34), 8 * s);
    fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, Color::rgb(0x07080a), 7.5f * s);
    const float x0 = r.x + 10 * s, maxX = r.right() - 10 * s;
    std::size_t start = 0;
    const std::size_t cur = std::min(t.cur, t.text.size());
    while (start < cur && uiW(std::string_view(t.text).substr(start, cur - start)) > maxX - x0 - 4 * s) start = utf8Next(t.text, start);
    const std::string_view shown = std::string_view(t.text).substr(start);
    if (t.text.empty()) uiLabel(x0, r.y, r.h, placeholder, theme::lineNum, maxX);
    else uiLabel(x0, r.y, r.h, shown, theme::text, maxX);
    if (focused && caretOn_) {
        const float cx = x0 + uiW(std::string_view(t.text).substr(start, cur - start));
        fill(std::round(cx), r.y + 8 * s, std::max(2.0f, std::round(1.4f * s)), r.h - 16 * s, theme::caret, s);
    }
}

void App::drawDialog() {
    if (dlg_.kind == DlgKind::None) return;
    const float s = S(), W = L_.status.w, H = L_.status.bottom();
    dlgHits_.clear();
    dlgRecentShown_.clear();
    const float top0 = L_.csd ? L_.title.h : 0;
    fill(0, top0, W, H - top0, Color{0, 0, 0, 150});   // dim everything behind (part of the scene the glass blurs)
    const bool welcome = dlg_.kind == DlgKind::Welcome;
    const float pw = std::min((welcome ? 840.0f : 940.0f) * s, W - 60 * s), ph = std::min((welcome ? 500.0f : 580.0f) * s, H - 80 * s);
    const Rect r{std::round((W - pw) / 2), std::round((H - ph) / 2), std::round(pw), std::round(ph)};
    dlgPanel_ = r;
    dlgHit(r, 0);   // swallows clicks inside the panel
    dlgHits_.back().id = -2;
    switch (dlg_.kind) {
        case DlgKind::Welcome: drawWelcome(r); break;
        case DlgKind::NewProject: drawNewProject(r); break;
        case DlgKind::Browse: drawBrowse(r); break;
        case DlgKind::Settings: drawSettings(r); break;
        default: break;
    }
}

void App::dlgSide(const Rect& r, float w) {
    const float s = S(), gap = std::round(6 * s), rad = std::round(12 * s);
    fill(r.x, r.y, w, r.h, theme::sideBg, rad);
    fill(r.x + w + gap, r.y, r.w - w - gap, r.h, theme::editorBg, rad);
}

void App::welcomeAction(const Rect& row, std::string_view label, std::string_view keys, int id, char32_t icon, bool primary) {
    const float s = S(), hair = std::max(1.0f, std::round(0.8f * s));
    dlgHit(row, id);
    const bool hot = dlgHot(id);
    const Color edge = primary ? theme::accent.alpha(hot ? 230 : 170) : Color::rgb(0xffffff, hot ? 60 : 34);
    fill(row, edge, row.h / 2);   // same outlined pill as the toolbar and the activity bar
    fill(row.x + hair, row.y + hair, row.w - 2 * hair, row.h - 2 * hair, hot ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), row.h / 2 - hair);
    const int ip = std::max(15, static_cast<int>(std::round(row.h * 0.42f)));
    Icons::glyph(dl_, font_, row.x + 18 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, icon, primary ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : Color::rgb(0xffffff));
    const float kx = row.right() - 18 * s;
    uiLabelR(kx, row.y, row.h, keys, theme::lineNum);
    uiLabel(row.x + 18 * s + static_cast<float>(ip) + 12 * s, row.y, row.h, label, theme::text, kx - uiW(keys) - 10 * s);
}

void App::drawWelcome(const Rect& r) {
    const float s = S(), sideW = std::round(320 * s);
    dlgSide(r, sideW);
    const float lx = r.x + 40 * s, cw0 = sideW - 64 * s;
    const int ls = static_cast<int>(std::round(44 * s));
    Icons::logo(dl_, font_, lx, r.y + 48 * s, ls, theme::accent);
    const float ty = r.y + 48 * s + static_cast<float>(ls) + 20 * s;
    dl_.titleText(font_, lx, ty, "Daemon", theme::text);
    const float sub = ty + font_.titleLineHeight() + 2 * s;
    uiLabel(lx, sub, uiH() + 6 * s, "C++ IDE for Wayland", theme::dim);
    uiLabel(lx, r.bottom() - 40 * s, uiH() + 6 * s, "Version " + std::string(version()), theme::lineNum);
    const float rowH = std::round(44 * s);
    float by = sub + uiH() + 44 * s;
    welcomeAction({lx, by, cw0, rowH}, "New Project", "Ctrl+Shift+N", kDlgNew, 0xF0EAB, true);
    by += rowH + 10 * s;
    welcomeAction({lx, by, cw0, rowH}, "Open Folder", "Ctrl+Shift+O", kDlgOpen, 0xF0770, false);
    by += rowH + 10 * s;
    {
        const bool bad = std::ranges::any_of(tools_, [](const auto& t) { return !t.ok(); });
        welcomeAction({lx, by, cw0, rowH}, "Settings", bad ? "tools missing" : "Ctrl+,", kDlgSettings, 0xF08BB, false);
    }

    // Recent projects.
    const float rx = r.x + sideW + 40 * s, rw = r.right() - rx - 30 * s;
    std::vector<std::string> recent;
    std::error_code ec;
    for (const auto& p : state_.recent) if (fs::is_directory(p, ec)) recent.push_back(p);
    dlgRecentShown_ = recent;
    uiLabel(rx, r.y + 36 * s, uiH() + 8 * s, "RECENT PROJECTS", theme::dim);
    if (!recent.empty()) uiLabelR(rx + rw, r.y + 36 * s, uiH() + 8 * s, std::to_string(recent.size()), theme::lineNum);
    const float top = r.y + 80 * s, rowH2 = std::round(54 * s), listH = r.bottom() - top - 24 * s;
    if (recent.empty()) {
        const float cy = r.y + r.h / 2 - 20 * s, cx = rx + rw / 2;
        const int ip = static_cast<int>(std::round(40 * s));
        Icons::glyph(dl_, font_, cx - static_cast<float>(ip) / 2, cy - static_cast<float>(ip), ip, 0xF0DCF, Color::rgb(0xffffff, 50));
        uiLabelC(cx, cy + 8 * s, uiH() + 10 * s, "No recent projects", theme::dim);
        uiLabelC(cx, cy + 8 * s + uiH() + 10 * s, uiH() + 10 * s, "Create a project or open a folder to get started", theme::lineNum);
        return;
    }
    dlgRecentMax_ = static_cast<float>(recent.size()) * rowH2 - listH;
    dlg_.recentScroll = std::clamp(dlg_.recentScroll, 0.0f, std::max(0.0f, dlgRecentMax_));
    const std::string home = homeDir();
    for (std::size_t i = 0; i < recent.size() && i < 99; ++i) {
        const float y = top + static_cast<float>(i) * rowH2 - dlg_.recentScroll;
        if (y + rowH2 < top || y > top + listH) continue;
        const Rect row{rx - 12 * s, y, rw + 24 * s, rowH2 - 4 * s};
        const bool hot = dlgHot(kDlgRecent + static_cast<int>(i)) || dlgHot(kDlgRecentDel + static_cast<int>(i));
        const bool inside = y >= top - 1 && y + rowH2 <= top + listH + 1;   // partly visible rows get no hit area
        if (inside) dlgHit(row, kDlgRecent + static_cast<int>(i));
        if (hot) fill(row, theme::sideSel, 8 * s);
        const fs::path p(recent[i]);
        const int ip = static_cast<int>(std::round(iconSize() * 1.4f));
        Icons::folder(dl_, font_, rx, y + (rowH2 - 4 * s - static_cast<float>(ip)) / 2, ip, p.filename().string(), false);
        const float tx = rx + static_cast<float>(ip) + 14 * s;
        std::string where = p.parent_path().string();
        if (!home.empty() && where.starts_with(home)) where = "~" + where.substr(home.size());
        uiLabel(tx, y + 6 * s, uiH() + 8 * s, p.filename().string(), theme::text, rx + rw - 40 * s);
        uiLabel(tx, y + 6 * s + uiH() + 6 * s, uiH() + 6 * s, where, theme::dim, rx + rw - 40 * s);
        if (hot && inside) {
            const float bs = std::round(24 * s);
            const Rect del{rx + rw - bs, y + (rowH2 - 4 * s - bs) / 2, bs, bs};
            dlgHit(del, kDlgRecentDel + static_cast<int>(i));
            if (dlgHot(kDlgRecentDel + static_cast<int>(i))) fill(del, Color::rgb(0xffffff, 40), 6 * s);
            const int cs = std::max(10, static_cast<int>(std::round(uiH() * 0.8f)));
            Icons::tool(dl_, font_, del.x + (bs - static_cast<float>(cs)) / 2, del.y + (bs - static_cast<float>(cs)) / 2, cs, ToolIcon::Close, theme::dim);
        }
    }
}

void App::dlgCheckbox(const Rect& r, bool on, std::string_view label, int id) {
    const float s = S();
    dlgHit(r, id);
    const float bs = std::round(20 * s);
    const Rect box{r.x, r.y + (r.h - bs) / 2, bs, bs};
    fill(box, on ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 70 : 44), 6 * s);
    if (!on) fill(box.x + 1, box.y + 1, bs - 2, bs - 2, Color::rgb(0x07080a), 5.5f * s);
    else {
        const int ip = std::max(12, static_cast<int>(std::round(bs * 0.85f)));
        Icons::glyph(dl_, font_, box.x + (bs - static_cast<float>(ip)) / 2, box.y + (bs - static_cast<float>(ip)) / 2, ip, 0xF012C, Color::rgb(0xffffff));
    }
    uiLabel(box.right() + 12 * s, r.y, r.h, label, theme::text);
}

void App::drawNewProject(const Rect& r) {
    const float s = S(), sideW = std::round(290 * s);
    dlgSide(r, sideW);
    const float lx = r.x + 32 * s;
    dl_.titleText(font_, lx, r.y + 30 * s, "New Project", theme::text);
    const auto& ts = projectTemplates();
    float y = r.y + 30 * s + font_.titleLineHeight() + 26 * s;
    const float rowH = std::round(48 * s);
    static constexpr std::array<char32_t, 5> icons = {0xF0672, 0xF0672, 0xF03D6, 0xF0671, 0xF0224};
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const Rect row{r.x + 14 * s, y, sideW - 28 * s, rowH - 4 * s};
        dlgHit(row, kDlgTmpl + static_cast<int>(i));
        const bool sel = static_cast<int>(i) == dlg_.tmpl, hot = dlgHot(kDlgTmpl + static_cast<int>(i));
        if (sel) fill(row, theme::sideSel, 8 * s);
        else if (hot) fill(row, theme::sideHover, 8 * s);
        const int ip = static_cast<int>(std::round(iconSize() * 1.25f));
        Icons::glyph(dl_, font_, row.x + 12 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, i < icons.size() ? icons[i] : 0xF0224, sel ? theme::accent.mix(Color::rgb(0xffffff), 0.3f) : theme::dim);
        uiLabel(row.x + 12 * s + static_cast<float>(ip) + 12 * s, row.y, row.h, ts[i].name, sel ? theme::text : Color::rgb(0xb9bdc6), row.right() - 8 * s);
        y += rowH;
    }

    const float fx = r.x + sideW + 40 * s, fw = r.right() - fx - 40 * s;
    const auto& t = ts[static_cast<std::size_t>(dlg_.tmpl)];
    float fy = r.y + 36 * s;
    uiLabel(fx, fy, uiH() + 8 * s, t.name, theme::text);
    fy += uiH() + 10 * s;
    uiLabel(fx, fy, uiH() + 6 * s, t.description, theme::dim, fx + fw);
    fy += uiH() + 34 * s;

    const float fh = std::round(38 * s);
    uiLabel(fx, fy, uiH() + 4 * s, "Name", theme::dim);
    fy += uiH() + 8 * s;
    dlgField({fx, fy, fw, fh}, dlg_.name, dlg_.focus == 0, kDlgName, "project-name");
    fy += fh + 22 * s;
    uiLabel(fx, fy, uiH() + 4 * s, "Location", theme::dim);
    fy += uiH() + 8 * s;
    const float browseW = std::round(110 * s);
    dlgField({fx, fy, fw - browseW - 10 * s, fh}, dlg_.location, dlg_.focus == 1, kDlgLoc, "/path/to/projects");
    dlgButton({fx + fw - browseW, fy, browseW, fh}, "Browse...", kDlgBrowseBtn, false);
    fy += fh + 22 * s;
    uiLabel(fx, fy, uiH() + 4 * s, "Language standard", theme::dim);
    fy += uiH() + 8 * s;
    const std::vector<std::string> opts = t.isC ? std::vector<std::string>{"C11", "C17"}
                                                : (t.needsModules ? std::vector<std::string>{"C++20", "C++23"} : std::vector<std::string>{"C++17", "C++20", "C++23"});
    // Modules need at least C++20: indices shift for that template.
    const int shift = (!t.isC && t.needsModules) ? 1 : 0;
    float sx = fx;
    for (std::size_t i = 0; i < opts.size(); ++i) {
        const float w = uiW(opts[i]) + 34 * s;
        const int id = kDlgStd + static_cast<int>(i) + shift;
        const Rect b{sx, fy, w, std::round(34 * s)};
        dlgHit(b, id);
        const bool on = dlg_.std_ == static_cast<int>(i) + shift;
        fill(b, on ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 56 : 34), 8 * s);
        if (!on) fill(b.x + 1, b.y + 1, b.w - 2, b.h - 2, Color::rgb(0x111317), 7.5f * s);
        uiLabelC(b.x + b.w / 2, b.y, b.h, opts[i], on ? Color::rgb(0xffffff) : theme::text);
        sx += w + 8 * s;
    }
    fy += std::round(34 * s) + 26 * s;
    dlgCheckbox({fx, fy, fw, std::round(28 * s)}, dlg_.git, "Create a git repository", kDlgGit);
    fy += std::round(28 * s) + 20 * s;
    const std::string where = (fs::path(expandPath(dlg_.location.text)) / dlg_.name.text).string();
    uiLabel(fx, fy, uiH() + 6 * s, "Will be created in  " + where, theme::lineNum, fx + fw);
    const std::string verr = validateProjectName(dlg_.name.text);
    const std::string& err = dlg_.error.empty() ? verr : dlg_.error;
    if (!err.empty() && (!dlg_.error.empty() || !dlg_.name.text.empty()))
        uiLabel(fx, fy + uiH() + 10 * s, uiH() + 6 * s, err, Color::rgb(0xf2777a), fx + fw);

    const float bh = std::round(40 * s), bw = std::round(120 * s);
    dlgButton({r.right() - 40 * s - bw, r.bottom() - 30 * s - bh, bw, bh}, "Create", kDlgCreate, true);
    dlgButton({r.right() - 40 * s - 2 * bw - 12 * s, r.bottom() - 30 * s - bh, bw, bh}, "Back", kDlgCancel, false);
}

void App::drawBrowse(const Rect& r) {
    const float s = S(), sideW = std::round(210 * s);
    dlgSide(r, sideW);
    const float lx = r.x + 28 * s;
    dl_.titleText(font_, lx, r.y + 30 * s, dlg_.forLocation ? "Choose Location" : "Open Project", theme::text);
    // Places.
    dlgPlaces_.clear();
    std::vector<std::pair<std::string, std::string>> places;
    const fs::path home = homeDir();
    std::error_code ec;
    places.emplace_back("Home", home.string());
    for (const char* n : {"Projects", "CLionProjects", "Documents", "Desktop", "Downloads"})
        if (fs::is_directory(home / n, ec)) places.emplace_back(n, (home / n).string());
    places.emplace_back("Computer", "/");
    float y = r.y + 30 * s + font_.titleLineHeight() + 24 * s;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const Rect row{r.x + 12 * s, y, sideW - 24 * s, std::round(34 * s)};
        const int id = kDlgBm + static_cast<int>(i);
        dlgHit(row, id);
        dlgPlaces_.push_back(places[i].second);
        const bool here = dlg_.dir == places[i].second;
        if (here) fill(row, theme::accent.alpha(60), 8 * s);
        else if (dlgHot(id)) fill(row, Color::rgb(0xffffff, 16), 8 * s);
        const int ip = std::max(14, static_cast<int>(std::round(iconSize() * 1.05f)));
        Icons::glyph(dl_, font_, row.x + 10 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, i == 0 ? 0xF02DC : 0xF0256, theme::dim);
        uiLabel(row.x + 10 * s + static_cast<float>(ip) + 10 * s, row.y, row.h, places[i].first, theme::text, row.right() - 6 * s);
        y += row.h + 2 * s;
    }

    const float cx = r.x + sideW + 28 * s, cw_ = r.right() - cx - 32 * s;
    const float fh = std::round(38 * s);
    float fy = r.y + 30 * s;
    const float upW = std::round(44 * s), hidW = std::round(150 * s);
    dlgButton({cx, fy, upW, fh}, "", kDlgUp, false);
    {
        const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.15f)));
        Icons::glyph(dl_, font_, cx + (upW - static_cast<float>(ip)) / 2, fy + (fh - static_cast<float>(ip)) / 2, ip, 0xF005D, theme::text);
    }
    dlgField({cx + upW + 10 * s, fy, cw_ - upW - hidW - 20 * s, fh}, dlg_.path, dlg_.pathFocus, kDlgPath, "/path/to/folder");
    dlgCheckbox({r.right() - 32 * s - hidW + 6 * s, fy, hidW, fh}, dlg_.showHidden, "Hidden", kDlgHidden);
    fy += fh + 18 * s;
    const float listTop = fy, listBottom = r.bottom() - 30 * s - std::round(40 * s) - 20 * s;
    browseListH_ = listBottom - listTop;
    fill(cx, listTop, cw_, browseListH_, Color::rgb(0x07080a), 10 * s);
    const float rowH = std::round(34 * s);
    dlg_.scroll = std::clamp(dlg_.scroll, 0.0f, std::max(0.0f, static_cast<float>(dlg_.entries.size()) * rowH - browseListH_));
    if (dlg_.entries.empty()) uiLabel(cx + 16 * s, listTop + 10 * s, rowH, "No sub-folders here", theme::lineNum);
    for (std::size_t i = 0; i < dlg_.entries.size() && i < 399; ++i) {
        const float ry = listTop + 4 * s + static_cast<float>(i) * rowH - dlg_.scroll;
        if (ry + rowH < listTop || ry > listTop + browseListH_) continue;
        const bool full = ry >= listTop && ry + rowH <= listTop + browseListH_;
        const Rect row{cx + 6 * s, ry, cw_ - 12 * s, rowH - 2 * s};
        const int id = kDlgEntry + static_cast<int>(i);
        if (full) dlgHit(row, id);
        const bool sel = dlg_.sel == static_cast<int>(i);
        if (sel) fill(row, theme::accent.alpha(80), 7 * s);
        else if (dlgHot(id)) fill(row, Color::rgb(0xffffff, 16), 7 * s);
        const BrowseEntry& e = dlg_.entries[i];
        drawFolderIcon(row.x + 10 * s, row.y + (row.h - iconSize()) / 2, e.name, false);
        const float nx = uiLabel(row.x + 10 * s + iconSize() + 10 * s, row.y, row.h, e.name, theme::text, row.right() - 90 * s);
        if (e.cmake) {
            const float w = uiW("CMake") + 16 * s;
            const Rect badge{std::min(nx + 12 * s, row.right() - w - 8 * s), row.y + 5 * s, w, row.h - 10 * s};
            fill(badge, theme::accent.alpha(50), 6 * s);
            uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, "CMake", theme::text);
        }
    }
    const float bh = std::round(40 * s), bw = std::round(120 * s);
    const std::string target = dlg_.sel >= 0 && dlg_.sel < static_cast<int>(dlg_.entries.size())
                                   ? (fs::path(dlg_.dir) / dlg_.entries[static_cast<std::size_t>(dlg_.sel)].name).string() : dlg_.dir;
    uiLabel(cx, r.bottom() - 30 * s - bh, bh, target, theme::dim, r.right() - 40 * s - 2 * bw - 40 * s);
    dlgButton({r.right() - 32 * s - bw, r.bottom() - 30 * s - bh, bw, bh}, dlg_.forLocation ? "Select" : "Open", kDlgOk, true);
    dlgButton({r.right() - 32 * s - 2 * bw - 12 * s, r.bottom() - 30 * s - bh, bw, bh}, "Cancel", kDlgBrowseCancel, false);
}

}  // namespace ide
