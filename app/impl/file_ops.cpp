// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / file_ops - part of the App implementation (see app.cppm for the class).
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

fs::path App::targetDir() const {
    const std::string& sel = explorer_.selected();
    if (sel.empty()) return explorer_.rootPath();
    std::error_code ec;
    fs::path p(sel);
    return fs::is_directory(p, ec) ? p : p.parent_path();
}

void App::drawSidePrompt() {
    if (L_.side.w <= 0 || prompt_.fields.empty()) return;
    const float s = S(), rh = L_.rowH, hair = std::max(1.0f, std::round(0.8f * s));
    const Rect card{L_.side.x + 8 * s, L_.side.bottom() - sideCardH() + 4 * s, L_.side.w - 16 * s, sideCardH() - 12 * s};
    fill(card, theme::accent.alpha(90), 10 * s);
    fill(card.x + hair, card.y + hair, card.w - 2 * hair, card.h - 2 * hair, Color::rgb(0x0d0e11), 10 * s - hair);
    const Field& f = prompt_.fields[0];
    const float pad = 12 * s;
    if (prompt_.kind == PromptKind::Delete) {
        uiLabel(card.x + pad, card.y + 4 * s, rh, f.label, theme::text, card.right() - pad);
        uiLabel(card.x + pad, card.y + 4 * s + rh, rh, "Enter - confirm    Esc - cancel", theme::dim, card.right() - pad);
        return;
    }
    uiLabel(card.x + pad, card.y + 4 * s, rh, f.label, theme::dim, card.right() - pad);
    const Rect in{card.x + 8 * s, card.y + 4 * s + rh, card.w - 16 * s, rh - 6 * s};
    fill(in, Color::rgb(0xffffff, 18), 6 * s);
    const float x = in.x + 8 * s, maxX = in.right() - 8 * s;
    std::size_t startByte = 0;
    while (startByte < f.cur && uiW(std::string_view(f.text).substr(startByte, f.cur - startByte)) > maxX - x - 4 * s)
        startByte = utf8Next(f.text, startByte);
    const std::string_view shown = std::string_view(f.text).substr(startByte);
    if (f.fresh)   // prefilled name: selected until the first edit
        fill(x - 2 * s, in.y + 3 * s, std::min(uiW(shown), maxX - x) + 4 * s, in.h - 6 * s, theme::selection, 3 * s);
    uiLabel(x, in.y, in.h, shown, theme::text, maxX);
    if (caretOn_ && focused_) {
        const float cx = x + uiW(std::string_view(f.text).substr(startByte, f.cur - startByte));
        fill(std::round(cx), in.y + 5 * s, std::max(2.0f, std::round(1.4f * s)), in.h - 10 * s, theme::caret, s);
    }
}

fs::path App::fileDropTarget() {
    if (!fdrag_.active || !L_.side.contains(mx_, my_) || !explorer_.isOpen()) return {};
    fs::path dir = explorer_.rootPath();
    if (L_.sideList.contains(mx_, my_)) {
        const auto& rows = explorer_.rows();
        const int row = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
        if (row >= 0 && row < static_cast<int>(rows.size())) {
            const FileNode& n = *rows[static_cast<std::size_t>(row)].node;
            dir = n.dir ? n.path : n.path.parent_path();
        }
    } else return {};
    if (dir == fdrag_.path.parent_path()) return {};
    if (dir == fdrag_.path || fsops::isInside(fdrag_.path, dir)) return {};
    return dir;
}

void App::finishFileDrag() {
    const fs::path src = fdrag_.path, dir = fdrag_.target;
    fdrag_ = {};
    dirty_ = true;
    if (dir.empty()) return;
    auto r = fsops::moveInto(src, dir);
    if (!r) { setMessage(r.error(), true); return; }
    retargetDocs(src, *r);
    afterFsChange(*r);
    setMessage("Moved " + src.filename().string() + " to " + relToRoot(dir) + "/");
}

