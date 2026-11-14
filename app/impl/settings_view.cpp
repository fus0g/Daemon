// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / settings_view - applies the user settings to the running IDE and draws / drives the Settings dialog.
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
import daemon.services.settings;
import daemon.services.project;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

// ------------------------------------------------------------------------------------------------ applying

void App::syncDocDefaults() {
    Document::defaultTabWidth = cfgI("editor.tabWidth");
    Document::defaultUseTabs = !cfgB("editor.insertSpaces");
    Document::defaultAutoPair = cfgB("editor.autoPair");
    Document::defaultDetectIndent = cfgB("editor.detectIndent");
    for (auto& d : docs_) {
        d->autoPair = Document::defaultAutoPair;
        if (isNotebook(*d) || isPreview(*d)) continue;
        d->tabWidth = Document::defaultTabWidth;
        d->useTabs = Document::defaultUseTabs;
        d->redetectIndent();
    }
}

void App::applySettings(const settings::Settings& before) {
    const settings::Settings& now = settings::current();
    auto changed = [&](std::string_view k) { return before.get(k) != now.get(k); };
    if (changed("editor.tabWidth") || changed("editor.insertSpaces") || changed("editor.autoPair") || changed("editor.detectIndent")) syncDocDefaults();
    if (changed("editor.fontSize") && static_cast<float>(cfgI("editor.fontSize")) != fontSize_) {
        const float next = static_cast<float>(cfgI("editor.fontSize"));
        rescaleViews(next / fontSize_);
        fontSize_ = next;
        applyFonts();
        scrollToCursor_ = true;
    }
    if (changed("appearance.uiFontSize")) applyFonts();
    if (changed("lsp.enabled") || changed("lsp.clangd") || changed("lsp.backgroundIndex")) {
        restartLsp();
        for (auto& [d, v] : notebooks_) { nbStopLsp(*v); v->lsp = {}; }   // each notebook starts its own clangd again when shown
        lspNoticeShown_ = false;
    }
    if (changed("markdown.remoteImages")) for (auto& [d, v] : previews_) v.keys.clear();
    if (changed("editor.cursorBlink")) resetCaret();
    dirty_ = true;
}

void App::settingChanged(const std::string& key, const settings::Value& v) {
    auto& cur = settings::current();
    const settings::Settings before = cur;
    if (!cur.set(key, v)) return;
    if (!settings::save(cur)) setMessage("Could not write " + settings::settingsFile().string(), true, 4);
    std::error_code ec;
    settingsMtime_ = fs::last_write_time(settings::settingsFile(), ec);
    applySettings(before);
}

void App::settingsPoll(Clock::time_point now) {
    if (now < nextSettingsCheck_) return;
    nextSettingsCheck_ = now + std::chrono::milliseconds(1500);
    const fs::path f = settings::settingsFile();
    if (f.empty()) return;
    std::error_code ec;
    const auto t = fs::exists(f, ec) ? fs::last_write_time(f, ec) : fs::file_time_type{};
    if (t == settingsMtime_) return;
    settingsMtime_ = t;
    std::vector<std::string> problems;
    const settings::Settings before = settings::current();
    settings::current() = settings::load(&problems);
    settingsNote_ = problems.empty() ? std::string() : problems.front();
    if (!problems.empty()) setMessage("settings.json: " + problems.front(), true, 6);
    else setMessage("Settings reloaded", false, 1.5f);
    applySettings(before);
}

void App::settingsInit() {
    std::vector<std::string> problems;
    settings::current() = settings::load(&problems);
    settingsNote_ = problems.empty() ? std::string() : problems.front();
    std::error_code ec;
    const fs::path f = settings::settingsFile();
    if (!f.empty() && fs::exists(f, ec)) settingsMtime_ = fs::last_write_time(f, ec);
    if (state_.setupMuted) {   // the old "don't remind me" flag became a setting
        settings::current().set("general.toolReminders", false);
        settings::save(settings::current());
        state_.setupMuted = false;
        saveState(state_);
    }
    syncDocDefaults();
}

