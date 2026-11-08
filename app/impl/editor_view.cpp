// daemon.app / editor_view - part of the App implementation (see app.cppm for the class).
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

void App::rebuildMini(Document& d) {
    mini_.doc = &d;
    mini_.version = d.buf.version();
    mini_.tab = d.tabWidth;
    const int n = std::min(d.buf.lineCount(), 20000);
    mini_.lines.assign(static_cast<std::size_t>(n), {});
    for (int ln = 0; ln < n; ++ln) {
        const std::string& str = d.buf.line(ln);
        if (str.empty()) continue;
        d.hl.highlight(d.buf, ln, toks_);
        auto& runs = mini_.lines[static_cast<std::size_t>(ln)];
        int dcol = 0, runStart = -1;
        std::uint8_t runTok = 0;
        auto close = [&] {
            if (runStart >= 0) runs.push_back({static_cast<std::uint16_t>(runStart), static_cast<std::uint16_t>(dcol), runTok});
            runStart = -1;
        };
        for (std::size_t i = 0; i < str.size() && dcol < 160;) {
            const std::size_t start = i;
            const char32_t cp = decodeUtf8(str, i);
            if (cp == '\t') { close(); dcol += d.tabWidth - dcol % d.tabWidth; continue; }
            if (cp == ' ') { close(); ++dcol; continue; }
            const auto t = static_cast<std::uint8_t>(toks_[start]);
            if (runStart < 0) { runStart = dcol; runTok = t; }
            else if (t != runTok) { close(); runStart = dcol; runTok = t; }
            ++dcol;
        }
        close();
    }
}

auto App::miniMetrics(const Document& d) const -> MiniMetrics {
    const float rowH = std::max(2.0f, std::round(2.6f * S()));
    const float content = static_cast<float>(d.buf.lineCount()) * rowH;
    const float maxS = maxScroll(d);
    const float offset = (content > L_.minimap.h && maxS > 0) ? (d.scrollY / maxS) * (content - L_.minimap.h) : 0.0f;
    return {rowH, offset, content};
}

void App::miniJump(Document& d) {
    const MiniMetrics m = miniMetrics(d);
    const float line = (my_ - L_.minimap.y + m.offset) / m.rowH;
    d.targetY = std::clamp(line * lh() - L_.editor.h / 2, 0.0f, maxScroll(d));
    dirty_ = true;
}

void App::drawMinimap(Document& d) {
    const Rect& mm = L_.minimap;
    if (mm.w <= 0 || mm.h <= 0) return;
    if (mini_.doc != &d || mini_.version != d.buf.version() || mini_.tab != d.tabWidth) rebuildMini(d);
    const float s = S();
    fill(mm, theme::editorBg);   // opaque: also hides text that runs underneath
    fill(mm.x, mm.y, std::max(1.0f, std::round(s * 0.5f)), mm.h, theme::guide);
    const MiniMetrics m = miniMetrics(d);
    const float cs = std::max(1.0f, std::round(s)), barH = std::max(1.0f, std::round(m.rowH * 0.6f));
    const int n = static_cast<int>(mini_.lines.size());
    const int first = std::max(0, static_cast<int>(std::floor(m.offset / m.rowH)));
    const int last = std::min(n - 1, static_cast<int>(std::floor((m.offset + mm.h) / m.rowH)));
    const float x0 = mm.x + 8 * s, xMax = mm.right() - 8 * s;
    for (int ln = first; ln <= last; ++ln) {
        const float y = mm.y + static_cast<float>(ln) * m.rowH - m.offset + std::round((m.rowH - barH) / 2);
        for (const MiniRun& r : mini_.lines[static_cast<std::size_t>(ln)]) {
            const float x = x0 + static_cast<float>(r.c0) * cs;
            if (x >= xMax) break;
            const Tok t = static_cast<Tok>(r.tok);
            fill(x, y, std::min(static_cast<float>(r.c1 - r.c0) * cs, xMax - x), barH,
                 t == Tok::Normal ? theme::text.alpha(120) : theme::syntax(t).alpha(190));
        }
    }
    // Viewport slider.
    const float top = d.scrollY / lh() * m.rowH - m.offset;
    const float vh = std::min(mm.h, L_.editor.h / lh() * m.rowH);
    const float sy = std::clamp(mm.y + top, mm.y, mm.bottom() - vh);
    fill(mm.x + 1, sy, mm.w - 1, vh, Color{255, 255, 255, static_cast<std::uint8_t>(miniDrag_ ? 38 : 20)});
}

}  // namespace ide