void App::drawFileDrag() {
    if (!fdrag_.active) return;
    const float s = S();
    const std::string name = fdrag_.path.filename().string();
    const float h = std::round(uiH() + 12 * s), w = uiW(name) + iconSize() + 36 * s;
    const Rect r{mx_ + 12 * s, my_ + 8 * s, w, h};
    glassPanel(r, 9 * s);
    std::error_code ec;
    if (fs::is_directory(fdrag_.path, ec)) drawFolderIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name, false);
    else drawFileIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name);
    uiLabel(r.x + 12 * s + iconSize() + 8 * s, r.y, h, name, theme::text, r.right() - 10 * s);
}

void App::openFileOp(PromptKind kind, const fs::path& target, std::string label, std::string text, bool fresh) {
    Prompt p;
    p.kind = kind;
    Field f;
    f.label = std::move(label);
    f.text = std::move(text);
    f.cur = f.text.size();
    f.fresh = fresh && !f.text.empty();
    p.fields.push_back(std::move(f));
    prompt_ = std::move(p);
    fileOpPath_ = target;
    overwriteArmed_.clear();
    const bool dbgPrompt = kind == PromptKind::DbgWatch || kind == PromptKind::DbgSetValue || kind == PromptKind::DbgBpEdit || kind == PromptKind::DbgFuncBp;
    if (dbgPrompt) { sidebar_ = true; }
    else if (!sidebar_ || sideGit_ || sideSearch_ || sideDebug_) { sidebar_ = true; sideGit_ = false; sideSearch_ = false; sideDebug_ = false; sideDebug_ = false; }
    sideFocus_ = false;
    dirty_ = true;
}

void App::beginDelete(const fs::path& p, bool forever) {
    if (!isItem(p)) return;
    deleteForever_ = forever;
    openFileOp(PromptKind::Delete, p, std::string(forever ? "Permanently delete '" : "Move to Trash '") + p.filename().string() + "'? Enter = yes, Esc = no");
}

void App::stageClipboard(const fs::path& p, bool cut) {
    if (!isItem(p)) return;
    fileClip_ = {{p}, cut};
    setMessage(std::string(cut ? "Cut " : "Copied ") + p.filename().string(), false, 2);
}

void App::afterFsChange(const fs::path& focus) {
    explorer_.refresh();
    if (!focus.empty()) explorer_.reveal(focus);
    sideReveal_ = true;
    dirty_ = true;
}

void App::retargetDocs(const fs::path& from, const fs::path& to) {
    for (auto& d : docs_) {
        if (d->path.empty()) continue;
        fs::path dp(d->path);
        if (dp == from) d->setPath(to.string());
        else if (fsops::isInside(from, dp)) d->setPath((to / dp.lexically_relative(from)).string());
    }
}

void App::pasteFiles(const fs::path& dir) {
    if (fileClip_.paths.empty()) { setMessage("Nothing to paste", true); return; }
    fs::path last;
    int done = 0;
    for (const auto& src : fileClip_.paths) {
        std::error_code ec;
        if (!fs::exists(src, ec)) continue;
        auto r = fileClip_.cut ? fsops::moveInto(src, dir) : fsops::copyInto(src, dir);
        if (!r) { setMessage(r.error(), true); break; }
        if (fileClip_.cut && *r != src) retargetDocs(src, *r);
        last = *r;
        ++done;
    }
    if (fileClip_.cut && done > 0) fileClip_ = {};
    afterFsChange(last);
    if (done > 0) setMessage(std::format("Pasted {} item{}", done, done == 1 ? "" : "s"));
}

void App::duplicateItem(const fs::path& p) {
    if (!isItem(p)) return;
    auto r = fsops::copyInto(p, p.parent_path());
    if (!r) { setMessage(r.error(), true); return; }
    afterFsChange(*r);
    setMessage("Created " + r->filename().string());
}

