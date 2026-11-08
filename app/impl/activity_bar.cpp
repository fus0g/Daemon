// daemon.app / activity_bar - part of the App implementation (see app.cppm for the class).
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

Rect App::activityPill(int group) const {
    const Rect& a = L_.activity;
    const float bs = activityCell(), pad = std::round(3.5f * S()), gap = std::round(2 * S()), between = std::round(10 * S());
    const float w = bs + 2 * pad, x = a.x + (a.w - w) / 2, y0 = a.y + std::round(6 * S());
    auto height = [&](int n) { return static_cast<float>(n) * bs + static_cast<float>(n - 1) * gap + 2 * pad + 2 * activityPadV(); };   // extra room above the first and below the last icon
    return {x, y0 + static_cast<float>(group) * (height(3) + between), w, group == 2 ? w : height(3)};   // the lone Debug button gets a circle
}

Rect App::activityRect(int i) const {
    const float bs = activityCell(), pad = std::round(3.5f * S()), gap = std::round(2 * S());
    const int p = activityPos(i);
    const int g = p < 3 ? 0 : p < 6 ? 1 : 2;
    const Rect pill = activityPill(g);
    return {pill.x + pad, pill.y + pad + (g == 2 ? 0.0f : activityPadV()) + static_cast<float>(p - g * 3) * (bs + gap), bs, bs};
}

int App::activityAt(float x, float y) const {
    if (!L_.activity.contains(x, y)) return 0;
    for (int i = 0; i < 7; ++i)
        if (activityRect(i).contains(x, y)) return i + 1;
    return 0;
}

bool App::activityActive(int i) const {
    switch (i) {
        case 0: return sidebar_ && !sideGit_ && !sideSearch_ && !sideDebug_ && !sideDebug_;
        case 6: return sidebar_ && sideDebug_;
        case 1: return sidebar_ && sideGit_;
        case 5: return sidebar_ && sideSearch_;
        case 2: return panelOpen_ && panelTab_ < 2;
        case 3: return panelOpen_ && panelTab_ == 3;
        default: return panelOpen_ && panelTab_ == 2;
    }
}

void App::activityClick(int i) {
    switch (i) {
        case 0:
            if (activityActive(0)) { sidebar_ = false; sideFocus_ = false; }
            else { sidebar_ = true; sideGit_ = false; sideSearch_ = false; sideDebug_ = false; revealCurrent(); }
            break;
        case 1:
            if (activityActive(1)) { sidebar_ = false; gitMsgFocus_ = false; }
            else { sidebar_ = true; sideGit_ = true; sideSearch_ = false; sideDebug_ = false; sideFocus_ = false; gitNextPoll_ = {}; }
            break;
        case 5:
            if (activityActive(5)) { sidebar_ = false; sFocus_ = -1; }
            else openSearch(false);
            break;
        case 6:   // the debugger lives in two places: the sidebar view and the Debug console; open or close both together
            if (activityActive(6) && panelOpen_ && panelTab_ == 4) { sidebar_ = false; panelOpen_ = false; }
            else {
                sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; sideFocus_ = false; sFocus_ = -1;
                panelOpen_ = true; panelTab_ = 4; panelFollow_[4] = true; runFocus_ = false; termFocus_ = false;
            }
            break;
        case 2:
            if (activityActive(2)) panelOpen_ = false;
            else { panelOpen_ = true; panelTab_ = runProc_ ? 1 : 0; }
            break;
        case 3:
            if (activityActive(3)) panelOpen_ = false;
            else openTerminal();
            break;
        default:
            if (activityActive(4)) panelOpen_ = false;
            else { panelOpen_ = true; panelTab_ = 2; termFocus_ = false; }
            break;
    }
    dirty_ = true;
}

void App::drawActivityBar() {
    const Rect& a = L_.activity;
    if (a.w <= 0) return;
    const float s = S(), hair = std::max(1.0f, std::round(0.8f * s));
    const int ip = std::max(15, static_cast<int>(std::round(activityCell() * 0.64f)));
    const bool hasErrors = !problems_.empty() ||
                           std::ranges::any_of(shownProblems_, [](const Problem& p) { return p.severity == Severity::Error; });
    for (int g = 0; g < 3; ++g) {   // white outline, dark fill (same as the toolbar pill)
        const Rect pill = activityPill(g);
        fill(pill, Color::rgb(0xffffff, 34), pill.w / 2);
        fill(pill.x + hair, pill.y + hair, pill.w - 2 * hair, pill.h - 2 * hair, Color::rgb(0x0d0e11), pill.w / 2 - hair);
    }
    for (int i = 0; i < 7; ++i) {
        const Rect r = activityRect(i);
        const bool active = activityActive(i), hot = hoverAct_ == i + 1;
        if (hot && !active) fill(r, Color::rgb(0xffffff, 26), r.w / 2);
        const float ix = std::round(r.x + (r.w - static_cast<float>(ip)) / 2), iy = std::round(r.y + (r.h - static_cast<float>(ip)) / 2);
        const Color col = active ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : Color::rgb(0xffffff);   // the active view's icon takes the accent
        // Material Design icons: file-tree, source-branch, script-text-play (output), console-line (terminal), alert.
        static constexpr std::array<char32_t, 7> glyphs = {0xF13D2, 0xF062C, 0xF1728, 0xF07B7, 0xF002A, 0xF0349, 0xF00E4};
        Icons::glyph(dl_, font_, ix, iy, ip, glyphs[static_cast<std::size_t>(i)], col);
        if (i == 4 && hasErrors) {
            const float d = std::round(7 * s);
            fill(ix + static_cast<float>(ip) - d * 0.6f, iy - d * 0.2f, d, d, theme::danger, d / 2);
        }
    }
}

}  // namespace ide
