// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / inlay_view - inlay hints: the names of the parameters (and, optionally, deduced types) that clangd knows, drawn
// as small labels inside the line. The text after a hint moves right by the hint's width; every place that turns a column into an
// x position (selection, caret, squiggles, mouse) goes through colX / dcolFromX.
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
import daemon.services.lsp;
import daemon.services.settings;
using Clock = std::chrono::steady_clock;

namespace ide {

const std::vector<App::InlayHint>* App::inlayLine(const Document& d, int line) const {
    auto it = inlays_.find(&d);
    if (it == inlays_.end() || !it->second.ready) return nullptr;
    const InlayData& id = it->second;
    if (id.lines != d.buf.lineCount()) return nullptr;                     // lines were added or removed: the numbers no longer fit
    if (line == d.cursor.line && id.version != d.buf.version()) return nullptr;   // typing on this line: wait for fresh hints
    auto l = id.byLine.find(line);
    return l == id.byLine.end() ? nullptr : &l->second;
}

float App::inlayW(const InlayHint& h) const {
    const float key = fontSize_ * S() + static_cast<float>(cfgI("appearance.uiFontSize")) * 1000.0f;
    if (h.wFor != key) {
        h.w = const_cast<App*>(this)->uiW(h.text) + (h.kind == 2 ? std::round(13 * S()) : std::round(4 * S()));   // pill padding / a little air
        h.wFor = key;
    }
    return h.w;
}

float App::inlayShift(const Document& d, int line, int byteCol) const {
    const auto* hl = inlayLine(d, line);
    if (!hl) return 0;
    float s = 0;
    for (const auto& h : *hl) if (h.col <= byteCol) s += inlayW(h);
    return s;
}

float App::colX(const Document& d, int line, int byteCol) const {
    return static_cast<float>(displayCol(d.buf.line(line), byteCol, d.tabWidth)) * cw() + inlayShift(d, line, byteCol);
}

int App::dcolFromX(const Document& d, int line, float x) const {
    const std::string& str = d.buf.line(line);
    float shift = 0;
    if (const auto* hl = inlayLine(d, line)) {
        for (const auto& h : *hl) {
            const float hx = static_cast<float>(displayCol(str, h.col, d.tabWidth)) * cw() + shift, hw = inlayW(h);
            if (x < hx) break;
            if (x < hx + hw) return displayCol(str, h.col, d.tabWidth);   // on the label itself: the column behind it
            shift += hw;
        }
    }
    return std::max(0, static_cast<int>(std::floor((x - shift) / cw() + 0.5f)));
}

void App::drawInlay(const InlayHint& h, float x, float y) {
    const float s = S(), line = lh();
    if (h.kind == 2) {   // parameter: a small rounded label
        const float w = inlayW(h) - std::round(3 * s);
        const Rect r{x + std::round(1 * s), y + std::round(2 * s), w, line - std::round(4 * s)};
        fill(r, Color::rgb(0xffffff, 17), std::round(4 * s));
        uiLabel(r.x + std::round(4 * s), y, line, h.text, theme::dim, r.right());
    } else {
        uiLabel(x + std::round(2 * s), y, line, h.text, theme::lineNum, x + inlayW(h));
    }
}

void App::inlayTick(Clock::time_point now) {
    if (docs_.empty() || !lsp_ || !lsp_->ready()) return;
    if (!cfgB("inlay.parameters") && !cfgB("inlay.types")) { if (!inlays_.empty()) { inlays_.clear(); dirty_ = true; } return; }
    Document& d = doc();
    if (!isCppDoc(d) || !d.lspOpen || isNotebook(d) || isPreview(d)) return;
    InlayData& id = inlays_[&d];
    if (id.busy) return;
    const std::uint64_t ver = d.buf.version();
    const int lines = d.buf.lineCount();
    const int first = std::max(0, static_cast<int>(std::floor(std::round(d.scrollY) / lh())));
    const int last = std::min(lines - 1, first + static_cast<int>(L_.editor.h / lh()) + 1);
    const bool needText = id.version != ver && id.askedVersion != ver;
    const bool needRange = !needText && id.ready && id.version == ver && ((first < id.from + 2 && id.from > 0) || (last > id.to - 2 && id.to < lines - 1));
    if (!needText && !needRange) return;
    if (id.dueVersion != ver || id.due == Clock::time_point{}) {
        id.dueVersion = ver;
        id.due = now + std::chrono::milliseconds(needText ? (id.ready ? 280 : 120) : 40);
    }
    if (now < id.due) return;
    id.due = {};
    flushDoc(d);
    const int from = std::max(0, first - 15), to = std::min(lines - 1, last + 40);
    id.busy = true;
    id.askedVersion = ver;
    const Document* dp = &d;
    lsp_->inlayHints(d.path, from, to, [this, dp, ver, lines, from, to](std::vector<LspInlay> hints) {
        if (!docAlive(dp)) return;
        InlayData& o = inlays_[dp];
        o.busy = false;
        o.byLine.clear();
        const Document& doc = *dp;
        for (const auto& h : hints) {
            if (h.line < 0 || h.line >= doc.buf.lineCount()) continue;
            const bool type = h.kind == 1;
            if (type ? !cfgB("inlay.types") : !cfgB("inlay.parameters")) continue;
            InlayHint x;
            x.col = byteColFromUtf16(doc.buf.line(h.line), h.col);
            x.kind = type ? 1 : 2;
            x.text = h.label;
            o.byLine[h.line].push_back(std::move(x));
        }
        for (auto& [ln, v] : o.byLine) std::ranges::stable_sort(v, [](const InlayHint& a, const InlayHint& b) { return a.col < b.col; });
        o.version = ver; o.lines = lines; o.from = from; o.to = to; o.ready = true;
        dirty_ = true;
    });
}

std::optional<Clock::time_point> App::inlayWake() const {
    if (docs_.empty() || !lsp_ || !lsp_->ready() || (!cfgB("inlay.parameters") && !cfgB("inlay.types"))) return std::nullopt;
    const Document& d = *docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))];
    auto it = inlays_.find(&d);
    if (it == inlays_.end()) return Clock::now() + std::chrono::milliseconds(60);
    const InlayData& id = it->second;
    if (id.busy) return std::nullopt;
    if (id.version != d.buf.version() && id.askedVersion != d.buf.version()) return id.due != Clock::time_point{} && id.dueVersion == d.buf.version() ? id.due : Clock::now() + std::chrono::milliseconds(40);
    return std::nullopt;
}

}  // namespace ide
