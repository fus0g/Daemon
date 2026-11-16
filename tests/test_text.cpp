// SPDX-License-Identifier: GPL-3.0-or-later
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

static void testRawStrings() {
    Buffer b;
    b.setText("const char* src = R\"(\n"
              "#version 450\n"
              "uniform vec3 pos; /* c\n"
              "still */ void main() {}\n"
              ")\";\n"
              "int x = 1;\n"
              "auto s = R\"x(a \"quoted\"\n"
              "more)x\"; int y;\n");
    Highlighter h;
    h.setLang(langForPath("a.cpp"));
    std::vector<Tok> t;
    h.highlight(b, 0, t);
    CHECK(t[0] == Tok::Keyword && t[18] == Tok::String);   // const ... R"(
    h.highlight(b, 1, t);
    CHECK(t[0] == Tok::Preproc);                              // #version: GLSL rules inside the string
    h.highlight(b, 2, t);
    CHECK(t[0] == Tok::Keyword && t[8] == Tok::Type);         // uniform vec3
    CHECK(t[18] == Tok::Comment);                              // a GLSL block comment...
    h.highlight(b, 3, t);
    CHECK(t[0] == Tok::Comment && t[9] == Tok::Type);          // ...continues on the next line; `void` is a type
    CHECK(t[14] == Tok::Function);                             // main(
    h.highlight(b, 4, t);
    CHECK(t[0] == Tok::String && t[1] == Tok::String);         // )" closes the string
    h.highlight(b, 5, t);
    CHECK(t[0] == Tok::Type && t[8] == Tok::Number);           // back to C++
    h.highlight(b, 6, t);
    CHECK(t[16] == Tok::String && t[18] == Tok::String);       // a raw string that is not GLSL is one big string
    h.highlight(b, 7, t);
    CHECK(t[0] == Tok::String && t[5] == Tok::String);
    CHECK(t[9] == Tok::Type);                                  // int y
    // the regions
    auto r = h.rawRegionAt(b, 2, 5);
    CHECK(r && r->glsl && r->begin.line == 0 && r->end.line == 4 && r->end.col == 0);
    CHECK(!h.rawRegionAt(b, 5, 3));
    auto r2 = h.rawRegionAt(b, 7, 2);
    CHECK(r2 && !r2->glsl);
    auto all = h.glslRegions(b);
    CHECK(all.size() == 1 && all[0].begin.line == 0 && all[0].end.line == 4);
    // editing inside a raw string re-evaluates the lines below
    b.replace({0, 20}, {0, 20}, "x", {});
    h.highlight(b, 5, t);
    CHECK(t[0] == Tok::Type);
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

static void testElectricBrace() {
    Document d;
    d.lang = langForPath("x.cpp");
    d.buf.setText("void f() {\n    int x;\n    ");
    d.setCursor({2, 4});
    d.insertText("}");
    CHECK(d.buf.line(2) == "}" && d.cursor.col == 1);
    d.buf.setText("if (a) {\n        x();\n        ");
    d.setCursor({2, 8});
    d.insertText("}");
    CHECK(d.buf.line(2) == "    }");                 // one level back, not all the way
    d.buf.setText("int a = {");
    d.setCursor({0, 9});
    d.insertText("}");
    CHECK(d.buf.line(0) == "int a = {}");              // not at the start of a line: an ordinary brace
    Document t;
    t.lang = langForPath("x.txt");
    t.buf.setText("    ");
    t.setCursor({0, 4});
    t.insertText("}");
    CHECK(t.buf.line(0) == "    }");                  // other languages are left alone
}

static void testDocumentSettings() {
    {   // closing brackets can be turned off
        Document d;
        d.autoPair = false;
        d.insertText("(");
        CHECK(d.buf.line(0) == "(");
    }
    {   // the defaults reach new documents; indentation detection can be switched off
        const int tw = Document::defaultTabWidth; const bool ut = Document::defaultUseTabs, det = Document::defaultDetectIndent;
        Document::defaultTabWidth = 8; Document::defaultUseTabs = true;
        Document a;
        CHECK(a.tabWidth == 8 && a.useTabs && a.indentUnit() == "\t");
        Document::defaultTabWidth = 2; Document::defaultUseTabs = false; Document::defaultDetectIndent = false;
        Document b;
        b.buf.setText("int f() {\n\treturn 1;\n}\n");
        b.redetectIndent();
        CHECK(!b.useTabs && b.tabWidth == 2);     // the file uses tabs, but detection is off
        Document::defaultDetectIndent = true;
        b.redetectIndent();
        CHECK(b.useTabs);                          // now the file wins
        Document::defaultTabWidth = tw; Document::defaultUseTabs = ut; Document::defaultDetectIndent = det;
    }
}

void runTextTests() {
    testElectricBrace();
    testDocumentSettings();
    testBuffer();
    testCoalescing();
    testGroups();
    testHighlighter();
    testRawStrings();
    testDocumentEditing();
    testNavigationAndSearch();
}
