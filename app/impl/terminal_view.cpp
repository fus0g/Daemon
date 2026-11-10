// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / terminal_view - part of the App implementation (see app.cppm for the class).
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

void App::startTerminal() {
    if (termProc_) return;
    std::string shell = std::getenv("SHELL") ? std::getenv("SHELL") : "";
    if (shell.empty() || !findExecutable(shell)) shell = findExecutable("bash") ? "bash" : "sh";
    ProcessOptions o;
    o.usePty = true;
    o.argv = {shell};
    o.cwd = cmakeOpen_ ? cmakeRoot_.string() : (explorer_.isOpen() ? explorer_.rootPath().string() : homeDir());
    o.env = {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}, {"TERM_PROGRAM", "daemon"}};
    termProc_ = std::make_unique<Process>();
    term_.reset();
    if (auto r = termProc_->start(o); !r) {
        term_.feed("Cannot start the shell: " + r.error() + "\r\n");
        termProc_.reset();
        termExited_ = true;
        return;
    }
    termExited_ = false;
    termScroll_ = 0;
    tsel_ = {};
    termProc_->resizePty(term_.cols(), term_.rows());
    win_.watch(termProc_->outFd(), [this] { pumpTerminal(); });
    dirty_ = true;
}

void App::pumpTerminal() {
    if (!termProc_) return;
    std::string chunk;
    const long n = termProc_->read(chunk);
    if (!chunk.empty()) {
        const int before = term_.scrollbackSize();
        term_.feed(chunk);
        if (std::string reply = term_.takeReply(); !reply.empty()) termProc_->write(reply);
        if (termScroll_ > 0) termScroll_ = std::min(termScroll_ + (term_.scrollbackSize() - before), term_.scrollbackSize());   // keep the view still
    }
    dirty_ = true;
    if (n == 0) {   // shell ended
        win_.unwatch(termProc_->outFd());
        termProc_->closeOutput();
        const int code = termProc_->tryWait().value_or(0);
        termProc_.reset();
        termExited_ = true;
        term_.feed(std::format("\r\n\x1b[2m[shell exited with code {} - press Enter to start a new one]\x1b[0m\r\n", code));
    }
}

void App::openTerminal() {
    panelOpen_ = true;
    panelTab_ = 3;
    termFocus_ = true;
    if (!termProc_ && !termExited_) startTerminal();
    dirty_ = true;
}

bool App::termKey(const KeyEvent& k) {
    if (k.super) return false;
    if (k.ctrl && k.key == Key::Char && k.ch == '`') return false;   // panel toggle
    if (!termProc_) {
        if (k.key == Key::Enter) { startTerminal(); return true; }
        return k.key != Key::None && k.key < Key::F1;   // swallow typing while there is no shell
    }
    if (k.ctrl && k.shift && k.key == Key::Char) {
        if (k.ch == 'c') { termCopy(); return true; }
        if (k.ch == 'v') {
            std::string c = win_.clipboard();
            std::erase(c, '\r');
            if (term_.bracketedPaste()) c = "\x1b[200~" + c + "\x1b[201~";
            termProc_->write(c);
            return true;
        }
        return false;
    }
    if (k.ctrl && !k.shift && k.key == Key::Char && k.ch == 'c' && tsel_.active && tsel_.moved) { termCopy(); tsel_.active = false; return true; }
    std::string out;
    const bool mod = k.shift || k.alt || k.ctrl;
    auto csi = [&](char fin) { return mod ? "\x1b[1;" + termMods(k) + fin : (term_.appCursorKeys() ? std::string("\x1bO") + fin : std::string("\x1b[") + fin); };
    auto tilde = [&](int n) { return mod ? "\x1b[" + std::to_string(n) + ";" + termMods(k) + "~" : "\x1b[" + std::to_string(n) + "~"; };
    switch (k.key) {
        case Key::Enter: out = "\r"; break;
        case Key::Backspace: out = k.alt ? "\x1b\x7f" : "\x7f"; break;
        case Key::Tab: out = k.shift ? "\x1b[Z" : "\t"; break;
        case Key::Escape: out = "\x1b"; break;
        case Key::Up: out = csi('A'); break;
        case Key::Down: out = csi('B'); break;
        case Key::Right: out = csi('C'); break;
        case Key::Left: out = csi('D'); break;
        case Key::Home: out = csi('H'); break;
        case Key::End: out = csi('F'); break;
        case Key::PageUp: out = tilde(5); break;
        case Key::PageDown: out = tilde(6); break;
        case Key::Delete: out = tilde(3); break;
        case Key::Insert: out = tilde(2); break;
        case Key::F1: case Key::F2: case Key::F3: case Key::F4:
            if (k.ctrl || k.shift) return false;
            out = std::string("\x1bO") + static_cast<char>('P' + (static_cast<int>(k.key) - static_cast<int>(Key::F1)));
            break;
        case Key::F5: case Key::F6: case Key::F7: case Key::F8: case Key::F9: case Key::F10: case Key::F11: case Key::F12: {
            if (k.ctrl || k.shift) return false;   // Ctrl+F9 / Shift+F10 ... stay build / run shortcuts
            static constexpr int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
            out = "\x1b[" + std::to_string(codes[static_cast<int>(k.key) - static_cast<int>(Key::F5)]) + "~";
            break;
        }
        case Key::Char:
            if (k.ctrl && !k.alt) {
                const char32_t c = k.ch;
                if (c >= 'a' && c <= 'z') out = std::string(1, static_cast<char>(c - 'a' + 1));
                else if (c == ' ' || c == '@' || c == '2') out = std::string(1, '\0');
                else if (c == '[') out = "\x1b";
                else if (c == '\\') out = "\x1c";
                else if (c == ']') out = "\x1d";
                else if (c == '/' || c == '_') out = "\x1f";
                else return false;
            } else if (!k.text.empty()) {
                out = (k.alt ? "\x1b" : "") + k.text;
            } else if (k.alt && k.ch < 0x80) {
                out = std::string("\x1b") + static_cast<char>(k.ch);
            } else {
                return false;
            }
            break;
        default: return false;
    }
    termScroll_ = 0;   // typing jumps back to the live screen
    tsel_.active = false;
    termProc_->write(out);
    return true;
}

