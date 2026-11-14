// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / documents - part of the App implementation (see app.cppm for the class).
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

void App::newDoc() {
    docs_.push_back(std::make_unique<Document>());
    cur_ = static_cast<int>(docs_.size()) - 1;
    dirty_ = true;
}

std::string App::expandPath(std::string p) {
    if (p.starts_with("~/") || p == "~") {
        if (const char* home = std::getenv("HOME")) p = std::string(home) + p.substr(1);
    }
    return p;
}

void App::openFile(const std::string& raw) {
    std::string path = expandPath(raw);
    std::error_code ec;
    if (fs::is_directory(path, ec)) { openFolder(path); return; }
    std::string abs = fs::absolute(path, ec).lexically_normal().string();
    if (abs.ends_with(".ipynb")) { openNotebook(abs); return; }
    for (std::size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i]->path == abs) { switchTab(static_cast<int>(i)); return; }

    auto d = std::make_unique<Document>();
    if (fs::exists(path, ec)) {
        if (auto r = d->load(path); !r) { setMessage(r.error(), true); return; }
    } else {
        d->setPath(path);
        setMessage("New file: " + d->name);
    }
    // Replace a pristine untitled tab instead of stacking up empty ones.
    if (docs_.size() == 1 && docs_[0]->path.empty() && !docs_[0]->modified() && docs_[0]->buf.lineCount() == 1 &&
        docs_[0]->buf.line(0).empty())
        docs_.clear();
    docs_.push_back(std::move(d));
    cur_ = static_cast<int>(docs_.size()) - 1;
    revealCurrent();
    ensureLsp(*docs_.back());
    dirty_ = true;
}

void App::syncFsWatches() {
    std::set<std::string> dirs;
    if (explorer_.isOpen()) for (const auto& d : explorer_.loadedDirs()) dirs.insert(d.string());
    for (const auto& d : docs_) if (!d->path.empty()) dirs.insert(fs::path(d->path).parent_path().string());
    fsw_.sync(dirs);
}

void App::handleFsChanges() {
    if (explorer_.isOpen()) explorer_.refresh();
    checkDocsOnDisk();
    gitNextPoll_ = {};
    if (sideSearch_ && !sQuery_.text.empty()) scheduleSearch();   // results may be stale
    syncFsWatches();
    dirty_ = true;
}

void App::checkDocsOnDisk() {
    for (auto& dp : docs_) {
        Document& d = *dp;
        if (d.path.empty() || isNotebook(d)) continue;
        std::error_code ec;
        if (!fs::exists(d.path, ec)) {
            if (!d.diskGone) { d.diskGone = true; setMessage(d.name + " was deleted on disk", true, 4); }
            continue;
        }
        d.diskGone = false;
        const auto t = fs::last_write_time(d.path, ec);
        if (ec || t == d.diskTime) continue;
        if (d.modified()) {   // keep the user's edits; just tell them once per change
            d.diskTime = t;
            setMessage(d.name + " changed on disk - you have unsaved edits", true, 5);
            continue;
        }
        const Pos c = d.cursor, a = d.anchor;
        const float sy = d.scrollY, ty = d.targetY;
        if (auto r = d.load(d.path); !r) { d.diskTime = t; continue; }
        d.cursor = d.buf.clamp(c);
        d.anchor = d.buf.clamp(a);
        d.scrollY = sy; d.targetY = ty;
        setMessage("Reloaded " + d.name + " (changed on disk)", false, 2.5f);
    }
}

void App::openFolder(const std::string& raw) {
    std::string err;
    saveSession(true);   // the project being left keeps its tabs
    if (!explorer_.open(expandPath(raw), err)) { setMessage(err, true); return; }
    sidebar_ = true;
    sideScroll_ = sideTarget_ = 0;
    sideFocus_ = false;
    revealCurrent();
    detectCMake();
    setMessage("Opened folder " + explorer_.rootPath().string());
    addRecent(state_, explorer_.rootPath().string());
    saveState(state_);
    gitReset();
    dbgLoad();
    if (restoreSession()) setMessage("Restored session of " + explorer_.rootPath().filename().string());
    dirty_ = true;
}

