// app_search_view - implementation of module daemon.app.
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

search::Options App::searchOptions() const {
    search::Options o;
    o.query = sQuery_.text; o.matchCase = sCase_; o.wholeWord = sWord_; o.regex = sRegex_;
    o.include = sInclude_.text; o.exclude = sExclude_.text;
    return o;
}

void App::openSearch(bool replace) {
    if (!explorer_.isOpen()) { setMessage("Open a folder to search in it", false, 2.5f); return; }
    sidebar_ = true; sideSearch_ = true; sideGit_ = false; sideDebug_ = false;
    sideFocus_ = false; gitMsgFocus_ = false;
    if (!docs_.empty() && doc().hasSelection()) {
        std::string t = doc().selectedText();
        if (t.find('\n') == std::string::npos && !t.empty() && t != sQuery_.text) { sQuery_ = {t, t.size()}; sPending_ = true; sRunAt_ = Clock::now(); }
    }
    sFocus_ = replace ? 1 : 0;
    sQuery_.cur = sQuery_.text.size();
    if (replace) sReplace_.cur = sReplace_.text.size();
    dirty_ = true;
}

void App::rebuildSearchRows() {
    sRows_.clear();
    for (int i = 0; i < static_cast<int>(sRes_.size()); ++i) {
        sRows_.push_back({i, -1});
        if (sRes_[static_cast<std::size_t>(i)].collapsed) continue;
        for (int m = 0; m < static_cast<int>(sRes_[static_cast<std::size_t>(i)].matches.size()); ++m) sRows_.push_back({i, m});
    }
    sRowsDirty_ = false;
}

void App::recountSearch() {
    sTotal_ = 0;
    for (const auto& f : sRes_) sTotal_ += static_cast<int>(f.matches.size());
    sRowsDirty_ = true;
}

void App::startSearch() {
    sPending_ = false;
    const int gen = ++*sGen_;
    sSearched_ = false;
    if (sQuery_.text.empty() || !explorer_.isOpen()) {
        sRes_.clear(); sTotal_ = 0; sError_.clear(); sBusy_ = false; sTrunc_ = false; sRowsDirty_ = true; sScroll_ = 0;
        dirty_ = true;
        return;
    }
    const search::Options o = searchOptions();
    if (search::Matcher m(o); !m.ok()) {   // typos in a regex are reported, not searched
        sRes_.clear(); sTotal_ = 0; sError_ = m.error(); sBusy_ = false; sTrunc_ = false; sRowsDirty_ = true;
        dirty_ = true;
        return;
    }
    std::map<std::string, std::string> overrides;   // unsaved edits are searched too
    for (const auto& d : docs_) if (!d->path.empty() && d->modified()) overrides[d->path] = d->buf.text();
    sBusy_ = true;
    sError_.clear();
    const fs::path root = explorer_.rootPath();
    auto g = sGen_;
    spawn([this, g, gen, root, o, overrides]() -> std::function<void()> {
        auto r = search::run(root, o, overrides, [g, gen] { return g->load() != gen; });
        return [this, g, gen, r = std::move(r)]() mutable {
            if (g->load() != gen) return;   // a newer search superseded this one
            sRes_ = std::move(r.files);
            sTotal_ = r.total; sTrunc_ = r.truncated; sError_ = r.error;
            sBusy_ = false; sSearched_ = true; sRowsDirty_ = true; sScroll_ = 0;
        };
    });
    dirty_ = true;
}

void App::searchOpenMatch(int fi, int mi) {
    if (fi < 0 || fi >= static_cast<int>(sRes_.size())) return;
    const auto& f = sRes_[static_cast<std::size_t>(fi)];
    openFile(f.path);
    if (docs_.empty() || doc().path != f.path) return;
    if (mi >= 0 && mi < static_cast<int>(f.matches.size())) {
        const auto& m = f.matches[static_cast<std::size_t>(mi)];
        doc().select({m.line, m.col}, {m.line, m.col + m.len});
        scrollToCursor_ = true;
    }
    sFocus_ = -1;
    sideFocus_ = false;
    dirty_ = true;
}