std::pair<int, int> App::termPosAt(float x, float y, const Rect& body) const {
    const float pad = 10 * S();
    const int c = std::clamp(static_cast<int>(std::floor((x - (body.x + pad)) / cw())), 0, term_.cols() - 1);
    const int r = std::clamp(static_cast<int>(std::floor((y - body.y) / lh())), 0, term_.rows() - 1);
    return {term_.scrollbackSize() + r - termScroll_, c};
}

std::string App::termSelectedText() const {
    if (!tsel_.active || !tsel_.moved) return {};
    int r0 = tsel_.ar, c0 = tsel_.ac, r1 = tsel_.cr, c1 = tsel_.cc;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) { std::swap(r0, r1); std::swap(c0, c1); }
    std::string out;
    for (int r = r0; r <= r1; ++r) {
        auto row = term_.absRow(r);
        const int a = r == r0 ? c0 : 0;
        int b = r == r1 ? c1 : static_cast<int>(row.size()) - 1;
        std::string line;
        for (int c = a; c <= b && c < static_cast<int>(row.size()); ++c) appendUtf8(line, row[static_cast<std::size_t>(c)].ch);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (r > r0) out += '\n';
        out += line;
    }
    return out;
}

void App::termCopy() {
    const std::string t = termSelectedText();
    if (t.empty()) return;
    win_.setClipboard(t);
    setMessage("Copied", false, 1.2f);
}

void App::termClick(const Rect& body) {
    termFocus_ = true;
    if (!termProc_ && termExited_) return;
    const auto [r, c] = termPosAt(mx_, my_, body);
    tsel_ = {};
    tsel_.active = true;
    tsel_.dragging = true;
    tsel_.ar = tsel_.cr = r;
    tsel_.ac = tsel_.cc = c;
    tsel_.sx = mx_; tsel_.sy = my_;
}

