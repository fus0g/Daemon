// SPDX-License-Identifier: GPL-3.0-or-later
// terminal - implementation of module daemon.services.terminal.
module daemon.services.terminal;
import std;

namespace ide {

std::span<const TermCell> Terminal::viewRow(int r, int offset) const {
    const int idx = r - offset;   // < 0: inside the scrollback
    if (idx >= 0) return {&screen()[static_cast<std::size_t>(idx) * static_cast<std::size_t>(cols_)], static_cast<std::size_t>(cols_)};
    const int h = static_cast<int>(history_.size()) + idx;
    if (h < 0 || alt_) return {};
    const auto& row = history_[static_cast<std::size_t>(h)];
    return {row.data(), row.size()};
}

void Terminal::resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;
    auto remake = [&](std::vector<TermCell>& g) {
        std::vector<TermCell> n(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
        const int keepRows = std::min(rows, rows_), keepCols = std::min(cols, cols_);
        // Keep the bottom rows of the old screen (the prompt is down there).
        const int shift = std::max(0, cr_ - rows + 1);
        for (int r = 0; r < keepRows; ++r)
            for (int c = 0; c < keepCols; ++c)
                if (!g.empty() && r + shift < rows_) n[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)] =
                    g[static_cast<std::size_t>(r + shift) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)];
        g = std::move(n);
        return shift;
    };
    const int shift = remake(main_);
    remake(altBuf_);
    cr_ = std::clamp(cr_ - shift, 0, rows - 1);
    cols_ = cols;
    rows_ = rows;
    cc_ = std::min(cc_, cols_ - 1);
    top_ = 0;
    bottom_ = rows_ - 1;
    tabs_.assign(static_cast<std::size_t>(cols_), false);
    for (int c = 8; c < cols_; c += 8) tabs_[static_cast<std::size_t>(c)] = true;
    wrapNext_ = false;
    ++version_;
}

void Terminal::reset() {
    main_.assign(main_.size(), TermCell{});
    altBuf_.assign(altBuf_.size(), TermCell{});
    history_.clear();
    alt_ = false;
    cr_ = cc_ = 0;
    top_ = 0;
    bottom_ = rows_ - 1;
    cur_ = {};
    showCursor_ = true;
    appCursor_ = bracketed_ = false;
    wrapNext_ = false;
    ++version_;
}

void Terminal::byte(unsigned char b) {
    // C0 controls work in every state (except inside strings).
    if (st_ == St::Ground || st_ == St::Esc || st_ == St::EscInter || st_ == St::Csi) {
        if (b == 0x1b) { st_ = St::Esc; return; }
        if (b < 0x20 || b == 0x7f) { control(b); return; }
    }
    switch (st_) {
        case St::Ground: ground(b); break;
        case St::Esc: escape(b); break;
        case St::EscInter: st_ = St::Ground; if (inter_ == '(' || inter_ == ')') charset(inter_, static_cast<char>(b)); break;
        case St::Csi:
            if ((b >= '0' && b <= '9') || b == ';' || b == ':') csiBuf_ += static_cast<char>(b);
            else if (b >= 0x3c && b <= 0x3f) csiBuf_ += static_cast<char>(b);   // private-mode marker '?', '>' ...
            else if (b >= 0x20 && b <= 0x2f) csiInter_ += static_cast<char>(b);
            else { csi(static_cast<char>(b)); st_ = St::Ground; }
            break;
        case St::Osc:
            if (b == 0x07) { osc(); st_ = St::Ground; }
            else if (b == 0x1b) st_ = St::OscEsc;
            else if (oscBuf_.size() < 4096) oscBuf_ += static_cast<char>(b);
            break;
        case St::OscEsc: osc(); st_ = St::Ground; if (b != '\\') escape(b); break;
        case St::Ignore: if (b == 0x07) st_ = St::Ground; else if (b == 0x1b) st_ = St::IgnoreEsc; break;
        case St::IgnoreEsc: st_ = St::Ground; break;
    }
}

