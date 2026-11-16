// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / outline_view - the structure view: the symbols of the current file (from clangd, or the headings of a Markdown
// file) in the lower part of the explorer, with icons per kind, the symbol under the cursor marked, click to jump.
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.theme;
import daemon.text.document;
import daemon.ui.icons;
import daemon.services.lsp;
import daemon.services.glsl;
import daemon.services.settings;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

namespace {

constexpr int kHeading = 1000;   // + level: Markdown headings use their own kinds

bool isMarkdownPath(const std::string& p) {
    const auto dot = p.rfind('.');
    if (dot == std::string::npos) return false;
    std::string e = p.substr(dot);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".md" || e == ".markdown";
}

// Markdown headings as a tree (level 2 under level 1, ...), skipping fenced code blocks.
std::vector<LspSymbol> markdownSymbols(const Buffer& b) {
    std::vector<LspSymbol> top;
    std::vector<std::pair<int, LspSymbol*>> stack;   // level, node
    bool fence = false;
    const int n = b.lineCount();
    for (int i = 0; i < n; ++i) {
        const std::string& l = b.line(i);
        if (l.starts_with("```") || l.starts_with("~~~")) { fence = !fence; continue; }
        if (fence || l.empty() || l[0] != '#') continue;
        int lvl = 0;
        while (lvl < static_cast<int>(l.size()) && l[static_cast<std::size_t>(lvl)] == '#') ++lvl;
        if (lvl > 6 || lvl >= static_cast<int>(l.size()) || l[static_cast<std::size_t>(lvl)] != ' ') continue;
        std::string name = l.substr(static_cast<std::size_t>(lvl) + 1);
        while (!name.empty() && (name.back() == '#' || name.back() == ' ')) name.pop_back();
        if (name.empty()) continue;
        for (auto it = stack.begin(); it != stack.end();) { if (it->first >= lvl) { it->second->endLine = i - 1; it = stack.erase(it); } else ++it; }
        LspSymbol s;
        s.name = name; s.kind = kHeading + lvl; s.line = i; s.endLine = n - 1;
        std::vector<LspSymbol>& into = stack.empty() ? top : stack.back().second->kids;
        into.push_back(std::move(s));
        stack.emplace_back(lvl, &into.back());
        // (the pointer stays valid: the vector only grows after this node is popped)
    }
    return top;
}

}  // namespace

bool App::outlineVisible() const {
    return sidebar_ && !sideGit_ && !sideSearch_ && !sideDebug_ && cfgB("outline.show");
}

std::pair<char32_t, Color> App::outlineIcon(int kind) {
    // Muted on purpose: three quiet hues (types, callables, data) and grey for everything else.
    const Color purple = Color::rgb(0x9f8bc4), blue = Color::rgb(0x7f9fc2), orange = Color::rgb(0xc9a06a), teal = Color::rgb(0x7fb0a4),
                gray = Color::rgb(0x8a8e98), green = gray, pink = gray;
    if (kind > kHeading && kind <= kHeading + 6) return {static_cast<char32_t>(0xF0274 + (kind - kHeading - 1)), kind == kHeading + 1 ? orange : (kind == kHeading + 2 ? blue : (kind == kHeading + 3 ? teal : gray))};   // format-header-1 .. 6
    switch (kind) {
        case 5: return {0xF0832, orange};          // class
        case 23: return {0xF0B84, orange};         // struct
        case 10: return {0xF027A, orange};         // enum
        case 22: return {0xF0316, blue};           // enum member
        case 6: case 9: return {0xF01A7, purple};  // method, constructor
        case 12: return {0xF0295, purple};         // function
        case 7: case 8: return {0xF04FC, blue};    // property, field
        case 13: return {0xF0AE7, blue};           // variable
        case 14: return {0xF01FD, blue};           // constant
        case 3: case 2: case 4: return {0xF03D6, gray};   // namespace, module, package
        case 11: return {0xF0339, teal};           // interface (concept)
        case 26: return {0xF0C26, teal};           // type parameter
        case 25: return {0xF065E, pink};           // operator
        case 24: return {0xF06D5, orange};         // event
        case 1: return {0xF0224, gray};            // file
        case 15: case 16: case 17: case 18: case 19: case 20: case 21: return {0xF0A66, green};
        default: return {0xF0A66, gray};
    }
}

