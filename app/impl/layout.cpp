// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / layout - part of the App implementation (see app.cppm for the class).
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

void App::syncTabs(Pane& p) {
    std::unordered_set<const Document*> live(p.tabs.begin(), p.tabs.end());
    std::unordered_map<const Document*, TabAnim> liveMap;
    std::vector<TabAnim> ghosts;
    const Document* lastLive = nullptr;
    for (auto& e : p.anim) {
        if (e.ghost) { if (e.w >= 0.6f) ghosts.push_back(std::move(e)); continue; }
        if (live.contains(e.doc)) { lastLive = e.doc; liveMap.emplace(e.doc, std::move(e)); }
        else { e.ghost = true; e.target = 0; e.anchor = lastLive; ghosts.push_back(std::move(e)); }
    }
    std::vector<TabAnim> next;
    auto emitGhosts = [&](const Document* anchor) {
        for (auto& g : ghosts)
            if (!g.used && g.anchor == anchor) { g.used = true; next.push_back(g); }
    };
    emitGhosts(nullptr);
    for (Document* d : p.tabs) {
        TabAnim e;
        if (auto it = liveMap.find(d); it != liveMap.end()) e = std::move(it->second);
        else { e.doc = d; e.w = 0; e.x = -1; }
        e.name = d->name;
        e.target = tabNaturalW(e.name);
        e.ghost = false;
        next.push_back(std::move(e));
        emitGhosts(d);
    }
    for (auto& g : ghosts) if (!g.used) next.push_back(g);
    p.anim = std::move(next);
    if (frames_ < 2 || snapAnimations())
        for (auto& e : p.anim) { e.w = e.target; e.x = -1; }
}

bool App::stepAnimations(float dt) {
    bool moving = false;
    const bool snap = snapAnimations();
    auto chase = [&](float& v, float target, float rate) {
        if (snap || std::abs(target - v) < 0.002f * std::max(1.0f, std::abs(target))) { v = target; return; }
        v += (target - v) * (1.0f - std::exp(-dt * rate));
        moving = true;
    };
    chase(panelAnim_, panelOpen_ ? 1.0f : 0.0f, 14.0f);
    chase(sideAnim_, sidebar_ ? 1.0f : 0.0f, 14.0f);
    chase(bar_.anim, bar_.shown ? 1.0f : 0.0f, 12.0f);
    for (Node* n : leaves())
        for (auto& e : n->pane->anim) {
            if (snap || std::abs(e.target - e.w) < 0.4f) e.w = e.target;
            else { e.w += (e.target - e.w) * (1.0f - std::exp(-dt * 16.0f)); moving = true; }
            if (snap || std::abs(e.tx - e.x) < 0.4f) e.x = e.tx;
            else { e.x += (e.tx - e.x) * (1.0f - std::exp(-dt * 18.0f)); moving = true; }
        }
    return moving;
}