void App::finishFileOp(const std::string& text) {
    const PromptKind kind = prompt_.kind;
    const fs::path target = fileOpPath_;
    closePrompt();
    switch (kind) {
        case PromptKind::NewFile: {
            auto r = fsops::createFile(target, text);
            if (!r) { setMessage(r.error(), true); break; }
            afterFsChange(*r);
            openFile(r->string());
            sideFocus_ = false;
            break;
        }
        case PromptKind::NewFolder: {
            auto r = fsops::createFolder(target, text);
            if (!r) { setMessage(r.error(), true); break; }
            afterFsChange(*r);
            setMessage("Created folder " + r->filename().string());
            break;
        }
        case PromptKind::Rename: {
            auto r = fsops::rename(target, text);
            if (!r) { setMessage(r.error(), true); break; }
            retargetDocs(target, *r);
            afterFsChange(*r);
            setMessage("Renamed to " + r->filename().string());
            break;
        }
        case PromptKind::Delete: {
            auto r = deleteForever_ ? fsops::removeForever(target) : fsops::trash(target);
            if (!r) { setMessage(r.error(), true); break; }
            explorer_.select(target.parent_path().string());
            afterFsChange({});
            setMessage((deleteForever_ ? "Deleted " : "Moved to Trash: ") + target.filename().string());
            break;
        }
        default: break;
    }
}

void App::openMenu(float x, float y, const std::string* path) {
    menu_ = {};
    menu_.open = true;
    menu_.target = path ? *path : std::string();
    std::error_code ec;
    const bool has = path != nullptr, isDir = has && fs::is_directory(*path, ec);
    const bool canPaste = !fileClip_.paths.empty();
    auto add = [&](std::string l, std::string h, Act a, bool en = true) { menu_.items.push_back({std::move(l), std::move(h), a, en}); };
    auto sep = [&] { add("", "", Act::Sep); };
    if (has) {
        if (!isDir) { add("Open", "Enter", Act::Open); sep(); }
        else { add("New File...", "Ctrl+N", Act::NewFile); add("New Folder...", "Ctrl+Shift+N", Act::NewFolder); sep(); }
        add("Cut", "Ctrl+X", Act::Cut);
        add("Copy", "Ctrl+C", Act::Copy);
        add("Paste", "Ctrl+V", Act::Paste, canPaste);
        add("Duplicate", "Ctrl+D", Act::Duplicate);
        sep();
        add("Rename...", "F2", Act::Rename);
        add("Move to Trash", "Del", Act::Trash);
        add("Delete Permanently", "Shift+Del", Act::DeleteForever);
        sep();
        if (isDir) add("Open in New Window", "", Act::NewWindow);
        add("Copy Path", "", Act::CopyPath);
        add("Copy Relative Path", "", Act::CopyRelPath);
        sep();
        add("Refresh", "F5", Act::Refresh);
    } else {
        add("New File...", "Ctrl+N", Act::NewFile);
        add("New Folder...", "Ctrl+Shift+N", Act::NewFolder);
        sep();
        add("Paste", "Ctrl+V", Act::Paste, canPaste);
        sep();
        add("Refresh", "F5", Act::Refresh);
    }
    layoutMenu(x, y);
}

void App::layoutMenu(float x, float y) {
    const float s = S();
    menu_.itemH = std::round(uiH() + 12 * s);
    menu_.sepH = std::round(11 * s);
    float maxLabel = 0, maxHint = 0, h = 12 * s;
    for (const auto& it : menu_.items) {
        maxLabel = std::max(maxLabel, uiW(it.label));
        maxHint = std::max(maxHint, uiW(it.hint));
        h += it.act == Act::Sep ? menu_.sepH : menu_.itemH;
    }
    const float w = maxLabel + (maxHint > 0 ? maxHint + 36 * s : 0) + 44 * s;
    const float W = L_.status.w, H = L_.status.bottom();
    menu_.rect = {std::clamp(x, 4 * s, std::max(4 * s, W - w - 4 * s)), std::clamp(y, 4 * s, std::max(4 * s, H - h - 4 * s)), w, h};
    dirty_ = true;
}