void App::settingsOpenFile() {
    const fs::path f = settings::settingsFile();
    if (f.empty()) return;
    std::error_code ec;
    if (!fs::exists(f, ec)) settings::save(settings::current());   // an empty "{}" the user can start from
    dlgClose();
    openFile(f.string());
}

// ------------------------------------------------------------------------------------------------ the dialog

namespace {

constexpr int kSetSection = 1000, kSetSearch = 1090, kSetOpenFile = 1091, kSetClose = 1092, kSetRow = 1100;   // row id = kSetRow + index * 4 + part

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

}  // namespace

void App::settingsOpen(const std::string& section) {
    dlg_ = {};
    dlg_.kind = DlgKind::Settings;
    dlg_.setSection = 0;
    const auto secs = settings::sections();
    if (auto it = std::ranges::find(secs, section); it != secs.end()) dlg_.setSection = static_cast<int>(it - secs.begin());
    if (section == "Tools") toolsCheck(false);
    dlg_.setFocus = -1;
    dirty_ = true;
}

// The definitions the right-hand side shows: one section, or every match of the search text.
std::vector<const settings::Def*> App::settingsShown() const {
    std::vector<const settings::Def*> out;
    const std::string q = lower(dlg_.setSearch.text);
    const auto secs = settings::sections();
    for (const auto& d : settings::defs()) {
        if (!q.empty()) {
            if (lower(d.label).find(q) != std::string::npos || lower(d.help).find(q) != std::string::npos ||
                lower(d.key).find(q) != std::string::npos || lower(d.section).find(q) != std::string::npos) out.push_back(&d);
        } else if (dlg_.setSection >= 0 && dlg_.setSection < static_cast<int>(secs.size()) && d.section == secs[static_cast<std::size_t>(dlg_.setSection)]) {
            out.push_back(&d);
        }
    }
    return out;
}

void App::settingsCommitEdit() {
    Dialog& d = dlg_;
    if (d.setFocus < 0) return;
    const auto shown = settingsShown();
    if (d.setFocus < static_cast<int>(shown.size())) {
        const settings::Def& def = *shown[static_cast<std::size_t>(d.setFocus)];
        if (settings::current().s(def.key) != d.setEdit.text) settingChanged(def.key, settings::Value(d.setEdit.text));
    }
    d.setFocus = -1;
}

void App::settingsClick(int id, float x) {
    Dialog& d = dlg_;
    auto hitRect = [&](int want) { for (const auto& h : dlgHits_) if (h.id == want) return h.r; return Rect{}; };
    if (id != kSetRow + d.setFocus * 4 && d.setFocus >= 0) settingsCommitEdit();   // clicking elsewhere finishes a text edit
    if (id == kSetClose) { dlgClose(); return; }
    if (id == kSetOpenFile) { settingsOpenFile(); return; }
    if (id == kSetSearch) { d.setFocus = -2; d.setSearch.cur = caretFromX(d.setSearch.text, x - (hitRect(id).x + 10 * S())); return; }
    if ((id >= kDlgSetupCopy && id <= kDlgSetupRecheck) || (id >= kDlgSetupDl && id < kDlgSetupDl + 20)) { setupClick(id); return; }
    if (id >= kSetSection && id < kSetSection + 20) { d.setSection = id - kSetSection; d.setScroll = 0; if (settings::sections()[static_cast<std::size_t>(d.setSection)] == "Tools") toolsCheck(false); d.setSearch = {}; if (d.setFocus == -2) d.setFocus = -1; return; }
    if (id < kSetRow) return;
    const int idx = (id - kSetRow) / 4, part = (id - kSetRow) % 4;
    const auto shown = settingsShown();
    if (idx < 0 || idx >= static_cast<int>(shown.size())) return;
    const settings::Def& def = *shown[static_cast<std::size_t>(idx)];
    const settings::Settings& cur = settings::current();
    if (d.setFocus == -2) d.setFocus = -1;
    if (part == 3) { settingChanged(def.key, def.def); return; }   // reset to the default
    switch (def.type) {
        case settings::Type::Bool: settingChanged(def.key, settings::Value(!cur.b(def.key))); break;
        case settings::Type::Int: {
            const int dir = part == 1 ? -1 : (part == 2 ? 1 : 0);
            if (dir) settingChanged(def.key, settings::Value(cur.i(def.key) + dir * def.step));
            break;
        }
        case settings::Type::Text:
            d.setFocus = idx;
            d.setEdit.text = cur.s(def.key);
            d.setEdit.cur = caretFromX(d.setEdit.text, x - (hitRect(id).x + 10 * S()));
            break;
        case settings::Type::Choice: {
            const auto it = std::ranges::find(def.choices, cur.s(def.key));
            const std::size_t n = def.choices.size();
            if (n) settingChanged(def.key, settings::Value(def.choices[((it - def.choices.begin()) + 1) % static_cast<std::ptrdiff_t>(n)]));
            break;
        }
    }
}

