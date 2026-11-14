// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / render - part of the App implementation (see app.cppm for the class).
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

void App::glassPanel(const Rect& r, float radius) {
    const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
    fill(r.x - 4 * s, r.y + 3 * s, r.w + 8 * s, r.h + 10 * s, Color{0, 0, 0, 26}, radius + 8 * s);
    fill(r.x - 1.5f * s, r.y + 1.5f * s, r.w + 3 * s, r.h + 5 * s, Color{0, 0, 0, 40}, radius + 3 * s);
    fill(r, Color::rgb(0xffffff, 38), radius);
    dl_.glass(true);
    fill(r.x + hair, r.y + hair, r.w - 2 * hair, r.h - 2 * hair, Color::rgb(0x0b0c0f, 205), radius - hair);
    dl_.glass(false);
}

void App::carve(const Rect& r, float radius) {
    Color f = frameC();
    const float a = static_cast<float>(f.a) / 255.0f;
    Color pre{static_cast<std::uint8_t>(f.r * a + 0.5f), static_cast<std::uint8_t>(f.g * a + 0.5f), static_cast<std::uint8_t>(f.b * a + 0.5f), 255};
    dl_.erase(true);
    dl_.rect(font_, r.x, r.y, r.w, r.h, pre, radius);
    dl_.erase(false);
}

void App::draw() {
    layout();
    dl_.clear();
    const float W = L_.status.w, H = L_.status.bottom();
    const Rect& isl = L_.island;
    blameRect_ = {};
    for (Node* n : leaves()) {
        Pane& p = *n->pane;
        fill(p.rect, theme::editorBg);
        if (!p.cur) continue;
        const PaneCtx sv = enterPane(p);
        drawActive_ = &p == activePane_;
        drawEditor(*p.cur);
        leavePane(p, sv);
    }
    drawActive_ = true;
    // Content may overflow the panes (partial lines, horizontal scroll): repaint the frame around them.
    carve({0, isl.bottom(), W, H - isl.bottom()});
    carve({0, isl.y, isl.x, isl.h});
    carve({isl.right(), isl.y, W - isl.right(), isl.h});
    for (Node* n : splits()) carve(n->gap);
    if (L_.side.w > 1) drawSidebar();
    for (Node* n : leaves()) {
        Pane& p = *n->pane;
        const PaneCtx sv = enterPane(p);
        drawTabs(p);
        if (p.cur) drawCrumbs();
        leavePane(p, sv);
    }
    drawPanel();
    if (prompt_.kind != PromptKind::None) { if (sidePrompt()) drawSidePrompt(); else drawPrompt(); }
    if (L_.panel.h > 0.5f) {
        const Rect& pn = L_.panel;
        carve({pn.x, pn.bottom(), pn.w, H - pn.bottom()});  // rows that overflow the bottom edge
        carve(pn, -L_.radius);
    }
    for (Node* n : leaves()) carve(n->pane->rect, -L_.radius);
    if (L_.side.w > 1) {
        if (sideAnim_ > 0.002f && sideAnim_ < 0.998f) carve({L_.activity.x, L_.side.y, W - L_.activity.x, L_.side.h});  // clip content sliding past the edge
        carve(L_.side, -L_.radius);
    }
    drawActivityBar();   // after the clip above, which would otherwise erase the buttons while the explorer slides  // inverted rounded rect = clips the four corners
    // Thin outline around every island so it reads as a separate surface against the window background.
    {
        const float t = std::max(1.0f, std::round(S() * 0.8f));
        const Color oc = Color::rgb(0xffffff, 34);
        auto outline = [&](const Rect& r) { if (r.w > 8 && r.h > 8) dl_.ring(font_, r.x, r.y, r.w, r.h, oc, L_.radius, t); };
        for (Node* n : leaves()) outline(n->pane->rect);
        if (L_.side.w > 1) outline(L_.side);
        if (L_.panel.h > 0.5f) outline(L_.panel);
    }
    if (L_.csd) drawTitleBar();
    drawToolbar();
    drawStatus();
    drawCompletion();
    drawHover();
    drawMenu();
    drawDebugBar();
    drawTabDrag();
    drawFileDrag();
    drawToolTip();
    drawDialog();
    if (L_.csd && !win_.maximized() && !win_.fullscreen()) {  // hairline window outline
        const float s = std::max(1.0f, std::round(S() * 0.5f));
        fill(0, 0, W, s, theme::border); fill(0, H - s, W, s, theme::border);
        fill(0, 0, s, H, theme::border); fill(W - s, 0, s, H, theme::border);
    }
}

Rect App::closePill() const {
    const float s = S(), d = std::round(L_.title.h - 8 * s);
    return {std::round(8 * s), L_.title.y + std::round((L_.title.h - d) / 2), d, d};
}

Rect App::logoPill() const {
    const float s = S(), d = std::min(L_.activity.w - 2 * std::round(3.5f * s), std::round(L_.title.h - 4 * s));
    return {std::round(L_.activity.x + (L_.activity.w - d) / 2), L_.title.y + std::round((L_.title.h - d) / 2), d, d};
}