void App::searchReplace(int fi, int mi) {
    const search::Options o = searchOptions();
    const std::string with = sReplace_.text;
    int total = 0, files = 0, skipped = 0;
    for (int i = 0; i < static_cast<int>(sRes_.size()); ++i) {
        if (fi >= 0 && i != fi) continue;
        const auto& fr = sRes_[static_cast<std::size_t>(i)];
        const bool unsaved = std::ranges::any_of(docs_, [&](const auto& d) { return d->path == fr.path && d->modified(); });
        if (unsaved) { ++skipped; continue; }   // never overwrite the user's unsaved edits
        std::set<std::pair<int, int>> only;
        if (mi >= 0) { const auto& m = fr.matches[static_cast<std::size_t>(mi)]; only.insert({m.line, m.col}); }
        else for (const auto& m : fr.matches) only.insert({m.line, m.col});
        const int n = search::replaceInFile(fr.path, o, with, &only);
        if (n > 0) { total += n; ++files; } else if (n < 0) ++skipped;
    }
    checkDocsOnDisk();   // open, unmodified files reload right away
    explorer_.refresh();
    gitNextPoll_ = {};
    std::string msg = std::format("Replaced {} occurrence{} in {} file{}", total, total == 1 ? "" : "s", files, files == 1 ? "" : "s");
    if (skipped > 0) msg += std::format(" ({} skipped: unsaved changes or unreadable)", skipped);
    setMessage(msg, skipped > 0 && total == 0, 4);
    startSearch();
}

void App::searchDismiss(int fi, int mi) {
    if (fi < 0 || fi >= static_cast<int>(sRes_.size())) return;
    auto& f = sRes_[static_cast<std::size_t>(fi)];
    if (mi >= 0 && mi < static_cast<int>(f.matches.size())) f.matches.erase(f.matches.begin() + mi);
    if (mi < 0 || f.matches.empty()) sRes_.erase(sRes_.begin() + fi);
    recountSearch();
    dirty_ = true;
}

bool App::searchKey(const KeyEvent& k) {
    TextBox* boxes[4] = {&sQuery_, &sReplace_, &sInclude_, &sExclude_};
    auto visible = [&](int i) { return i < 2 || sFilters_; };
    if (k.key == Key::Escape) { sFocus_ = -1; return true; }
    if (k.key == Key::Tab) {
        int n = sFocus_;
        for (int step = 0; step < 4; ++step) { n = (n + (k.shift ? 3 : 1)) % 4; if (visible(n)) break; }
        sFocus_ = n;
        return true;
    }
    if (k.key == Key::Enter) {
        if (k.ctrl && sFocus_ == 1) { searchReplace(-1, -1); return true; }
        startSearch();
        return true;
    }
    if (k.alt && !k.ctrl && k.key == Key::Char) {
        if (k.ch == 'c') { sCase_ = !sCase_; startSearch(); return true; }
        if (k.ch == 'w') { sWord_ = !sWord_; startSearch(); return true; }
        if (k.ch == 'r') { sRegex_ = !sRegex_; startSearch(); return true; }
    }
    if (k.ctrl && k.key != Key::Char) return false;
    if (k.key >= Key::F1) return false;
    if (k.ctrl && k.key == Key::Char && k.ch != 'v' && k.ch != 'a') return false;   // IDE shortcuts keep working
    if (sFocus_ < 0 || sFocus_ > 3) return false;
    TextBox& t = *boxes[sFocus_];
    const std::string before = t.text;
    const bool used = editText(t, k);
    if (used && t.text != before && sFocus_ != 1) scheduleSearch();
    return used;
}

