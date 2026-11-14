// SPDX-License-Identifier: GPL-3.0-or-later
// document - implementation of module daemon.text.document.
module;
#include <cerrno>
module daemon.text.document;
import std;
import daemon.base.util;
import daemon.text.buffer;
import daemon.text.syntax;

namespace ide {

std::vector<int> findAll(std::string_view line, std::string_view needle, bool matchCase) {
    std::vector<int> out;
    if (needle.empty() || line.size() < needle.size()) return out;
    auto eq = [&](char a, char b) {
        return matchCase ? a == b : std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    for (std::size_t i = 0; i + needle.size() <= line.size();) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size() && ok; ++k) ok = eq(line[i + k], needle[k]);
        if (ok) { out.push_back(static_cast<int>(i)); i += needle.size(); } else ++i;
    }
    return out;
}

std::vector<int> matchStarts(std::string_view line, std::string_view needle, bool matchCase) {
    std::vector<int> out;
    if (needle.empty() || line.size() < needle.size()) return out;
    for (std::size_t i = 0; i + needle.size() <= line.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size() && ok; ++k) {
            unsigned char a = static_cast<unsigned char>(line[i + k]), b = static_cast<unsigned char>(needle[k]);
            ok = matchCase ? a == b : std::tolower(a) == std::tolower(b);
        }
        if (ok) out.push_back(static_cast<int>(i));
    }
    return out;
}

std::expected<void, std::string> Document::load(const std::string& file) {
    auto data = readFile(file);
    if (!data) return std::unexpected("cannot read " + file);
    if (data->size() > (256u << 20)) return std::unexpected("file is too large (>256 MiB)");
    if (data->substr(0, 8192).find('\0') != std::string::npos) return std::unexpected("binary file: " + file);
    if (data->starts_with("\xEF\xBB\xBF")) data->erase(0, 3);  // UTF-8 BOM
    buf.setText(*data);
    buf.markSaved();
    setPath(file);
    stampDisk();
    cursor = anchor = {};
    detectIndentation();
    return {};
}

std::expected<void, std::string> Document::save() {
    if (path.empty()) return std::unexpected("no file name");
    if (!writeFile(path, buf.text())) return std::unexpected("cannot write " + path + ": " + std::strerror(errno));
    buf.markSaved();
    stampDisk();
    return {};
}

std::pair<Pos, Pos> Document::wordRange(Pos p) const {
    p = buf.clamp(p);
    const std::string& l = buf.line(p.line);
    if (l.empty()) return {p, p};
    std::size_t at = std::min<std::size_t>(p.col, l.size() - 1);
    if (p.col >= static_cast<int>(l.size())) at = utf8Prev(l, l.size());
    std::size_t pos = at;
    const CharClass cls = classify(decodeUtf8(l, pos));
    std::size_t s = at, e = utf8Next(l, at);
    while (s > 0) {
        std::size_t pv = utf8Prev(l, s), t = pv;
        if (classify(decodeUtf8(l, t)) != cls) break;
        s = pv;
    }
    while (e < l.size()) {
        std::size_t t = e;
        if (classify(decodeUtf8(l, t)) != cls) break;
        e = t;
    }
    return {{p.line, static_cast<int>(s)}, {p.line, static_cast<int>(e)}};
}

void Document::moveLeft(bool extend, bool byWord) {
    if (!extend && hasSelection() && !byWord) { setCursor(selBegin()); return; }
    Pos p = cursor;
    if (byWord) p = wordLeft(p);
    else if (p.col > 0) p.col = static_cast<int>(utf8Prev(buf.line(p.line), p.col));
    else if (p.line > 0) p = {p.line - 1, static_cast<int>(buf.line(p.line - 1).size())};
    setCursor(p, extend);
}

void Document::moveRight(bool extend, bool byWord) {
    if (!extend && hasSelection() && !byWord) { setCursor(selEnd()); return; }
    Pos p = cursor;
    const std::string& l = buf.line(p.line);
    if (byWord) p = wordRight(p);
    else if (p.col < static_cast<int>(l.size())) p.col = static_cast<int>(utf8Next(l, p.col));
    else if (p.line + 1 < buf.lineCount()) p = {p.line + 1, 0};
    setCursor(p, extend);
}

void Document::moveVertical(int lines, bool extend) {
    int dcol = wantCol >= 0 ? wantCol : displayCol(buf.line(cursor.line), cursor.col, tabWidth);
    int target = cursor.line + lines;
    Pos p;
    if (target < 0) p = {0, 0};
    else if (target >= buf.lineCount()) p = buf.end();
    else p = {target, byteColAt(buf.line(target), dcol, tabWidth)};
    setCursor(p, extend);
    wantCol = dcol;
}