void Terminal::control(unsigned char b) {
    switch (b) {
        case 0x08: if (cc_ > 0) --cc_; wrapNext_ = false; break;                     // BS
        case 0x09: {                                                                  // HT
            int c = cc_ + 1;
            while (c < cols_ - 1 && !tabs_[static_cast<std::size_t>(c)]) ++c;
            cc_ = std::min(c, cols_ - 1);
            wrapNext_ = false;
            break;
        }
        case 0x0a: case 0x0b: case 0x0c: lineFeed(); break;                          // LF VT FF
        case 0x0d: cc_ = 0; wrapNext_ = false; break;                                // CR
        case 0x0e: gl_ = 1; break;
        case 0x0f: gl_ = 0; break;
        default: break;
    }
}

void Terminal::ground(unsigned char b) {
    if (b >= 0x80) {   // UTF-8
        if (b >= 0xf0) { utf_ = b & 0x07; need_ = 3; }
        else if (b >= 0xe0) { utf_ = b & 0x0f; need_ = 2; }
        else if (b >= 0xc0) { utf_ = b & 0x1f; need_ = 1; }
        else if (need_ > 0) { utf_ = (utf_ << 6) | (b & 0x3f); if (--need_ == 0) put(static_cast<char32_t>(utf_)); }
        return;
    }
    need_ = 0;
    put(static_cast<char32_t>(b));
}

char32_t Terminal::decGraphics(char32_t c) {
    switch (c) {
        case 'j': return U'┘'; case 'k': return U'┐'; case 'l': return U'┌'; case 'm': return U'└'; case 'n': return U'┼';
        case 'q': return U'─'; case 't': return U'├'; case 'u': return U'┤'; case 'v': return U'┴'; case 'w': return U'┬';
        case 'x': return U'│'; case 'a': return U'▒'; case '`': return U'◆'; case 'f': return U'°'; case 'g': return U'±';
        case '~': return U'·'; case 'y': return U'≤'; case 'z': return U'≥'; case '{': return U'π'; case '|': return U'≠';
        default: return c;
    }
}

void Terminal::put(char32_t ch) {
    if (g_[gl_] == '0' && ch < 0x80) ch = decGraphics(ch);
    if (wrapNext_) {
        if (autowrap_) { cc_ = 0; lineFeed(); }
        wrapNext_ = false;
    }
    TermCell& c = at(cr_, cc_);
    c = cur_;
    c.ch = ch;
    if (cc_ + 1 >= cols_) wrapNext_ = true; else ++cc_;
}

void Terminal::scrollUp(int n) {   // content moves up inside the scroll region
    for (int i = 0; i < n; ++i) {
        if (!alt_ && top_ == 0) {   // lines leaving the top of the main screen go to the scrollback
            std::vector<TermCell> row(main_.begin() + 0, main_.begin() + cols_);
            history_.push_back(std::move(row));
            if (static_cast<int>(history_.size()) > kMaxScrollback) history_.pop_front();
        }
        auto& g = screen();
        std::move(g.begin() + static_cast<std::ptrdiff_t>((top_ + 1) * cols_), g.begin() + static_cast<std::ptrdiff_t>((bottom_ + 1) * cols_),
                  g.begin() + static_cast<std::ptrdiff_t>(top_ * cols_));
        std::fill(g.begin() + static_cast<std::ptrdiff_t>(bottom_ * cols_), g.begin() + static_cast<std::ptrdiff_t>((bottom_ + 1) * cols_), blank());
    }
}

void Terminal::scrollDown(int n) {
    for (int i = 0; i < n; ++i) {
        auto& g = screen();
        std::move_backward(g.begin() + static_cast<std::ptrdiff_t>(top_ * cols_), g.begin() + static_cast<std::ptrdiff_t>(bottom_ * cols_),
                           g.begin() + static_cast<std::ptrdiff_t>((bottom_ + 1) * cols_));
        std::fill(g.begin() + static_cast<std::ptrdiff_t>(top_ * cols_), g.begin() + static_cast<std::ptrdiff_t>((top_ + 1) * cols_), blank());
    }
}

