// buffer - implementation of module daemon.text.buffer.
module daemon.text.buffer;
import std;
import daemon.base.util;

namespace ide {

void Buffer::setText(std::string_view text) {
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

std::string Buffer::text() const {
    std::string out;
    const char* eol = crlf_ ? "\r\n" : "\n";
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += eol;
        out += lines_[i];
    }
    return out;
}

std::string Buffer::slice(Pos a, Pos b) const {
    if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
    std::string out = lines_[a.line].substr(a.col);
    for (int l = a.line + 1; l < b.line; ++l) { out += '\n'; out += lines_[l]; }
    out += '\n';
    out += lines_[b.line].substr(0, b.col);
    return out;
}

Pos Buffer::replace(Pos a, Pos b, std::string_view text, Selection before, bool coalesce) {
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

std::optional<Selection> Buffer::undo() {
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

std::optional<Selection> Buffer::redo() {
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

Pos Buffer::insertRaw(Pos at, std::string_view text) {
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

void Buffer::eraseRaw(Pos a, Pos b) {
    touch(a.line);
    if (a.line == b.line) {
        lines_[a.line].erase(static_cast<std::size_t>(a.col), static_cast<std::size_t>(b.col - a.col));
        return;
    }
    lines_[a.line].resize(static_cast<std::size_t>(a.col));
    lines_[a.line] += lines_[b.line].substr(static_cast<std::size_t>(b.col));
    lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
}

}  // namespace ide