void App::openCustomMenu(float x, float y, const std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>>& entries) {
    menu_ = {};
    menu_.open = true;
    menuActions_.clear();
    for (const auto& [label, hint, checked, fn] : entries) {
        menu_.items.push_back({(checked ? std::string("\xE2\x9C\x93  ") : std::string("    ")) + label, hint, Act::Custom, true,
                               static_cast<int>(menuActions_.size())});
        menuActions_.push_back(fn);
    }
    layoutMenu(x, y);
}

int App::menuItemAt(float px, float py) const {
    if (!menu_.open || !menu_.rect.contains(px, py)) return -1;
    float y = menu_.rect.y + 6 * S();
    for (std::size_t i = 0; i < menu_.items.size(); ++i) {
        float h = menu_.items[i].act == Act::Sep ? menu_.sepH : menu_.itemH;
        if (py >= y && py < y + h) return menu_.items[i].act == Act::Sep ? -1 : static_cast<int>(i);
        y += h;
    }
    return -1;
}

void App::runAct(Act a, const std::string& target) {
    const fs::path t(target);
    std::error_code ec;
    const bool has = !target.empty();
    const fs::path dir = has ? (fs::is_directory(t, ec) ? t : t.parent_path()) : explorer_.rootPath();
    switch (a) {
        case Act::Open: openFile(target); sideFocus_ = false; break;
        case Act::NewFile: beginNewFile(dir); break;
        case Act::NewFolder: beginNewFolder(dir); break;
        case Act::Rename: beginRename(t); break;
        case Act::Trash: beginDelete(t, false); break;
        case Act::DeleteForever: beginDelete(t, true); break;
        case Act::Copy: stageClipboard(t, false); break;
        case Act::Cut: stageClipboard(t, true); break;
        case Act::Paste: pasteFiles(dir); break;
        case Act::Duplicate: duplicateItem(t); break;
        case Act::CopyPath: win_.setClipboard(target); setMessage("Copied path", false, 1.5f); break;
        case Act::CopyRelPath: win_.setClipboard(relToRoot(t)); setMessage("Copied relative path", false, 1.5f); break;
        case Act::Refresh: explorer_.refresh(); setMessage("Refreshed", false, 1.5f); break;
        case Act::NewWindow: openNewWindow(target); break;
        case Act::Sep: case Act::Custom: break;
    }
    dirty_ = true;
}

void App::activateMenuItem(int i) {
    if (i < 0 || i >= static_cast<int>(menu_.items.size()) || !menu_.items[static_cast<std::size_t>(i)].enabled) return;
    const MenuItem item = menu_.items[static_cast<std::size_t>(i)];
    std::string target = menu_.target;
    closeMenu();
    if (item.act == Act::Custom) {
        if (item.arg >= 0 && item.arg < static_cast<int>(menuActions_.size())) { auto fn = menuActions_[static_cast<std::size_t>(item.arg)]; fn(); }
        return;
    }
    runAct(item.act, target);
}

void App::menuKey(const KeyEvent& k) {
    auto step = [&](int dir) {
        const int n = static_cast<int>(menu_.items.size());
        int i = menu_.hover;
        for (int tries = 0; tries < n; ++tries) {
            i = ((i + dir) % n + n) % n;
            if (menu_.items[static_cast<std::size_t>(i)].act != Act::Sep && menu_.items[static_cast<std::size_t>(i)].enabled) { menu_.hover = i; return; }
        }
    };
    switch (k.key) {
        case Key::Down: if (menu_.hover < 0) menu_.hover = -1; step(1); break;
        case Key::Up: if (menu_.hover < 0) menu_.hover = 0; step(-1); break;
        case Key::Enter: activateMenuItem(menu_.hover); break;
        case Key::Escape: closeMenu(); break;
        default: break;
    }
}