void Terminal::escape(unsigned char b) {
    st_ = St::Ground;
    switch (b) {
        case '[': st_ = St::Csi; csiBuf_.clear(); csiInter_.clear(); break;
        case ']': st_ = St::Osc; oscBuf_.clear(); break;
        case 'P': case '_': case '^': case 'X': st_ = St::Ignore; break;   // DCS / APC / PM / SOS: skipped
        case '(': case ')': case '*': case '+': st_ = St::EscInter; inter_ = static_cast<char>(b); break;
        case '7': saved_ = {cr_, cc_, cur_, g_[0], g_[1], gl_}; hasSaved_ = true; break;
        case '8': if (hasSaved_) { cr_ = saved_.r; cc_ = saved_.c; cur_ = saved_.attr; g_[0] = saved_.g0; g_[1] = saved_.g1; gl_ = saved_.gl; wrapNext_ = false; } break;
        case 'D': lineFeed(); break;
        case 'E': cc_ = 0; lineFeed(); break;
        case 'M': if (cr_ == top_) scrollDown(1); else if (cr_ > 0) --cr_; break;
        case 'H': if (cc_ < cols_) tabs_[static_cast<std::size_t>(cc_)] = true; break;
        case 'c': reset(); break;
        case '=': case '>': break;
        default: break;
    }
}

std::vector<int> Terminal::params(int def) const {
    std::vector<int> out;
    std::string_view s = csiBuf_;
    if (!s.empty() && (s[0] == '?' || s[0] == '>' || s[0] == '<' || s[0] == '=')) s.remove_prefix(1);
    std::size_t i = 0;
    while (true) {
        auto e = s.find_first_of(";:", i);
        std::string_view tok = s.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i);
        int v = def;
        if (!tok.empty()) { v = 0; std::from_chars(tok.data(), tok.data() + tok.size(), v); }
        out.push_back(v);
        if (e == std::string_view::npos) break;
        i = e + 1;
    }
    return out;
}

