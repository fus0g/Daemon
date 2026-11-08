// daemon.terminal — a small VT100 / xterm terminal emulator (screen buffer, scrollback, colours, alternate screen).
// Pure logic: feed() consumes the bytes a program writes to its terminal, the grid can then be drawn by the UI.
export module daemon.services.terminal;
import std;

export namespace ide {

struct TermCell {
    char32_t ch = U' ';
    std::uint32_t fg = 0, bg = 0;   // 0 = default colour, otherwise 0xFF000000 | 0xRRGGBB
    std::uint8_t attr = 0;          // bit flags below
    static constexpr std::uint8_t Bold = 1, Dim = 2, Italic = 4, Underline = 8, Inverse = 16, Strike = 32;
    bool operator==(const TermCell&) const = default;
};

class Terminal {
public:
    Terminal(int cols = 80, int rows = 24) { resize(cols, rows); }

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    int cursorRow() const { return cr_; }
    int cursorCol() const { return cc_; }
    bool cursorVisible() const { return showCursor_; }
    bool altScreen() const { return alt_; }
    bool appCursorKeys() const { return appCursor_; }
    bool bracketedPaste() const { return bracketed_; }
    const std::string& title() const { return title_; }
    std::uint64_t version() const { return version_; }

    // Cell of the live screen (row 0 = top).
    const TermCell& cell(int r, int c) const { return screen()[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)]; }

    int scrollbackSize() const { return static_cast<int>(history_.size()); }
    // Row `r` of the view that is `offset` lines scrolled back from the live screen (offset 0 = live).
    // Returns nullptr-free access through a copy-free span.
    std::span<const TermCell> viewRow(int r, int offset) const;

    // Absolute rows: scrollback first, then the live screen (stable while output arrives; used for selections).
    int totalRows() const { return static_cast<int>(history_.size()) + rows_; }
    std::span<const TermCell> absRow(int a) const {
        const int h = static_cast<int>(history_.size());
        if (a < 0 || a >= h + rows_) return {};
        if (a < h) { const auto& row = history_[static_cast<std::size_t>(a)]; return {row.data(), row.size()}; }
        return {&screen()[static_cast<std::size_t>(a - h) * static_cast<std::size_t>(cols_)], static_cast<std::size_t>(cols_)};
    }

    void resize(int cols, int rows);

    // Bytes the terminal wants to send back to the program (answers to cursor-position / device queries).
    std::string takeReply() { return std::exchange(reply_, {}); }

    void feed(std::string_view bytes) {
        for (unsigned char b : bytes) byte(b);
        ++version_;
    }

    void reset();

    static constexpr int kMaxScrollback = 5000;

private:
    enum class St { Ground, Esc, EscInter, Csi, Osc, OscEsc, Ignore, IgnoreEsc };

    std::vector<TermCell>& screen() { return alt_ ? altBuf_ : main_; }
    const std::vector<TermCell>& screen() const { return alt_ ? altBuf_ : main_; }
    TermCell& at(int r, int c) { return screen()[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(c)]; }

    TermCell blank() const { TermCell c; c.bg = cur_.bg; return c; }   // erased cells keep the current background

    void byte(unsigned char b);

    void control(unsigned char b);

    void ground(unsigned char b);

    static char32_t decGraphics(char32_t c);

    void put(char32_t ch);

    void lineFeed() {
        if (cr_ == bottom_) scrollUp(1);
        else if (cr_ < rows_ - 1) ++cr_;
    }

    void scrollUp(int n);
    void scrollDown(int n);

    void escape(unsigned char b);

    void charset(char which, char set) { g_[which == '(' ? 0 : 1] = set; }

    std::vector<int> params(int def = 0) const;

    void csi(char f);

    void scrollUp2(int n);

    void modes(bool priv, const std::vector<int>& p, bool on);

    void switchScreen(bool toAlt, bool save);

    void eraseDisplay(int mode);
    void eraseLine(int mode) {
        const int a = mode == 0 ? cc_ : 0, b = mode == 1 ? cc_ + 1 : cols_;
        for (int c = a; c < std::min(b, cols_); ++c) at(cr_, c) = blank();
    }

    static std::uint32_t rgb(int r, int g, int b) {
        return 0xFF000000u | (static_cast<std::uint32_t>(std::clamp(r, 0, 255)) << 16) | (static_cast<std::uint32_t>(std::clamp(g, 0, 255)) << 8) | static_cast<std::uint32_t>(std::clamp(b, 0, 255));
    }
    static std::uint32_t palette(int i);

    void sgr(const std::vector<int>& p);

    void osc() {
        const auto semi = oscBuf_.find(';');
        if (semi == std::string::npos) return;
        const int code = std::atoi(oscBuf_.substr(0, semi).c_str());
        if (code == 0 || code == 2) { title_ = oscBuf_.substr(semi + 1); ++version_; }
    }

    struct Saved { int r = 0, c = 0; TermCell attr; char g0 = 'B', g1 = 'B'; int gl = 0; };

    int cols_ = 0, rows_ = 0, cr_ = 0, cc_ = 0, top_ = 0, bottom_ = 0;
    std::vector<TermCell> main_, altBuf_;
    std::deque<std::vector<TermCell>> history_;
    TermCell cur_;
    Saved saved_;
    bool hasSaved_ = false, alt_ = false, showCursor_ = true, appCursor_ = false, bracketed_ = false, autowrap_ = true, wrapNext_ = false;
    std::vector<bool> tabs_;
    St st_ = St::Ground;
    std::string csiBuf_, csiInter_, oscBuf_, reply_, title_;
    char inter_ = 0;
    char g_[2] = {'B', 'B'};
    int gl_ = 0;
    std::uint32_t utf_ = 0;
    int need_ = 0;
    std::uint64_t version_ = 0;
};

}  // namespace ide