void App::layout() {
    const float W = static_cast<float>(wLog_ * scale_), H = static_cast<float>(hLog_ * scale_);
    const float s = S(), uh = uiH();
    L_.csd = !win_.serverDecorations() && !win_.fullscreen();
    const float titleH = L_.csd ? std::round(uh + 18 * s) : 0;
    const float tabH = std::round(uh + 20 * s);
    const float crumbH = (!docs_.empty() && !doc().path.empty()) ? std::round(uh + 8 * s) : 0;
    const float statusH = std::round(uh + 10 * s);
    const float m = std::round(5 * s);     // gap between the frame and the islands
    const float act = std::round(50 * s);  // activity bar width
    L_.rowH = std::round(uh + 14 * s);
    L_.radius = std::round(12 * s);
    const float promptH = (prompt_.kind == PromptKind::None || sidePrompt()) ? 0 : L_.rowH * static_cast<float>(prompt_.fields.size());
    const float tbH = cmakeOpen_ ? std::round(uh + 18 * s) : 0;
    // With client-side decorations the toolbar shares the title-bar row; otherwise it is its own row.
    L_.toolbar = L_.csd ? Rect{0, 0, W, titleH} : Rect{0, 0, W, tbH};
    const float chromeBottom = L_.csd ? titleH : tbH;
    const float top = chromeBottom > 0 ? chromeBottom + std::round(1 * s) : m;
    L_.title = {0, 0, W, titleH};
    L_.status = {0, H - statusH, W, statusH};
    const float h = std::max(0.0f, L_.status.y - top);
    // The output panel is its own island below the editor island.
    float panelH = 0;
    const bool panelShown = panelOpen_ || panelAnim_ > 0.002f;
    if (panelShown) panelH = std::round(std::clamp(std::round(panelH_ * s), 90 * s, std::max(90 * s, h * 0.6f)) * easeInOut(panelAnim_));
    const float panelTotal = panelH > 0 ? panelH + m * easeInOut(panelAnim_) : 0;
    const float contentRight = W - act;
    L_.activity = {contentRight, top, act, h};
    float sbW = 0;
    const bool sideShown = sidebar_ || sideAnim_ > 0.002f;
    if (sideShown) sbW = std::clamp(sidebarWidth_ * s, 150 * s, std::max(150 * s, W * 0.5f)) * easeInOut(sideAnim_);
    const float mainW = std::max(120 * s, contentRight - m - (sideShown ? sbW + m * easeInOut(sideAnim_) : 0));
    L_.island = {m, top, mainW, std::max(0.0f, h - panelTotal)};
    L_.side = sideShown ? Rect{L_.island.right() + m, top, std::max(0.0f, contentRight - (L_.island.right() + m)), h} : Rect{};
    L_.split = sideShown ? Rect{L_.island.right(), top, m, h} : Rect{};
    L_.sideRow = std::round(uh + 10 * s);
    L_.outline = L_.outlineHead = L_.outlineList = L_.outlineSplit = {};
    float outlineTotal = 0;
    if (sideShown && outlineVisible()) {   // the structure view takes the lower part of the explorer
        const float avail = std::max(0.0f, L_.side.h - tabH - sideCardH());
        const float headH = std::round(uh + 14 * s);
        float bodyH = 0;
        if (outlineOpen_) bodyH = std::clamp(std::round(static_cast<float>(cfgI("outline.heightPercent")) / 100.0f * avail), std::round(90 * s), std::max(std::round(90 * s), avail - std::round(120 * s) - headH));
        outlineTotal = std::min(avail, headH + bodyH);
        const float oy = L_.side.bottom() - sideCardH() - outlineTotal;
        L_.outline = {L_.side.x, oy, L_.side.w, outlineTotal};
        L_.outlineHead = {L_.side.x, oy, L_.side.w, std::min(headH, outlineTotal)};
        L_.outlineList = {L_.side.x, oy + headH, L_.side.w, std::max(0.0f, outlineTotal - headH)};
        L_.outlineSplit = outlineOpen_ ? Rect{L_.side.x, oy - std::round(4 * s), L_.side.w, std::round(8 * s)} : Rect{};
    }
    L_.sideList = sideShown ? Rect{L_.side.x, L_.side.y + tabH, L_.side.w, std::max(0.0f, L_.side.h - tabH - sideCardH() - outlineTotal)} : Rect{};
    L_.tabs = {L_.island.x, L_.island.y, L_.island.w, tabH};
    L_.panel = {L_.island.x, L_.island.bottom() + (panelH > 0 ? m * easeInOut(panelAnim_) : 0), L_.island.w, panelH};
    (void)crumbH;

    // Toolbar hit rectangles.
    tool_ = {};
    if (cmakeOpen_) {
        const float cy = L_.toolbar.y + std::round(4 * s), ch = L_.toolbar.h - std::round(8 * s);
        auto chipW = [&](const std::string& t) { return uiW(t) + std::round(56 * s); };
        barLayout();
        float x = L_.csd ? std::round(46 * s) + barShift() : m;
        const std::string pn = profiles_.empty() ? "-" : profiles_[static_cast<std::size_t>(profile_)].name;
        const std::string tn = target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : "No target";
        tool_.profile = {x, cy, chipW(pn), ch};
        x += tool_.profile.w + std::round(6 * s);
        tool_.target = {x, cy, chipW(tn), ch};
        x += tool_.target.w + std::round(14 * s);
        // Action cluster: one pill holding Run | Debug | Build | Regenerate | Stop (groups separated by dividers).
        const float bw = std::round(ch * 1.08f), div = std::round(9 * s), pad = std::round(2 * s);
        const float cw_ = pad + bw * 5 + div * 2 + pad;
        tool_.cluster = {x, cy, cw_, ch};
        float bx = x + pad;
        tool_.run = {bx, cy, bw, ch}; bx += bw;
        tool_.debug = {bx, cy, bw, ch}; bx += bw + div;
        tool_.build = {bx, cy, bw, ch}; bx += bw;
        tool_.reload = {bx, cy, bw, ch}; bx += bw + div;
        tool_.stop = {bx, cy, bw, ch};
    }

    // Editor panes: split tree inside the island, each pane with its own tab strip, breadcrumbs, gutter and minimap.
    syncPanes();
    layoutTree(*root_, L_.island, m);
    for (Node* n : leaves()) layoutPane(*n->pane, n->pane.get() == activePane_, promptH);
    {
        Pane& ap = *activePane_;   // the rest of the app works on the active pane through L_
        L_.tabs = ap.tabsR; L_.crumbs = ap.crumbs; L_.editor = ap.editor; L_.minimap = ap.minimap;
        L_.gutterW = ap.gutterW; L_.textX = ap.textX; L_.textW = ap.textW;
        L_.prompt = {ap.rect.x, ap.editor.bottom(), ap.rect.w, promptH};
    }
}