void App::drawMenu() {
    if (!menu_.open) return;
    const Rect& r = menu_.rect;
    const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
    glassPanel(r, 11 * s);
    float y = r.y + 6 * s;
    for (std::size_t i = 0; i < menu_.items.size(); ++i) {
        const MenuItem& it = menu_.items[i];
        if (it.act == Act::Sep) {
            fill(r.x + 10 * s, y + std::round(menu_.sepH / 2), r.w - 20 * s, hair, theme::border);
            y += menu_.sepH;
            continue;
        }
        if (static_cast<int>(i) == menu_.hover && it.enabled) fill(r.x + 5 * s, y, r.w - 10 * s, menu_.itemH, theme::selection, 6 * s);
        const Color tc = it.enabled ? (it.act == Act::DeleteForever || it.act == Act::Trash ? Color::rgb(0xe8a09f) : theme::text) : Color::rgb(0x5d606a);
        uiLabel(r.x + 16 * s, y, menu_.itemH, it.label, tc);
        if (!it.hint.empty()) uiLabelR(r.right() - 16 * s, y, menu_.itemH, it.hint, Color::rgb(0x7a7e88));
        y += menu_.itemH;
    }
}

bool App::explorerKey(const KeyEvent& k) {
    if (!explorer_.isOpen() || k.key >= Key::F6) return false;
    const fs::path sel = explorer_.selected().empty() ? fs::path() : fs::path(explorer_.selected());
    if (k.ctrl && !k.alt && k.key == Key::Char) {
        switch (k.ch) {
            case 'c': stageClipboard(sel, false); return true;
            case 'x': stageClipboard(sel, true); return true;
            case 'v': pasteFiles(targetDir()); return true;
            case 'd': duplicateItem(sel); return true;
            case 'n': if (k.shift) beginNewFolder(targetDir()); else beginNewFile(targetDir()); return true;
            default: return false;
        }
    }
    if (k.ctrl || k.alt) return false;
    const auto& rows = explorer_.rows();
    const int n = static_cast<int>(rows.size());
    int i = explorer_.indexOfSelected();
    auto go = [&](int to) {
        if (n == 0) return;
        to = std::clamp(to, 0, n - 1);
        explorer_.select(rows[static_cast<std::size_t>(to)].node->path.string());
        sideReveal_ = true;
    };
    switch (k.key) {
        case Key::Up: go(i < 0 ? 0 : i - 1); break;
        case Key::Down: go(i + 1); break;
        case Key::Home: go(0); break;
        case Key::End: go(n - 1); break;
        case Key::PageUp: go(i - 10); break;
        case Key::PageDown: go(i + 10); break;
        case Key::Right:
            if (i >= 0 && rows[static_cast<std::size_t>(i)].node->dir) {
                FileNode* nd = rows[static_cast<std::size_t>(i)].node;
                if (!nd->expanded) explorer_.setExpanded(*nd, true); else go(i + 1);
            }
            break;
        case Key::Left:
            if (i >= 0) {
                FileNode* nd = rows[static_cast<std::size_t>(i)].node;
                if (nd->dir && nd->expanded) explorer_.setExpanded(*nd, false);
                else if (FileNode* parent = explorer_.parentOf(*nd)) { explorer_.select(parent->path.string()); sideReveal_ = true; }
            }
            break;
        case Key::Enter:
            if (i >= 0) activateRow(*rows[static_cast<std::size_t>(i)].node);
            break;
        case Key::F5: explorer_.refresh(); setMessage("Refreshed", false, 1.5f); break;
        case Key::F2: beginRename(sel); break;
        case Key::Delete: beginDelete(sel, k.shift); break;
        case Key::Escape: case Key::Tab: sideFocus_ = false; break;
        default: break;  // swallow everything else so typing does not leak into the editor
    }
    return true;
}

void App::activateRow(FileNode& node) {
    explorer_.select(node.path.string());
    if (node.dir) { explorer_.toggle(node); return; }
    openFile(node.path.string());
    sideFocus_ = false;
}