void App::drawTitleBar() {
    const Rect& t = L_.title;
    const float s = S(), hair = std::max(1.0f, std::round(0.8f * s));
    carve(t);
    auto pill = [&](const Rect& r, bool hot, const Color& hotFill) {   // same look as the activity-bar pills
        fill(r, Color::rgb(0xffffff, 34), r.w / 2);
        fill(r.x + hair, r.y + hair, r.w - 2 * hair, r.h - 2 * hair, hot ? hotFill : Color::rgb(0x0d0e11), r.w / 2 - hair);
    };
    // close: a circular pill at the left
    const Rect c = closePill();
    const bool hotClose = hoverBtn_ == 3;
    pill(c, hotClose, theme::danger);
    const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.9f)));
    Icons::tool(dl_, font_, std::round(c.x + (c.w - static_cast<float>(ip)) / 2), std::round(c.y + (c.h - static_cast<float>(ip)) / 2), ip, ToolIcon::Close,
                hotClose ? Color::rgb(0xffffff) : theme::text);
    // logo: a circular pill above the activity-bar pills
    const Rect lg = logoPill();
    pill(lg, hoverBtn_ == 4, Color::rgb(0x23252b));
    const int ls = std::max(12, static_cast<int>(std::round(lg.w * 0.56f)));
    Icons::logo(dl_, font_, std::round(lg.x + (lg.w - static_cast<float>(ls)) / 2), std::round(lg.y + (lg.h - static_cast<float>(ls)) / 2), ls, theme::accent);

    std::string title = docs_.empty() ? "Daemon" : doc().name + (doc().modified() ? " \xE2\x80\xA2" : "");
    if (explorer_.isOpen()) title += "  \xE2\x80\x94  " + explorer_.rootName();
    const float maxX = lg.x - 12 * s;
    const float tw = std::min(uiW(title), maxX - c.right() - 24 * s);
    if (!cmakeOpen_) uiLabel(std::max(c.right() + 12 * s, (t.w - tw) / 2), t.y, t.h, title, theme::dim, maxX);
}