// ---------------------------------------------------------------------------------------------------------- data

void App::outlineTick(Clock::time_point now) {
    if (!outlineVisible() || docs_.empty()) return;
    Document& d = doc();
    if (isNotebook(d) || isPreview(d)) return;
    OutlineData& od = outlines_[&d];
    const std::uint64_t ver = d.buf.version();
    if (isMarkdownPath(d.path)) {
        if (od.version != ver) { od.tree = markdownSymbols(d.buf); od.version = ver; od.ready = true; }
        return;
    }
    if (isGlslDoc(d)) {
        if (od.version != ver) { od.tree = glsl::symbols(d.buf.text()); od.version = ver; od.ready = true; }
        return;
    }
    if (!isCppDoc(d)) return;
    if (od.version == ver || od.askedVersion == ver) return;
    if (od.due == Clock::time_point{} || od.due < now - std::chrono::seconds(30)) od.due = now + std::chrono::milliseconds(od.ready ? 600 : 100);
    if (now < od.due) return;
    auto target = lspTarget(d, d.cursor);
    if (!target) { od.due = now + std::chrono::milliseconds(700); return; }   // clangd is not up yet: look again shortly
    od.askedVersion = ver;
    const Document* dp = &d;
    target->c->documentSymbols(target->path, [this, dp, ver](std::vector<LspSymbol> tree) {
        if (!docAlive(dp)) return;
        OutlineData& o = outlines_[dp];
        if (o.version != ~0ull && ver < o.version) return;
        o.tree = std::move(tree);
        o.version = ver;
        o.ready = true;
        o.due = {};
        dirty_ = true;
    });
}

std::optional<Clock::time_point> App::outlineWake() const {
    if (!outlineVisible() || docs_.empty()) return std::nullopt;
    const Document& d = *docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))];
    auto it = outlines_.find(&d);
    if (it == outlines_.end()) return Clock::now() + std::chrono::milliseconds(50);
    const OutlineData& od = it->second;
    if (od.version != d.buf.version() && od.askedVersion != d.buf.version() && !isMarkdownPath(d.path) && isCppDoc(d))
        return od.due == Clock::time_point{} ? Clock::now() + std::chrono::milliseconds(50) : od.due;
    return std::nullopt;
}

void App::outlineRebuild() {
    outlineRows_.clear();
    if (docs_.empty()) return;
    const Document& d = doc();
    outlineRowsDoc_ = &d;
    auto it = outlines_.find(&d);
    if (it == outlines_.end()) return;
    outlineRowsVer_ = it->second.version;
    outlineRowsGen_ = outlineGen_;
    std::function<void(const std::vector<LspSymbol>&, int, const std::string&)> walk = [&](const std::vector<LspSymbol>& v, int depth, const std::string& parent) {
        for (const auto& s : v) {
            OutlineRow r;
            r.name = s.name; r.detail = s.detail; r.kind = s.kind; r.line = s.line; r.col = s.col; r.endLine = s.endLine; r.depth = depth;
            r.key = parent + "/" + s.name + ":" + std::to_string(s.kind);
            r.hasKids = !s.kids.empty();
            outlineRows_.push_back(r);
            if (r.hasKids && !outlineCollapsed_.contains(r.key)) walk(s.kids, depth + 1, r.key);
        }
    };
    walk(it->second.tree, 0, "");
}

// ---------------------------------------------------------------------------------------------------------- drawing