auto App::leaves() -> std::vector<Node*> {
    std::vector<Node*> out;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n) return;
        if (n->leaf()) out.push_back(n);
        else { walk(n->a.get()); walk(n->b.get()); }
    };
    walk(root_.get());
    return out;
}

auto App::splits() -> std::vector<Node*> {
    std::vector<Node*> out;
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n || n->leaf()) return;
        out.push_back(n);
        walk(n->a.get());
        walk(n->b.get());
    };
    walk(root_.get());
    return out;
}

auto App::paneOfDoc(const Document* d) -> Pane* {
    for (Node* n : leaves())
        if (std::ranges::find(n->pane->tabs, d) != n->pane->tabs.end()) return n->pane.get();
    return nullptr;
}

auto App::gapAt(float x, float y) -> Node* {
    const float pad = 3 * S();
    for (Node* n : splits()) {
        Rect g = n->gap;
        g.x -= pad; g.y -= pad; g.w += 2 * pad; g.h += 2 * pad;
        if (g.contains(x, y)) return n;
    }
    return nullptr;
}

bool App::pruneNode(std::unique_ptr<Node>& n) {   // true when the subtree holds no tabs at all
    if (n->leaf()) return n->pane->tabs.empty();
    const bool ea = pruneNode(n->a), eb = pruneNode(n->b);
    if (ea && eb) return true;
    if (ea) { auto keep = std::move(n->b); n = std::move(keep); }
    else if (eb) { auto keep = std::move(n->a); n = std::move(keep); }
    return false;
}

void App::syncPanes() {
    if (!root_) {
        root_ = std::make_unique<Node>();
        root_->pane = std::make_unique<Pane>();
        activePane_ = root_->pane.get();
    }
    std::unordered_set<Document*> alive;
    for (auto& d : docs_) alive.insert(d.get());
    std::unordered_set<Document*> assigned;
    for (Node* n : leaves())
        std::erase_if(n->pane->tabs, [&](Document* d) { return !alive.contains(d) || !assigned.insert(d).second; });
    if (!std::ranges::contains(leaves(), activePane_, [](Node* n) { return n->pane.get(); })) activePane_ = leaves().front()->pane.get();
    for (auto& d : docs_)
        if (!assigned.contains(d.get())) activePane_->tabs.push_back(d.get());
    if (pruneNode(root_)) { /* no documents: keep the (empty) root */ }
    if (!std::ranges::contains(leaves(), activePane_, [](Node* n) { return n->pane.get(); })) activePane_ = leaves().front()->pane.get();
    if (!docs_.empty()) {
        Document* c = docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))].get();
        if (Pane* p = paneOfDoc(c)) { activePane_ = p; p->cur = c; }
    }
    for (Node* n : leaves()) {
        Pane& p = *n->pane;
        if (p.cur && std::ranges::find(p.tabs, p.cur) == p.tabs.end()) p.cur = nullptr;
        if (!p.cur && !p.tabs.empty()) p.cur = p.tabs.back();
    }
}

