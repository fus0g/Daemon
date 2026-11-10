// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / setup - checks the external tools at start and shows what is missing and how to install it.
module daemon.app;
import std;
import daemon.base.util;
import daemon.base.process;
import daemon.ui.input;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.ui.icons;
import daemon.app.theme;
import daemon.services.toolchain;
import daemon.services.project;

namespace ide {

void App::toolsCheck(bool openDialog) {
    if (toolsChecking_) return;
    toolsChecking_ = true;
    spawn([this, openDialog] {
        auto found = toolchain::detect();
        auto distro = toolchain::distro();
        return [this, openDialog, found = std::move(found), distro = std::move(distro)]() mutable {
            toolsChecking_ = false;
            tools_ = std::move(found);
            distro_ = std::move(distro);
            toolsCmd_ = toolchain::installCommand(tools_, distro_);
            const bool missing = std::ranges::any_of(tools_, [](const auto& s) { return !s.ok(); });
            // At start-up only interrupt the user when something is wrong and they have not muted the reminder.
            if (openDialog && missing && !state_.setupMuted && (dlg_.kind == DlgKind::None || dlg_.kind == DlgKind::Welcome)) {
                dlg_ = {};
                dlg_.kind = DlgKind::Setup;
            }
            dirty_ = true;
        };
    });
}

void App::setupClick(int id) {
    if (id == kDlgSetupClose) { dlgClose(); return; }
    if (id == kDlgSetupRecheck) { toolsCheck(false); setMessage("Checking tools...", false, 1.5f); return; }
    if (id == kDlgSetupMute) { state_.setupMuted = true; saveState(state_); dlgClose(); setMessage("Tool reminders turned off (Welcome > Check Tools reopens this)", false, 4); return; }
    if (id == kDlgSetupCopy && !toolsCmd_.empty()) { win_.setClipboard(toolsCmd_); setMessage("Install command copied", false, 2); return; }
    if (id == kDlgSetupRun && !toolsCmd_.empty()) {   // typed into the integrated terminal: the user sees it and enters their password there
        dlgClose();
        openTerminal();
        if (termProc_) termProc_->write(toolsCmd_ + "\n");
        return;
    }
}

void App::drawSetup(const Rect& r) {
    const float s = S(), pad = std::round(32 * s);
    fill(r, theme::sideBg, std::round(12 * s));
    const int ls = static_cast<int>(std::round(34 * s));
    Icons::logo(dl_, font_, r.x + pad, r.y + pad, ls, theme::accent);
    dl_.titleText(font_, r.x + pad + static_cast<float>(ls) + 14 * s, r.y + pad - 2 * s, "Set up your toolchain", theme::text);
    const bool checking = toolsChecking_ || tools_.empty();
    const bool allOk = !checking && std::ranges::all_of(tools_, [](const auto& t) { return t.ok(); });
    const std::string sub = checking ? "Checking installed tools..."
        : allOk ? "Everything Daemon uses is installed."
                : "Daemon drives these programs. Install what is missing to unlock the matching features.";
    uiLabel(r.x + pad + static_cast<float>(ls) + 14 * s, r.y + pad + font_.titleLineHeight() - 4 * s, uiH() + 6 * s, sub, theme::dim);

    const float rowH = std::round(38 * s), top = r.y + pad + static_cast<float>(ls) + 22 * s;
    const Color okC = Color::rgb(0x6cc070), warnC = Color::rgb(0xe5b567);
    const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.2f)));
    const float right = r.right() - pad;
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        const auto& t = tools_[i];
        const float y = top + static_cast<float>(i) * rowH;
        if (i % 2 == 0) fill(r.x + pad - 10 * s, y, r.w - 2 * pad + 20 * s, rowH, Color::rgb(0xffffff, 6), 8 * s);
        Icons::glyph(dl_, font_, r.x + pad, y + (rowH - static_cast<float>(ip)) / 2, ip, t.ok() ? 0xF012C : 0xF002A, t.ok() ? okC : warnC);
        const float nx = r.x + pad + static_cast<float>(ip) + 12 * s;
        uiLabel(nx, y, rowH, t.tool.name, theme::text);
        std::string status;
        Color sc = theme::dim;
        if (!t.found) { status = t.tool.need == toolchain::Need::Optional ? "not installed (optional)" : "not installed"; sc = warnC; }
        else if (t.tooOld) { status = std::format("{} - too old (need {}.{}+)", t.version, t.tool.minMajor, t.tool.minMinor); sc = warnC; }
        else status = (t.version.empty() ? "installed" : t.version) + (t.bundled ? "  (bundled)" : "");
        const float sw = uiW(status);
        uiLabelR(right, y, rowH, status, sc);
        uiLabel(nx + 96 * s, y, rowH, t.tool.purpose, theme::lineNum, right - sw - 16 * s);
    }

    // Install command + buttons.
    const float bh = std::round(36 * s), by = r.bottom() - pad - bh;
    if (!allOk && !checking) {
        const float ch = std::round(44 * s), cy = by - ch - 18 * s;
        const std::string head = toolsCmd_.empty()
            ? std::format("Install the missing tools with your package manager ({}).", distro_.name)
            : std::format("Install on {}:", distro_.name);
        uiLabel(r.x + pad, cy - uiH() - 12 * s, uiH() + 8 * s, head, theme::dim);
        if (!toolsCmd_.empty()) {
            fill(r.x + pad, cy, r.w - 2 * pad, ch, Color::rgb(0x07080a), 8 * s);
            uiLabel(r.x + pad + 14 * s, cy, ch, toolsCmd_, theme::accent.mix(Color::rgb(0xffffff), 0.3f), right - 14 * s);
        }
    }
    float bx = r.x + pad;
    if (!allOk && !checking && !toolsCmd_.empty()) {
        dlgButton({bx, by, std::round(190 * s), bh}, "Run in terminal", kDlgSetupRun, true, 0xF07B7);
        bx += std::round(190 * s) + 10 * s;
        dlgButton({bx, by, std::round(150 * s), bh}, "Copy command", kDlgSetupCopy, false, 0);
        bx += std::round(150 * s) + 10 * s;
    }
    dlgButton({bx, by, std::round(120 * s), bh}, "Re-check", kDlgSetupRecheck, false, 0xF0450);
    float rx = right;
    const float cw = std::round(100 * s);
    rx -= cw;
    dlgButton({rx, by, cw, bh}, allOk ? "Done" : "Close", kDlgSetupClose, allOk, 0);
    if (!allOk && !checking) {
        const float mw = std::round(190 * s);
        rx -= mw + 10 * s;
        dlgButton({rx, by, mw, bh}, "Don't remind me", kDlgSetupMute, false, 0);
    }
}

}  // namespace ide