Pos Document::wordLeft(Pos p) const {
    if (p.col == 0) return p.line > 0 ? Pos{p.line - 1, static_cast<int>(buf.line(p.line - 1).size())} : p;
    const std::string& l = buf.line(p.line);
    std::size_t i = p.col;
    auto prevClass = [&](std::size_t at) {
        std::size_t s = utf8Prev(l, at), t = s;
        return classify(decodeUtf8(l, t));
    };
    while (i > 0 && prevClass(i) == CharClass::Space) i = utf8Prev(l, i);
    if (i > 0) {
        CharClass c = prevClass(i);
        while (i > 0 && prevClass(i) == c) i = utf8Prev(l, i);
    }
    return {p.line, static_cast<int>(i)};
}

Pos Document::wordRight(Pos p) const {
    const std::string& l = buf.line(p.line);
    if (p.col >= static_cast<int>(l.size()))
        return p.line + 1 < buf.lineCount() ? Pos{p.line + 1, 0} : p;
    std::size_t i = p.col;
    auto classAt = [&](std::size_t at) { std::size_t t = at; return classify(decodeUtf8(l, t)); };
    CharClass c = classAt(i);
    if (c != CharClass::Space) while (i < l.size() && classAt(i) == c) i = utf8Next(l, i);
    while (i < l.size() && classAt(i) == CharClass::Space) i = utf8Next(l, i);
    return {p.line, static_cast<int>(i)};
}

void Document::insertRaw(std::string_view t) {  // paste: no pairing, never coalesced
    buf.breakGroup();
    std::string clean;
    clean.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\r') { if (i + 1 < t.size() && t[i + 1] == '\n') continue; clean += '\n'; }
        else clean += t[i];
    }
    replaceSelection(clean);
    buf.breakGroup();
}

void Document::newline() {
    buf.breakGroup();
    Pos a = selBegin();
    const std::string& l = buf.line(a.line);
    std::size_t indentLen = std::min<std::size_t>(l.find_first_not_of(" \t") == std::string::npos ? l.size() : l.find_first_not_of(" \t"), a.col);
    std::string indent = l.substr(0, indentLen);

    char before = 0;
    for (int i = a.col - 1; i >= 0; --i)
        if (l[i] != ' ' && l[i] != '\t') { before = l[i]; break; }
    const Pos b = selEnd();
    const std::string& lb = buf.line(b.line);
    char after = b.col < static_cast<int>(lb.size()) ? lb[b.col] : 0;
    bool opens = before == '{' || before == '(' || before == '[' || (before == ':' && lang && lang->name == "Python");
    bool closes = (before == '{' && after == '}') || (before == '(' && after == ')') || (before == '[' && after == ']');

    if (opens && closes) {
        std::string mid = "\n" + indent + indentUnit();
        Pos end = buf.replace(a, b, mid + "\n" + indent, sel());
        (void)end;
        cursor = anchor = endOf(a, mid);
    } else {
        std::string text = "\n" + indent + (opens ? indentUnit() : std::string());
        Pos end = buf.replace(a, b, text, sel());
        cursor = anchor = end;
    }
    wantCol = -1;
    buf.breakGroup();
}

void Document::backspace() {
    if (hasSelection()) { buf.breakGroup(); deleteSelection(); return; }
    Pos c = cursor;
    if (c.col == 0) {
        if (c.line == 0) return;
        Pos a{c.line - 1, static_cast<int>(buf.line(c.line - 1).size())};
        buf.replace(a, c, {}, sel(), true);
        cursor = anchor = a;
    } else {
        const std::string& l = buf.line(c.line);
        std::size_t prev = utf8Prev(l, c.col);
        bool leadingSpaces = !useTabs && l.find_first_not_of(' ') >= static_cast<std::size_t>(c.col);
        if (leadingSpaces && c.col % tabWidth == 0) prev = static_cast<std::size_t>(c.col - tabWidth);
        else if (leadingSpaces) prev = static_cast<std::size_t>(c.col / tabWidth * tabWidth);
        Pos from{c.line, static_cast<int>(prev)}, to = c;
        // Remove an empty pair such as () or "" in one go.
        if (c.col < static_cast<int>(l.size()) && prev + 1 == static_cast<std::size_t>(c.col) && isPair(l[prev], l[c.col]))
            to.col += 1;
        buf.replace(from, to, {}, sel(), true);
        cursor = anchor = from;
    }
    wantCol = -1;
}

