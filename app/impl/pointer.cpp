// daemon.app / pointer - part of the App implementation (see app.cppm for the class).
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

Edge App::edgeAt(float x, float y) const {
    if (!L_.csd || win_.maximized() || win_.fullscreen()) return Edge::None;
    const float m = 6 * S(), W = L_.status.w, H = L_.status.bottom();
    bool l = x < m, r = x >= W - m, t = y < m, b = y >= H - m;
    if (t && l) return Edge::TopLeft;
    if (t && r) return Edge::TopRight;
    if (b && l) return Edge::BottomLeft;
    if (b && r) return Edge::BottomRight;
    if (t) return Edge::Top;
    if (b) return Edge::Bottom;
    if (l) return Edge::Left;
    if (r) return Edge::Right;
    return Edge::None;
}

Cursor App::cursorForEdge(Edge e) {
    switch (e) {
        case Edge::Top: return Cursor::ResizeN;
        case Edge::Bottom: return Cursor::ResizeS;
        case Edge::Left: return Cursor::ResizeW;
        case Edge::Right: return Cursor::ResizeE;
        case Edge::TopLeft: return Cursor::ResizeNW;
        case Edge::TopRight: return Cursor::ResizeNE;
        case Edge::BottomLeft: return Cursor::ResizeSW;
        case Edge::BottomRight: return Cursor::ResizeSE;
        default: return Cursor::Default;
    }
}

int App::titleButtonAt(float x, float y) const {
    if (!L_.csd || !L_.title.contains(x, y)) return 0;
    float bw = L_.title.h * 1.25f;
    int idx = static_cast<int>((L_.title.right() - x) / bw);
    return idx >= 0 && idx < 3 ? 3 - idx : 0;
}