void App::drawTerminalBody(const Rect& body) {
    const float s = S(), pad = 10 * s;
    const int cols = std::max(10, static_cast<int>((body.w - 2 * pad - 8 * s) / cw()));
    const int rows = std::max(2, static_cast<int>(body.h / lh()));
    if (cols != term_.cols() || rows != term_.rows()) {
        term_.resize(cols, rows);
        if (termProc_) termProc_->resizePty(cols, rows);
    }
    if (!termProc_ && !termExited_) startTerminal();
    termScroll_ = std::clamp(termScroll_, 0, term_.altScreen() ? 0 : term_.scrollbackSize());
    const float cwid = cw(), lineH = lh();
    int s0r = 0, s0c = 0, s1r = -1, s1c = 0;
    const bool haveSel = tsel_.active && tsel_.moved;
    if (haveSel) {
        s0r = tsel_.ar; s0c = tsel_.ac; s1r = tsel_.cr; s1c = tsel_.cc;
        if (s0r > s1r || (s0r == s1r && s0c > s1c)) { std::swap(s0r, s1r); std::swap(s0c, s1c); }
    }
    const Color defFg = theme::text, defBg = theme::panelBg;
    for (int r = 0; r < term_.rows(); ++r) {
        const float y = body.y + static_cast<float>(r) * lineH;
        auto row = term_.viewRow(r, termScroll_);
        const int absr = term_.scrollbackSize() + r - termScroll_;
        const int n = std::min(static_cast<int>(row.size()), term_.cols());
        // Backgrounds in runs.
        for (int c = 0; c < n;) {
            const TermCell& cell = row[static_cast<std::size_t>(c)];
            const bool inv = cell.attr & TermCell::Inverse;
            std::uint32_t bgv = inv ? (cell.fg ? cell.fg : 0xFF000000u | 0xcdd0d6u) : cell.bg;
            bool sel = haveSel && ((absr > s0r || (absr == s0r && c >= s0c)) && (absr < s1r || (absr == s1r && c <= s1c)));
            int e = c + 1;
            while (e < n) {
                const TermCell& o = row[static_cast<std::size_t>(e)];
                const bool oi = o.attr & TermCell::Inverse;
                const std::uint32_t ob = oi ? (o.fg ? o.fg : 0xFF000000u | 0xcdd0d6u) : o.bg;
                const bool os = haveSel && ((absr > s0r || (absr == s0r && e >= s0c)) && (absr < s1r || (absr == s1r && e <= s1c)));
                if (ob != bgv || os != sel) break;
                ++e;
            }
            if (sel) fill(body.x + pad + static_cast<float>(c) * cwid, y, static_cast<float>(e - c) * cwid, lineH, Color::rgb(0x3a4152));
            else if (bgv) fill(body.x + pad + static_cast<float>(c) * cwid, y, static_cast<float>(e - c) * cwid, lineH, termColor(bgv, defBg));
            c = e;
        }
        for (int c = 0; c < n; ++c) {
            const TermCell& cell = row[static_cast<std::size_t>(c)];
            const bool inv = cell.attr & TermCell::Inverse;
            Color fg = inv ? (cell.bg ? termColor(cell.bg, defBg) : defBg) : termColor(cell.fg, defFg);
            if (cell.attr & TermCell::Dim) fg = fg.alpha(150);
            if (cell.attr & TermCell::Bold && !cell.fg && !inv) fg = Color::rgb(0xffffff);
            const float x = body.x + pad + static_cast<float>(c) * cwid;
            if (cell.ch != U' ' && cell.ch != 0) dl_.glyph(font_.glyph(cell.ch), x, y + font_.baseline(), fg);
            if (cell.attr & TermCell::Underline) fill(x, y + lineH - std::max(2.0f, std::round(2 * s)), cwid, std::max(1.0f, std::round(s)), fg);
            if (cell.attr & TermCell::Strike) fill(x, y + lineH / 2, cwid, std::max(1.0f, std::round(s)), fg);
        }
    }
    // Cursor.
    if (termScroll_ == 0 && term_.cursorVisible() && termProc_) {
        const float cx = body.x + pad + static_cast<float>(term_.cursorCol()) * cwid, cy = body.y + static_cast<float>(term_.cursorRow()) * lineH;
        if (termFocus_ && focused_) {
            if (caretOn_) {
                fill(cx, cy, cwid, lineH, theme::caret.alpha(220));
                const TermCell& cell = term_.cell(term_.cursorRow(), term_.cursorCol());
                if (cell.ch != U' ') dl_.glyph(font_.glyph(cell.ch), cx, cy + font_.baseline(), defBg);
            }
        } else {
            const float t = std::max(1.0f, std::round(s));
            fill(cx, cy, cwid, t, theme::caret); fill(cx, cy + lineH - t, cwid, t, theme::caret);
            fill(cx, cy, t, lineH, theme::caret); fill(cx + cwid - t, cy, t, lineH, theme::caret);
        }
    }
    // Scrollback indicator.
    if (termScroll_ > 0) {
        const float frac = static_cast<float>(termScroll_) / static_cast<float>(std::max(1, term_.scrollbackSize()));
        const float th = std::max(20 * s, body.h * 0.15f);
        fill(body.right() - 8 * s, body.y + (body.h - th) * (1.0f - frac), 4 * s, th, theme::scrollbar, 2 * s);
    }
}

}  // namespace ide