void Document::deleteForward() {
    if (hasSelection()) { buf.breakGroup(); deleteSelection(); return; }
    Pos c = cursor;
    const std::string& l = buf.line(c.line);
    Pos to;
    if (c.col < static_cast<int>(l.size())) to = {c.line, static_cast<int>(utf8Next(l, c.col))};
    else if (c.line + 1 < buf.lineCount()) to = {c.line + 1, 0};
    else return;
    buf.replace(c, to, {}, sel(), true);
    wantCol = -1;
}

void Document::deleteWordBack() {
    if (hasSelection()) { deleteSelection(); return; }
    buf.breakGroup();
    Pos from = wordLeft(cursor);
    buf.replace(from, cursor, {}, sel());
    cursor = anchor = from;
    buf.breakGroup();
}

void Document::deleteWordForward() {
    if (hasSelection()) { deleteSelection(); return; }
    buf.breakGroup();
    buf.replace(cursor, wordRight(cursor), {}, sel());
    anchor = cursor;
    buf.breakGroup();
}

void Document::deleteLines() {
    buf.breakGroup();
    auto [a, b] = lineSpan();
    Pos from, to;
    if (b + 1 < buf.lineCount()) { from = {a, 0}; to = {b + 1, 0}; }
    else if (a > 0) { from = {a - 1, static_cast<int>(buf.line(a - 1).size())}; to = buf.end(); }
    else { from = {0, 0}; to = buf.end(); }
    buf.replace(from, to, {}, sel());
    cursor = anchor = buf.clamp({a, cursor.col});
    wantCol = -1;
    buf.breakGroup();
}

void Document::duplicateLines() {
    buf.breakGroup();
    if (hasSelection()) {
        std::string text = selectedText();
        Pos e = selEnd();
        Pos end = buf.replace(e, e, text, sel());
        select(e, end);
    } else {
        auto [a, b] = lineSpan();
        std::string text = "\n" + buf.slice({a, 0}, {b, static_cast<int>(buf.line(b).size())});
        Pos at{b, static_cast<int>(buf.line(b).size())};
        buf.replace(at, at, text, sel());
        cursor = anchor = {cursor.line + (b - a + 1), cursor.col};
    }
    buf.breakGroup();
}

void Document::moveLines(int dir) {
    auto [a, b] = lineSpan();
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= buf.lineCount())) return;
    buf.breakGroup();
    int lo = dir < 0 ? a - 1 : a, hi = dir < 0 ? b : b + 1;
    std::vector<std::string> ls;
    for (int i = lo; i <= hi; ++i) ls.push_back(buf.line(i));
    if (dir < 0) std::rotate(ls.begin(), ls.begin() + 1, ls.end());
    else std::rotate(ls.begin(), ls.end() - 1, ls.end());
    std::string joined;
    for (std::size_t i = 0; i < ls.size(); ++i) { if (i) joined += '\n'; joined += ls[i]; }
    Selection before = sel();
    buf.replace({lo, 0}, {hi, static_cast<int>(buf.line(hi).size())}, joined, before);
    anchor = buf.clamp({before.anchor.line + dir, before.anchor.col});
    cursor = buf.clamp({before.cursor.line + dir, before.cursor.col});
    buf.breakGroup();
}

void Document::indentLines(bool outdent) {
    auto [a, b] = lineSpan();
    buf.beginGroup();
    Selection before = sel();
    Pos newAnchor = anchor, newCursor = cursor;
    for (int l = a; l <= b; ++l) {
        const std::string& s = buf.line(l);
        if (!outdent) {
            if (s.empty()) continue;
            std::string unit = indentUnit();
            buf.replace({l, 0}, {l, 0}, unit, before);
            if (newAnchor.line == l) newAnchor.col += static_cast<int>(unit.size());
            if (newCursor.line == l) newCursor.col += static_cast<int>(unit.size());
        } else {
            int n = 0;
            if (!s.empty() && s[0] == '\t') n = 1;
            else while (n < tabWidth && n < static_cast<int>(s.size()) && s[n] == ' ') ++n;
            if (n == 0) continue;
            buf.replace({l, 0}, {l, n}, {}, before);
            if (newAnchor.line == l) newAnchor.col = std::max(0, newAnchor.col - n);
            if (newCursor.line == l) newCursor.col = std::max(0, newCursor.col - n);
        }
    }
    buf.endGroup();
    anchor = newAnchor;
    cursor = newCursor;
    wantCol = -1;
}