void App::searchClick(Button b) {
    if (b != Button::Left) return;
    sFocus_ = -1;
    for (auto it = sHits_.rbegin(); it != sHits_.rend(); ++it) {
        if (!it->r.contains(mx_, my_)) continue;
        switch (it->kind) {
            case 1: case 2: case 3: case 4: {
                sFocus_ = it->kind - 1;
                TextBox& t = sFocus_ == 0 ? sQuery_ : sFocus_ == 1 ? sReplace_ : sFocus_ == 2 ? sInclude_ : sExclude_;
                t.cur = caretFromX(t.text, mx_ - (it->r.x + 10 * S()));   // approximate: ignores horizontal scroll of long text
                break;
            }
            case 5: sCase_ = !sCase_; startSearch(); sFocus_ = 0; break;
            case 6: sWord_ = !sWord_; startSearch(); sFocus_ = 0; break;
            case 7: sRegex_ = !sRegex_; startSearch(); sFocus_ = 0; break;
            case 8: {
                if (sTotal_ == 0) break;
                const auto now = Clock::now();
                if (now - sReplaceArmAt_ < std::chrono::seconds(4)) { sReplaceArmAt_ = {}; searchReplace(-1, -1); }
                else {
                    sReplaceArmAt_ = now;
                    setMessage(std::format("Replace {} occurrence{} with \"{}\"? Click again to confirm", sTotal_, sTotal_ == 1 ? "" : "s", sReplace_.text), false, 4);
                }
                break;
            }
            case 9: sFilters_ = !sFilters_; break;
            default: break;
        }
        dirty_ = true;
        return;
    }
    if (!L_.sideList.contains(mx_, my_) || my_ < sResultsTop_) return;
    if (sRowsDirty_) rebuildSearchRows();
    const float rh = L_.sideRow;
    const int row = static_cast<int>(std::floor((my_ - sResultsTop_ + sScroll_) / rh));
    if (row < 0 || row >= static_cast<int>(sRows_.size())) return;
    const SRow r = sRows_[static_cast<std::size_t>(row)];
    const float s = S(), bs = std::round(rh - 8 * s);
    const Rect dismiss{L_.side.right() - 10 * s - bs, sResultsTop_ + static_cast<float>(row) * rh - sScroll_ + 4 * s, bs, bs};
    const Rect repl{dismiss.x - 2 * s - bs, dismiss.y, bs, bs};
    if (dismiss.contains(mx_, my_)) { searchDismiss(r.file, r.match); return; }
    if (repl.contains(mx_, my_)) { searchReplace(r.file, r.match); return; }
    if (r.match < 0) { auto& f = sRes_[static_cast<std::size_t>(r.file)]; f.collapsed = !f.collapsed; sRowsDirty_ = true; }
    else searchOpenMatch(r.file, r.match);
    dirty_ = true;
}

void App::searchField(const Rect& r, TextBox& t, bool focused, int kind, std::string_view placeholder, float rightPad) {
    const float s = S();
    sHits_.push_back({r, kind});
    fill(r, focused ? theme::accent : Color::rgb(0xffffff, 34), 7 * s);
    fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, Color::rgb(0x07080a), 6 * s);
    const float x0 = r.x + 10 * s, maxX = r.right() - rightPad;
    std::size_t start = 0;
    const std::size_t cur = std::min(t.cur, t.text.size());
    while (start < cur && uiW(std::string_view(t.text).substr(start, cur - start)) > maxX - x0 - 4 * s) start = utf8Next(t.text, start);
    if (t.text.empty()) uiLabel(x0, r.y, r.h, placeholder, theme::lineNum, maxX);
    else uiLabel(x0, r.y, r.h, std::string_view(t.text).substr(start), theme::text, maxX);
    if (focused && caretOn_ && focused_) {
        const float cx = x0 + uiW(std::string_view(t.text).substr(start, cur - start));
        fill(std::round(cx), r.y + 7 * s, std::max(2.0f, std::round(1.4f * s)), r.h - 14 * s, theme::caret, s);
    }
}

float App::searchChip(float right, const Rect& field, std::string_view label, bool on, int kind) {
    const float s = S();
    const float w = std::max(uiW(label) + 12 * s, std::round(26 * s));
    const Rect c{right - w, field.y + 4 * s, w, field.h - 8 * s};
    sHits_.push_back({c, kind});
    const bool hot = c.contains(mx_, my_);
    if (on) fill(c, theme::accent.alpha(80), 5 * s);
    else if (hot) fill(c, Color::rgb(0xffffff, 24), 5 * s);
    uiLabelC(c.x + c.w / 2, c.y, c.h, label, on ? Color::rgb(0xffffff) : theme::dim);
    return c.x - 2 * s;
}