void App::drawOutline() {
    if (L_.outline.h <= 0) return;
    const float s = S(), hair = std::max(1.0f, std::round(0.8f * s)), rh = std::round(uiH() + 9 * s);
    const Rect& o = L_.outline;
    fill(o, theme::sideBg);
    fill(o.x + 10 * s, o.y, o.w - 20 * s, hair, theme::border);

    // header: chevron, title, number of symbols
    const Rect& hd = L_.outlineHead;
    const int cs = std::max(8, static_cast<int>(std::round(uiH() * 0.6f)));
    Icons::chevron(dl_, font_, hd.x + 14 * s, hd.y + (hd.h - static_cast<float>(cs)) / 2, cs, outlineOpen_, theme::dim);
    uiLabel(hd.x + 14 * s + static_cast<float>(cs) + 8 * s, hd.y, hd.h, "STRUCTURE", theme::dim, hd.right() - 12 * s);
    if (docs_.empty()) return;
    const Document& d = doc();
    if (!outlineOpen_ || L_.outlineList.h <= 0) {
        if (outlineRowsDoc_ == &d) uiLabelR(hd.right() - 14 * s, hd.y, hd.h, std::to_string(outlineRows_.size()), theme::lineNum);
        return;
    }

    auto it = outlines_.find(&d);
    const std::uint64_t ver = it == outlines_.end() ? ~0ull : it->second.version;
    if (outlineRowsDoc_ != &d || outlineRowsVer_ != ver || outlineRowsGen_ != outlineGen_) outlineRebuild();
    uiLabelR(hd.right() - 14 * s, hd.y, hd.h, std::to_string(outlineRows_.size()), theme::lineNum);

    const Rect& lst = L_.outlineList;
    if (outlineRows_.empty()) {
        std::string msg = "No symbols in this file";
        if (isNotebook(d) || isPreview(d)) msg = "Not available here";
        else if (isCppDoc(d) && (it == outlines_.end() || !it->second.ready)) msg = lsp_ ? "Reading the file..." : "Waiting for clangd...";
        else if (!isCppDoc(d) && !isGlslDoc(d) && !isMarkdownPath(d.path)) msg = "No structure for this file type";
        uiLabel(lst.x + 20 * s, lst.y + 6 * s, rh, msg, theme::lineNum, lst.right() - 8 * s);
        return;
    }

    // the symbol that contains the cursor: the deepest row whose lines include it (rows are in document order)
    outlineCursorRow_ = -1;
    if (!isNotebook(d) && !isPreview(d))
        for (int i = 0; i < static_cast<int>(outlineRows_.size()); ++i)
            if (outlineRows_[static_cast<std::size_t>(i)].line <= d.cursor.line && d.cursor.line <= outlineRows_[static_cast<std::size_t>(i)].endLine) outlineCursorRow_ = i;
    const float contentH = static_cast<float>(outlineRows_.size()) * rh;
    outlineMax_ = std::max(0.0f, contentH - lst.h + 4 * s);
    if (outlineCursorRow_ != outlineLastCursorRow_) {   // the cursor moved into another symbol: keep it in view
        outlineLastCursorRow_ = outlineCursorRow_;
        if (outlineCursorRow_ >= 0) {
            const float top = static_cast<float>(outlineCursorRow_) * rh;
            if (top < outlineScroll_) outlineScroll_ = top;
            else if (top + rh > outlineScroll_ + lst.h - 4 * s) outlineScroll_ = top + rh - lst.h + 4 * s;
        }
    }
    outlineScroll_ = std::clamp(outlineScroll_, 0.0f, outlineMax_);

    const float indent = std::round(14 * s), tile = std::round(rh - 6 * s);
    const int ip = std::max(11, static_cast<int>(std::round(tile * 0.74f)));
    const int first = std::max(0, static_cast<int>(std::floor(outlineScroll_ / rh)));
    const int last = std::min(static_cast<int>(outlineRows_.size()) - 1, static_cast<int>(std::floor((outlineScroll_ + lst.h) / rh)));
    for (int i = first; i <= last; ++i) {
        const OutlineRow& r = outlineRows_[static_cast<std::size_t>(i)];
        const float y = lst.y + static_cast<float>(i) * rh - outlineScroll_ + 2 * s;
        if (y + rh < lst.y || y > lst.bottom()) continue;
        const Rect rr{o.x + 6 * s, y, o.w - 12 * s, rh - 1};
        if (i == outlineCursorRow_) fill(rr, theme::accent.alpha(40), 6 * s);
        else if (i == outlineHover_) fill(rr, theme::sideHover, 6 * s);
        const float x = o.x + 12 * s + static_cast<float>(r.depth) * indent;
        if (r.hasKids) Icons::chevron(dl_, font_, x + (indent - static_cast<float>(cs)) / 2, y + (rh - static_cast<float>(cs)) / 2, cs, !outlineCollapsed_.contains(r.key), theme::dim);
        const auto [glyph, color] = outlineIcon(r.kind);
        const float tx = x + indent;
        Icons::glyph(dl_, font_, tx + (tile - static_cast<float>(ip)) / 2, y + (rh - static_cast<float>(ip)) / 2, ip, glyph, color);
        const float nx = tx + tile + 8 * s, maxX = o.right() - 12 * s;
        const float after = uiLabel(nx, y, rh, r.name, i == outlineCursorRow_ ? Color::rgb(0xffffff).mix(theme::accent, 0.25f) : Color::rgb(0xd1d4da), maxX);
        if (!r.detail.empty() && after + 14 * s < maxX) uiLabel(after + 8 * s, y, rh, r.detail, theme::lineNum, maxX);
    }
    if (outlineMax_ > 0) {
        const float thumbH = std::max(24 * s, lst.h * lst.h / (contentH + lst.h * 0.5f));
        fill(o.right() - 9 * s, lst.y + (lst.h - thumbH) * (outlineScroll_ / outlineMax_), 5 * s, thumbH, theme::scrollbar, 2.5f * s);
    }
}