void Document::tab() {
    if (hasSelection() && selBegin().line != selEnd().line) { indentLines(false); return; }
    buf.breakGroup();
    std::string t;
    if (useTabs) t = "\t";
    else {
        int dcol = displayCol(buf.line(selBegin().line), selBegin().col, tabWidth);
        t.assign(static_cast<std::size_t>(tabWidth - dcol % tabWidth), ' ');
    }
    replaceSelection(t);
    buf.breakGroup();
}

void Document::toggleComment() {
    if (!lang || lang->lineComment.empty()) return;
    const std::string& pre = lang->lineComment;
    auto [a, b] = lineSpan();
    bool allCommented = true, any = false;
    std::size_t minIndent = std::string::npos;
    for (int l = a; l <= b; ++l) {
        const std::string& s = buf.line(l);
        auto first = s.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        any = true;
        minIndent = std::min(minIndent, first);
        if (s.compare(first, pre.size(), pre) != 0) allCommented = false;
    }
    if (!any) return;
    buf.beginGroup();
    Selection before = sel();
    Pos na = anchor, nc = cursor;
    for (int l = a; l <= b; ++l) {
        const std::string& s = buf.line(l);
        auto first = s.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        int delta;
        if (allCommented) {
            std::size_t len = pre.size() + (first + pre.size() < s.size() && s[first + pre.size()] == ' ' ? 1 : 0);
            buf.replace({l, static_cast<int>(first)}, {l, static_cast<int>(first + len)}, {}, before);
            delta = -static_cast<int>(len);
        } else {
            std::string ins = pre + " ";
            buf.replace({l, static_cast<int>(minIndent)}, {l, static_cast<int>(minIndent)}, ins, before);
            delta = static_cast<int>(ins.size());
        }
        if (na.line == l) na.col = std::max(0, na.col + delta);
        if (nc.line == l) nc.col = std::max(0, nc.col + delta);
    }
    buf.endGroup();
    anchor = buf.clamp(na);
    cursor = buf.clamp(nc);
}

bool Document::undo() {
    auto r = buf.undo();
    if (!r) return false;
    anchor = buf.clamp(r->anchor);
    cursor = buf.clamp(r->cursor);
    wantCol = -1;
    return true;
}

bool Document::redo() {
    auto r = buf.redo();
    if (!r) return false;
    anchor = buf.clamp(r->anchor);
    cursor = buf.clamp(r->cursor);
    wantCol = -1;
    return true;
}

bool Document::find(std::string_view needle, bool forward, bool matchCase, bool fromStart) {
    if (needle.empty() || needle.find('\n') != std::string_view::npos) return false;
    const int n = buf.lineCount();
    Pos origin = forward ? (fromStart ? selBegin() : selEnd()) : selBegin();
    for (int step = 0; step <= n; ++step) {
        int l = forward ? (origin.line + step) % n : ((origin.line - step) % n + n) % n;
        const std::vector<int> all = matchStarts(buf.line(l), needle, matchCase);
        if (all.empty()) continue;
        int pick = -1;
        if (forward) {
            for (int c : all) {
                bool ok = step == 0 ? c >= origin.col : true;
                if (step == n) ok = c < origin.col;  // wrapped back into the first line
                if (ok) { pick = c; break; }
            }
        } else {
            for (auto it = all.rbegin(); it != all.rend(); ++it) {
                bool ok = step == 0 ? *it + static_cast<int>(needle.size()) <= origin.col : true;
                if (step == n) ok = *it + static_cast<int>(needle.size()) > origin.col;
                if (ok) { pick = *it; break; }
            }
        }
        if (pick < 0) continue;
        select({l, pick}, {l, pick + static_cast<int>(needle.size())});
        return true;
    }
    return false;
}

bool Document::replaceCurrent(std::string_view needle, std::string_view with, bool matchCase) {
    if (selectionMatches(needle, matchCase)) {
        buf.breakGroup();
        Pos end = buf.replace(selBegin(), selEnd(), with, sel());
        cursor = anchor = end;
        buf.breakGroup();
    }
    return find(needle, true, matchCase);
}

int Document::replaceAll(std::string_view needle, std::string_view with, bool matchCase) {
    if (needle.empty()) return 0;
    int count = 0;
    buf.beginGroup();
    Selection before = sel();
    for (int l = 0; l < buf.lineCount(); ++l) {
        auto cols = findAll(buf.line(l), needle, matchCase);
        for (auto it = cols.rbegin(); it != cols.rend(); ++it) {
            buf.replace({l, *it}, {l, *it + static_cast<int>(needle.size())}, with, before);
            ++count;
        }
    }
    buf.endGroup();
    cursor = anchor = buf.clamp(cursor);
    return count;
}

