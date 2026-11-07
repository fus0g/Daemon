// Headless tests for the parts of the editor that do not need a window or GPU.
#include <cstdlib>
#include <poll.h>
import std;
import ide.util;
import ide.text;
import ide.syntax;
import ide.editor;
import ide.explorer;
import ide.icons;
import ide.json;
import ide.process;
import ide.cmake;
import ide.project;
import ide.lsp;
import ide.font;
import ide.draw;

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::println(std::cerr, "FAIL {}:{}: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

using namespace ide;

static void testUtf8() {
    std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // a é € 😀
    std::size_t i = 0;
    CHECK(decodeUtf8(s, i) == U'a');
    CHECK(decodeUtf8(s, i) == U'é');
    CHECK(decodeUtf8(s, i) == U'€');
    CHECK(decodeUtf8(s, i) == U'\U0001F600');
    CHECK(i == s.size());
    CHECK(utf8Length(s) == 4);
    CHECK(utf8Prev(s, s.size()) == 6);
    std::string bad = "\xFF\xC3";
    i = 0;
    CHECK(decodeUtf8(bad, i) == kReplacementChar && i == 1);
    CHECK(displayCol("\tab", 3, 4) == 6);
    CHECK(byteColAt("\tab", 5, 4) == 2);
}

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

static void testFileOps() {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() / ("ide-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "data");
    ::setenv("XDG_DATA_HOME", (base / "data").c_str(), 1);

    auto f = fsops::createFile(base, "src/a.txt");
    CHECK(f && fs::exists(base / "src/a.txt"));
    CHECK(!fsops::createFile(base, "src/a.txt"));            // already exists
    CHECK(!fsops::createFile(base, "../evil.txt"));          // path escape
    CHECK(!fsops::createFile(base, "/abs.txt"));
    CHECK(fsops::createFolder(base, "docs/deep").has_value() && fs::is_directory(base / "docs/deep"));

    CHECK(fsops::uniqueName(base / "src", "a.txt") == "a copy.txt");
    auto c1 = fsops::copyInto(base / "src/a.txt", base / "src");
    CHECK(c1 && c1->filename() == "a copy.txt");
    auto c2 = fsops::copyInto(base / "src/a.txt", base / "src");
    CHECK(c2 && c2->filename() == "a copy 2.txt");
    CHECK(!fsops::copyInto(base / "src", base / "src"));      // folder into itself
    auto cf = fsops::copyInto(base / "src", base / "docs");   // recursive folder copy
    CHECK(cf && fs::exists(base / "docs/src/a.txt"));

    auto r = fsops::rename(base / "src/a.txt", "b.txt");
    CHECK(r && fs::exists(base / "src/b.txt") && !fs::exists(base / "src/a.txt"));
    CHECK(!fsops::rename(base / "src/b.txt", "a copy.txt"));  // target exists
    CHECK(!fsops::rename(base / "src/b.txt", "x/y"));         // invalid name

    auto mv = fsops::moveInto(base / "src/b.txt", base / "docs");
    CHECK(mv && fs::exists(base / "docs/b.txt") && !fs::exists(base / "src/b.txt"));
    CHECK(!fsops::moveInto(base / "docs", base / "docs/deep"));  // into itself

    CHECK(fsops::trash(base / "docs/b.txt").has_value());
    CHECK(!fs::exists(base / "docs/b.txt"));
    CHECK(fs::exists(base / "data/Trash/files/b.txt"));
    auto info = readFile(base / "data/Trash/info/b.txt.trashinfo");
    CHECK(info && info->find("[Trash Info]") == 0 && info->find("DeletionDate=") != std::string::npos);
    CHECK(info && info->find("Path=" + fsops::percentEncode((base / "docs/b.txt").string())) != std::string::npos);

    CHECK(fsops::removeForever(base / "docs").has_value() && !fs::exists(base / "docs"));
    CHECK(fsops::isInside(base, base / "src") && !fsops::isInside(base / "src", base));

    Explorer ex;
    std::string err;
    CHECK(ex.open(base, err));
    CHECK(!ex.rows().empty() && ex.rows().front().node->dir);       // folders sort first
    auto* node = ex.reveal(base / "src/a copy.txt");
    CHECK(node && node->name == "a copy.txt");
    CHECK(ex.indexOfSelected() >= 0);

    CHECK(fileStyle("main.cpp").color == 0x56a8f5 && fileStyle("main.cpp").kind == FileKind::Code);
    CHECK(fileStyle("CMakeLists.txt").kind == FileKind::Build && fileStyle("zzz.unknown").kind == FileKind::Generic);
    CHECK(fileStyle("PHOTO.PNG").kind == FileKind::Image);
    CHECK(folderColor("src") == 0x6fa8f5 && folderColor("whatever") == 0xdcb56d);
    fs::remove_all(base);
}


static void testJson() {
    auto v = json::parse(R"({"a":[1,2.5,"x\né😀"],"b":{"c":true,"d":null},"e":-3e2})");
    CHECK(v.has_value());
    CHECK((*v)["a"][0].integer() == 1 && (*v)["a"][1].num() == 2.5);
    CHECK((*v)["a"][2].str() == "x\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK((*v)["b"]["c"].boolean() && (*v)["b"]["d"].isNull() && (*v)["e"].num() == -300);
    CHECK((*v)["missing"]["deeper"].isNull());  // safe chained lookup
    auto round = json::parse(json::dump(*v));
    CHECK(round && json::dump(*round) == json::dump(*v));
    CHECK(!json::parse("{") && !json::parse("[1,]") && !json::parse("{\"a\":1} x") && !json::parse(""));
    auto o = json::object();
    o.set("n", 5).set("s", "q\"uote\t").set("arr", json::array().push(1).push(true));
    CHECK(json::dump(o) == R"({"n":5,"s":"q\"uote\t","arr":[1,true]})");
    CHECK(utf16Col("a\xF0\x9F\x98\x80z", 5) == 3 && byteColFromUtf16("a\xF0\x9F\x98\x80z", 3) == 5);
    CHECK(uriToPath(pathToUri("/tmp/a b/é.cpp")) == "/tmp/a b/é.cpp");
}

static void testProcess() {
    auto r = Process::runCapture({{"echo", "hello"}});
    CHECK(r.exitCode == 0 && r.output == "hello\n");
    r = Process::runCapture({{"sh", "-c", "echo out; echo err 1>&2; exit 3"}});
    CHECK(r.exitCode == 3 && r.output.find("out") != std::string::npos && r.output.find("err") != std::string::npos);
    r = Process::runCapture({{"sh", "-c", "sleep 5"}}, 150);
    CHECK(r.timedOut);
    r = Process::runCapture({{"definitely-not-a-command-xyz"}});
    CHECK(!r.error.empty());
    r = Process::runCapture({{"pwd"}, "/tmp"});
    CHECK(r.output == "/tmp\n");
    CHECK(findExecutable("sh").has_value() && !findExecutable("nope-nope").has_value());
}

static void testCMake() {
    namespace fs = std::filesystem;
    const fs::path root("/proj");
    auto p = parseProblem("../src/main.cpp:12:5: error: expected ';' after expression", "/proj/cmake-build-debug", root);
    CHECK(p && p->severity == Severity::Error && p->line == 12 && p->col == 5);
    CHECK(p && p->file == fs::path("/proj/src/main.cpp") && p->message == "expected ';' after expression");
    p = parseProblem("/abs/x.h:3: warning: unused", "/b", root);
    CHECK(p && p->severity == Severity::Warning && p->line == 3 && p->col == 1 && p->file == fs::path("/abs/x.h"));
    p = parseProblem("CMake Error at CMakeLists.txt:7 (add_executable):", "/b", root);
    CHECK(p && p->line == 7 && p->file == fs::path("/proj/CMakeLists.txt"));
    CHECK(!parseProblem("[3/9] Building CXX object a.o", "/b", root));
    auto pr = parseProgress("[12/34] Building CXX object x");
    CHECK(pr && pr->first == 12 && pr->second == 34);
    CHECK(!parseProgress("not progress"));
    CHECK(stripAnsi("\x1b[1;31merror\x1b[0m: x") == "error: x");
    auto args = splitArgs(R"(a "b c" 'd e' f\ g "")");
    CHECK(args.size() == 5 && args[1] == "b c" && args[2] == "d e" && args[3] == "f g" && args[4].empty());

    // A real project: configure, discover targets, build, run.
    fs::path dir = fs::temp_directory_path() / ("ide-cmake-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "src");
    std::ofstream(dir / "CMakeLists.txt") << "cmake_minimum_required(VERSION 3.20)\nproject(t CXX)\n"
        "add_library(util STATIC src/util.cpp)\nadd_executable(hello src/main.cpp)\ntarget_link_libraries(hello util)\n";
    std::ofstream(dir / "src/util.cpp") << "int answer() { return 42; }\n";
    std::ofstream(dir / "src/main.cpp") << "#include <cstdio>\nint answer();\nint main(int argc, char** argv) { std::printf(\"hello %d %d\\n\", answer(), argc); }\n";
    CHECK(hasCMakeLists(dir));
    auto profiles = defaultProfiles(dir);
    CHECK(profiles.size() == 3 && profiles[0].dir == dir / "cmake-build-debug");
    CHECK(!isConfigured(profiles[0]));
    prepareFileApi(profiles[0].dir);
    auto cfg = Process::runCapture({configureArgv(dir, profiles[0], findExecutable("ninja").has_value()), dir.string()}, 120000);
    CHECK(cfg.exitCode == 0);
    CHECK(isConfigured(profiles[0]) && fs::exists(compileCommands(profiles[0])));
    auto targets = loadTargets(profiles[0].dir);
    CHECK(targets.size() >= 2 && targets[0].name == "hello" && targets[0].executable());
    CHECK(targets[0].artifact == profiles[0].dir / "hello");
    auto build = Process::runCapture({buildArgv(profiles[0], "hello"), dir.string()}, 180000);
    CHECK(build.exitCode == 0);
    auto run = Process::runCapture({{targets[0].artifact.string(), "x"}, profiles[0].dir.string()});
    CHECK(run.exitCode == 0 && run.output == "hello 42 2\n");

    // A compile error is reported with its location.
    std::ofstream(dir / "src/main.cpp") << "int main() { return 1 }\n";
    auto bad = Process::runCapture({buildArgv(profiles[0], "hello"), dir.string()}, 180000);
    CHECK(bad.exitCode != 0);
    std::optional<Problem> found;
    std::istringstream lines(bad.output);
    for (std::string l; std::getline(lines, l);)
        if (auto q = parseProblem(l, profiles[0].dir, dir); q && q->severity == Severity::Error) { found = q; break; }
    CHECK(found && found->line == 1 && found->file == dir / "src/main.cpp");
    fs::remove_all(dir);
}

static void testLsp() {
    LspClient c;
    std::vector<LspDiag> diags;
    std::string diagPath;
    int stateChanges = 0;
    c.onDiagnostics = [&](const std::string& p, std::vector<LspDiag> d) { diagPath = p; diags = std::move(d); };
    c.onState = [&](LspClient::State) { ++stateChanges; };
    auto st = c.start({"python3", "-I", std::string(IDE_SOURCE_DIR) + "/tests/mock_lsp.py"}, "/tmp");
    CHECK(st.has_value());
    auto waitFor = [&](auto pred, int ms = 5000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!pred() && std::chrono::steady_clock::now() < end) {
            pollfd pf{c.fd(), POLLIN, 0};
            if (::poll(&pf, 1, 50) > 0) c.pump();
        }
        return pred();
    };
    // Messages sent before the handshake completes are queued, then flushed.
    c.didOpen("/tmp/a b/main.cpp", "cpp", 1, "int x;\nint y;\n");
    CHECK(waitFor([&] { return c.ready(); }));
    CHECK(waitFor([&] { return diags.size() == 2; }));
    CHECK(diagPath == "/tmp/a b/main.cpp" && diags[0].severity == 1 && diags[0].col == 2 && diags[1].severity == 2);
    CHECK(diags[0].message == "unknown type \xC3\xA9 'foo'" && diags[0].code == "unknown_typename");

    std::vector<LspCompletion> items;
    bool gotCompletion = false;
    c.completion("/tmp/a b/main.cpp", 0, 3, 2, [&](std::vector<LspCompletion> r) { items = std::move(r); gotCompletion = true; });
    CHECK(waitFor([&] { return gotCompletion; }));
    CHECK(items.size() == 3 && items[0].insertText == "push_back" && items[0].kind == 2 && items[0].detail == "void");
    CHECK(items[1].insertText == "size" && items[2].insertText == " value");  // textEdit.newText / label fallbacks

    std::vector<LspLocation> locs;
    bool gotDef = false;
    c.definition("/tmp/a b/main.cpp", 0, 0, [&](std::vector<LspLocation> r) { locs = std::move(r); gotDef = true; });
    CHECK(waitFor([&] { return gotDef; }));
    CHECK(locs.size() == 1 && locs[0].path == "/tmp/a b/main.cpp" && locs[0].line == 4 && locs[0].col == 7);

    std::string hover;
    c.hover("/tmp/a b/main.cpp", 0, 0, [&](std::string t) { hover = std::move(t); });
    CHECK(waitFor([&] { return !hover.empty(); }));
    CHECK(hover == "int x");

    std::string other;
    c.switchSourceHeader("/proj/src/main.cpp", [&](std::string t) { other = std::move(t); });
    CHECK(waitFor([&] { return !other.empty(); }));
    CHECK(other == "/proj/src/main.h");

    diags.clear();
    c.didChange("/tmp/a b/main.cpp", 2, "int z;\n");
    CHECK(waitFor([&] { return diags.size() == 1; }));
    CHECK(diags[0].message == "changed:7");
    c.didChange("/tmp/a b/main.cpp", 3, "ok");
    CHECK(waitFor([&] { return diags.empty(); }));  // diagnostics cleared by the server

    c.stop();
    CHECK(c.state() == LspClient::State::Stopped);

    LspClient bad;
    CHECK(!bad.start({"no-such-language-server"}, "/tmp").has_value());
    CHECK(bad.state() == LspClient::State::Failed);
}

static void testIcons() {
    using namespace ide::icon_detail;
    // A rounded rect fills most of the grid; its corners and an empty layer stay clear.
    std::array<std::uint8_t, 18 * 18> px{};
    rasterize({rr(8, 8, 7, 7, 2)}, 18, px.data());
    CHECK(px[9 * 18 + 9] == 255);   // centre fully covered
    CHECK(px[0] < 40);              // rounded corner nearly empty
    int edge = 0;
    for (auto v : px) edge += (v > 0 && v < 255);
    CHECK(edge > 20);               // anti-aliased edge pixels exist
    // Subtraction: a hole in the middle.
    rasterize({rr(8, 8, 7, 7, 1), circ(8, 8, 3, true)}, 18, px.data());
    CHECK(px[9 * 18 + 9] == 0 && px[2 * 18 + 2] == 255);
    // The page icon rasterises into the atlas without overflowing and is cached.
    FontAtlas atlas;
    if (atlas.load()) {
        DrawList dl;
        Icons::file(dl, atlas, 0, 0, 18, "main.cpp");
        Icons::folder(dl, atlas, 0, 0, 18, "src", true);
        Icons::chevron(dl, atlas, 0, 0, 12, true, Color::rgb(0xffffff));
        const std::size_t perFile = atlas.hasIcons() ? 1 : 3;  // one font glyph, or three vector layers
        CHECK(dl.size() == perFile + (atlas.hasIcons() ? 1 : 2) + 1);
        std::size_t before = dl.size();
        Icons::file(dl, atlas, 0, 0, 18, "main.cpp");
        CHECK(dl.size() == before + perFile);
    }
}

void testCMakeCompletion() {
    CMakeKnowledge kb;
    kb.load();
    if (!kb.ready()) return;  // no cmake on this machine
    auto has = [](const std::vector<CMakeItem>& v, std::string_view l) {
        return std::ranges::any_of(v, [&](const CMakeItem& i) { return i.label == l; });
    };
    const std::string doc = "cmake_minimum_required(VERSION 3.20)\nproject(demo)\nset(MY_OPT ON)\nadd_executable(app main.cpp)\n";
    auto at = [&](std::string_view before) { return cmakeCompletions(kb, before, doc, {"extlib"}); };
    CHECK(has(at(doc + "ta"), "target_link_libraries"));          // statement start: commands
    CHECK(!has(at(doc + "ta"), "CMAKE_CXX_STANDARD"));
    CHECK(has(at(doc + "message(STATUS \"x ${CMAKE_"), "CMAKE_CXX_COMPILER"));  // variable refs
    CHECK(has(at(doc + "message(${"), "MY_OPT"));                  // project variables
    CHECK(has(at(doc + "target_link_libraries(app "), "PRIVATE"));  // keywords
    CHECK(has(at(doc + "target_link_libraries(app PRIVATE "), "app"));  // targets from the file
    CHECK(has(at(doc + "target_link_libraries(app PRIVATE "), "extlib"));  // targets from the File API
    CHECK(has(at(doc + "set_target_properties(app PROPERTIES "), "CXX_STANDARD"));
    CHECK(has(at(doc + "include("), "CTest"));
    CHECK(at(doc + "# target_li").empty());                        // comments get nothing
    CHECK(has(at(doc + "target_link_libraries(app PRIVATE ${x})\nad"), "add_library"));  // closed paren: statement start again
}

static void testModules() {
    // import-name detection
    CHECK(importNameStart("import ide.js", 12) == std::optional<std::size_t>(7));
    CHECK(importNameStart("export import std;", 14) == std::optional<std::size_t>(14));
    CHECK(!importNameStart("int import_x = 1;", 10).has_value());
    CHECK(!importNameStart("import <vector>;", 10).has_value());
    CHECK(importNameAt("import ide.json;", 9) == std::optional<std::string>("ide.json"));
    CHECK(importNameAt("import :part;", 10) == std::optional<std::string>(":part"));

    // Derived clangd database from a GCC-module-flavoured CMake one.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "daemon-modules-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream o(dir / "compile_commands.json");
        o << R"([
 {"directory":"/b","command":"/usr/bin/c++ -DX=1 -Iinc -std=c++23 -fmodules-ts -fmodule-mapper=a.modmap -MD -fdeps-format=p1689r5 -x c++ -o a.o -c /src/a.cppm","file":"/src/a.cppm"},
 {"directory":"/b","command":"/usr/bin/c++ -std=c++23 -fmodule-only -fmodules-ts -fmodule-mapper=a2.modmap -MD -x c++ -o a.bmi -c /src/a.cppm","file":"/src/a.cppm"},
 {"directory":"/b","command":"/usr/bin/c++ -std=c++23 -fmodules-ts -x c++ -o std.o -c /usr/include/c++/16/bits/std.cc","file":"/usr/include/c++/16/bits/std.cc"}])";
    }
    if (findExecutable("clang++")) {
        auto db = prepareClangdDb(dir);
        CHECK(db && db->derived && db->sources == 2);  // duplicate BMI-only entry folded away
        auto j = readJson(dir / ".daemon/clangd-v2/compile_commands.json");
        CHECK(j && j->array() && j->array()->size() == 2);
        if (j && j->array() && j->array()->size() == 2) {
            const auto& args = (*j)[0]["arguments"];
            std::string all;
            for (const auto& a : *args.array()) all += a.str() + " ";
            CHECK(all.find("-fmodules-ts") == std::string::npos && all.find("-fmodule-mapper") == std::string::npos);
            CHECK(all.find("-x c++-module") != std::string::npos && all.find("-DX=1") != std::string::npos);
            CHECK(all.find("-std=c++23") != std::string::npos);
        }
    }
    fs::remove_all(dir);
}