void App::onPointerMove(float lx, float ly) {
    mx_ = lx * S();
    my_ = ly * S();
    if (dlg_.kind != DlgKind::None && !(L_.csd && L_.title.contains(mx_, my_))) {
        int id = -1;
        for (auto it = dlgHits_.rbegin(); it != dlgHits_.rend(); ++it)
            if (it->r.contains(mx_, my_)) { id = it->id; break; }
        if (id < 0) id = -1;
        if (id != dlg_.hover) { dlg_.hover = id; dirty_ = true; }
        win_.setCursor(id == kDlgName || id == kDlgLoc || id == kDlgPath ? Cursor::Text : Cursor::Default);
        return;
    }
    if (menu_.open) {
        int i = menuItemAt(mx_, my_);
        if (i != menu_.hover) { menu_.hover = i; dirty_ = true; }
        win_.setCursor(Cursor::Default);
        return;
    }
    Edge edge = edgeAt(mx_, my_);
    Cursor c = Cursor::Default;
    if (edge != Edge::None) c = cursorForEdge(edge);
    else if (splitDragNode_) c = splitDragNode_->sideBySide ? Cursor::ResizeW : Cursor::ResizeN;
    else if (Node* g = gapAt(mx_, my_); g && !tabDrag_.active) c = g->sideBySide ? Cursor::ResizeW : Cursor::ResizeN;
    else if (splitDrag_ || (sidebar_ && L_.split.contains(mx_, my_))) c = Cursor::ResizeW;
    else if (panelDrag_ || (panelOpen_ && L_.panel.contains(mx_, my_) && my_ < L_.panel.y + 6 * S())) c = Cursor::ResizeN;
    else if (!docs_.empty() && L_.editor.contains(mx_, my_) && mx_ >= L_.textX && !inScrollbar(doc(), mx_, my_) && !inMinimap(mx_, my_)) c = Cursor::Text;
    win_.setCursor(c);

    if (splitDragNode_) {
        Node& n = *splitDragNode_;
        if (n.sideBySide) n.ratio = (mx_ - n.rect.x - n.gap.w / 2) / std::max(1.0f, n.rect.w - n.gap.w);
        else n.ratio = (my_ - n.rect.y - n.gap.h / 2) / std::max(1.0f, n.rect.h - n.gap.h);
        dirty_ = true;
    }
    if (fdrag_.pending && !fdrag_.active && std::hypot(mx_ - fdrag_.sx, my_ - fdrag_.sy) > 6 * S()) fdrag_.active = true;
    if (fdrag_.active) { fdrag_.target = fileDropTarget(); win_.setCursor(Cursor::Default); dirty_ = true; }
    if (tabDrag_.doc) {
        if (!tabDrag_.active && std::hypot(mx_ - tabDrag_.sx, my_ - tabDrag_.sy) > 7 * S()) tabDrag_.active = true;
        if (tabDrag_.active) { updateDropTarget(); win_.setCursor(Cursor::Default); dirty_ = true; }
    }
    if (splitDrag_) {
        const float m = std::round(8 * S());
        float w = L_.status.w - m - mx_ - m / 2;
        sidebarWidth_ = std::clamp(w / S(), 150.0f, L_.status.w / S() * 0.5f);
        dirty_ = true;
    }
    int hr = -1;
    if (sidebar_ && !sideGit_ && !sideSearch_ && !sideDebug_ && L_.sideList.contains(mx_, my_)) {
        hr = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
        if (hr < 0 || hr >= static_cast<int>(explorer_.rows().size())) hr = -1;
    }
    if (hr != hoverRow_) { hoverRow_ = hr; dirty_ = true; }

    int hb = titleButtonAt(mx_, my_), ht = -1, hc = -1;
    Pane* hp = nullptr;
    for (Node* n : leaves())
        for (std::size_t i = 0; i < n->pane->hits.size(); ++i)
            if (n->pane->hits[i].tab.contains(mx_, my_)) {
                hp = n->pane.get();
                ht = static_cast<int>(i);
                if (n->pane->hits[i].close.contains(mx_, my_)) hc = ht;
            }
    if (hp != hoverPane_) { hoverPane_ = hp; dirty_ = true; }
    const int hAct = activityAt(mx_, my_);
    if (hAct != hoverAct_) { hoverAct_ = hAct; dirty_ = true; }
    const int hover = opt_.screenshot.empty() ? toolAt(mx_, my_) : hoverTool_;   // scripted captures drive the hover state themselves
    int hpr = -1;
    if (panelOpen_ && panelBody().contains(mx_, my_))
        hpr = static_cast<int>(std::floor((my_ - panelBody().y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
    if (gitView()) gitUpdateHover();
    if ((searchView() || debugView()) && L_.side.contains(mx_, my_)) dirty_ = true;
    if (hb != hoverBtn_ || ht != hoverTab_ || hc != hoverClose_ || hover != hoverTool_ || hpr != hoverPanelRow_) {
        if (hover != hoverTool_) { toolHoverAt_ = Clock::now(); toolTipShown_ = false; }
        hoverBtn_ = hb; hoverTab_ = ht; hoverClose_ = hc; hoverTool_ = hover; hoverPanelRow_ = hpr;
        dirty_ = true;
    }
    if (tsel_.dragging) {
        const auto [r, c] = termPosAt(mx_, my_, panelBody());
        tsel_.cr = r; tsel_.cc = c;
        if (std::hypot(mx_ - tsel_.sx, my_ - tsel_.sy) > 3 * S()) tsel_.moved = true;
        const Rect body = panelBody();
        if (my_ < body.y) termScroll_ = std::min(termScroll_ + 1, term_.scrollbackSize());
        else if (my_ > body.bottom()) termScroll_ = std::max(termScroll_ - 1, 0);
        dirty_ = true;
    }
    if (psel_.dragging) {
        const auto [row, col] = panelPosAt(mx_, my_);
        psel_.cr = row; psel_.cc = col;
        if (std::hypot(mx_ - psel_.sx, my_ - psel_.sy) > 3 * S()) psel_.moved = true;
        // keep the pointer inside the panel body while dragging: scroll when it leaves it
        const Rect body = panelBody();
        auto& sc = panelScroll_[static_cast<std::size_t>(panelTab_)];
        if (my_ < body.y) { sc = std::max(0.0f, sc - lh() * 0.6f); panelFollow_[static_cast<std::size_t>(panelTab_)] = false; }
        else if (my_ > body.bottom()) sc = std::min(panelMaxScroll(), sc + lh() * 0.6f);
        dirty_ = true;
    }
    if (hsel_.dragging) {
        hoverSelUpdate();
        dirty_ = true;
    }
    Rect keepZone = hoverRect_;   // the popup (plus the gap up to the text it belongs to) stays while the pointer is on it
    keepZone.y -= 30 * S(); keepZone.h += 38 * S(); keepZone.x -= 44 * S(); keepZone.w += 54 * S();
    const bool onPopup = hover_.show && !menu_.open && keepZone.contains(mx_, my_);
    const bool overBlame = blameRect_.w > 0 && blameRect_.contains(mx_, my_);
    if (!opt_.screenshot.empty()) {}  // scripted runs drive hover themselves
    else if (overBlame) {
        if (!(blameHover_ && hover_.show)) { blameHover_ = true; showBlameCard(blameHash_, blameUncommitted_); dirty_ = true; }
    }
    else if (hsel_.dragging || onPopup) {}
    else if (!docs_.empty() && !menu_.open && !dragging_ && L_.editor.contains(mx_, my_) && mx_ >= L_.textX && mx_ < L_.minimap.x) { blameHover_ = false; hoverMoved(); }
    else { blameHover_ = false; hideHover(); }
    if (panelDrag_) {
        panelH_ = std::clamp((L_.panel.bottom() - my_) / S(), 90.0f, std::max(90.0f, (L_.side.h > 0 ? L_.side.h : L_.island.h + L_.panel.h) / S() * 0.6f));
        dirty_ = true;
    }
    if (dragging_ || scrollbarDrag_ || miniDrag_) { dragStep(doc()); dirty_ = true; }
}

void App::dragStep(Document& d) {
    if (miniDrag_) { miniJump(d); return; }
    if (scrollbarDrag_) {
        float frac = std::clamp((my_ - L_.editor.y) / std::max(1.0f, L_.editor.h), 0.0f, 1.0f);
        d.targetY = frac * maxScroll(d);
        d.scrollY = d.targetY;
        return;
    }
    if (!dragging_) return;
    // Auto-scroll while dragging above / below the text area.
    if (my_ < L_.editor.y) d.targetY -= lh() * 0.5f;
    else if (my_ > L_.editor.bottom()) d.targetY += lh() * 0.5f;
    d.setCursor(posAt(d, mx_, std::clamp(my_, L_.editor.y, L_.editor.bottom() - 1)), true);
    dirty_ = true;
}

void App::updateDropTarget() {
    TabDrag& d = tabDrag_;
    d.zone = Zone::None;
    d.target = nullptr;
    Pane* p = paneAt(mx_, my_);
    if (!p) return;
    d.target = p;
    if (p->tabsR.contains(mx_, my_)) {
        d.zone = Zone::Tabs;
        int idx = 0;
        for (std::size_t i = 0; i < p->tabs.size() && i < p->hits.size(); ++i) {
            if (p->tabs[i] == d.doc) continue;
            const Rect& t = p->hits[i].tab;
            if (mx_ > t.x + t.w / 2) ++idx;
        }
        d.insertAt = idx;
        return;
    }
    const Rect body{p->rect.x, p->tabsR.bottom(), p->rect.w, std::max(1.0f, p->rect.h - p->tabsR.h)};
    const float fx = (mx_ - body.x) / body.w, fy = (my_ - body.y) / body.h;
    Zone z = Zone::Center;
    if (fx < 0.28f) z = Zone::Left;
    else if (fx > 0.72f) z = Zone::Right;
    else if (fy < 0.28f) z = Zone::Top;
    else if (fy > 0.72f) z = Zone::Bottom;
    if (p == d.from && p->tabs.size() == 1) z = Zone::Center;  // nothing to split off
    d.zone = z;
}

void App::moveTab(Document* doc, Pane* to, int at) {
    if (Pane* from = paneOfDoc(doc)) std::erase(from->tabs, doc);
    at = std::clamp(at, 0, static_cast<int>(to->tabs.size()));
    to->tabs.insert(to->tabs.begin() + at, doc);
    to->cur = doc;
}

void App::splitPane(Pane* target, Zone z, Document* doc) {
    Node* leafNode = nodeOfPane(target);
    if (!leafNode) return;
    if (Pane* from = paneOfDoc(doc)) std::erase(from->tabs, doc);
    auto fresh = std::make_unique<Pane>();
    fresh->tabs = {doc};
    fresh->cur = doc;
    auto oldLeaf = std::make_unique<Node>();
    oldLeaf->pane = std::move(leafNode->pane);
    auto newLeaf = std::make_unique<Node>();
    newLeaf->pane = std::move(fresh);
    leafNode->sideBySide = z == Zone::Left || z == Zone::Right;
    leafNode->ratio = 0.5f;
    if (z == Zone::Left || z == Zone::Top) { leafNode->a = std::move(newLeaf); leafNode->b = std::move(oldLeaf); }
    else { leafNode->a = std::move(oldLeaf); leafNode->b = std::move(newLeaf); }
}

void App::performDrop() {
    const TabDrag d = tabDrag_;
    if (!d.doc || !d.target || d.zone == Zone::None) return;
    Pane* from = paneOfDoc(d.doc);
    if (!from) return;
    switch (d.zone) {
        case Zone::Tabs: moveTab(d.doc, d.target, d.insertAt); break;
        case Zone::Center: if (d.target != from) moveTab(d.doc, d.target, static_cast<int>(d.target->tabs.size())); break;
        case Zone::Left: case Zone::Right: case Zone::Top: case Zone::Bottom: splitPane(d.target, d.zone, d.doc); break;
        default: break;
    }
    const int idx = docIndex(d.doc);
    if (idx >= 0) cur_ = idx;
    resetCaret();
    scrollToCursor_ = true;
    dirty_ = true;
}

void App::drawTabDrag() {
    if (!tabDrag_.active || !tabDrag_.doc) return;
    const float s = S();
    if (Pane* p = tabDrag_.target; p && tabDrag_.zone != Zone::None) {
        const Rect body{p->rect.x, p->tabsR.bottom(), p->rect.w, std::max(1.0f, p->rect.h - p->tabsR.h)};
        Rect hl = body;
        switch (tabDrag_.zone) {
            case Zone::Left: hl = {body.x, body.y, body.w / 2, body.h}; break;
            case Zone::Right: hl = {body.x + body.w / 2, body.y, body.w / 2, body.h}; break;
            case Zone::Top: hl = {body.x, body.y, body.w, body.h / 2}; break;
            case Zone::Bottom: hl = {body.x, body.y + body.h / 2, body.w, body.h / 2}; break;
            case Zone::Center: hl = {body.x + 6 * s, body.y + 6 * s, body.w - 12 * s, body.h - 12 * s}; break;
            default: break;
        }
        if (tabDrag_.zone == Zone::Tabs) {
            // Insertion marker between tabs.
            float x = p->tabsR.x + 8 * s;
            int seen = 0;
            for (std::size_t i = 0; i < p->tabs.size() && i < p->hits.size(); ++i) {
                if (p->tabs[i] == tabDrag_.doc) continue;
                if (seen == tabDrag_.insertAt) break;
                x = p->hits[i].tab.right() + 1 * s;
                ++seen;
            }
            fill(x - s, p->tabsR.y + 6 * s, std::round(2.5f * s), p->tabsR.h - 10 * s, theme::accent, std::round(s));
        } else {
            fill(hl, theme::accent.alpha(46), 10 * s);
            const float t = std::max(1.5f, std::round(1.5f * s));
            fill(hl.x, hl.y, hl.w, t, theme::accent.alpha(150)); fill(hl.x, hl.bottom() - t, hl.w, t, theme::accent.alpha(150));
            fill(hl.x, hl.y, t, hl.h, theme::accent.alpha(150)); fill(hl.right() - t, hl.y, t, hl.h, theme::accent.alpha(150));
        }
    }
    const std::string name = tabDrag_.doc->name;
    const float w = tabNaturalW(name), h = std::round(uiH() + 12 * s);
    Rect r{mx_ - tabDrag_.grabX, my_ - h / 2, w, h};
    glassPanel(r, 9 * s);
    drawFileIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name);
    uiLabel(r.x + 12 * s + iconSize() + 8 * s, r.y, h, name, theme::text, r.right() - 10 * s);
}

void App::onPointerButton(Button b, bool pressed, float lx, float ly) {
    mx_ = lx * S();
    my_ = ly * S();
    dirty_ = true;
    if (dlg_.kind != DlgKind::None && !(L_.csd && L_.title.contains(mx_, my_))) {   // dialogs are modal (window controls stay live)
        if (pressed) dlgClick(b, mx_, my_);
        return;
    }
    if (pressed && !(panelOpen_ && L_.panel.contains(mx_, my_))) { runFocus_ = false; termFocus_ = false; dbgConFocus_ = false; psel_.active = false; gitMsgFocus_ = false; if (!(sidebar_ && L_.side.contains(mx_, my_))) sFocus_ = -1; }
    if (hover_.show && !hover_.lines.empty() && !menu_.open && hoverRect_.contains(mx_, my_)) {   // the tooltip is selectable
        if (pressed && b == Button::Left) {
            if (hoverCopyRect_.contains(mx_, my_)) { std::string t; for (std::size_t i = 0; i < hover_.lines.size(); ++i) t += (i ? "\n" : "") + hover_.lines[i].first; win_.setClipboard(t); setMessage("Copied", false, 1.2f); }
            else { hsel_ = {}; hsel_.active = true; hsel_.dragging = true; hoverSelAnchor(); }
        }
        return;
    }
    if (pressed && b == Button::Left && !menu_.open && dbgBarClick()) return;
    if (menu_.open) {  // the menu captures the click; clicking outside just dismisses it
        if (pressed) {
            int i = menuItemAt(mx_, my_);
            if (i >= 0) activateMenuItem(i); else if (!menu_.rect.contains(mx_, my_)) closeMenu();
        }
        return;
    }
    if (!pressed) {
        if (fdrag_.pending || fdrag_.active) {
            const bool wasDrag = fdrag_.active;
            if (wasDrag) { fdrag_.target = fileDropTarget(); finishFileDrag(); }
            else {
                fdrag_ = {};
                const auto& rows = explorer_.rows();
                if (L_.sideList.contains(mx_, my_) && dragRow_ >= 0 && dragRow_ < static_cast<int>(rows.size())
                    && static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow)) == dragRow_)
                    activateRow(*rows[static_cast<std::size_t>(dragRow_)].node);
            }
            dragRow_ = -1;
            return;
        }
        if (tsel_.dragging) { tsel_.dragging = false; if (!tsel_.moved) tsel_.active = false; }
        if (psel_.dragging) {
            psel_.dragging = false;
            if (!psel_.moved) { const int row = psel_.ar; psel_.active = false; panelRowClick(row); }
        }
        hsel_.dragging = false;
        if (tabDrag_.doc && tabDrag_.active) performDrop();
        tabDrag_ = {};
        splitDragNode_ = nullptr;
        dragging_ = scrollbarDrag_ = splitDrag_ = panelDrag_ = miniDrag_ = false;
        return;
    }
    resetCaret();
    sideFocus_ = sidebar_ && L_.side.contains(mx_, my_);
    if (sidebar_ && b == Button::Left && L_.split.contains(mx_, my_)) { splitDrag_ = true; return; }
    if (sidebar_ && L_.side.contains(mx_, my_)) {
        if (sideDebug_) { debugClick(b); return; }
        if (sideSearch_) { searchClick(b); return; }
        if (sideGit_) {   // the Git view handles its own hits
            if (b == Button::Left && L_.sideList.contains(mx_, my_)) gitPanelClick(); else gitMsgFocus_ = false;
            return;
        }
        if (!explorer_.isOpen()) { if (b == Button::Left) browseOpen(false); return; }
        const auto& rows = explorer_.rows();
        int row = -1;
        if (L_.sideList.contains(mx_, my_)) {
            row = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
            if (row < 0 || row >= static_cast<int>(rows.size())) row = -1;
        }
        if (b == Button::Right) {
            sideFocus_ = true;
            if (row >= 0) {
                std::string p = rows[static_cast<std::size_t>(row)].node->path.string();
                explorer_.select(p);
                openMenu(mx_, my_, &p);
            } else {
                explorer_.select(explorer_.rootPath().string());
                openMenu(mx_, my_, nullptr);
            }
        } else if (b == Button::Left) {
            if (row >= 0) {   // opens / toggles on release, unless the press turns into a drag
                FileNode& nd = *rows[static_cast<std::size_t>(row)].node;
                explorer_.select(nd.path.string());
                fdrag_ = {};
                fdrag_.path = nd.path; fdrag_.pending = true; fdrag_.sx = mx_; fdrag_.sy = my_;
                dragRow_ = row;
            }
            else explorer_.select(explorer_.rootPath().string());  // header / empty area targets the root
        }
        return;
    }

    if (Edge e = edgeAt(mx_, my_); e != Edge::None && b == Button::Left) { win_.startResize(e); return; }

    if (L_.title.contains(mx_, my_) && L_.csd) {
        if (cmakeOpen_ && toolAt(mx_, my_) > 0) {  // the toolbar shares this row
            if (b == Button::Left) toolbarClick();
            return;
        }
        if (b != Button::Left) return;
        switch (titleButtonAt(mx_, my_)) {
            case 1: win_.minimize(); return;
            case 2: win_.toggleMaximize(); return;
            case 3: requestQuit(); return;
            default: break;
        }
        if (mx_ < std::round(14 * S()) + std::max(14.0f, std::round(L_.title.h * 0.5f)) + 10 * S()) { welcomeOpen(); return; }   // logo: projects
        auto now = Clock::now();
        if (now - lastTitleClick_ < std::chrono::milliseconds(400)) { win_.toggleMaximize(); lastTitleClick_ = {}; }
        else { lastTitleClick_ = now; win_.startMove(); }
        return;
    }

    if (int act = activityAt(mx_, my_); act > 0) {
        if (b == Button::Left) activityClick(act - 1);
        return;
    }
    if (cmakeOpen_ && L_.toolbar.contains(mx_, my_)) {
        if (b == Button::Left) toolbarClick();
        return;
    }
    if (panelOpen_ && L_.panel.contains(mx_, my_)) { panelClick(b); return; }

    if (b == Button::Left)
        if (Node* g = gapAt(mx_, my_)) { splitDragNode_ = g; return; }
    if (Pane* pp = paneAt(mx_, my_)) {
        if (pp != activePane_ && pp->cur) {  // clicking another editor pane focuses it
            cur_ = std::max(0, docIndex(pp->cur));
            revealCurrent();
            resetCaret();
            layout();
        }
        if (pp->tabsR.contains(mx_, my_)) {
            for (std::size_t i = 0; i < pp->hits.size() && i < pp->tabs.size(); ++i) {
                if (!pp->hits[i].tab.contains(mx_, my_)) continue;
                Document* dd = pp->tabs[i];
                const int idx = docIndex(dd);
                if (b == Button::Middle || (b == Button::Left && pp->hits[i].close.contains(mx_, my_))) closeTab(idx);
                else if (b == Button::Left) {
                    switchTab(idx);
                    tabDrag_ = {};
                    tabDrag_.doc = dd;
                    tabDrag_.from = pp;
                    tabDrag_.sx = mx_; tabDrag_.sy = my_;
                    tabDrag_.grabX = mx_ - pp->hits[i].tab.x;
                }
                return;
            }
            return;
        }
    }

    if (docs_.empty() || !L_.editor.contains(mx_, my_)) return;
    Document& d = doc();
    if (b == Button::Right && mx_ < L_.textX + 0 && L_.editor.contains(mx_, my_)) { openEditorMenu(posAt(d, mx_, my_)); return; }
    if (b == Button::Right) {
        const Pos pp = posAt(d, mx_, my_);
        if (!d.hasSelection() || pp < d.selBegin() || pp > d.selEnd()) d.setCursor(pp);
        openEditorMenu(pp);
        return;
    }
    if (b != Button::Left) return;
    if (inScrollbar(d, mx_, my_)) { scrollbarDrag_ = true; dragStep(d); return; }
    if (inMinimap(mx_, my_)) { miniDrag_ = true; miniJump(d); return; }
    if (mx_ < L_.textX) {  // gutter: breakpoint toggle in the left strip, otherwise select the whole line
        int line = posAt(d, mx_, my_).line;
        if (mx_ < L_.editor.x + 1.9f * cw()) { dbgToggleBpAt(d.path, line + 1); return; }
        d.selectLine(line);
        dragging_ = false;
        return;
    }

    if (win_.ctrlDown() && lsp_ && lsp_->ready()) {  // Ctrl+click: go to definition
        d.setCursor(posAt(d, mx_, my_));
        gotoDefinition();
        return;
    }
    auto now = Clock::now();
    Pos p = posAt(d, mx_, my_);
    bool near = std::abs(mx_ - lastClickX_) < 4 * S() && std::abs(my_ - lastClickY_) < 4 * S();
    clickCount_ = (near && now - lastClick_ < std::chrono::milliseconds(450)) ? clickCount_ % 3 + 1 : 1;
    lastClick_ = now;
    lastClickX_ = mx_;
    lastClickY_ = my_;
    switch (clickCount_) {
        case 1: d.setCursor(p, win_.shiftDown()); dragging_ = true; break;
        case 2: { auto [a, e] = d.wordRange(p); d.select(a, e); break; }
        default: d.selectLine(p.line); break;
    }
}