void App::layoutTree(Node& n, Rect r, float gap) {
    n.rect = r;
    if (n.leaf()) { n.pane->rect = r; return; }
    const float s = S();
    if (n.sideBySide) {
        const float usable = std::max(0.0f, r.w - gap);
        n.ratio = std::clamp(n.ratio, std::min(0.5f, 160 * s / std::max(1.0f, usable)), std::max(0.5f, 1.0f - 160 * s / std::max(1.0f, usable)));
        const float wa = std::round(usable * n.ratio);
        n.gap = {r.x + wa, r.y, gap, r.h};
        layoutTree(*n.a, {r.x, r.y, wa, r.h}, gap);
        layoutTree(*n.b, {r.x + wa + gap, r.y, usable - wa, r.h}, gap);
    } else {
        const float usable = std::max(0.0f, r.h - gap);
        n.ratio = std::clamp(n.ratio, std::min(0.5f, 120 * s / std::max(1.0f, usable)), std::max(0.5f, 1.0f - 120 * s / std::max(1.0f, usable)));
        const float ha = std::round(usable * n.ratio);
        n.gap = {r.x, r.y + ha, r.w, gap};
        layoutTree(*n.a, {r.x, r.y, r.w, ha}, gap);
        layoutTree(*n.b, {r.x, r.y + ha + gap, r.w, usable - ha}, gap);
    }
}

void App::layoutPane(Pane& p, bool active, float promptH) {
    const float s = S(), uh = uiH();
    const float tabH = std::round(uh + 20 * s);
    // A file directly in the project root would only repeat its tab title, so it gets no breadcrumb row.
    const float crumbH = (cfgB("editor.breadcrumbs") && p.cur && !p.cur->path.empty() && crumbParts(*p.cur).size() > 1) ? std::round(uh + 8 * s) : 0;
    const Rect& r = p.rect;
    p.tabsR = {r.x, r.y, r.w, std::min(tabH, r.h)};
    p.crumbs = {r.x, p.tabsR.bottom(), r.w, std::min(crumbH, std::max(0.0f, r.h - tabH))};
    float bottom = r.bottom();
    if (active) bottom -= promptH;
    p.editor = {r.x, p.crumbs.bottom(), r.w, std::max(0.0f, bottom - p.crumbs.bottom())};
    const float mapW = cfgB("editor.minimap") && r.w > 640 * s && !(p.cur && (isPreview(*p.cur) || isNotebook(*p.cur))) ? std::round(92 * s) : 0;
    p.minimap = {p.editor.right() - mapW, p.editor.y, mapW, p.editor.h};
    const int lines = p.cur ? p.cur->buf.lineCount() : 1;
    const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
    p.gutterW = (static_cast<float>(digits) + 3.4f) * cw();
    p.textX = p.editor.x + p.gutterW;
    p.textW = std::max(0.0f, p.editor.w - p.gutterW - mapW - 12 * s);

    // Tab strip: animated entries left to right (closed tabs shrink away as ghosts).
    syncTabs(p);
    p.draw.clear();
    p.hits.clear();
    float x = 8 * s;   // offsets are relative to the strip, so tabs move rigidly with their pane
    const float gap = 2 * s;
    int idx = 0;
    // Too many tabs: first shrink them (down to a minimum), then show a window of them that follows the current tab
    // (the mouse wheel over the strip moves it). Tabs outside the window are not drawn.
    std::vector<TabAnim*> liveTabs;
    for (auto& e : p.anim) if (!e.ghost) liveTabs.push_back(&e);
    std::unordered_set<const TabAnim*> hidden;
    {
        const int n = static_cast<int>(liveTabs.size());
        const float avail = std::max(0.0f, p.tabsR.w - 16 * s);
        float sum = 0;
        for (auto* e : liveTabs) sum += e->target;
        const float gaps = gap * static_cast<float>(std::max(0, n - 1));
        if (n > 0 && sum + gaps > avail) {
            const float minW = 110 * s, f = std::max(0.0f, (avail - gaps) / sum);
            for (auto* e : liveTabs) e->target = std::min(e->target, std::max(minW, e->target * f));
            float total = gaps;
            for (auto* e : liveTabs) total += e->target;
            if (total > avail) {   // still too wide: a window of tabs
                int curIdx = 0;
                for (int i = 0; i < n; ++i) if (liveTabs[static_cast<std::size_t>(i)]->doc == p.cur) curIdx = i;
                auto fits = [&](int first, int upto) {
                    float w = 0;
                    for (int i = first; i <= upto; ++i) w += liveTabs[static_cast<std::size_t>(i)]->target + (i > first ? gap : 0);
                    return w <= avail;
                };
                if (p.tabLastCur != p.cur) p.tabWheeled = false;
                if (!p.tabWheeled) {   // keep the current tab in view (until the user scrolls the strip by hand)
                    p.tabFirst = std::min(p.tabFirst, curIdx);
                    while (p.tabFirst < curIdx && !fits(p.tabFirst, curIdx)) ++p.tabFirst;
                }
                p.tabFirst = std::clamp(p.tabFirst, 0, n - 1);
                while (p.tabFirst > 0 && fits(p.tabFirst - 1, n - 1)) --p.tabFirst;   // do not scroll past the end
                int last = p.tabFirst;
                while (last + 1 < n && fits(p.tabFirst, last + 1)) ++last;
                for (int i = 0; i < n; ++i)
                    if (i < p.tabFirst || i > last) hidden.insert(liveTabs[static_cast<std::size_t>(i)]);
            } else p.tabFirst = 0;
        } else p.tabFirst = 0;
        p.tabLastCur = p.cur;
    }
    if (frames_ < 2 || snapAnimations())
        for (auto* e : liveTabs) e->w = e->target;   // no animation: use the fitted widths right away
    for (auto& e : p.anim) {
        if (hidden.contains(&e)) {   // out of the window: no room, no drawing, no hit
            e.w = e.target;
            p.hits.push_back({Rect{-100000, 0, 0, 0}, Rect{-100000, 0, 0, 0}});
            ++idx;
            continue;
        }
        e.tx = x;
        if (e.x < -0.5f) e.x = x;
        const float w = std::max(0.0f, e.w);
        Rect t{p.tabsR.x + e.x, p.tabsR.y + 5 * s, w, p.tabsR.h - 5 * s};
        Rect c{t.right() - 28 * s, t.y, 24 * s, t.h};
        const float alpha = e.target > 0 ? std::clamp(w / e.target, 0.0f, 1.0f) : std::clamp(w / (96 * s), 0.0f, 1.0f);
        p.draw.push_back({t, c, e.ghost ? -1 : idx, e.name, alpha, e.doc});
        if (!e.ghost) { p.hits.push_back({t, c}); ++idx; }
        x += w + gap;
    }
}