void App::drawSearchBody() {
    const Rect& sd = L_.side;
    const float s = S(), rh = L_.sideRow, fh = std::round(uiH() + 12 * s), gap = std::round(6 * s);
    sHits_.clear();
    const float x0 = sd.x + 10 * s, w = sd.w - 20 * s;
    float y = L_.sideList.y + 6 * s;
    const Rect qR{x0, y, w, fh};
    y += fh + gap;
    const Rect rR{x0, y, w, fh};
    y += fh + gap;
    Rect iR{}, eR{};
    if (sFilters_) { iR = {x0, y, w, fh}; y += fh + gap; eR = {x0, y, w, fh}; y += fh + gap; }
    const Rect sumR{sd.x, y, sd.w, rh};
    sResultsTop_ = std::round(sumR.bottom() + 2 * s);

    // ---- results (drawn first; the field block is painted over rows that scroll beneath it)
    if (sRowsDirty_) rebuildSearchRows();
    const float listBottom = L_.sideList.bottom();
    const float viewH = std::max(0.0f, listBottom - sResultsTop_);
    sMax_ = std::max(0.0f, static_cast<float>(sRows_.size()) * rh - viewH * 0.5f);
    sScroll_ = std::clamp(sScroll_, 0.0f, sMax_);
    const float sy = std::round(sScroll_);
    const int first = std::max(0, static_cast<int>(std::floor(sy / rh)));
    const int last = std::min(static_cast<int>(sRows_.size()) - 1, static_cast<int>(std::floor((sy + viewH) / rh)));
    const bool hasRepl = !sReplace_.text.empty();
    const float indent = std::round(16 * s);
    for (int i = first; i <= last; ++i) {
        const SRow r = sRows_[static_cast<std::size_t>(i)];
        const auto& f = sRes_[static_cast<std::size_t>(r.file)];
        const float ry = sResultsTop_ + static_cast<float>(i) * rh - sy;
        const Rect rr{sd.x + 6 * s, ry + 1, sd.w - 12 * s, rh - 2};
        const bool hot = L_.sideList.contains(mx_, my_) && my_ >= sResultsTop_ && rr.contains(mx_, my_);
        if (hot) fill(rr, theme::sideHover, 6 * s);
        const float bs = std::round(rh - 8 * s);
        const Rect dismiss{sd.right() - 10 * s - bs, ry + 4 * s, bs, bs};
        const Rect repl{dismiss.x - 2 * s - bs, dismiss.y, bs, bs};
        auto actionIcon = [&](const Rect& b, char32_t g) {
            if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 36), 5 * s);
            const int ip = std::max(10, static_cast<int>(std::round(bs * 0.62f)));
            Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, g, Color::rgb(0xffffff));
        };
        if (r.match < 0) {
            const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
            Icons::chevron(dl_, font_, sd.x + 12 * s + (indent - static_cast<float>(cs)) / 2, ry + (rh - static_cast<float>(cs)) / 2, cs, !f.collapsed, theme::dim);
            const float ix = sd.x + 12 * s + indent, iy = ry + (rh - iconSize()) / 2;
            const std::string name = fs::path(f.rel).filename().string();
            drawFileIcon(ix, iy, name);
            const float nx = ix + iconSize() + 8 * s;
            const float rightEdge = hot ? repl.x - 6 * s : sd.right() - 14 * s;
            float after = uiLabel(nx, ry, rh, name, theme::text, rightEdge - 30 * s);
            const std::string dir = fs::path(f.rel).parent_path().generic_string();
            if (!dir.empty()) uiLabel(after + 8 * s, ry, rh, dir, theme::lineNum, rightEdge - 30 * s);
            if (hot) { actionIcon(repl, 0xF06D4); actionIcon(dismiss, 0xF0156); }
            else {
                const std::string cnt = std::to_string(f.matches.size());
                const float bw = std::max(uiW(cnt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                const Rect badge{sd.right() - 14 * s - bw, ry + (rh - bh) / 2, bw, bh};
                fill(badge, Color::rgb(0xffffff, 26), bh / 2);
                uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, cnt, theme::dim);
            }
        } else {
            const auto& m = f.matches[static_cast<std::size_t>(r.match)];
            float x = sd.x + 12 * s + indent * 2;
            const float maxX = (hot ? repl.x - 6 * s : sd.right() - 14 * s);
            const std::string_view pv = m.preview;
            const std::size_t pc = std::min(static_cast<std::size_t>(std::max(0, m.pcol)), pv.size());
            const std::size_t pl = std::min(static_cast<std::size_t>(m.len), pv.size() - pc);
            const Color base = Color::rgb(0xb9bdc6);
            x = uiLabel(x, ry, rh, pv.substr(0, pc), base, maxX);
            const float mw = uiW(pv.substr(pc, pl));
            if (hasRepl) {
                fill(x, ry + 4 * s, std::min(mw, maxX - x), rh - 8 * s, Color::rgb(0xe06c75, 60), 3 * s);
                const float xe = uiLabel(x, ry, rh, pv.substr(pc, pl), Color::rgb(0xe9a3a8), maxX);
                fill(x, ry + rh / 2, std::max(0.0f, std::min(mw, maxX - x)), std::max(1.0f, std::round(s)), Color::rgb(0xe9a3a8));
                const float nw = uiW(sReplace_.text);
                fill(xe, ry + 4 * s, std::max(0.0f, std::min(nw, maxX - xe)), rh - 8 * s, Color::rgb(0x73c990, 60), 3 * s);
                x = uiLabel(xe, ry, rh, sReplace_.text, Color::rgb(0xa5e0b8), maxX);
            } else {
                fill(x, ry + 4 * s, std::min(mw, maxX - x), rh - 8 * s, Color::rgb(0xe5c07b, 70), 3 * s);
                x = uiLabel(x, ry, rh, pv.substr(pc, pl), Color::rgb(0xffffff), maxX);
            }
            if (pc + pl < pv.size()) uiLabel(x, ry, rh, pv.substr(pc + pl), base, maxX);
            if (hot) { actionIcon(repl, 0xF06D4); actionIcon(dismiss, 0xF0156); }
        }
    }
    if (static_cast<float>(sRows_.size()) * rh > viewH) {
        const float contentH = static_cast<float>(sRows_.size()) * rh;
        const float thumbH = std::max(24 * s, viewH * viewH / (contentH + viewH * 0.5f));
        const float thumbY = sResultsTop_ + (viewH - thumbH) * std::clamp(sy / sMax_, 0.0f, 1.0f);
        fill(sd.right() - 9 * s, thumbY, 5 * s, thumbH, theme::scrollbar, 2.5f * s);
    }

    // ---- inputs and summary (opaque, so results scrolling underneath disappear)
    fill(sd.x, L_.sideList.y, sd.w, sResultsTop_ - L_.sideList.y, theme::sideBg);
    sHits_.clear();
    {
        auto chipW = [&](std::string_view l) { return std::max(uiW(l) + 12 * s, std::round(26 * s)) + 2 * s; };
        const float reserve = chipW(".*") + chipW("ab") + chipW("Aa") + 10 * s;
        searchField(qR, sQuery_, sFocus_ == 0, 1, "Search", reserve);
        float rp = qR.right() - 6 * s;
        rp = searchChip(rp, qR, ".*", sRegex_, 7);
        rp = searchChip(rp, qR, "ab", sWord_, 6);
        searchChip(rp, qR, "Aa", sCase_, 5);
    }
    {   // replace-all button docked in the replace field
        const float bw = std::round(fh - 6 * s);
        const Rect b{rR.right() - 3 * s - bw, rR.y + 3 * s, bw, rR.h - 6 * s};
        searchField(rR, sReplace_, sFocus_ == 1, 2, "Replace", bw + 12 * s);
        sHits_.push_back({b, 8});
        const bool armed = Clock::now() - sReplaceArmAt_ < std::chrono::seconds(4);
        if (armed) fill(b, theme::accent.alpha(120), 5 * s);
        else if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 28), 5 * s);
        const int ip = std::max(12, static_cast<int>(std::round(b.h * 0.7f)));
        Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, 0xF06D4,
                     sTotal_ > 0 ? Color::rgb(0xffffff) : theme::lineNum);
    }
    if (sFilters_) {
        searchField(iR, sInclude_, sFocus_ == 2, 3, "files to include (*.cpp, src/**)", 10 * s);
        searchField(eR, sExclude_, sFocus_ == 3, 4, "files to exclude", 10 * s);
    }
    // summary line
    std::string sum;
    Color sc = theme::dim;
    if (!sError_.empty()) { sum = sError_; sc = sError_ == "Type to search" ? theme::lineNum : Color::rgb(0xf2777a); }
    else if (sBusy_ || sPending_) sum = "Searching...";
    else if (sSearched_ && sTotal_ == 0) sum = "No results";
    else if (sTotal_ > 0) sum = std::format("{}{} result{} in {} file{}", sTotal_, sTrunc_ ? "+" : "", sTotal_ == 1 ? "" : "s", sRes_.size(), sRes_.size() == 1 ? "" : "s");
    const std::string flt = "Filters";
    const float fw = uiW(flt) + 16 * s;
    const Rect fR{sd.right() - 10 * s - fw, sumR.y + 3 * s, fw, rh - 6 * s};
    sHits_.push_back({fR, 9});
    if (sFilters_) fill(fR, theme::accent.alpha(60), 5 * s);
    else if (fR.contains(mx_, my_)) fill(fR, Color::rgb(0xffffff, 22), 5 * s);
    uiLabelC(fR.x + fR.w / 2, fR.y, fR.h, flt, sFilters_ ? Color::rgb(0xffffff) : theme::dim);
    uiLabel(sd.x + 14 * s, sumR.y, rh, sum, sc, fR.x - 6 * s);
}

}  // namespace ide