void App::onScroll(float dxPx, float dyPx, int stepsX, int stepsY) {
    if (dlg_.kind != DlgKind::None) { dlgScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * 34 * S() : dyPx * S()); dirty_ = true; return; }
    if (menu_.open) closeMenu();
    hideHover();
    if (panelOpen_ && panelTab_ == 4 && L_.panel.contains(mx_, my_)) {
        panelScroll_[4] = std::max(0.0f, panelScroll_[4] + (stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S()));
        panelFollow_[4] = false;
        dirty_ = true;
        return;
    }
    if (panelOpen_ && panelTab_ == 3 && L_.panel.contains(mx_, my_)) {
        const int n = stepsY != 0 ? stepsY * 3 : static_cast<int>(std::round(dyPx * S() / lh()));
        if (term_.altScreen() && termProc_) {   // full-screen programs get arrow keys instead of scrollback
            const std::string key = n > 0 ? "\x1b[A" : "\x1b[B";
            std::string out;
            for (int i = 0; i < std::min(std::abs(n), 10); ++i) out += key;
            termProc_->write(out);
        }
        else termScroll_ = std::clamp(termScroll_ - n, 0, term_.scrollbackSize());
        dirty_ = true;
        return;
    }
    if (panelOpen_ && L_.panel.contains(mx_, my_)) {
        auto t = static_cast<std::size_t>(panelTab_);
        float dy = stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S();
        panelScroll_[t] = std::clamp(panelScroll_[t] + dy, 0.0f, panelMaxScroll());
        panelFollow_[t] = panelScroll_[t] >= panelMaxScroll() - 1;
        dirty_ = true;
        return;
    }
    if (debugView() && L_.side.contains(mx_, my_)) { debugScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S()); return; }
    if (searchView() && L_.side.contains(mx_, my_)) { searchScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S()); return; }
    if (gitView() && L_.side.contains(mx_, my_)) {
        gitScroll_ = std::clamp(gitScroll_ + (stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S()), 0.0f, gitMax_);
        dirty_ = true;
        return;
    }
    if (sidebar_ && L_.side.contains(mx_, my_)) {
        sideTarget_ += stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S();
        clampSide();
        dirty_ = true;
        return;
    }
    if (Pane* pp = paneAt(mx_, my_); pp && pp != activePane_ && pp->cur) { cur_ = std::max(0, docIndex(pp->cur)); layout(); }
    if (docs_.empty()) return;
    Document& d = doc();
    if (win_.ctrlDown()) {
        float v = static_cast<float>(stepsY ? stepsY : (dyPx > 0 ? 1 : dyPx < 0 ? -1 : 0));
        if (v != 0) zoom(-v);
        return;
    }
    if (win_.shiftDown() && dyPx != 0 && dxPx == 0) { dxPx = dyPx; dyPx = 0; stepsX = stepsY; stepsY = 0; }
    float dy = stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S();
    float dx = stepsX != 0 ? static_cast<float>(stepsX) * 6 * cw() : dxPx * S();
    d.targetY += dy;
    d.scrollX += dx;
    d.scrollX = std::max(0.0f, d.scrollX);
    clampScroll(d);
    dirty_ = true;
}

}  // namespace ide