void App::settingsKey(const KeyEvent& k) {
    Dialog& d = dlg_;
    if (d.setFocus == -2) {   // search box
        if (k.key == Key::Escape) { if (!d.setSearch.text.empty()) d.setSearch = {}; else d.setFocus = -1; return; }
        if (k.key == Key::Enter || k.key == Key::Tab || k.key == Key::Down) { d.setFocus = -1; return; }
        if (editText(d.setSearch, k)) d.setScroll = 0;
        return;
    }
    if (d.setFocus >= 0) {   // a text setting
        if (k.key == Key::Escape) { d.setFocus = -1; return; }       // cancel: keep the old value
        if (k.key == Key::Enter) { settingsCommitEdit(); return; }
        editText(d.setEdit, k);
        return;
    }
    if (k.key == Key::Escape) { if (!d.setSearch.text.empty()) d.setSearch = {}; else dlgClose(); return; }
    const int n = static_cast<int>(settings::sections().size());
    if (d.setSearch.text.empty()) {
        if (k.key == Key::Down) { d.setSection = std::min(n - 1, d.setSection + 1); d.setScroll = 0; return; }
        if (k.key == Key::Up) { d.setSection = std::max(0, d.setSection - 1); d.setScroll = 0; return; }
    }
    if ((k.ctrl && k.key == Key::Char && k.ch == 'f') || k.key == Key::Tab) { d.setFocus = -2; return; }
    if (!k.ctrl && !k.alt && k.key == Key::Char && !k.text.empty()) { d.setFocus = -2; editText(d.setSearch, k); d.setScroll = 0; }   // typing searches
}