static void testProjects() {
    namespace fs = std::filesystem;
    CHECK(validateProjectName("my-app_2.0").empty());
    CHECK(!validateProjectName("").empty() && !validateProjectName("bad name").empty() && !validateProjectName(".hidden").empty());
    const fs::path root = fs::temp_directory_path() / "daemon-project-test";
    fs::remove_all(root);
    for (const auto& t : projectTemplates()) {
        NewProjectSpec spec;
        spec.templateId = t.id;
        spec.name = "demo-" + t.id;
        spec.parent = root;
        spec.initGit = false;
        auto r = createProject(spec);
        CHECK(r.has_value());
        if (!r) continue;
        CHECK(fs::exists(r->dir / "CMakeLists.txt") && fs::exists(r->mainFile));
        CHECK(!createProject(spec).has_value());  // refuses a non-empty directory
    }
    {   // the modules template builds with a real CMake + compiler when available
        const fs::path dir = root / "demo-cpp-modules";
        if (findExecutable("cmake") && findExecutable("ninja")) {
            ProcessOptions o;
            o.argv = {"cmake", "-S", dir.string(), "-B", (dir / "b").string(), "-G", "Ninja"};
            auto cfg = Process::runCapture(o, 60000);
            CHECK(cfg.exitCode == 0);
            if (cfg.exitCode == 0) {
                o.argv = {"cmake", "--build", (dir / "b").string()};
                CHECK(Process::runCapture(o, 120000).exitCode == 0);
            }
        }
    }
    {   // recents: newest first, de-duplicated, capped
        DaemonState st;
        for (int i = 0; i < 20; ++i) addRecent(st, "/p/" + std::to_string(i));
        addRecent(st, "/p/15");
        CHECK(st.recent.size() == 12 && st.recent.front() == "/p/15");
        CHECK(std::ranges::count(st.recent, std::string("/p/15")) == 1);
        setenv("XDG_CONFIG_HOME", (root / "cfg").c_str(), 1);
        st.lastLocation = "/somewhere";
        saveState(st);
        const auto back = loadState();
        CHECK(back.recent == st.recent && back.lastLocation == "/somewhere");
        unsetenv("XDG_CONFIG_HOME");
    }
    fs::remove_all(root);
}

int main() {
    testProjects();
    testModules();
    testCMakeCompletion();
    testJson();
    testProcess();
    testLsp();
    testCMake();
    testIcons();
    testFileOps();
    testUtf8();
    testBuffer();
    testCoalescing();
    testGroups();
    testHighlighter();
    testDocumentEditing();
    testNavigationAndSearch();
    if (failures == 0) std::println("all tests passed");
    return failures == 0 ? 0 : 1;
}