void Terminal::csi(char f) {
    const bool priv = !csiBuf_.empty() && csiBuf_[0] == '?';
    auto p = params(0);
    auto n = [&](int idx, int def = 1) { return idx < static_cast<int>(p.size()) && p[static_cast<std::size_t>(idx)] > 0 ? p[static_cast<std::size_t>(idx)] : def; };
    wrapNext_ = false;
    switch (f) {
        case 'A': cr_ = std::max(cr_ - n(0), cr_ >= top_ ? top_ : 0); break;
        case 'B': cr_ = std::min(cr_ + n(0), cr_ <= bottom_ ? bottom_ : rows_ - 1); break;
        case 'C': cc_ = std::min(cc_ + n(0), cols_ - 1); break;
        case 'D': cc_ = std::max(cc_ - n(0), 0); break;
        case 'E': cr_ = std::min(cr_ + n(0), rows_ - 1); cc_ = 0; break;
        case 'F': cr_ = std::max(cr_ - n(0), 0); cc_ = 0; break;
        case 'G': case '`': cc_ = std::clamp(n(0) - 1, 0, cols_ - 1); break;
        case 'H': case 'f': cr_ = std::clamp(n(0) - 1, 0, rows_ - 1); cc_ = std::clamp(n(1) - 1, 0, cols_ - 1); break;
        case 'd': cr_ = std::clamp(n(0) - 1, 0, rows_ - 1); break;
        case 'J': eraseDisplay(p.empty() ? 0 : p[0]); break;
        case 'K': eraseLine(p.empty() ? 0 : p[0]); break;
        case 'L': if (cr_ >= top_ && cr_ <= bottom_) { const int saveTop = top_; top_ = cr_; scrollDown(n(0)); top_ = saveTop; } break;
        case 'M': if (cr_ >= top_ && cr_ <= bottom_) { const int saveTop = top_; top_ = cr_; scrollUp2(n(0)); top_ = saveTop; } break;
        case 'P': {   // delete characters
            const int k = std::min(n(0), cols_ - cc_);
            for (int c = cc_; c < cols_ - k; ++c) at(cr_, c) = at(cr_, c + k);
            for (int c = cols_ - k; c < cols_; ++c) at(cr_, c) = blank();
            break;
        }
        case '@': {   // insert characters
            const int k = std::min(n(0), cols_ - cc_);
            for (int c = cols_ - 1; c >= cc_ + k; --c) at(cr_, c) = at(cr_, c - k);
            for (int c = cc_; c < cc_ + k; ++c) at(cr_, c) = blank();
            break;
        }
        case 'X': for (int c = cc_; c < std::min(cols_, cc_ + n(0)); ++c) at(cr_, c) = blank(); break;
        case 'S': scrollUp(n(0)); break;
        case 'T': scrollDown(n(0)); break;
        case 'm': sgr(p); break;
        case 'r': {
            const int t = std::clamp(n(0) - 1, 0, rows_ - 1), b = std::clamp((p.size() > 1 && p[1] > 0 ? p[1] : rows_) - 1, 0, rows_ - 1);
            if (t < b) { top_ = t; bottom_ = b; }
            cr_ = 0; cc_ = 0;
            break;
        }
        case 's': saved_ = {cr_, cc_, cur_, g_[0], g_[1], gl_}; hasSaved_ = true; break;
        case 'u': if (hasSaved_) { cr_ = saved_.r; cc_ = saved_.c; } break;
        case 'h': case 'l': modes(priv, p, f == 'h'); break;
        case 'n':
            if (!p.empty() && p[0] == 6) reply_ += std::format("\x1b[{};{}R", cr_ + 1, cc_ + 1);
            else if (!p.empty() && p[0] == 5) reply_ += "\x1b[0n";
            break;
        case 'c': if (csiBuf_.empty() || csiBuf_[0] != '>') reply_ += "\x1b[?1;2c"; else reply_ += "\x1b[>0;0;0c"; break;
        case 'g': if (p.empty() || p[0] == 0) { if (cc_ < cols_) tabs_[static_cast<std::size_t>(cc_)] = false; } else if (p[0] == 3) tabs_.assign(tabs_.size(), false); break;
        default: break;
    }
}

void Terminal::scrollUp2(int n) {   // delete lines inside the region (no scrollback)
    for (int i = 0; i < n; ++i) {
        auto& g = screen();
        std::move(g.begin() + static_cast<std::ptrdiff_t>((top_ + 1) * cols_), g.begin() + static_cast<std::ptrdiff_t>((bottom_ + 1) * cols_),
                  g.begin() + static_cast<std::ptrdiff_t>(top_ * cols_));
        std::fill(g.begin() + static_cast<std::ptrdiff_t>(bottom_ * cols_), g.begin() + static_cast<std::ptrdiff_t>((bottom_ + 1) * cols_), blank());
    }
}

void Terminal::modes(bool priv, const std::vector<int>& p, bool on) {
    for (int m : p) {
        if (!priv) continue;
        switch (m) {
            case 1: appCursor_ = on; break;
            case 7: autowrap_ = on; break;
            case 25: showCursor_ = on; break;
            case 47: case 1047: case 1049: switchScreen(on, m == 1049); break;
            case 2004: bracketed_ = on; break;
            default: break;
        }
    }
}