void App::drawSettings(const Rect& r) {
    const float s = S(), pad = std::round(26 * s);
    const float sideW = std::round(250 * s);
    dlgSide(r, sideW);
    Dialog& d = dlg_;
    const auto secs = settings::sections();
    const bool searching = !d.setSearch.text.empty();

    // ---- left: title, search, sections, the file
    dl_.titleText(font_, r.x + 26 * s, r.y + 24 * s, "Settings", theme::text);
    float y = r.y + 24 * s + font_.titleLineHeight() + 16 * s;
    dlgField({r.x + 16 * s, y, sideW - 32 * s, std::round(36 * s)}, d.setSearch, d.setFocus == -2, kSetSearch, "Search settings");
    y += std::round(36 * s) + 14 * s;
    for (std::size_t i = 0; i < secs.size(); ++i) {
        const Rect row{r.x + 12 * s, y, sideW - 24 * s, std::round(34 * s)};
        const int id = kSetSection + static_cast<int>(i);
        dlgHit(row, id);
        const bool on = !searching && d.setSection == static_cast<int>(i);
        if (on) fill(row, theme::accent.alpha(60), 8 * s);
        else if (dlgHot(id)) fill(row, Color::rgb(0xffffff, 16), 8 * s);
        uiLabel(row.x + 14 * s, row.y, row.h, secs[i], on ? theme::text : theme::dim, row.right() - 8 * s);
        y += row.h + 2 * s;
    }
    const float bh = std::round(36 * s);
    dlgButton({r.x + 16 * s, r.bottom() - 16 * s - bh, sideW - 32 * s, bh}, "Open settings.json", kSetOpenFile, false, 0xF0214);

    // ---- right: the rows
    const float cx = r.x + sideW + std::round(6 * s) + pad, cr = r.right() - pad, cw = cr - cx;
    const float headH = std::round(70 * s), footH = std::round(66 * s);
    const float areaTop = r.y + headH, areaBottom = r.bottom() - footH;
    const float rowH = std::round(60 * s);
    const auto shown = settingsShown();
    const bool toolsPage = !searching && d.setSection < static_cast<int>(secs.size()) && secs[static_cast<std::size_t>(d.setSection)] == "Tools";
    const float rowsH = static_cast<float>(shown.size()) * rowH;
    const float toolsH = toolsPage ? std::round(420 * s) + static_cast<float>(tools_.size()) * std::round(38 * s) : 0;   // generous: refined below while drawing
    float content = rowsH + toolsH;
    settingsMax_ = std::max(0.0f, content - (areaBottom - areaTop));
    d.setScroll = std::clamp(d.setScroll, 0.0f, settingsMax_);
    const settings::Settings& cur = settings::current();
    const auto switchW = std::round(44 * s), switchH = std::round(24 * s);

    for (std::size_t i = 0; i < shown.size(); ++i) {
        const settings::Def& def = *shown[i];
        const float ry = areaTop + static_cast<float>(i) * rowH - d.setScroll;
        if (ry + rowH < areaTop || ry > areaBottom) continue;
        const bool whole = ry >= areaTop && ry + rowH <= areaBottom;
        const int base = kSetRow + static_cast<int>(i) * 4;
        if (i % 2 == 0) fill(cx - 12 * s, ry + 2 * s, cw + 24 * s, rowH - 4 * s, Color::rgb(0xffffff, 6), 8 * s);
        const bool dflt = cur.isDefault(def.key);
        // control, right aligned
        float cxr = cr;
        const float cy = ry + (rowH - switchH) / 2;
        switch (def.type) {
            case settings::Type::Bool: {
                const bool on = cur.b(def.key);
                const Rect sw{cxr - switchW, cy, switchW, switchH};
                if (whole) dlgHit(sw, base);
                fill(sw, on ? theme::accent : Color::rgb(0xffffff, dlgHot(base) ? 70 : 44), switchH / 2);
                if (!on) fill(sw.x + 1, sw.y + 1, sw.w - 2, sw.h - 2, Color::rgb(0x07080a), switchH / 2 - 1);
                const float kn = switchH - std::round(8 * s);
                fill(on ? sw.right() - kn - 4 * s : sw.x + 4 * s, sw.y + 4 * s, kn, kn, on ? Color::rgb(0xffffff) : theme::dim, kn / 2);
                cxr = sw.x;
                break;
            }
            case settings::Type::Int: {
                const float bsz = std::round(28 * s);
                auto round = [&](float x, int part, char32_t glyph) {
                    const Rect b{x, ry + (rowH - bsz) / 2, bsz, bsz};
                    if (whole) dlgHit(b, base + part);
                    const bool hot = dlgHot(base + part);
                    fill(b, Color::rgb(0xffffff, hot ? 60 : 34), bsz / 2);
                    fill(b.x + 1, b.y + 1, b.w - 2, b.h - 2, hot ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), bsz / 2 - 1);
                    const int ip = std::max(12, static_cast<int>(std::round(bsz * 0.6f)));
                    Icons::glyph(dl_, font_, b.x + (bsz - static_cast<float>(ip)) / 2, b.y + (bsz - static_cast<float>(ip)) / 2, ip, glyph, theme::text);
                };
                round(cxr - bsz, 2, 0xF0415);   // plus
                const float vw = std::round(70 * s);
                uiLabelC(cxr - bsz - vw / 2, ry, rowH, std::to_string(cur.i(def.key)) + (def.unit.empty() ? "" : " " + def.unit), theme::text);
                round(cxr - bsz - vw - bsz, 1, 0xF0376);   // minus
                cxr -= 2 * bsz + vw;
                break;
            }
            case settings::Type::Text: {
                const float fw = std::min(std::round(300 * s), cw * 0.5f);
                const Rect fr{cxr - fw, ry + (rowH - std::round(34 * s)) / 2, fw, std::round(34 * s)};
                const bool editing = d.setFocus == static_cast<int>(i);
                if (whole) {
                    const TextBox shownText = editing ? d.setEdit : TextBox{cur.s(def.key), 0};
                    dlgField(fr, shownText, editing, base, "default");
                }
                cxr = fr.x;
                break;
            }
            case settings::Type::Choice: {
                const Rect b{cxr - std::round(150 * s), cy - 2 * s, std::round(150 * s), switchH + 4 * s};
                if (whole) dlgButton(b, cur.s(def.key), base, false);
                cxr = b.x;
                break;
            }
        }
        if (!dflt && whole) {   // reset to default
            const float rs = std::round(26 * s);
            const Rect rb{cxr - rs - 12 * s, ry + (rowH - rs) / 2, rs, rs};
            dlgHit(rb, base + 3);
            if (dlgHot(base + 3)) fill(rb, Color::rgb(0xffffff, 26), rs / 2);
            const int ip = std::max(12, static_cast<int>(std::round(rs * 0.62f)));
            Icons::glyph(dl_, font_, rb.x + (rs - static_cast<float>(ip)) / 2, rb.y + (rs - static_cast<float>(ip)) / 2, ip, 0xF0450, theme::dim);
            cxr = rb.x;
        }
        // label + help
        const float lw = cxr - cx - 16 * s;
        const std::string label = searching ? def.section + ": " + def.label : def.label;
        uiLabel(cx, ry + 6 * s, std::round(rowH * 0.5f), label, dflt ? theme::text : theme::accent.mix(Color::rgb(0xffffff), 0.35f), cx + lw);
        std::string help = def.help + (def.needsRestart ? " (applies after a restart)" : "");
        uiLabel(cx, ry + std::round(rowH * 0.48f), std::round(rowH * 0.42f), help, theme::lineNum, cx + lw);
    }
    if (shown.empty()) uiLabel(cx, areaTop + 10 * s, rowH, "No setting matches \"" + d.setSearch.text + "\"", theme::dim);

    if (toolsPage) {   // the tool check: a table under the setting(s) of the section
        const float used = drawToolsBody(cx, areaTop + rowsH + 14 * s - d.setScroll, cw, areaTop, areaBottom);
        settingsMax_ = std::max(0.0f, rowsH + 14 * s + used - (areaBottom - areaTop));
    }

    // header and footer cover rows that are scrolled past their edge
    const Rect main{r.x + sideW + std::round(6 * s), r.y, r.w - sideW - std::round(6 * s), r.h};
    fill(main.x, main.y, main.w, headH, theme::editorBg, std::round(12 * s));
    fill(main.x, main.y + headH - std::round(12 * s), main.w, std::round(12 * s), theme::editorBg);
    fill(main.x, areaBottom, main.w, footH, theme::editorBg, std::round(12 * s));
    fill(main.x, areaBottom, main.w, std::round(12 * s), theme::editorBg);
    dlgHit({main.x, main.y, main.w, headH}, -2);
    dlgHit({main.x, areaBottom, main.w, footH}, -2);
    const std::string title = searching ? std::format("{} result{}", shown.size(), shown.size() == 1 ? "" : "s")
                                        : (d.setSection < static_cast<int>(secs.size()) ? secs[static_cast<std::size_t>(d.setSection)] : std::string());
    dl_.titleText(font_, cx, r.y + 22 * s, title, theme::text);
    if (settingsMax_ > 0) {   // scrollbar
        const float trackH = areaBottom - areaTop, thumbH = std::max(24 * s, trackH * trackH / (trackH + settingsMax_));
        fill(r.right() - 12 * s, areaTop + (trackH - thumbH) * (d.setScroll / settingsMax_), 5 * s, thumbH, theme::scrollbar, 2.5f * s);
    }
    // footer: where the file is, problems in it, Done
    const float fy = areaBottom + (footH - bh) / 2;
    dlgButton({cr - std::round(110 * s), fy, std::round(110 * s), bh}, "Done", kSetClose, true);
    const std::string note = settingsNote_.empty() ? "Saved as you change them in " + settings::settingsFile().string() : "settings.json: " + settingsNote_;
    uiLabel(cx, areaBottom, footH, note, settingsNote_.empty() ? theme::lineNum : Color::rgb(0xe5b567), cr - std::round(130 * s));
}

}  // namespace ide
