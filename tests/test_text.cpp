// Tests for the text libraries.
#include <cstdlib>
#include <poll.h>
import std;
import daemon.base.util;
import daemon.text.buffer;
import daemon.text.syntax;
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
import daemon.ui.font;
import daemon.ui.draw;
#include "check.hpp"

using namespace ide;

static void testBuffer() {
    Buffer b;
    b.setText("hello\nworld\n");
    CHECK(b.lineCount() == 3);
    CHECK(b.text() == "hello\nworld\n");
    Pos end = b.replace({0, 5}, {0, 5}, " there\nnew", {}, false);
    CHECK(end == (Pos{1, 3}));
    CHECK(b.line(0) == "hello there" && b.line(1) == "new" && b.lineCount() == 4);
    CHECK(b.undo().has_value());
    CHECK(b.text() == "hello\nworld\n");
    CHECK(b.redo().has_value());
    CHECK(b.line(1) == "new");
    b.replace({0, 0}, {1, 0}, {}, {});
    CHECK(b.line(0) == "new");
    b.undo();
    b.undo();
    CHECK(b.text() == "hello\nworld\n");

    b.setText("a\r\nb\r\n");
    CHECK(b.crlf() && b.line(0) == "a" && b.text() == "a\r\nb\r\n");
}

static void testCoalescing() {
    Buffer b;
    b.setText("");
    Pos p{0, 0};
    for (char c : std::string("hello")) p = b.replace(p, p, std::string(1, c), {}, true);
    CHECK(b.line(0) == "hello");
    b.undo();
    CHECK(b.line(0).empty());  // one undo step for the whole word

    b.setText("abc");
    Pos c{0, 3};
    for (int k = 0; k < 3; ++k) { Pos from{0, c.col - 1}; b.replace(from, c, {}, {}, true); c = from; }
    CHECK(b.line(0).empty());
    b.undo();
    CHECK(b.line(0) == "abc");

    b.setText("x");
    b.markSaved();
    CHECK(!b.modified());
    b.replace({0, 1}, {0, 1}, "y", {}, true);
    CHECK(b.modified());
    b.undo();
    CHECK(!b.modified());
}

static void testGroups() {
    Buffer b;
    b.setText("a\nb\nc");
    b.beginGroup();
    b.replace({0, 0}, {0, 0}, "1", {});
    b.replace({1, 0}, {1, 0}, "2", {});
    b.endGroup();
    CHECK(b.line(0) == "1a" && b.line(1) == "2b");
    b.undo();
    CHECK(b.text() == "a\nb\nc");
}

static void testHighlighter() {
    Buffer b;
    b.setText("int main() { /* c\nstill */ return 0; } // x\n\"str\"");
    Highlighter h;
    h.setLang(langForPath("main.cpp"));
    std::vector<Tok> t;
    h.highlight(b, 0, t);
    CHECK(t[0] == Tok::Type);      // int
    CHECK(t[4] == Tok::Function);  // main
    CHECK(t[13] == Tok::Comment);  // inside block comment
    h.highlight(b, 1, t);
    CHECK(t[0] == Tok::Comment);   // continued comment
    CHECK(t[9] == Tok::Keyword);   // return
    CHECK(t[16] == Tok::Number);   // 0
    h.highlight(b, 2, t);
    CHECK(t[0] == Tok::String);

    // Editing a line must invalidate cached state below it.
    b.replace({0, 13}, {0, 15}, "", {});  // delete "/*"
    h.highlight(b, 1, t);
    CHECK(t[0] != Tok::Comment);

    CHECK(langForPath("/x/CMakeLists.txt") && langForPath("/x/CMakeLists.txt")->name == "CMake");
    CHECK(langForPath("a.rs") && langForPath("a.rs")->name == "Rust");
    CHECK(langForPath("a.unknown") == nullptr);
}

static void testDocumentEditing() {
    Document d;
    d.lang = langForPath("x.cpp");
    d.hl.setLang(d.lang);
    d.insertText("f");
    d.insertText("(");  // auto-pairs
    CHECK(d.buf.line(0) == "f()" && d.cursor.col == 2);
    d.insertText(")");  // types over the closer
    CHECK(d.buf.line(0) == "f()" && d.cursor.col == 3);
    d.insertText("{");
    d.newline();        // splits {} onto three lines with indentation
    CHECK(d.buf.lineCount() == 3);
    CHECK(d.buf.line(1) == "    " && d.cursor == (Pos{1, 4}));
    d.insertText("x");
    d.newline();
    CHECK(d.buf.line(2) == "    ");  // keeps indentation

    d.selectAll();
    d.deleteSelection();
    CHECK(d.buf.text().empty());
    d.undo();
    CHECK(d.buf.lineCount() == 4 || d.buf.lineCount() == 3);

    Document e;
    e.buf.setText("one\ntwo\nthree");
    e.setCursor({1, 0});
    e.moveLines(-1);
    CHECK(e.buf.line(0) == "two" && e.buf.line(1) == "one" && e.cursor.line == 0);
    e.moveLines(1);
    CHECK(e.buf.line(0) == "one" && e.buf.line(1) == "two");
    e.duplicateLines();
    CHECK(e.buf.lineCount() == 4 && e.buf.line(1) == "two" && e.buf.line(2) == "two" && e.cursor.line == 2);
    e.deleteLines();
    CHECK(e.buf.lineCount() == 3);

    Document c;
    c.lang = langForPath("x.py");
    c.buf.setText("a\n  b\n\nc");
    c.selectAll();
    c.toggleComment();
    CHECK(c.buf.line(0) == "# a" && c.buf.line(1) == "#   b" && c.buf.line(2).empty());
    c.toggleComment();
    CHECK(c.buf.line(0) == "a" && c.buf.line(1) == "  b");
    c.undo();
    CHECK(c.buf.line(0) == "# a");
}

static void testNavigationAndSearch() {
    Document d;
    d.buf.setText("foo bar_baz  qux\nsecond line foo");
    d.setCursor({0, 0});
    d.moveRight(false, true);
    CHECK(d.cursor.col == 4);
    d.moveRight(false, true);
    CHECK(d.cursor.col == 13);  // skips both spaces
    d.moveLeft(false, true);
    CHECK(d.cursor.col == 4);

    auto [a, b] = d.wordRange({0, 6});
    CHECK(a.col == 4 && b.col == 11);

    CHECK(d.find("foo", true, true));
    CHECK(d.selBegin() == (Pos{1, 12}));
    CHECK(d.find("foo", true, true));  // wraps
    CHECK(d.selBegin() == (Pos{0, 0}));
    CHECK(d.find("FOO", false, false));
    CHECK(d.selBegin() == (Pos{1, 12}));
    CHECK(!d.find("nope", true, true));
    CHECK(d.replaceAll("foo", "X", true) == 2);
    CHECK(d.buf.line(0) == "X bar_baz  qux" && d.buf.line(1) == "second line X");
    d.undo();
    CHECK(d.buf.line(0) == "foo bar_baz  qux");

    Document m;
    m.buf.setText("a(b[c]{\n})");
    m.setCursor({0, 1});
    auto br = m.matchBracket();
    CHECK(br && br->second == (Pos{1, 1}));
    m.setCursor({1, 2});
    br = m.matchBracket();
    CHECK(br && br->second == (Pos{0, 1}));
}

void runTextTests() {
    testBuffer();
    testCoalescing();
    testGroups();
    testHighlighter();
    testDocumentEditing();
    testNavigationAndSearch();
}