void Terminal::switchScreen(bool toAlt, bool save) {
    if (toAlt == alt_) return;
    if (toAlt) {
        if (save) { saved_ = {cr_, cc_, cur_, g_[0], g_[1], gl_}; hasSaved_ = true; }
        alt_ = true;
        altBuf_.assign(altBuf_.size(), blank());
        cr_ = cc_ = 0;
    } else {
        alt_ = false;
        if (save && hasSaved_) { cr_ = saved_.r; cc_ = saved_.c; cur_ = saved_.attr; }
    }
    top_ = 0;
    bottom_ = rows_ - 1;
    wrapNext_ = false;
}

void Terminal::eraseDisplay(int mode) {
    auto& g = screen();
    if (mode == 0) {
        for (int c = cc_; c < cols_; ++c) at(cr_, c) = blank();
        std::fill(g.begin() + static_cast<std::ptrdiff_t>((cr_ + 1) * cols_), g.end(), blank());
    } else if (mode == 1) {
        std::fill(g.begin(), g.begin() + static_cast<std::ptrdiff_t>(cr_ * cols_), blank());
        for (int c = 0; c <= cc_; ++c) at(cr_, c) = blank();
    } else {
        std::fill(g.begin(), g.end(), blank());
        if (mode == 3) history_.clear();
    }
}

std::uint32_t Terminal::palette(int i) {   // 0..255 -> colour
    static constexpr std::array<std::uint32_t, 16> base = {0x1d2026, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xabb2bf,
                                                          0x5c6370, 0xff7b86, 0xaee58a, 0xf0d28a, 0x78bdff, 0xd98cf0, 0x6fd0dc, 0xffffff};
    if (i < 16) return 0xFF000000u | base[static_cast<std::size_t>(i)];
    if (i < 232) {
        const int v = i - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
        auto lvl = [](int x) { return x == 0 ? 0 : 55 + 40 * x; };
        return rgb(lvl(r), lvl(g), lvl(b));
    }
    const int l = 8 + 10 * (i - 232);
    return rgb(l, l, l);
}

void Terminal::sgr(const std::vector<int>& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const int v = p[i];
        if (v == 0) cur_ = {};
        else if (v == 1) cur_.attr |= TermCell::Bold;
        else if (v == 2) cur_.attr |= TermCell::Dim;
        else if (v == 3) cur_.attr |= TermCell::Italic;
        else if (v == 4) cur_.attr |= TermCell::Underline;
        else if (v == 7) cur_.attr |= TermCell::Inverse;
        else if (v == 9) cur_.attr |= TermCell::Strike;
        else if (v == 22) cur_.attr &= static_cast<std::uint8_t>(~(TermCell::Bold | TermCell::Dim));
        else if (v == 23) cur_.attr &= static_cast<std::uint8_t>(~TermCell::Italic);
        else if (v == 24) cur_.attr &= static_cast<std::uint8_t>(~TermCell::Underline);
        else if (v == 27) cur_.attr &= static_cast<std::uint8_t>(~TermCell::Inverse);
        else if (v == 29) cur_.attr &= static_cast<std::uint8_t>(~TermCell::Strike);
        else if (v >= 30 && v <= 37) cur_.fg = palette(v - 30);
        else if (v == 39) cur_.fg = 0;
        else if (v >= 40 && v <= 47) cur_.bg = palette(v - 40);
        else if (v == 49) cur_.bg = 0;
        else if (v >= 90 && v <= 97) cur_.fg = palette(v - 90 + 8);
        else if (v >= 100 && v <= 107) cur_.bg = palette(v - 100 + 8);
        else if (v == 38 || v == 48) {
            std::uint32_t& dst = v == 38 ? cur_.fg : cur_.bg;
            if (i + 1 < p.size() && p[i + 1] == 5 && i + 2 < p.size()) { dst = palette(p[i + 2]); i += 2; }
            else if (i + 1 < p.size() && p[i + 1] == 2 && i + 4 < p.size()) { dst = rgb(p[i + 2], p[i + 3], p[i + 4]); i += 4; }
        }
    }
}

}  // namespace ide