// ---------------------------------------------------------------------------------------------------------- input

void App::outlineMoved() {
    int hr = -1;
    if (L_.outlineList.h > 0 && L_.outlineList.contains(mx_, my_)) {
        const float rh = std::round(uiH() + 9 * S());
        hr = static_cast<int>(std::floor((my_ - L_.outlineList.y - 2 * S() + outlineScroll_) / rh));
        if (hr < 0 || hr >= static_cast<int>(outlineRows_.size())) hr = -1;
    }
    if (hr != outlineHover_) { outlineHover_ = hr; dirty_ = true; }
}

bool App::outlineScrollBy(float dy) {
    if (L_.outline.h <= 0 || !L_.outline.contains(mx_, my_)) return false;
    outlineScroll_ = std::clamp(outlineScroll_ + dy, 0.0f, outlineMax_);
    dirty_ = true;
    return true;
}

bool App::outlineClick(Button b) {
    if (L_.outline.h <= 0) return false;
    if (b == Button::Left && L_.outlineSplit.h > 0 && L_.outlineSplit.contains(mx_, my_)) { outlineSplitDrag_ = true; return true; }
    if (!L_.outline.contains(mx_, my_)) return false;
    if (b != Button::Left) return true;
    if (L_.outlineHead.contains(mx_, my_)) { outlineOpen_ = !outlineOpen_; dirty_ = true; return true; }
    outlineMoved();
    if (outlineHover_ < 0 || docs_.empty()) return true;
    const OutlineRow r = outlineRows_[static_cast<std::size_t>(outlineHover_)];
    const float indent = std::round(14 * S());
    const float chevX = L_.outline.x + 12 * S() + static_cast<float>(r.depth) * indent;
    if (r.hasKids && mx_ < chevX + indent) {   // the chevron folds / unfolds
        if (!outlineCollapsed_.erase(r.key)) outlineCollapsed_.insert(r.key);
        ++outlineGen_;
        dirty_ = true;
        return true;
    }
    Document& d = doc();
    if (isNotebook(d) || isPreview(d)) return true;
    const int line = std::clamp(r.line, 0, d.buf.lineCount() - 1);
    d.setCursor({line, byteColFromUtf16(d.buf.line(line), r.col)});
    sideFocus_ = false;
    scrollToCursor_ = true;
    resetCaret();
    dirty_ = true;
    return true;
}

}  // namespace ide
