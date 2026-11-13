// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.text — the text buffer: lines of UTF-8, with grouped/coalescing undo & redo.
export module daemon.text.buffer;
import std;
import daemon.base.util;

export namespace ide {

struct Pos {
    int line = 0;
    int col = 0;  // byte offset into the line
    constexpr auto operator<=>(const Pos&) const = default;
    constexpr bool operator==(const Pos&) const = default;
};

struct Selection {
    Pos anchor;
    Pos cursor;
};

// Position reached after inserting `text` at `at`.
inline Pos endOf(Pos at, std::string_view text) {
    auto nl = std::ranges::count(text, '\n');
    if (nl == 0) return {at.line, at.col + static_cast<int>(text.size())};
    auto last = text.rfind('\n');
    return {at.line + static_cast<int>(nl), static_cast<int>(text.size() - last - 1)};
}

class Buffer {
public:
    Buffer() : lines_(1) {}

    // ----- content
    void setText(std::string_view text);

    std::string text() const;

    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int i) const { return lines_[static_cast<std::size_t>(i)]; }
    bool crlf() const { return crlf_; }
    void setCrlf(bool v) { crlf_ = v; }
    Pos end() const { return {lineCount() - 1, static_cast<int>(lines_.back().size())}; }

    Pos clamp(Pos p) const {
        p.line = std::clamp(p.line, 0, lineCount() - 1);
        p.col = std::clamp(p.col, 0, static_cast<int>(lines_[static_cast<std::size_t>(p.line)].size()));
        return p;
    }

    // Text in [a, b); requires a <= b and both valid.
    std::string slice(Pos a, Pos b) const;

    // ----- recorded edits (undoable)

    // Replaces [a, b) with `text`; returns the position after the inserted text.
    // `before` is the selection to restore on undo. With `coalesce`, adjacent typing /
    // backspacing is merged into the previous undo step.
    Pos replace(Pos a, Pos b, std::string_view text, Selection before, bool coalesce = false);

    // A read-only buffer ignores every edit (used by the Markdown preview tab).
    bool readOnly = false;

    // Everything between begin/end is undone as one step.
    void beginGroup() { groupOpen_ = true; group_ = ++groupCounter_; noMerge_ = true; }
    void endGroup() { groupOpen_ = false; noMerge_ = true; }
    // Prevents the next edit from merging into the previous undo step.
    void breakGroup() { noMerge_ = true; }

    std::optional<Selection> undo();

    std::optional<Selection> redo();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // ----- change tracking
    std::uint64_t version() const { return version_; }
    // Lowest line changed since the last call (INT_MAX if nothing changed).
    int takeDirtyLine() { int d = dirty_; dirty_ = std::numeric_limits<int>::max(); return d; }

    void markSaved() { savedGroup_ = topGroup(); noMerge_ = true; }
    bool modified() const { return topGroup() != savedGroup_; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr auto npos = std::string::npos;

    struct Edit {
        Pos at;
        std::string removed, inserted;
        Selection before;
        std::uint64_t group;
        Clock::time_point time;
    };

    std::uint64_t topGroup() const { return undo_.empty() ? 0 : undo_.back().group; }

    void touch(int line) {
        ++version_;
        dirty_ = std::min(dirty_, line);
    }

    Pos insertRaw(Pos at, std::string_view text);

    void eraseRaw(Pos a, Pos b);

    std::vector<std::string> lines_;
    std::vector<Edit> undo_, redo_;
    std::uint64_t groupCounter_ = 0, group_ = 0, savedGroup_ = 0, version_ = 1;
    int dirty_ = std::numeric_limits<int>::max();
    bool groupOpen_ = false, noMerge_ = true, crlf_ = false;
};

}  // namespace ide