std::optional<std::pair<Pos, Pos>> Document::matchBracket() const {
    const std::string& l = buf.line(cursor.line);
    for (int off : {0, -1}) {
        int col = cursor.col + off;
        if (col < 0 || col >= static_cast<int>(l.size())) continue;
        char c = l[col];
        std::string_view opens = "([{", closes = ")]}";
        bool isOpen = opens.find(c) != std::string_view::npos;
        bool isClose = closes.find(c) != std::string_view::npos;
        if (!isOpen && !isClose) continue;
        char partner = isOpen ? closes[opens.find(c)] : opens[closes.find(c)];
        int depth = 0, budget = 40000;
        if (isOpen) {
            for (int ln = cursor.line; ln < buf.lineCount() && budget > 0; ++ln) {
                const std::string& s = buf.line(ln);
                for (int i = ln == cursor.line ? col : 0; i < static_cast<int>(s.size()); ++i, --budget) {
                    if (s[i] == c) ++depth;
                    else if (s[i] == partner && --depth == 0) return std::pair{Pos{cursor.line, col}, Pos{ln, i}};
                }
            }
        } else {
            for (int ln = cursor.line; ln >= 0 && budget > 0; --ln) {
                const std::string& s = buf.line(ln);
                for (int i = ln == cursor.line ? col : static_cast<int>(s.size()) - 1; i >= 0; --i, --budget) {
                    if (s[i] == c) ++depth;
                    else if (s[i] == partner && --depth == 0) return std::pair{Pos{cursor.line, col}, Pos{ln, i}};
                }
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void Document::detectIndentation() {
    int tabs = 0, spaces = 0;
    std::array<int, 9> widths{};
    for (int i = 0; i < std::min(buf.lineCount(), 400); ++i) {
        const std::string& l = buf.line(i);
        if (l.empty()) continue;
        if (l[0] == '\t') ++tabs;
        else if (l[0] == ' ') {
            ++spaces;
            auto n = l.find_first_not_of(' ');
            if (n != std::string::npos && n >= 2 && n <= 8 && n % 2 == 0) ++widths[n];
        }
    }
    tabWidth = defaultTabWidth;
    useTabs = defaultUseTabs;
    if (!defaultDetectIndent) return;   // always the configured indentation
    if (tabs != spaces) useTabs = tabs > spaces;
    if (!useTabs && spaces > 0) {
        // Guess the unit as the smallest common indentation seen.
        if (widths[2] > widths[4] / 2 && widths[2] > 2) tabWidth = 2;
    }
}

bool Document::tryAutoPair(char c) {
    const std::string& l = buf.line(cursor.line);
    char next = cursor.col < static_cast<int>(l.size()) ? l[cursor.col] : 0;
    char prev = cursor.col > 0 ? l[cursor.col - 1] : 0;
    const std::string_view openers = "([{", closers = ")]}";
    const bool isQuote = c == '"' || c == '\'' || c == '`';

    if (!hasSelection() && (closers.find(c) != std::string_view::npos || isQuote) && next == c) {
        setCursor({cursor.line, cursor.col + 1});  // type over the closing character
        return true;
    }
    const bool canOpen = next == 0 || next == ' ' || next == '\t' || closers.find(next) != std::string_view::npos ||
                         next == ',' || next == ';';
    if (openers.find(c) != std::string_view::npos) {
        char close = closers[openers.find(c)];
        if (hasSelection()) { wrapSelection(c, close); return true; }
        if (!canOpen) return false;
        insertPair(c, close);
        return true;
    }
    if (isQuote) {
        if (c == '\'' && lang && (lang->name == "Rust" || lang->name == "Lua")) return false;
        if (hasSelection()) { wrapSelection(c, c); return true; }
        if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_' || prev == c || !canOpen) return false;
        insertPair(c, c);
        return true;
    }
    return false;
}

void Document::insertPair(char open, char close) {
    buf.breakGroup();
    std::string s{open, close};
    Pos at = cursor;
    buf.replace(at, at, s, sel());
    cursor = anchor = {at.line, at.col + 1};
    wantCol = -1;
    buf.breakGroup();
}

void Document::wrapSelection(char open, char close) {
    buf.breakGroup();
    Pos a = selBegin();
    std::string text = std::string(1, open) + selectedText() + std::string(1, close);
    Pos end = buf.replace(a, selEnd(), text, sel());
    anchor = {a.line, a.col + 1};
    cursor = end;
    cursor.col -= 1;
    wantCol = -1;
    buf.breakGroup();
}

}  // namespace ide