void App::editorKey(const KeyEvent& k) {
    if (comp_.open && completionKey(k)) { scrollToCursor_ = true; return; }
    hideHover();
    if (isPreview(doc()) && previewKey(k)) return;
    if (isNotebook(doc()) && notebookKey(k)) return;
    const Document* dp = &doc();
    const std::uint64_t verBefore = doc().buf.version();
    editorKeyImpl(k);
    if (docs_.empty() || &doc() != dp) { closeCompletion(); return; }
    afterEditorKey(k, verBefore);
}

void App::editorKeyImpl(const KeyEvent& k) {
    Document& d = doc();
    const bool shift = k.shift, ctrl = k.ctrl, alt = k.alt;
    scrollToCursor_ = true;

    if (k.key == Key::F12) { gotoDefinition(); scrollToCursor_ = false; return; }
    if (alt && !ctrl && k.key == Key::Char && k.ch == 'o') { switchHeaderSource(); scrollToCursor_ = false; return; }
    if (alt && !ctrl && k.key == Key::Left) { navBack(); return; }
    if (alt && !ctrl && k.key == Key::Right) { navForward(); return; }
    if (debugKey(k)) { scrollToCursor_ = false; return; }
    if (k.key == Key::F9 && ctrl) { cmakeBuild(false); scrollToCursor_ = false; return; }
    if (k.key == Key::F10 && shift) { cmakeBuild(true); scrollToCursor_ = false; return; }
    if (k.key == Key::F9 && shift) { cmakeBuild(true, true); scrollToCursor_ = false; return; }
    if (k.key == Key::F2 && ctrl) { cmakeStop(); scrollToCursor_ = false; return; }

    if (ctrl && alt && k.key == Key::Char && !shift) {   // Ctrl+Alt+N: a new window, Ctrl+Alt+O: open a folder in a new window
        if (k.ch == 'n') { openNewWindow(); scrollToCursor_ = false; return; }
        if (k.ch == 'o') { pickNewWindow_ = true; browseOpen(false); scrollToCursor_ = false; return; }
    }
    if (ctrl && !alt && k.key == Key::Char) {
        switch (k.ch) {
            case 'r': if (shift) cmakeReload(); else openPrompt(PromptKind::Replace); scrollToCursor_ = false; break;
            case '`': if (panelOpen_ && panelTab_ == 3 && termFocus_) togglePanel(); else openTerminal(); scrollToCursor_ = false; break;
            case 'm': if (shift) { panelOpen_ = true; panelTab_ = 2; dirty_ = true; scrollToCursor_ = false; } break;   // Problems
            case 's': if (shift) openPrompt(PromptKind::SaveAs); else saveCurrent(); break;
            case 'o': if (shift) browseOpen(false); else openPrompt(PromptKind::Open); break;
            case 'p': if (shift) welcomeOpen(); break;
            case 'b': toggleSidebar(); scrollToCursor_ = false; break;
            case 'e': if (shift) focusSidebar(); scrollToCursor_ = false; break;
            case 'n': if (shift) newProjectOpen(); else newDoc(); break;
            case ',': settingsOpen(); break;
            case 'w': closeTab(cur_); break;
            case 'q': requestQuit(); break;
            case 'f': if (shift) openSearch(false); else openPrompt(PromptKind::Find); scrollToCursor_ = false; break;
            case 'h': if (shift) openSearch(true); else openPrompt(PromptKind::Replace); scrollToCursor_ = false; break;
            case 'g': if (shift) { sidebar_ = true; sideGit_ = true; sideSearch_ = false; sideDebug_ = false; sideFocus_ = false; gitNextPoll_ = {}; dirty_ = true; scrollToCursor_ = false; } else openPrompt(PromptKind::Goto); break;
            case 'z': if (shift) d.redo(); else d.undo(); break;
            case 'y': d.redo(); break;
            case 'a': d.selectAll(); break;
            case 'c': copy(false); scrollToCursor_ = false; break;
            case 'x': copy(true); break;
            case 'v': if (shift) { togglePreview(); scrollToCursor_ = false; } else paste(); break;
            case 'l': d.selectLine(d.cursor.line); break;
            case '/': d.toggleComment(); break;
            case 'd': d.duplicateLines(); break;                    // Ctrl+D (Ctrl+Shift+D too)
            case 'j': d.joinLines(); break;
            case 'u': if (shift) d.toggleCase(); else scrollToCursor_ = false; break;
            case 't': if (shift) reopenClosedTab(); else scrollToCursor_ = false; break;
            case 'k': if (shift) d.deleteLines(); break;
            case ']': d.indentLines(false); break;
            case '[': d.indentLines(true); break;
            case '=': case '+': zoom(1); break;
            case '-': zoom(-1); break;
            case '0': zoom(0, true); break;
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':   // Ctrl+1..9: that tab (9: the last)
                if (!docs_.empty()) switchTab(k.ch == '9' ? static_cast<int>(docs_.size()) - 1 : std::min(static_cast<int>(k.ch - '1'), static_cast<int>(docs_.size()) - 1));
                scrollToCursor_ = false;
                break;
            default: scrollToCursor_ = false; break;
        }
        return;
    }

    switch (k.key) {
        case Key::Left: d.moveLeft(shift, ctrl); break;
        case Key::Right: d.moveRight(shift, ctrl); break;
        case Key::Up: if (alt && shift) d.copyLines(-1); else if (alt || (ctrl && shift)) d.moveLines(-1); else d.moveVertical(-1, shift); break;
        case Key::Down: if (alt && shift) d.copyLines(1); else if (alt || (ctrl && shift)) d.moveLines(1); else d.moveVertical(1, shift); break;
        case Key::Home: if (ctrl) d.docStart(shift); else d.home(shift); break;
        case Key::End: if (ctrl) d.docEnd(shift); else d.end(shift); break;
        case Key::PageUp:
            if (ctrl) { switchTab(cur_ - 1); break; }
            pageMove(d, -1, shift);
            break;
        case Key::PageDown:
            if (ctrl) { switchTab(cur_ + 1); break; }
            pageMove(d, 1, shift);
            break;
        case Key::Backspace: if (ctrl) d.deleteWordBack(); else d.backspace(); break;
        case Key::Delete: if (ctrl) d.deleteWordForward(); else d.deleteForward(); break;
        case Key::Enter: if (ctrl && shift) d.insertLineAbove(); else if (ctrl) d.insertLineBelow(); else d.newline(); break;   // Ctrl+Enter: a new line below, Ctrl+Shift+Enter: above
        case Key::Tab:
            if (ctrl) switchTab(cur_ + (shift ? -1 : 1));
            else if (shift) d.indentLines(true);
            else d.tab();
            break;
        case Key::Escape:
            if (d.hasSelection()) d.setCursor(d.cursor);
            break;
        case Key::F3:
            if (!findText_.empty()) {
                if (!d.find(findText_, !shift, matchCase_)) setMessage("No results for '" + findText_ + "'", true);
            }
            break;
        default:
            if (!k.text.empty() && !k.alt) d.insertText(k.text);
            else scrollToCursor_ = false;
            break;
    }
}

void App::pageMove(Document& d, int dir, bool extend) {
    int n = std::max(1, static_cast<int>(L_.editor.h / lh()) - 1);
    d.moveVertical(dir * n, extend);
    d.targetY += static_cast<float>(dir * n) * lh();
}

void App::copy(bool cut) {
    Document& d = doc();
    std::string t;
    if (d.hasSelection()) t = d.selectedText();
    else t = d.lineAt(d.cursor.line) + "\n";
    win_.setClipboard(t);
    if (cut) {
        if (d.hasSelection()) { d.buf.breakGroup(); d.deleteSelection(); }
        else d.deleteLines();
    }
}

}  // namespace ide
