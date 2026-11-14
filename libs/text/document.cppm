// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.editor — a document: buffer + cursor/selection + editing commands (no rendering).
module;
#include <cerrno>
export module daemon.text.document;
import std;
import daemon.base.util;
import daemon.text.buffer;
import daemon.text.syntax;

export namespace ide {

enum class CharClass { Space, Word, Punct };

inline CharClass classify(char32_t c) {
    if (c == ' ' || c == '\t') return CharClass::Space;
    if (c == '_' || c >= 0x80 || std::isalnum(static_cast<int>(c & 0x7F)) != 0) return CharClass::Word;
    return CharClass::Punct;
}

// Columns at which `needle` occurs in `line` (non-overlapping).
std::vector<int> findAll(std::string_view line, std::string_view needle, bool matchCase);

// Every column at which `needle` starts in `line` (overlapping matches included).
std::vector<int> matchStarts(std::string_view line, std::string_view needle, bool matchCase);

class Document {
public:
    Buffer buf;
    Highlighter hl;
    std::string path;           // empty for untitled documents
    std::string name = "untitled";
    const Lang* lang = nullptr;

    Pos cursor, anchor;
    int wantCol = -1;           // remembered display column for vertical movement
    int tabWidth = 4;
    bool useTabs = false;

    // View state, owned by the UI but stored per document so tabs keep their position.
    float scrollY = 0, targetY = 0, scrollX = 0;

    // Language-server sync state (managed by the app).
    int lspVersion = 0;
    std::uint64_t lspSent = 0;  // buffer version last sent to the server
    bool lspOpen = false;

    // Disk state at the last load / save, to notice changes made by other programs.
    std::filesystem::file_time_type diskTime{};
    bool diskGone = false;

    // ------------------------------------------------------------ files

    std::expected<void, std::string> load(const std::string& file);

    std::expected<void, std::string> save();

    void stampDisk() {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(path, ec);
        if (!ec) diskTime = t;
        diskGone = false;
    }

    void setPath(const std::string& file) {
        path = std::filesystem::absolute(file).lexically_normal().string();
        name = std::filesystem::path(path).filename().string();
        lang = langForPath(path);
        hl.setLang(lang);
    }

    // Set by views that keep their own model (notebooks): the document counts as modified although its buffer is not.
    bool extraModified = false;
    bool modified() const { return buf.modified() || extraModified; }

    // ------------------------------------------------------------ selection helpers

    bool hasSelection() const { return anchor != cursor; }
    Pos selBegin() const { return std::min(anchor, cursor); }
    Pos selEnd() const { return std::max(anchor, cursor); }
    Selection sel() const { return {anchor, cursor}; }
    std::string selectedText() const { return buf.slice(selBegin(), selEnd()); }
    const std::string& lineAt(int l) const { return buf.line(l); }

    void setCursor(Pos p, bool extend = false) {
        cursor = buf.clamp(p);
        if (!extend) anchor = cursor;
        wantCol = -1;
        buf.breakGroup();
    }

    void select(Pos a, Pos b) {
        anchor = buf.clamp(a);
        cursor = buf.clamp(b);
        wantCol = -1;
        buf.breakGroup();
    }

    void selectAll() { select({0, 0}, buf.end()); }

    void selectLine(int l) {
        l = std::clamp(l, 0, buf.lineCount() - 1);
        Pos a{l, 0};
        Pos b = l + 1 < buf.lineCount() ? Pos{l + 1, 0} : Pos{l, static_cast<int>(buf.line(l).size())};
        select(a, b);
    }

    // [begin, end) of the word / whitespace / punctuation run around p.
    std::pair<Pos, Pos> wordRange(Pos p) const;

    // ------------------------------------------------------------ movement

    void moveLeft(bool extend, bool byWord = false);

    void moveRight(bool extend, bool byWord = false);

    void moveVertical(int lines, bool extend);

    void home(bool extend) {
        const std::string& l = buf.line(cursor.line);
        int first = static_cast<int>(std::min(l.find_first_not_of(" \t"), l.size()));
        setCursor({cursor.line, cursor.col == first ? 0 : first}, extend);
    }
    void end(bool extend) { setCursor({cursor.line, static_cast<int>(buf.line(cursor.line).size())}, extend); }
    void docStart(bool extend) { setCursor({0, 0}, extend); }
    void docEnd(bool extend) { setCursor(buf.end(), extend); }

    Pos wordLeft(Pos p) const;

    Pos wordRight(Pos p) const;

    // ------------------------------------------------------------ editing

    std::string indentUnit() const { return useTabs ? "\t" : std::string(static_cast<std::size_t>(tabWidth), ' '); }

    void replaceSelection(std::string_view text, bool coalesce = false) {
        Pos end = buf.replace(selBegin(), selEnd(), text, sel(), coalesce);
        cursor = anchor = end;
        wantCol = -1;
    }

    void deleteSelection() { replaceSelection({}); }

    // Typed text, including auto-pairing of brackets and quotes.
    void insertText(std::string_view t) {
        if (t.empty()) return;
        if (t.size() == 1 && tryAutoPair(t[0])) return;
        if (hasSelection()) buf.breakGroup();
        replaceSelection(t, true);
    }

    void insertRaw(std::string_view t);

    void newline();

    void backspace();

    void deleteForward();

    void deleteWordBack();

    void deleteWordForward();

    void deleteLines();

    void duplicateLines();

    // Moves the selected lines (or the current one) up (-1) or down (+1).
    void moveLines(int dir);

    void indentLines(bool outdent);

    // Tab key: indent a multi-line selection, otherwise insert up to the next tab stop.
    void tab();

    void toggleComment();

    // ------------------------------------------------------------ undo / redo

    bool undo();
    bool redo();

    // ------------------------------------------------------------ search

    // Selects the next/previous match. With `fromStart` the search begins at the start of the
    // current selection (so typing in the find box keeps the current match when still valid).
    bool find(std::string_view needle, bool forward, bool matchCase, bool fromStart = false);

    bool selectionMatches(std::string_view needle, bool matchCase) const {
        if (!hasSelection() || needle.empty()) return false;
        std::string s = selectedText();
        if (s.size() != needle.size()) return false;
        return !findAll(s, needle, matchCase).empty();
    }

    // Replaces the current match (if the selection is one) and advances to the next.
    bool replaceCurrent(std::string_view needle, std::string_view with, bool matchCase);

    int replaceAll(std::string_view needle, std::string_view with, bool matchCase);

    void gotoLine(int line, int col = 0) { setCursor({std::clamp(line, 0, buf.lineCount() - 1), col}); }

    // Matching bracket for the one at / before the cursor.
    std::optional<std::pair<Pos, Pos>> matchBracket() const;

private:
    static bool isPair(char open, char close) {
        return (open == '(' && close == ')') || (open == '[' && close == ']') || (open == '{' && close == '}') ||
               (open == '"' && close == '"') || (open == '\'' && close == '\'') || (open == '`' && close == '`');
    }

    std::pair<int, int> lineSpan() const {
        int a = selBegin().line, b = selEnd().line;
        if (b > a && selEnd().col == 0) --b;
        return {a, b};
    }

    void detectIndentation();

    bool tryAutoPair(char c);

    void insertPair(char open, char close);

    void wrapSelection(char open, char close);
};

}  // namespace ide