void App::drawSidebar() {
    const Rect& sd = L_.side;
    const float s = S(), rh = L_.sideRow;
    fill(sd, theme::sideBg);
    const float headH = L_.tabs.h;

    if (sideDebug_) {
        drawDebugBody();
        fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
        uiLabel(sd.x + 16 * s, sd.y, headH, "RUN AND DEBUG", theme::dim, sd.right() - 12 * s);
        return;
    }
    if (sideSearch_) {
        drawSearchBody();
        fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
        uiLabel(sd.x + 16 * s, sd.y, headH, "SEARCH", theme::dim, sd.right() - 12 * s);
        if (explorer_.isOpen()) uiLabelR(sd.right() - 16 * s, sd.y, headH, explorer_.rootName(), theme::lineNum);
        return;
    }
    if (sideGit_) {   // Source Control instead of the file tree
        drawGitBody(L_.sideList);
        fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
        uiLabel(sd.x + 16 * s, sd.y, headH, "SOURCE CONTROL", theme::dim, sd.right() - 12 * s);
        if (!gitBranch_.empty()) {
            const std::string b = gitBranch_;
            uiLabelR(sd.right() - 16 * s, sd.y, headH, b, theme::lineNum);
        }
        return;
    }

    if (!explorer_.isOpen()) {
        const float y = sd.y + headH + 22 * s, lhh = std::round(uiH() + 8 * s);
        uiLabel(sd.x + 20 * s, y, lhh, "No folder open", theme::dim, sd.right() - 8 * s);
        uiLabel(sd.x + 20 * s, y + lhh, lhh, "Open one with Ctrl+Shift+O", theme::accent, sd.right() - 8 * s);
    } else {
        const auto& rows = explorer_.rows();
        const float sy = std::round(sideScroll_), indent = std::round(16 * s);
        const int first = std::max(0, static_cast<int>(std::floor(sy / rh)));
        const int last = std::min(static_cast<int>(rows.size()) - 1, static_cast<int>(std::floor((sy + L_.sideList.h) / rh)));
        const std::string activePath = docs_.empty() ? std::string() : doc().path;
        for (int i = first; i <= last; ++i) {
            const auto& row = rows[static_cast<std::size_t>(i)];
            const FileNode& nd = *row.node;
            const float y = L_.sideList.y + static_cast<float>(i) * rh - sy;
            const Rect rr{sd.x + 6 * s, y + 1, sd.w - 12 * s, rh - 2};
            const bool selected = nd.path.string() == explorer_.selected();
            const bool active = !activePath.empty() && nd.path.string() == activePath;
            if (selected && sideFocus_) fill(rr, theme::selection, 6 * s);
            else if (selected) fill(rr, theme::sideSel, 6 * s);
            else if (active) fill(rr, theme::accent.alpha(40), 6 * s);
            else if (i == hoverRow_ && !fdrag_.active) fill(rr, theme::sideHover, 6 * s);
            if (fdrag_.active && !fdrag_.target.empty() && nd.dir && nd.path == fdrag_.target) {
                fill(rr, theme::accent.alpha(46), 6 * s);
                const float t = std::max(1.0f, std::round(1.5f * s));
                fill(rr.x, rr.y, rr.w, t, theme::accent.alpha(170)); fill(rr.x, rr.bottom() - t, rr.w, t, theme::accent.alpha(170));
                fill(rr.x, rr.y, t, rr.h, theme::accent.alpha(170)); fill(rr.right() - t, rr.y, t, rr.h, theme::accent.alpha(170));
            }
            const float x = sd.x + 12 * s + static_cast<float>(row.depth) * indent;
            for (int lvl = 0; lvl < row.depth; ++lvl)
                fill(std::round(sd.x + 12 * s + static_cast<float>(lvl) * indent + indent * 0.5f), y, std::max(1.0f, std::round(s * 0.5f)), rh, theme::guide);
            Color nameColor = nd.name.starts_with('.') ? theme::dim : (nd.dir ? theme::text : Color::rgb(0xb9bdc6));
            if (active) nameColor = Color::rgb(0xffffff).mix(theme::accent, 0.25f);
            float nameMax = sd.right() - 10 * s;
            float rightEdge = sd.right() - 22 * s;
            const std::string pathStr = nd.path.lexically_normal().string();
            Color gitColor{};
            bool hasGit = false;
            if (auto gf = gitFiles_.find(pathStr); gf != gitFiles_.end()) {   // git status letter (files, untracked folders)
                const git::Entry& ge = gf->second;
                const char ch = ge.x == '?' ? 'U' : (ge.conflicted() ? '!' : (ge.y != ' ' ? ge.y : ge.x));
                gitColor = (ch == 'U' || ch == 'A') ? Color::rgb(0x73c990) : ch == 'D' || ch == '!' ? Color::rgb(0xe06c75) : ch == 'R' ? Color::rgb(0x61afef) : Color::rgb(0xe5c07b);
                const std::string letter(1, ch);
                const float lw = uiW(letter);
                uiLabel(rightEdge - lw, y, rh, letter, gitColor);
                rightEdge -= lw + 10 * s;
                hasGit = true;
            } else if (nd.dir && gitDirs_.contains(pathStr)) {   // folder containing changes: a dot
                const float dot = std::round(6 * s);
                fill(rightEdge - dot, y + (rh - dot) / 2, dot, dot, Color::rgb(0xe5c07b), dot / 2);
                rightEdge -= dot + 10 * s;
            }
            if (hasGit && !active) nameColor = gitColor;
            nameMax = std::min(nameMax, rightEdge - 6 * s);
            if (auto pc = problemCounts_.find(nd.path.string()); pc != problemCounts_.end()) {   // problems in this file / folder
                const auto [errs, warns] = pc->second;
                const Color pcol = errs > 0 ? Color::rgb(0xf2777a) : Color::rgb(0xe5c07b);
                if (!nd.dir) nameColor = pcol;
                const int cnt = errs + warns;
                const std::string txt = cnt > 99 ? "99+" : std::to_string(cnt);
                const float bw = std::max(uiW(txt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                const Rect badge{rightEdge - bw, y + (rh - bh) / 2, bw, bh};
                fill(badge, pcol.alpha(46), bh / 2);
                uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, txt, pcol);
                nameMax = badge.x - 6 * s;
            }
            const bool cutPending = fileClip_.cut && std::ranges::find(fileClip_.paths, nd.path) != fileClip_.paths.end();
            if (cutPending) nameColor = nameColor.alpha(110);
            const float fade = cutPending ? 0.45f : (nd.name.starts_with('.') ? 0.7f : 1.0f);
            if (nd.dir) {
                const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                Icons::chevron(dl_, font_, x + (indent - static_cast<float>(cs)) / 2, y + (rh - static_cast<float>(cs)) / 2, cs, nd.expanded,
                               selected && sideFocus_ ? Color::rgb(0xffffff) : theme::dim);
            }
            const float ix = x + indent, iy = y + (rh - iconSize()) / 2;
            if (nd.dir) drawFolderIcon(ix, iy, nd.name, nd.expanded, fade);
            else drawFileIcon(ix, iy, nd.name, fade);
            uiLabel(ix + iconSize() + 8 * s, y, rh, nd.name, nameColor, nameMax);
        }
        const float contentH = static_cast<float>(rows.size()) * rh, listH = L_.sideList.h;
        if (contentH > listH) {
            const float maxS = std::max(1.0f, contentH - listH * 0.5f);
            const float thumbH = std::max(24 * s, listH * listH / (contentH + listH * 0.5f));
            const float thumbY = L_.sideList.y + (listH - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
            fill(sd.right() - 9 * s, thumbY, 5 * s, thumbH, theme::scrollbar, 2.5f * s);
        }
    }

    // Header (opaque, so scrolled rows disappear beneath it).
    fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
    std::string title = explorer_.isOpen() ? explorer_.rootName() : "Explorer";
    for (auto& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const float hx = sd.x + 16 * s;
    uiLabel(hx, sd.y, headH, title, theme::dim, sd.right() - 12 * s);
}

void App::drawTabs(Pane& p) {
    const float s = S();
    fill(p.tabsR, theme::stripBg);
    const bool focusedPane = &p == activePane_;
    const int xs = std::max(8, static_cast<int>(std::round(uiH() * 0.62f)));
    auto fade = [](Color c, float a) { return c.alpha(static_cast<std::uint8_t>(static_cast<float>(c.a) * a)); };
    for (const auto& e : p.draw) {
        const Rect& t = e.tab;
        if (t.w < 2) continue;
        const bool live = e.index >= 0;
        const bool dragged = tabDrag_.active && e.doc == tabDrag_.doc;
        const float al = e.alpha * (dragged ? 0.35f : 1.0f);
        const bool active = live && e.doc == p.cur;
        const bool hover = live && hoverPane_ == &p && e.index == hoverTab_;
        if (active) {
            fill(t, fade(theme::editorBg, al), 9 * s);
            fill(t.x, t.y + t.h - 12 * s, t.w, 12 * s, fade(theme::editorBg, al));  // square off the bottom: merges with the editor
            fill(t.x + 12 * s, t.y, std::max(0.0f, t.w - 24 * s), std::round(2 * s), fade(focusedPane ? theme::accent : theme::accent.alpha(90), al), std::round(s));
        } else if (hover) {
            fill(t.x + 1 * s, t.y + 3 * s, t.w - 2 * s, t.h - 3 * s, fade(Color::rgb(0x15171b), al), 8 * s);
        }
        const float ix = t.x + 14 * s;
        drawFileIcon(ix, t.y + (t.h - iconSize()) / 2 + s, e.name, (active ? 1.0f : 0.7f) * al);
        const Color tc = fade(active && focusedPane ? theme::text : (active ? Color::rgb(0xb4b8c1) : theme::dim), al);
        uiLabel(ix + iconSize() + 8 * s, t.y + s, t.h, e.name, tc, e.close.x - 2 * s);
        if (!live) continue;
        const bool overClose = hoverPane_ == &p && hoverClose_ == e.index;
        if (e.doc->modified() && !overClose) {
            const float dot = std::round(7 * s);
            fill(e.close.x + (e.close.w - dot) / 2, t.y + (t.h - dot) / 2 + s, dot, dot, fade(theme::accent, al), dot / 2);
        } else if (active || hover) {
            const float bs = std::round(18 * s);
            if (overClose) fill(e.close.x + (e.close.w - bs) / 2, t.y + (t.h - bs) / 2 + s, bs, bs, Color::rgb(0xffffff, 28), 5 * s);
            Icons::tool(dl_, font_, std::round(e.close.x + (e.close.w - static_cast<float>(xs)) / 2), std::round(t.y + (t.h - static_cast<float>(xs)) / 2 + s), xs,
                        ToolIcon::Close, overClose ? Color::rgb(0xffffff) : theme::dim);
        }
    }
}

std::vector<std::string> App::crumbParts(const Document& d) const {
    fs::path rel = d.path;
    if (explorer_.isOpen()) {
        auto r = fs::path(d.path).lexically_relative(explorer_.rootPath());
        if (!r.empty() && *r.begin() != "..") rel = r;
    }
    std::vector<std::string> parts;
    for (const auto& p : rel) if (p != "/" && !p.empty()) parts.push_back(p.string());
    if (parts.size() > 4) parts.erase(parts.begin(), parts.end() - 4);
    return parts;
}

void App::drawCrumbs() {
    if (L_.crumbs.h <= 0 || docs_.empty()) return;
    const Document& d = doc();
    const Rect& c = L_.crumbs;
    const float s = S();
    fill(c, theme::editorBg);
    const std::vector<std::string> parts = crumbParts(d);
    float x = c.x + 16 * s;
    const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        if (last) { drawFileIcon(x, c.y + (c.h - iconSize()) / 2, parts[i], 1.0f); x += iconSize() + 6 * s; }
        x = uiLabel(x, c.y, c.h, parts[i], last ? theme::text : theme::dim, c.right() - 12 * s);
        if (!last) {
            Icons::chevron(dl_, font_, x + 5 * s, c.y + (c.h - static_cast<float>(cs)) / 2, cs, false, theme::lineNum);
            x += static_cast<float>(cs) + 10 * s;
        }
    }
}

void App::drawEditor(Document& d) {
    const Rect& e = L_.editor;
    if (e.h <= 0) return;
    if (isPreview(d)) { drawPreview(d); return; }
    if (isNotebook(d)) { drawNotebook(d); return; }
    const float line = lh(), cell = cw(), s = S();
    const float sy = std::round(d.scrollY), sx = std::round(d.scrollX);
    const float baseX = L_.textX - sx;
    const int total = d.buf.lineCount();
    const int first = std::max(0, static_cast<int>(std::floor(sy / line)));
    const int last = std::min(total - 1, static_cast<int>(std::floor((sy + e.h) / line)));
    const Pos selA = d.selBegin(), selB = d.selEnd();
    const bool hasSel = d.hasSelection();
    const bool showMatches = inFindMode() && !findText_.empty();
    std::optional<std::pair<Pos, Pos>> bracket;
    if (!hasSel) bracket = d.matchBracket();

    // Background layers: current line, selection, search matches.
    for (int ln = first; ln <= last; ++ln) {
        const float y = e.y + static_cast<float>(ln) * line - sy;
        const std::string& str = d.buf.line(ln);
        if (!hasSel && ln == d.cursor.line) fill(e.x, y, e.w, line, theme::lineHl);
        if (dbgLine_.line > 0 && dbgLine_.line - 1 == ln && dbgLine_.file == d.path) fill(e.x, y, e.w, line, Color::rgb(0xe5c07b, 38));
        if (showMatches)
            for (int c : findAll(str, findText_, matchCase_)) {
                float x0 = baseX + static_cast<float>(displayCol(str, c, d.tabWidth)) * cell;
                float x1 = baseX + static_cast<float>(displayCol(str, c + static_cast<int>(findText_.size()), d.tabWidth)) * cell;
                fill(x0, y + 1, x1 - x0, line - 2, theme::match, 3 * s);
            }
        if (hasSel && ln >= selA.line && ln <= selB.line) {
            int c0 = ln == selA.line ? selA.col : 0;
            int c1 = ln == selB.line ? selB.col : static_cast<int>(str.size());
            float x0 = baseX + static_cast<float>(displayCol(str, c0, d.tabWidth)) * cell;
            float x1 = baseX + static_cast<float>(displayCol(str, c1, d.tabWidth)) * cell;
            if (ln < selB.line) x1 += cell * 0.6f;
            fill(x0, y, x1 - x0, line, focused_ && drawActive_ ? theme::selection : theme::selectionIdle);
        }
    }
    if (bracket) {
        for (Pos p : {bracket->first, bracket->second}) {
            if (p.line < first || p.line > last) continue;
            const std::string& str = d.buf.line(p.line);
            float x = baseX + static_cast<float>(displayCol(str, p.col, d.tabWidth)) * cell;
            float y = e.y + static_cast<float>(p.line) * line - sy, t = std::max(1.0f, std::round(s));
            fill(x, y, cell, t, theme::bracket); fill(x, y + line - t, cell, t, theme::bracket);
            fill(x, y, t, line, theme::bracket); fill(x + cell - t, y, t, line, theme::bracket);
        }
    }

    // Text.
    for (int ln = first; ln <= last; ++ln) {
        const float y = e.y + static_cast<float>(ln) * line - sy;
        const std::string& str = d.buf.line(ln);
        d.hl.highlight(d.buf, ln, toks_);
        const float baseline = y + font_.baseline();
        float x = baseX;
        int dcol = 0;
        const float right = e.right();
        // Indent guides for leading whitespace.
        if (!str.empty()) {
            std::size_t ws = 0;
            int cols = 0;
            while (ws < str.size() && (str[ws] == ' ' || str[ws] == '\t')) { cols += str[ws] == '\t' ? d.tabWidth : 1; ++ws; }
            if (ws < str.size())
                for (int g = 0; g < cols / d.tabWidth; ++g)
                    fill(baseX + static_cast<float>(g * d.tabWidth) * cell, y, std::max(1.0f, std::round(s * 0.5f)), line, theme::guide);
        }
        for (std::size_t i = 0; i < str.size() && x < right;) {
            const std::size_t start = i;
            char32_t cp = decodeUtf8(str, i);
            if (cp == '\t') {
                int adv = d.tabWidth - dcol % d.tabWidth;
                dcol += adv;
                x += static_cast<float>(adv) * cell;
                continue;
            }
            if (cp != ' ' && x + cell > L_.textX) {
                if (cp < 0x20 || cp == 0x7f) cp = 0xFFFD;
                dl_.glyph(font_.glyph(cp), x, baseline, theme::syntax(toks_[start]));
            }
            x += cell;
            ++dcol;
        }
    }

    // Diagnostics: squiggles under the offending text; the worst severity per line marks the gutter.
    std::unordered_map<int, int> worstSeverity;
    if (auto dit = diags_.find(d.path); dit != diags_.end()) {
        for (const LspDiag& dg : dit->second) {
            const Color c = diagColor(dg.severity);
            for (int ln = std::max(dg.line, first); ln <= std::min(dg.endLine, last); ++ln) {
                const std::string& str = d.buf.line(ln);
                int sc = ln == dg.line ? byteColFromUtf16(str, dg.col) : static_cast<int>(std::min(str.find_first_not_of(" \t"), str.size()));
                int ec = ln == dg.endLine ? byteColFromUtf16(str, dg.endCol) : static_cast<int>(str.size());
                if (ec <= sc) ec = static_cast<int>(utf8Next(str, static_cast<std::size_t>(sc)));
                float x0 = baseX + static_cast<float>(displayCol(str, sc, d.tabWidth)) * cell;
                float x1 = std::max(x0 + cell, baseX + static_cast<float>(displayCol(str, ec, d.tabWidth)) * cell);
                const float y = e.y + static_cast<float>(ln + 1) * line - sy - std::round(3.5f * s);
                const float step = std::max(2.0f, std::round(2.0f * s)), th = std::max(1.0f, std::round(1.2f * s));
                int k = 0;
                for (float x = x0; x < x1; x += step, ++k) fill(x, y + ((k & 1) ? std::round(1.5f * s) : 0.0f), std::min(step, x1 - x), th, c);
                auto it = worstSeverity.find(ln);
                if (it == worstSeverity.end() || dg.severity < it->second) worstSeverity[ln] = dg.severity;
            }
        }
    }

    // Caret: the animated, stretchy bar (gutter and tab strip are painted over any overflow).
    if ((focused_ || !opt_.screenshot.empty()) && caretOn_ && caret_.init && caret_.doc == &d) {
        const float cwid = caret_.xr - caret_.xl, chei = caret_.yb - caret_.yt;
        fill(baseX + caret_.xl, e.y + caret_.yt - sy, cwid, chei, theme::caret, std::min(cwid, chei) / 2);
    }

    // Inline blame for the cursor line: who changed it, when, and the commit summary.
    if (drawActive_) {
        if (const git::Commit* bc = blameOf(d, d.cursor.line)) {
            const std::string& cl = d.buf.line(d.cursor.line);
            const float by = e.y + static_cast<float>(d.cursor.line) * line - sy;
            const float limit = (L_.minimap.w > 0 ? L_.minimap.x : e.right()) - 14 * s;
            const float bx = baseX + static_cast<float>(displayCol(cl, static_cast<int>(cl.size()), d.tabWidth) + 3) * cell;
            if (by > e.y - line && by < e.bottom() && bx < limit - 14 * cell) {
                std::string t = blameText(*bc);
                const std::size_t maxCells = static_cast<std::size_t>((limit - bx) / cell);
                if (utf8Length(t) > maxCells) {
                    std::size_t i = 0, cells = 0;
                    while (i < t.size() && cells + 1 < maxCells) { i = utf8Next(t, i); ++cells; }
                    t = t.substr(0, i) + "\xE2\x80\xA6";
                }
                label(bx, by, t, Color::rgb(0x646a78), limit);
                blameRect_ = {bx - cell, by, static_cast<float>(utf8Length(t) + 2) * cell, line};
                blameHash_ = bc->hash;
                blameUncommitted_ = bc->uncommitted;
            }
        }
    }

    // Gutter (opaque, so scrolled-away text disappears beneath it).
    fill(e.x, e.y, L_.gutterW, e.h, theme::editorBg);
    const GitDoc* gd = nullptr;
    if (auto git_it = gitDocs_.find(&d); git_it != gitDocs_.end() && git_it->second.hunksVersion == d.buf.version()) gd = &git_it->second;
    for (int ln = first; ln <= last; ++ln) {
        const float y = e.y + static_cast<float>(ln) * line - sy;
        std::string num = std::to_string(ln + 1);
        bool active = ln == d.cursor.line;
        if (active && !hasSel) fill(e.x, y, L_.gutterW, line, theme::lineHl);
        label(e.x + L_.gutterW - 1.5f * cell - static_cast<float>(num.size()) * cell, y, num,
              active ? theme::lineNumActive : theme::lineNum);
        if (auto wit = worstSeverity.find(ln); wit != worstSeverity.end())
            fill(e.x + std::round(3 * s), y + std::round(3 * s), std::max(2.0f, std::round(3 * s)), line - std::round(6 * s), diagColor(wit->second), 1.5f * s);
        drawDebugGutter(d, ln, y, line);
        if (gd && ln < static_cast<int>(gd->kinds.size()) && gd->kinds[static_cast<std::size_t>(ln)])   // change bar vs HEAD
            fill(e.x + L_.gutterW - std::round(5 * s), y + 1, std::max(2.0f, std::round(3 * s)), line - 2,
                 gd->kinds[static_cast<std::size_t>(ln)] == 1 ? Color::rgb(0x4fb36d) : Color::rgb(0x5b9bff), 1.5f * s);
    }
    if (gd)
        for (int dl : gd->deletedAt) {   // removed lines: a small red wedge at the boundary
            if (dl < first || dl > last + 1) continue;
            const float y = e.y + static_cast<float>(dl) * line - sy;
            fill(e.x + L_.gutterW - std::round(6 * s), y - std::round(s), std::round(6 * s), std::max(2.0f, std::round(2 * s)), Color::rgb(0xe5484d), s);
        }

    drawMinimap(d);

    // Scrollbar.
    float maxS = maxScroll(d);
    if (maxS > 0) {
        float contentH = maxS + e.h;
        float thumbH = std::max(24 * s, e.h * e.h / contentH);
        float thumbY = e.y + (e.h - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
        float w = (scrollbarDrag_ ? 10.0f : 7.0f) * s;
        fill(e.right() - w - 4 * s, thumbY, w, thumbH, theme::scrollbar, w / 2);
    }

    // Welcome hint on an empty untitled buffer.
    if (docs_.size() == 1 && d.path.empty() && total == 1 && d.buf.line(0).empty() && (prompt_.kind == PromptKind::None || sidePrompt())) {
        static constexpr std::array<std::string_view, 8> hints = {
            "Ctrl+O   open file", "Ctrl+Shift+O   open folder", "Ctrl+B   toggle sidebar", "Ctrl+N   new file", "Ctrl+S   save", "Ctrl+F   find / Ctrl+H replace",
            "Ctrl+G   go to line", "Ctrl+Q   quit"};
        const float rowH = std::round(uiH() + 10 * s);
        float y = e.y + e.h * 0.30f;
        uiLabel(L_.textX + 4 * cell, y - 2 * rowH, rowH, "Daemon", theme::text);
        uiLabel(L_.textX + 4 * cell, y - rowH, rowH, "Wayland + Vulkan code editor", theme::dim);
        const float kx = L_.textX + 4 * cell, vx = kx + std::round(180 * s);
        for (auto h : hints) {
            auto gap = h.find("   ");
            const std::string_view key = h.substr(0, gap), what = gap == std::string_view::npos ? std::string_view() : h.substr(gap + 3);
            uiLabel(kx, y, rowH, key, theme::accent.mix(theme::text, 0.35f));
            uiLabel(vx, y, rowH, what, theme::dim);
            y += rowH;
        }
    }
}

void App::drawPrompt() {
    const Rect& p = L_.prompt;
    const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
    fill(p, theme::promptBg);
    fill(p.x, p.y, p.w, hair, theme::border);
    for (std::size_t i = 0; i < prompt_.fields.size(); ++i) {
        const Field& f = prompt_.fields[i];
        const float rowY = p.y + static_cast<float>(i) * L_.rowH;
        const bool active = static_cast<int>(i) == prompt_.active;
        float x = p.x + 16 * s;
        const std::string lab = f.label + ":";
        uiLabel(x, rowY, L_.rowH, lab, active ? theme::accent : theme::dim);
        x += uiW(lab) + 10 * s;

        float rightPad = 12 * s;
        std::string info;
        if (inFindMode() && i == 0) {
            info = matchSummary();
            rightPad = uiW(info) + uiW("Aa") + 50 * s;
        }
        const float maxX = p.right() - rightPad;
        // Scroll the field so the caret stays visible.
        std::size_t startByte = 0;
        while (startByte < f.cur && uiW(std::string_view(f.text).substr(startByte, f.cur - startByte)) > maxX - x - 8 * s)
            startByte = utf8Next(f.text, startByte);
        const std::string_view shown = std::string_view(f.text).substr(startByte);
        if (f.fresh && active)
            fill(x - 3 * s, rowY + 5 * s, std::min(uiW(shown), maxX - x) + 6 * s, L_.rowH - 10 * s, theme::selection, 4 * s);
        uiLabel(x, rowY, L_.rowH, shown, theme::text, maxX);
        if (active && caretOn_ && focused_) {
            const float cx = x + uiW(std::string_view(f.text).substr(startByte, f.cur - startByte));
            fill(std::round(cx), rowY + 7 * s, std::max(2.0f, std::round(1.4f * s)), L_.rowH - 14 * s, theme::caret, s);
        }
        if (inFindMode() && i == 0) {
            const float chipW = uiW("Aa") + 16 * s;
            const Rect chip{p.right() - 14 * s - chipW, rowY + 5 * s, chipW, L_.rowH - 10 * s};
            if (matchCase_) fill(chip, theme::accent.alpha(60), 5 * s);
            uiLabel(chip.x + 8 * s, chip.y, chip.h, "Aa", matchCase_ ? theme::accent : theme::lineNum);
            uiLabelR(chip.x - 12 * s, rowY, L_.rowH, info, theme::dim);
        }
    }
}

std::string App::matchSummary() {
    Document& d = doc();
    if (findText_.empty()) return "";
    auto key = std::tuple{&d, d.buf.version(), findText_, matchCase_};
    if (key != countKey_) {
        countKey_ = key;
        matchCount_ = 0;
        for (int l = 0; l < d.buf.lineCount(); ++l) matchCount_ += static_cast<int>(findAll(d.buf.line(l), findText_, matchCase_).size());
    }
    return matchCount_ == 0 ? "no results" : std::format("{} found", matchCount_);
}

void App::drawStatus() {
    const Rect& r = L_.status;
    const float s = S();
    carve(r);
    if (docs_.empty()) return;
    Document& d = doc();
    if (isNotebook(d)) {
        const auto& nv = *notebooks_.at(&d);
        uiLabelR(r.w - std::round(16 * s), r.y, r.h, nv.runnable ? "Notebook - C++" : "Notebook", theme::dim);
        uiLabel(L_.island.x + 8 * s, r.y, r.h, !message_.empty() ? message_ : d.path, !message_.empty() && messageError_ ? Color::rgb(0xe6a23c) : theme::dim, r.w - 220 * s);
        return;
    }
    if (isPreview(d)) {   // nothing editable: just say what is shown
        uiLabelR(r.w - std::round(16 * s), r.y, r.h, "Markdown preview", theme::dim);
        const auto& v = previews_.at(&d);
        const std::string hint = !message_.empty() ? message_ : !v.hoverUrl.empty() ? v.hoverUrl : (v.src ? v.src->path : std::string());
        uiLabel(L_.island.x + 8 * s, r.y, r.h, hint, !message_.empty() && messageError_ ? Color::rgb(0xe6a23c) : theme::dim, r.w - 200 * s);
        return;
    }

    float x = r.w - std::round(16 * s);
    const float gap = std::round(18 * s);
    auto seg = [&](const std::string& t, Color c = theme::dim) { x = uiLabelR(x, r.y, r.h, t, c) - gap; };
    seg(d.lang ? d.lang->name : "Plain Text", theme::text.mix(theme::dim, 0.3f));
    seg(d.buf.crlf() ? "CRLF" : "LF");
    seg("UTF-8");
    seg(d.useTabs ? "Tabs" : std::format("Spaces: {}", d.tabWidth));
    std::string pos = std::format("Ln {}, Col {}", d.cursor.line + 1, displayCol(d.buf.line(d.cursor.line), d.cursor.col, d.tabWidth) + 1);
    if (d.hasSelection()) pos += std::format(" ({} selected)", utf8Length(d.selectedText()));
    seg(pos);
    if (!gitBranch_.empty()) {   // current branch, ahead / behind and whether the work tree has changes
        std::string t = gitBranch_;
        if (gitAhead_) t += std::format(" \xE2\x86\x91{}", gitAhead_);
        if (gitBehind_) t += std::format(" \xE2\x86\x93{}", gitBehind_);
        if (!gitEntries_.empty()) t += std::format("  \xE2\x80\xA2 {}", gitEntries_.size());
        x = uiLabelR(x, r.y, r.h, t, gitEntries_.empty() ? theme::dim : Color::rgb(0xe5c07b));
        const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.95f)));
        Icons::glyph(dl_, font_, x - static_cast<float>(ip) - 4 * s, r.y + (r.h - static_cast<float>(ip)) / 2, ip, 0xF062C, theme::dim);
        x -= static_cast<float>(ip) + 4 * s + gap;
    }

    // Language server and diagnostics.
    if (isCppDoc(d) || !lspStatus_.empty()) {
        const float dot = std::round(7 * s);
        if (!lspStatus_.empty()) {
            const bool ok = lsp_ && lsp_->ready();
            const bool starting = lsp_ && lsp_->state() == LspClient::State::Starting;
            const Color dc = ok ? Color::rgb(0x6aab73) : (starting ? diagColor(2) : theme::lineNum);
            x = uiLabelR(x, r.y, r.h, lspStatus_, theme::dim);
            x -= dot + 6 * s;
            fill(x, r.y + (r.h - dot) / 2, dot, dot, dc, dot / 2);
            x -= gap;
        }
        if (auto it = diags_.find(d.path); it != diags_.end()) {
            int errs = 0, warns = 0;
            for (const auto& dg : it->second) { if (dg.severity == 1) ++errs; else if (dg.severity == 2) ++warns; }
            auto badge = [&](int n, Color c) {
                if (n == 0) return;
                x = uiLabelR(x, r.y, r.h, std::to_string(n), c);
                x -= dot + 5 * s;
                fill(x, r.y + (r.h - dot) / 2, dot, dot, c, dot / 2);
                x -= 12 * s;
            };
            badge(warns, diagColor(2));
            badge(errs, diagColor(1));
        }
    }

    const float lx = L_.island.x + 8 * s, maxX = x - 12 * s;
    if (!message_.empty()) {
        uiLabel(lx, r.y, r.h, message_, messageError_ ? Color::rgb(0xe6a23c) : theme::text, maxX);
    } else if (const LspDiag* dg = diagOnLine(d, d.cursor.line)) {
        std::string m = dg->message;
        if (auto nl = m.find('\n'); nl != std::string::npos) m.resize(nl);
        uiLabel(lx, r.y, r.h, m, diagColor(dg->severity), maxX);
    } else {
        uiLabel(lx, r.y, r.h, d.path.empty() ? std::string("untitled") : d.path, theme::dim, maxX);
    }
}

}  // namespace ide