auto App::enterPane(Pane& p) -> PaneCtx {
    PaneCtx sv{L_.tabs, L_.crumbs, L_.editor, L_.minimap, L_.gutterW, L_.textX, L_.textW, cur_};
    L_.tabs = p.tabsR; L_.crumbs = p.crumbs; L_.editor = p.editor; L_.minimap = p.minimap;
    L_.gutterW = p.gutterW; L_.textX = p.textX; L_.textW = p.textW;
    if (p.cur) cur_ = std::max(0, docIndex(p.cur));
    std::swap(mini_, p.mini);
    return sv;
}

void App::leavePane(Pane& p, const PaneCtx& sv) {
    std::swap(mini_, p.mini);
    L_.tabs = sv.tabs; L_.crumbs = sv.crumbs; L_.editor = sv.editor; L_.minimap = sv.minimap;
    L_.gutterW = sv.g; L_.textX = sv.tx; L_.textW = sv.tw;
    cur_ = sv.cur;
}

void App::clampScroll(Document& d) {
    float m = maxScroll(d);
    d.targetY = std::clamp(d.targetY, 0.0f, m);
    d.scrollY = std::clamp(d.scrollY, 0.0f, m);
    d.scrollX = std::max(0.0f, d.scrollX);
}

void App::ensureCursorVisible(Document& d) {
    if (isPreview(d) || isNotebook(d)) return;
    const Rect& e = L_.editor;
    if (e.h <= 0) return;
    float margin = std::min(2 * lh(), e.h / 4);
    float cy = static_cast<float>(d.cursor.line) * lh();
    if (cy - margin < d.targetY) d.targetY = std::max(0.0f, cy - margin);
    else if (cy + lh() + margin > d.targetY + e.h) d.targetY = cy + lh() + margin - e.h;
    float cx = colX(d, d.cursor.line, d.cursor.col);
    if (cx < d.scrollX) d.scrollX = std::max(0.0f, cx - 6 * cw());
    else if (cx + 2 * cw() > d.scrollX + L_.textW) d.scrollX = cx + 2 * cw() - L_.textW + 6 * cw();
}

Pos App::posAt(Document& d, float mx, float my) const {
    float y = my - L_.editor.y + std::round(d.scrollY);
    int line = std::clamp(static_cast<int>(std::floor(y / lh())), 0, d.buf.lineCount() - 1);
    float x = mx - L_.textX + std::round(d.scrollX);
    const int dcol = dcolFromX(d, line, x);
    return {line, byteColAt(d.buf.line(line), dcol, d.tabWidth)};
}

}  // namespace ide