void App::toggleSidebar() {
    sidebar_ = !sidebar_;
    if (!sidebar_) sideFocus_ = false;
    else if (!explorer_.isOpen()) setMessage("No folder open - press Ctrl+Shift+O to open one", false, 3);
    else revealCurrent();
    dirty_ = true;
}

void App::focusSidebar() {
    if (!sidebar_) { sidebar_ = true; }
    if (!explorer_.isOpen()) { browseOpen(false); return; }
    sideFocus_ = true;
    if (explorer_.indexOfSelected() < 0 && !explorer_.rows().empty()) explorer_.select(explorer_.rows().front().node->path.string());
    sideReveal_ = true;
    dirty_ = true;
}

void App::switchTab(int i) {
    if (docs_.empty()) return;
    int n = static_cast<int>(docs_.size());
    cur_ = ((i % n) + n) % n;
    revealCurrent();
    resetCaret();
    scrollToCursor_ = true;
    dirty_ = true;
}

void App::closeTab(int i, bool force) {
    if (i < 0 || i >= static_cast<int>(docs_.size())) return;
    if (!force && docs_[static_cast<std::size_t>(i)]->modified()) {
        if (closeArmed_ != i) {
            closeArmed_ = i;
            setMessage("'" + docs_[static_cast<std::size_t>(i)]->name + "' has unsaved changes - close again to discard", true);
            return;
        }
    }
    closeArmed_ = -1;
    // Keep focus in the same pane: after closing the current tab, move to its neighbour there.
    Document* keep = nullptr;
    if (i == cur_) {
        Document* closing = docs_[static_cast<std::size_t>(i)].get();
        if (Pane* pp = paneOfDoc(closing); pp && pp->tabs.size() > 1) {
            const auto at = static_cast<std::size_t>(std::ranges::find(pp->tabs, closing) - pp->tabs.begin());
            keep = at + 1 < pp->tabs.size() ? pp->tabs[at + 1] : pp->tabs[at - 1];
        }
    }
    Document* closingDoc = docs_[static_cast<std::size_t>(i)].get();
    std::vector<Document*> dependents;   // previews of the document being closed go with it
    for (const auto& [pd, v] : previews_) if (v.src == closingDoc) dependents.push_back(const_cast<Document*>(pd));
    previews_.erase(closingDoc);
    notebooks_.erase(closingDoc);
    lspDetach(*docs_[static_cast<std::size_t>(i)]);
    closeCompletion();
    docs_.erase(docs_.begin() + i);
    if (docs_.empty()) newDoc();
    cur_ = std::clamp(cur_ > i ? cur_ - 1 : cur_, 0, static_cast<int>(docs_.size()) - 1);
    if (keep) if (int k = docIndex(keep); k >= 0) cur_ = k;
    for (Document* dep : dependents) if (int k = docIndex(dep); k >= 0) closeTab(k, true);
    if (previews_.empty()) releasePictures();
    scrollToCursor_ = true;
    dirty_ = true;
}

void App::saveCurrent() {
    Document& d = doc();
    if (isNotebook(d)) { saveNotebook(d); return; }
    flushDoc(d);
    if (d.path.empty()) { openPrompt(PromptKind::SaveAs); return; }
    if (auto r = d.save(); r) {
        setMessage("Saved " + d.name);
        gitNextPoll_ = {};   // refresh the change markers / status right away
        if (lsp_ && d.lspOpen) lsp_->didSave(d.path);
        if (isCppDoc(d) && !modules_.empty()) refreshModules();  // a module may have been renamed
        if (explorer_.isOpen()) explorer_.refresh();
        if (cmakeOpen_ && (d.name == "CMakeLists.txt" || d.name.ends_with(".cmake")) && !buildProc_) cmakeReload();
    } else setMessage(r.error(), true);
}

void App::requestQuit() {
    bool anyModified = std::ranges::any_of(docs_, [](const auto& d) { return d->modified(); });
    if (anyModified && !quitArmed_) {
        quitArmed_ = true;
        setMessage("Unsaved changes - press Ctrl+Q again to quit without saving", true);
        return;
    }
    running_ = false;
}

}  // namespace ide
