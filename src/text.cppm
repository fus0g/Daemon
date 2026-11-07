// ide.text — the text buffer: lines of UTF-8, with grouped/coalescing undo & redo.
export module ide.text;
import std;
import ide.util;

export namespace ide {

struct Pos {
    int line = 0;
    int col = 0;  // byte offset into the line
    friend constexpr auto operator<=>(const Pos&, const Pos&) = default;
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
    void setText(std::string_view text) {
        lines_.clear();
        crlf_ = text.find("\r\n") != std::string_view::npos &&
                text.find("\r\n") == text.find('\n') - 1;
        std::size_t start = 0;
        while (true) {
            auto nl = text.find('\n', start);
            auto len = (nl == std::string_view::npos ? text.size() : nl) - start;
            std::string_view l = text.substr(start, len);
            if (nl != std::string_view::npos && !l.empty() && l.back() == '\r') l.remove_suffix(1);
            lines_.emplace_back(l);
            if (nl == std::string_view::npos) break;
            start = nl + 1;
        }
        undo_.clear();
        redo_.clear();
        savedGroup_ = 0;
        noMerge_ = true;
        ++version_;
        dirty_ = 0;
    }

    std::string text() const {
        std::string out;
        const char* eol = crlf_ ? "\r\n" : "\n";
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            if (i) out += eol;
            out += lines_[i];
        }
        return out;
    }

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
    std::string slice(Pos a, Pos b) const {
        if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
        std::string out = lines_[a.line].substr(a.col);
        for (int l = a.line + 1; l < b.line; ++l) { out += '\n'; out += lines_[l]; }
        out += '\n';
        out += lines_[b.line].substr(0, b.col);
        return out;
    }

    // ----- recorded edits (undoable)

    // Replaces [a, b) with `text`; returns the position after the inserted text.
    // `before` is the selection to restore on undo. With `coalesce`, adjacent typing /
    // backspacing is merged into the previous undo step.
    Pos replace(Pos a, Pos b, std::string_view text, Selection before, bool coalesce = false) {
        a = clamp(a);
        b = clamp(b);
        if (b < a) std::swap(a, b);
        std::string removed = slice(a, b);
        if (removed.empty() && text.empty()) return a;
        const auto now = Clock::now();

        if (coalesce && !noMerge_ && !groupOpen_ && !undo_.empty() && removed.find('\n') == npos &&
            text.find('\n') == npos) {
            Edit& p = undo_.back();
            if (now - p.time < std::chrono::milliseconds(1500) && p.inserted.find('\n') == npos &&
                p.removed.find('\n') == npos) {
                if (p.removed.empty() && !p.inserted.empty() && removed.empty() &&
                    a == endOf(p.at, p.inserted)) {  // continued typing
                    Pos end = insertRaw(a, text);
                    p.inserted += text;
                    p.time = now;
                    redo_.clear();
                    return end;
                }
                if (p.inserted.empty() && !p.removed.empty() && text.empty() && !removed.empty()) {
                    if (b == p.at) {  // backspace run
                        eraseRaw(a, b);
                        p.at = a;
                        p.removed = removed + p.removed;
                        p.time = now;
                        redo_.clear();
                        return a;
                    }
                    if (a == p.at) {  // delete-forward run
                        eraseRaw(a, b);
                        p.removed += removed;
                        p.time = now;
                        redo_.clear();
                        return a;
                    }
                }
            }
        }

        Edit e{a, std::move(removed), std::string(text), before, groupOpen_ ? group_ : ++groupCounter_, now};
        if (!e.removed.empty()) eraseRaw(a, b);
        Pos end = e.inserted.empty() ? a : insertRaw(a, e.inserted);
        undo_.push_back(std::move(e));
        redo_.clear();
        noMerge_ = false;
        return end;
    }

    // Everything between begin/end is undone as one step.
    void beginGroup() { groupOpen_ = true; group_ = ++groupCounter_; noMerge_ = true; }
    void endGroup() { groupOpen_ = false; noMerge_ = true; }
    // Prevents the next edit from merging into the previous undo step.
    void breakGroup() { noMerge_ = true; }

    std::optional<Selection> undo() {
        if (undo_.empty()) return std::nullopt;
        const auto g = undo_.back().group;
        Selection result;
        while (!undo_.empty() && undo_.back().group == g) {
            Edit e = std::move(undo_.back());
            undo_.pop_back();
            eraseRaw(e.at, endOf(e.at, e.inserted));
            if (!e.removed.empty()) insertRaw(e.at, e.removed);
            result = e.before;
            redo_.push_back(std::move(e));
        }
        noMerge_ = true;
        return result;
    }

    std::optional<Selection> redo() {
        if (redo_.empty()) return std::nullopt;
        const auto g = redo_.back().group;
        Pos result;
        while (!redo_.empty() && redo_.back().group == g) {
            Edit e = std::move(redo_.back());
            redo_.pop_back();
            eraseRaw(e.at, endOf(e.at, e.removed));
            result = e.inserted.empty() ? e.at : insertRaw(e.at, e.inserted);
            undo_.push_back(std::move(e));
        }
        noMerge_ = true;
        return Selection{result, result};
    }

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

    Pos insertRaw(Pos at, std::string_view text) {
        touch(at.line);
        auto nl = text.find('\n');
        std::string& l = lines_[static_cast<std::size_t>(at.line)];
        if (nl == npos) {
            l.insert(static_cast<std::size_t>(at.col), text);
            return {at.line, at.col + static_cast<int>(text.size())};
        }
        std::string tail = l.substr(static_cast<std::size_t>(at.col));
        l.resize(static_cast<std::size_t>(at.col));
        l.append(text.substr(0, nl));
        std::vector<std::string> added;
        std::size_t start = nl + 1;
        while (true) {
            auto next = text.find('\n', start);
            if (next == npos) { added.emplace_back(text.substr(start)); break; }
            added.emplace_back(text.substr(start, next - start));
            start = next + 1;
        }
        Pos end{at.line + static_cast<int>(added.size()), static_cast<int>(added.back().size())};
        added.back() += tail;
        lines_.insert(lines_.begin() + at.line + 1, std::make_move_iterator(added.begin()),
                      std::make_move_iterator(added.end()));
        return end;
    }

    void eraseRaw(Pos a, Pos b) {
        touch(a.line);
        if (a.line == b.line) {
            lines_[a.line].erase(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
            return;
        }
        lines_[a.line].resize(static_cast<std::size_t>(a.col));
        lines_[a.line] += lines_[b.line].substr(static_cast<std::size_t>(b.col));
        lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    }

    std::vector<std::string> lines_;
    std::vector<Edit> undo_, redo_;
    std::uint64_t groupCounter_ = 0, group_ = 0, savedGroup_ = 0, version_ = 1;
    int dirty_ = std::numeric_limits<int>::max();
    bool groupOpen_ = false, noMerge_ = true, crlf_ = false;
};

}  // namespace ide
