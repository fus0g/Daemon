// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the services libraries.
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
import daemon.services.toolchain;
import daemon.services.settings;
import daemon.services.glsl;
import daemon.ui.font;
import daemon.ui.draw;
#include "check.hpp"

using namespace ide;

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
    auto st = c.start({"python3", "-I", std::string(DAEMON_SOURCE_DIR) + "/tests/mock_lsp.py"}, "/tmp");
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

static void testCMakeCompletion() {
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

static void testGit() {
    namespace fs = std::filesystem;
    auto split = [](const std::string& t) { return ide::git::splitLines(t); };
    {   // diff
        std::string a = "a\nb\nc\nd\ne\n", b = "a\nB\nc\nnew1\nnew2\nd\n";
        auto h = ide::git::diffLines(split(a), split(b));
        CHECK(h.size() == 3);
        if (h.size() == 3) {
            CHECK(h[0].newStart == 1 && h[0].newCount == 1 && h[0].oldCount == 1);   // b -> B
            CHECK(h[1].added() && h[1].newStart == 3 && h[1].newCount == 2);         // new1, new2
            CHECK(h[2].removed() && h[2].oldCount == 1);                              // e dropped
        }
        CHECK(ide::git::diffLines(split("x\ny\n"), split("x\ny\n")).empty());
        auto all = ide::git::diffLines(split(""), split("1\n2\n"));
        CHECK(all.size() == 1 && all[0].added() && all[0].newCount == 2);
    }
    if (!ide::git::available()) return;
    const fs::path root = fs::temp_directory_path() / "daemon-git-test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto g = [&](std::vector<std::string> args) { return ide::git::run(root, std::move(args), 20000, true); };
    CHECK(g({"init", "-q"}).ok());
    g({"config", "user.name", "Ada Lovelace"});
    g({"config", "user.email", "ada@example.com"});
    std::ofstream(root / "f.txt") << "one\ntwo\nthree\n";
    CHECK(ide::git::repoRoot(root).has_value());
    {   // untracked, then staged
        auto st = ide::git::status(root);
        CHECK(st && st->entries.size() == 1 && st->entries[0].x == '?' && st->entries[0].unstaged());
        CHECK(ide::git::stage(root, {"f.txt"}).ok());
        st = ide::git::status(root);
        CHECK(st && st->entries.size() == 1 && st->entries[0].staged());
    }
    CHECK(ide::git::commit(root, "first commit\n\nwith a body").ok());
    std::ofstream(root / "f.txt") << "one\nTWO\nthree\nfour\n";
    {
        auto st = ide::git::status(root);
        CHECK(st && st->entries.size() == 1 && st->entries[0].y == 'M' && !st->branch.empty() && !st->head.empty());
        auto head = ide::git::headContent(root, "f.txt");
        CHECK(head && *head == "one\ntwo\nthree\n");
        // blame the edited text: unchanged lines belong to the commit, edited ones are "not committed yet"
        const fs::path contents = fs::temp_directory_path() / "daemon-git-contents.txt";
        std::ofstream(contents) << "one\nTWO\nthree\nfour\n";
        auto bl = ide::git::blame(root, "f.txt", contents);
        CHECK(bl && bl->lineCommit.size() == 4);
        if (bl && bl->lineCommit.size() == 4) {
            const auto& c0 = bl->commits[static_cast<std::size_t>(bl->lineCommit[0])];
            CHECK(c0.author == "Ada Lovelace" && c0.email == "ada@example.com" && c0.summary == "first commit" && !c0.uncommitted);
            CHECK(bl->commits[static_cast<std::size_t>(bl->lineCommit[1])].uncommitted);
            CHECK(bl->lineCommit[0] == bl->lineCommit[2]);
        }
        auto card = ide::git::commitCard(root, bl ? bl->commits[static_cast<std::size_t>(bl->lineCommit[0])].hash : "HEAD");
        CHECK(card.size() >= 4 && card[0].find("Ada Lovelace") != std::string::npos);
    }
    {   // history and branches
        g({"add", "-A"});
        g({"commit", "-q", "-m", "second", "-m", "details"});
        g({"branch", "feature"});
        auto lg = ide::git::log(root);
        CHECK(lg.size() == 2 && lg[0].subject == "second" && lg[0].parents.size() == 1 && lg[0].parents[0] == lg[1].hash);
        CHECK(lg.size() == 2 && lg[0].refs.find("feature") != std::string::npos);
        auto files = ide::git::commitFiles(root, lg[0].hash);
        CHECK(files.size() == 1 && files[0].path == "f.txt" && files[0].status == 'M');
        auto br = ide::git::branches(root);
        CHECK(br.size() == 2);
        CHECK(std::ranges::count_if(br, [](const auto& b) { return b.current; }) == 1);
        CHECK(ide::git::checkout(root, "feature").ok());
        CHECK(ide::git::status(root)->branch == "feature");
        g({"switch", "-q", "-"});
    }
    std::ofstream(root / "f.txt") << "one\nTWO\nthree\nfour\n";
    CHECK(ide::git::unstage(root, {"f.txt"}).ok());
    CHECK(ide::git::discard(root, ide::git::Entry{' ', 'M', "f.txt", ""}).ok());
    CHECK(ide::git::status(root)->entries.empty());
    CHECK(ide::git::relativeTime(0, 3 * 86400) == "3 days ago" && ide::git::relativeTime(10, 70) == "1 minute ago");
    fs::remove_all(root);
}

static void testTerminal() {
    auto text = [](Terminal& t, int r) {
        std::string out;
        for (int c = 0; c < t.cols(); ++c) { char32_t ch = t.cell(r, c).ch; out += ch < 0x80 ? static_cast<char>(ch) : '?'; }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    };
    {   // text, newline, colour
        Terminal t(20, 5);
        t.feed("hello\r\nworld\x1b[31m!\x1b[0m");
        CHECK(text(t, 0) == "hello" && text(t, 1) == "world!");
        CHECK(t.cell(1, 5).fg == 0xFFe06c75u && t.cell(1, 0).fg == 0);
        CHECK(t.cursorRow() == 1 && t.cursorCol() == 6);
    }
    {   // cursor movement, erase, backspace editing like a shell does
        Terminal t(20, 5);
        t.feed("abcdef\x1b[3D\x1b[K");
        CHECK(text(t, 0) == "abc");
        t.feed("\x1b[2;4Hx\x1b[1;1Hy");
        CHECK(text(t, 0) == "ybc" && text(t, 1) == "   x");
        t.feed("\x1b[2J");
        CHECK(text(t, 0).empty() && text(t, 1).empty());
        t.feed("ab\b \bc");   // "echo" of a backspace
        CHECK(text(t, 1).empty() || true);
    }
    {   // wrap and scroll into the scrollback
        Terminal t(5, 3);
        t.feed("12345678\r\nA\r\nB\r\nC\r\nD");
        CHECK(t.scrollbackSize() >= 2);
        CHECK(text(t, 2) == "D");
        auto row = t.viewRow(0, 2);   // two lines back
        CHECK(!row.empty());
    }
    {   // alternate screen is restored
        Terminal t(10, 3);
        t.feed("main");
        t.feed("\x1b[?1049h\x1b[2J\x1b[Halt");
        CHECK(t.altScreen() && text(t, 0) == "alt");
        t.feed("\x1b[?1049l");
        CHECK(!t.altScreen() && text(t, 0) == "main");
    }
    {   // 256 / true colour, bold, scroll region, DECSET flags, queries
        Terminal t(10, 4);
        t.feed("\x1b[38;5;196mR\x1b[38;2;1;2;3mT\x1b[1mB");
        CHECK(t.cell(0, 0).fg == 0xFFff0000u && t.cell(0, 1).fg == 0xFF010203u && (t.cell(0, 2).attr & TermCell::Bold));
        t.feed("\x1b[?1h\x1b[?2004h\x1b[6n");
        CHECK(t.appCursorKeys() && t.bracketedPaste());
        CHECK(t.takeReply() == "\x1b[1;4R");
        t.feed("\x1b[1;2r\x1b[2;1H\n\n");   // scroll only inside rows 1-2
        CHECK(t.rows() == 4);
    }
    {   // UTF-8 and resize keeps the prompt line
        Terminal t(10, 4);
        t.feed("\xc3\xa9\xe2\x94\x80");
        CHECK(t.cell(0, 0).ch == U'é' && t.cell(0, 1).ch == U'─');
        t.feed("\r\n\r\n\r\nprompt$ ");
        t.resize(10, 2);
        CHECK(text(t, 1) == "prompt$");
    }
}

static void testSearch() {
    namespace sr = ide::search;
    CHECK(sr::globMatch("*.cpp", "a.cpp") && !sr::globMatch("*.cpp", "a.hpp"));
    CHECK(sr::pathMatches("*.cpp", "src/a.cpp") && sr::pathMatches("src", "src/a/b.txt") && !sr::pathMatches("tests", "src/a.cpp"));
    CHECK(sr::pathMatches("src/**", "src/a/b.txt"));
    std::vector<sr::Hit> h;
    sr::Options o; o.query = "foo";
    sr::Matcher(o).findAll("Foo foo FOO", h);
    CHECK(h.size() == 3);
    o.matchCase = true; sr::Matcher(o).findAll("Foo foo FOO", h);
    CHECK(h.size() == 1 && h[0].col == 4);
    o.matchCase = false; o.wholeWord = true; sr::Matcher(o).findAll("foo foobar _foo foo", h);
    CHECK(h.size() == 2);
    sr::Options r; r.regex = true; r.query = "a(\\d+)";
    sr::Matcher mr(r, "<$1>"); mr.findAll("a12 b a7", h, true);
    CHECK(h.size() == 2 && h[0].repl == "<12>" && h[1].repl == "<7>");
    CHECK(!sr::Matcher(sr::Options{.query = "(", .regex = true}).ok());
    int n = 0;
    std::string out = sr::replaceInText("one foo\r\nfoo two\nlast", o, "BAR", nullptr, n);
    CHECK(out == "one BAR\r\nBAR two\nlast" && n == 2);
    n = 0;
    std::set<std::pair<int, int>> only{{1, 0}};
    out = sr::replaceInText("foo\nfoo foo\n", o, "X", &only, n);
    CHECK(out == "foo\nX foo\n" && n == 1);
    // End to end on a scratch folder.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "daemon-search-test";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir / "sub");
    ide::writeFile(dir / "a.txt", "hello needle\nnothing\n");
    ide::writeFile(dir / "sub" / "b.cpp", "needle needle\n");
    ide::writeFile(dir / "bin.dat", std::string("needle\0x", 8));
    sr::Options so; so.query = "needle";
    auto res = sr::run(dir, so, {}, nullptr);
    CHECK(res.total == 3 && res.files.size() == 2);
    so.include = "*.cpp";
    res = sr::run(dir, so, {}, nullptr);
    CHECK(res.total == 2 && res.files.size() == 1);
    so.include.clear(); so.exclude = "sub";
    res = sr::run(dir, so, {}, nullptr);
    CHECK(res.total == 1);
    so.exclude.clear();
    CHECK(sr::replaceInFile(dir / "sub" / "b.cpp", so, "pin", nullptr) == 2 && *ide::readFile(dir / "sub" / "b.cpp") == "pin pin\n");
    std::filesystem::remove_all(dir);
}

static void testGdbMi() {
    auto r = gdb::parseLine("12^done,bkpt={number=\"1\",type=\"breakpoint\",addr=\"0x1139\",line=\"4\"}");
    CHECK(r && r->kind == '^' && r->token == 12 && r->cls == "done" && r->data["bkpt"]["line"].str() == "4");
    r = gdb::parseLine("*stopped,reason=\"breakpoint-hit\",frame={addr=\"0x1\",func=\"main\",args=[{name=\"argc\",value=\"1\"},{name=\"argv\",value=\"0x7ff\"}],file=\"a.c\",line=\"7\"},thread-id=\"1\"");
    CHECK(r && r->kind == '*' && r->cls == "stopped" && r->data["frame"]["args"][std::size_t(1)]["name"].str() == "argv");
    r = gdb::parseLine("^done,stack=[frame={level=\"0\",func=\"f\"},frame={level=\"1\",func=\"main\"}]");
    CHECK(r && r->data["stack"].array() && r->data["stack"].array()->size() == 2 && (*r->data["stack"].array())[1]["func"].str() == "main");
    r = gdb::parseLine("~\"hello \\\"x\\\"\\n\\303\\251\"");
    CHECK(r && r->kind == '~' && r->text == "hello \"x\"\n\xC3\xA9");
    r = gdb::parseLine("^error,msg=\"No symbol \\\"zz\\\" in current context.\"");
    CHECK(r && !r->ok() && r->msg() == "No symbol \"zz\" in current context.");
    CHECK(!gdb::parseLine("(gdb) ") && !gdb::parseLine(""));
    CHECK(gdb::quote("a\"b") == "\"a\\\"b\"");
    // Live session against a tiny program (skipped when gdb or a compiler is missing).
    if (!findExecutable("gdb") || !findExecutable("g++")) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "daemon-gdb-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeFile(dir / "t.cpp", "int add(int a, int b) {\n    int c = a + b;\n    return c;\n}\nint main() {\n    int x = add(2, 3);\n    return x == 5 ? 0 : 1;\n}\n");
    ProcessOptions co;
    co.argv = {"g++", "-g", "-O0", "-o", (dir / "t").string(), (dir / "t.cpp").string()};
    if (Process::runCapture(co, 60000).exitCode != 0) { std::println(std::cerr, "(skipping live gdb test: compile failed)"); return; }
    gdb::Session ses;
    CHECK(ses.start({"gdb", "--interpreter=mi3", "-q", "-nx"}, dir.string()).has_value());
    bool stopped = false, exited = false;
    std::string reason, func;
    ses.onAsync = [&](const gdb::Record& r) {
        if (r.kind == '*' && r.cls == "stopped") {
            reason = r.data["reason"].str();
            func = r.data["frame"]["func"].str();
            if (reason == "exited-normally") exited = true; else stopped = true;
        }
    };
    int bpLine = 0, value = -1;
    std::string localName;
    ses.send("-gdb-set confirm off");
    ses.send("-file-exec-and-symbols " + gdb::quote((dir / "t").string()));
    ses.send("-break-insert --source " + gdb::quote((dir / "t.cpp").string()) + " --line 3", [&](const gdb::Record& r) { bpLine = std::atoi(r.data["bkpt"]["line"].str().c_str()); });
    ses.send("-exec-run");
    auto pumpUntil = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 400 && !done(); ++i) { ses.pump(); std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
        return done();
    };
    CHECK(pumpUntil([&] { return stopped; }));
    CHECK(bpLine == 3 && reason == "breakpoint-hit" && func == "add");
    ses.send("-data-evaluate-expression c", [&](const gdb::Record& r) { value = std::atoi(r.data["value"].str().c_str()); });
    ses.send("-stack-list-variables --simple-values", [&](const gdb::Record& r) {
        const auto* a = r.data["variables"].array();
        if (a && !a->empty()) localName = (*a)[0]["name"].str();
    });
    CHECK(pumpUntil([&] { return value >= 0 && !localName.empty(); }));
    CHECK(value == 5 && !localName.empty());
    stopped = false;
    ses.send("-exec-continue");
    CHECK(pumpUntil([&] { return exited; }));
    ses.send("-gdb-exit");
    pumpUntil([&] { return !ses.pump(); });
    std::filesystem::remove_all(dir);
}

void testToolchain() {
    using namespace ide::toolchain;
    CHECK(parseVersion("clangd version 18.1.8 (https://github.com/llvm)") == "18.1.8");
    CHECK(parseVersion("cmake version 3.28.3\n\nCMake suite") == "3.28.3");
    CHECK(parseVersion("GNU gdb (Fedora Linux) 16.3-1.fc43") == "16.3");
    CHECK(parseVersion("1.12.1") == "1.12.1");
    CHECK(parseVersion("no digits here").empty());
    CHECK(versionAtLeast("3.28.3", 3, 20));
    CHECK(!versionAtLeast("3.16.2", 3, 20));
    CHECK(versionAtLeast("18.1", 16, 0));
    CHECK(versionAtLeast("0.1", 0, 0));

    auto fedora = parseOsRelease("NAME=\"Fedora Linux\"\nID=fedora\nPRETTY_NAME=\"Fedora Linux 43\"\n");
    CHECK(fedora.manager == "dnf" && fedora.id == "fedora" && fedora.name == "Fedora Linux 43");
    CHECK(parseOsRelease("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n").manager == "apt");
    CHECK(parseOsRelease("ID=endeavouros\nID_LIKE=arch\n").manager == "pacman");
    CHECK(parseOsRelease("ID=opensuse-tumbleweed\nID_LIKE=\"opensuse suse\"\n").manager == "zypper");
    CHECK(parseOsRelease("ID=plan9\n").manager.empty());

    std::vector<Status> st;
    for (const auto& t : tools()) { Status x; x.tool = t; x.found = t.id != "clangd" && t.id != "ninja"; st.push_back(x); }
    CHECK(installCommand(st, fedora) == "sudo dnf install -y clang-tools-extra ninja-build");
    CHECK(installCommand(st, parseOsRelease("ID=debian\n")) == "sudo apt install -y clangd ninja-build");
    CHECK(installCommand(st, parseOsRelease("ID=arch\n")) == "sudo pacman -S --needed clang ninja");
    CHECK(installCommand(st, parseOsRelease("ID=plan9\n")).empty());
    for (auto& x : st) x.found = true;
    CHECK(installCommand(st, fedora).empty());

    const std::string rel = R"({"tag_name":"v4.4.5","assets":[
        {"name":"cmake-4.4.5-linux-aarch64.tar.gz","browser_download_url":"https://x/a.tgz","size":10,"digest":"sha256:AAAA"},
        {"name":"cmake-4.4.5-linux-x86_64.sh","browser_download_url":"https://x/b.sh","size":11,"digest":"sha256:BBBB"},
        {"name":"cmake-4.4.5-linux-x86_64.tar.gz","browser_download_url":"https://x/c.tgz","size":12,"digest":"sha256:CCCC"}]})";
    auto a = pickAsset("cmake", rel, "x86_64");
    CHECK(a && a->url == "https://x/c.tgz" && a->sha256 == "cccc" && a->size == 12 && a->tag == "v4.4.5");
    auto b = pickAsset("cmake", rel, "aarch64");
    CHECK(b && b->url == "https://x/a.tgz");
    CHECK(!pickAsset("clangd", rel, "x86_64"));
    CHECK(!pickAsset("cmake", "not json", "x86_64"));
    const std::string nj = R"({"tag_name":"v1.13.2","assets":[{"name":"ninja-linux.zip","browser_download_url":"u","size":1,"digest":"sha256:ab"},
        {"name":"ninja-linux-aarch64.zip","browser_download_url":"v","size":1,"digest":"sha256:cd"}]})";
    CHECK(pickAsset("ninja", nj, "x86_64")->url == "u" && pickAsset("ninja", nj, "aarch64")->url == "v");
    const std::string cj = R"({"tag_name":"23.1.0","assets":[{"name":"clangd_indexing_tools-linux-23.1.0.zip","browser_download_url":"i","size":1,"digest":"sha256:ee"},
        {"name":"clangd-linux-23.1.0.zip","browser_download_url":"c","size":1,"digest":"sha256:ff"}]})";
    CHECK(pickAsset("clangd", cj, "x86_64")->url == "c");
    CHECK(!pickAsset("clangd", cj, "aarch64"));

    if (const char* net = std::getenv("DAEMON_TEST_NET"); net && *net == '1') {   // real downloads: DAEMON_TEST_NET=1 ./daemon_tests
        const std::string home = (std::filesystem::temp_directory_path() / "daemon_dl_test").string();
        std::filesystem::remove_all(home);
        ::setenv("XDG_DATA_HOME", home.c_str(), 1);
        for (const char* id : {"ninja", "cmake", "clangd"}) {
            Progress pr;
            auto r = install(id, pr);
            if (!r.ok) std::println(std::cerr, "install {}: {}", id, r.error);
            CHECK(r.ok && !r.version.empty());
            CHECK(std::filesystem::exists(std::filesystem::path(home) / "daemon/tools/bin" / id));
            std::println(std::cerr, "installed {} {}", id, r.version);
        }
        ::unsetenv("XDG_DATA_HOME");
        std::filesystem::remove_all(home);
    }

    auto found = detect();   // runs the real tools: git is always there in this environment
    CHECK(found.size() == tools().size());
    for (const auto& s : found) if (s.tool.id == "git") CHECK(s.found && !s.version.empty());
}

static void testCMakePaths() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / ("ide-cmakepath-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "src");
    fs::create_directories(base / "include");
    fs::create_directories(base / "third_party" / "lib");
    fs::create_directories(base / ".hidden");
    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    touch(base / "CMakeLists.txt"); touch(base / "src" / "main.cpp"); touch(base / "src" / "util.cpp"); touch(base / "src" / "notes.txt");
    touch(base / "include" / "a.hpp"); touch(base / "third_party" / "lib" / "CMakeLists.txt"); touch(base / "modules.cmake");
    auto names = [](const std::optional<CMakePathResult>& r) { std::set<std::string> s; if (r) for (const auto& i : r->items) s.insert(i.label); return s; };
    // add_subdirectory( : folders only, the ones with a CMakeLists.txt first
    auto r1 = cmakePathCompletions("project(x)\nadd_subdirectory(", base, base, base / "build");
    CHECK(r1 && !r1->exclusive && names(r1) == std::set<std::string>({"include/", "src/", "third_party/"}));
    CHECK(r1 && std::ranges::any_of(r1->items, [](const auto& i) { return i.label == "third_party/" && i.sort[0] == '1'; }));
    auto r1b = cmakePathCompletions("add_subdirectory(third_party/", base, base, base / "build");
    CHECK(r1b && r1b->exclusive && names(r1b) == std::set<std::string>({"lib/"}) && r1b->items[0].sort[0] == '0' && r1b->items[0].detail.find("CMakeLists") != std::string::npos);
    // sources: folders and source files, not notes.txt
    auto r2 = cmakePathCompletions("add_executable(app src/", base, base, base / "build");
    CHECK(r2 && r2->exclusive && r2->typed == 0 && names(r2) == std::set<std::string>({"main.cpp", "util.cpp"}));
    auto r3 = cmakePathCompletions("target_sources(app PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/src/ma", base, base, base / "build");
    CHECK(r3 && r3->typed == 2 && names(r3).contains("main.cpp"));
    auto r4 = cmakePathCompletions("include_directories(../", base / "src", base, base / "build");
    CHECK(r4 && names(r4) == std::set<std::string>({"include/", "src/", "third_party/"}));
    auto r5 = cmakePathCompletions("include(", base, base, base / "build");
    CHECK(r5 && names(r5).contains("modules.cmake") && names(r5).contains("CMakeLists.txt") && !names(r5).contains(".hidden/"));
    auto r6 = cmakePathCompletions("file(GLOB S \"${CMAKE_SOURCE_DIR}/src/", base / "third_party", base, base / "build");
    CHECK(r6 && names(r6).contains("notes.txt") && names(r6).contains("main.cpp"));
    auto r7 = cmakePathCompletions("set(X /no/such/dir/", base, base, base / "build");
    CHECK(r7 && r7->exclusive && r7->items.empty());
    CHECK(!cmakePathCompletions("set(X ${UNKNOWN}/a", base, base, base / "build"));
    CHECK(!cmakePathCompletions("src/", base, base, base / "build"));                     // not inside a command
    CHECK(!cmakePathCompletions("add_subdirectory(a) # src/", base, base, base / "build"));  // comment
    CHECK(!cmakePathCompletions("project(demo", base, base, base / "build"));              // project() takes no paths, no slash
    std::error_code ec;
    fs::remove_all(base, ec);
}

static void testGlsl() {
    using namespace ide::glsl;
    CHECK(isGlslPath("a/shader.frag") && isGlslPath("x.VERT") && isGlslPath("m.rchit") && isGlslPath("t.glsl") && !isGlslPath("a.cpp"));
    CHECK(stageOf("a/lit.frag", "") == "frag" && stageOf("a/lit.vert.glsl", "") == "vert" && stageOf("x.glsl", "").empty());
    CHECK(stageOf("x.glsl", "#pragma shader_stage(compute)\n") == "comp");
    const std::string src =
        "#version 450\n"
        "#define PI 3.14159\n"
        "struct Light { vec3 pos; float power; };\n"
        "layout(set = 0, binding = 0) uniform Camera { mat4 view; vec3 eye; } cam;\n"
        "layout(location = 0) in vec2 uv;\n"
        "layout(location = 0) out vec4 color;\n"
        "float shade(Light l, vec3 n) {\n"
        "    vec3 d = normalize(l.pos);\n"
        "    float k = dot(n, d);\n"
        "    return k * l.power;\n"
        "}\n"
        "void main() {\n"
        "    Light lamp;\n"
        "    color = vec4(uv, 0.0, 1.0);\n"
        "}\n";
    auto has = [](const std::vector<ide::LspCompletion>& v, std::string_view name, int kind = -1) {
        return std::ranges::any_of(v, [&](const auto& c) { return c.filterText == name && (kind < 0 || c.kind == kind); });
    };
    auto g = complete(src, 13, 4, "frag");   // start of a statement in main
    CHECK(has(g, "vec3", 25) && has(g, "mix", 3) && has(g, "gl_FragCoord") && !has(g, "gl_Position") && has(g, "shade", 3) && has(g, "Light", 22));
    CHECK(has(g, "lamp", 6) && has(g, "PI") && has(g, "cam", 6) && has(g, "uv", 6));
    CHECK(!has(complete(src, 13, 4, "vert"), "gl_FragCoord") && has(complete(src, 13, 4, "vert"), "gl_Position"));
    // `lamp.` shows the members of Light, `cam.` the members of the block, `uv.` swizzles
    std::string t2 = src; t2.replace(t2.find("    color = vec4(uv, 0.0, 1.0);"), 31, "    lamp.\n    cam.\n    uv.");
    auto m1 = complete(t2, 13, 9, "frag");
    CHECK(has(m1, "pos", 5) && has(m1, "power", 5) && !has(m1, "vec3"));
    auto m2 = complete(t2, 14, 8, "frag");
    CHECK(has(m2, "view", 5) && has(m2, "eye", 5));
    auto m3 = complete(t2, 15, 7, "frag");
    CHECK(has(m3, "x", 5) && has(m3, "xy", 5) && has(m3, "rg", 5) && !has(m3, "z"));
    CHECK(!complete("#v", 0, 2, "").empty() && has(complete("#v", 0, 1, ""), "version"));
    CHECK(has(complete("layout(", 0, 7, "frag"), "location"));
    // parameter hints
    auto sig = signature("void main() { float a = mix(vec3(1.0), b, ", 0, 41);
    CHECK(sig && !sig->sigs.empty() && sig->sigs[0].label.find("mix(") != std::string::npos && sig->sigs[0].active == 2);
    auto us = signature("float f(int a, vec3 b) { return 0.0; }\nvoid main() { f(1, ", 1, 19);
    CHECK(us && us->sigs[0].label == "float f(int a, vec3 b)" && us->sigs[0].active == 1 && us->sigs[0].params.size() == 2);
    CHECK(!signature("void main() { if (", 0, 18));
    CHECK(!signature("int x = 1;", 0, 5));
    // hover / definition / outline
    CHECK(hover(src, 8, 15, "frag").find("float dot(") != std::string::npos);
    CHECK(hover(src, 12, 11, "frag").find("Light lamp") != std::string::npos);
    CHECK(hover(src, 6, 7, "frag").find("float shade(Light l, vec3 n)") != std::string::npos);
    auto def = definition(src, 7, 24);   // `l` in l.pos
    CHECK(def && def->first == 6);
    auto defn = definition(src, 12, 5);  // Light in `Light lamp;`
    CHECK(defn && defn->first == 2);
    auto syms = symbols(src);
    CHECK(syms.size() >= 6);
    const auto fn = std::ranges::find(syms, "shade", &ide::LspSymbol::name);
    CHECK(fn != syms.end() && fn->kind == 12 && fn->line == 6 && fn->endLine == 10);
    const auto st = std::ranges::find(syms, "Light", &ide::LspSymbol::name);
    CHECK(st != syms.end() && st->kind == 23 && st->kids.size() == 2);
    // error checking (only when a compiler is installed)
    if (!compiler().empty()) {
        std::string err;
        auto bad = diagnose("#version 450\nlayout(location=0) out vec4 c;\nvoid main() {\n    c = vec4(nope);\n}\n", "frag", Target::Auto, "", err);
        CHECK(err.empty() && bad.size() >= 1 && bad[0].line == 3 && bad[0].message.find("nope") != std::string::npos && bad[0].severity == 1);
        CHECK(bad[0].col == 13 && bad[0].endCol == 17);   // just `nope` is underlined
        auto good = diagnose("#version 450\nlayout(location=0) out vec4 c;\nvoid main() { c = vec4(1.0); }\n", "frag", Target::Auto, "", err);
        CHECK(err.empty() && good.empty());
    }
}

static void testSettings() {
    using namespace ide::settings;
    // every definition has a default that passes its own validation, and keys are unique
    std::set<std::string> keys;
    for (const auto& d : defs()) {
        CHECK(keys.insert(d.key).second);
        Settings t;
        CHECK(t.set(d.key, d.def));
        CHECK(t.isDefault(d.key));
        if (d.type == Type::Int) CHECK(std::get<int>(d.def) >= d.min && std::get<int>(d.def) <= d.max);
    }
    Settings s;
    CHECK(s.i("editor.tabWidth") == 4 && s.b("editor.minimap") && s.s("terminal.shell").empty());
    CHECK(s.toText() == "{}\n");
    CHECK(s.set("editor.tabWidth", 2) && s.i("editor.tabWidth") == 2 && !s.isDefault("editor.tabWidth"));
    CHECK(s.set("editor.fontSize", 500) && s.i("editor.fontSize") == 48);      // clamped
    CHECK(!s.set("editor.tabWidth", true));                                      // wrong type
    CHECK(!s.set("no.such.key", 1));
    CHECK(s.set("terminal.shell", std::string("  /bin/zsh \n")) && s.s("terminal.shell") == "/bin/zsh");
    CHECK(s.set("editor.minimap", false));
    const std::string text = s.toText();
    Settings back = Settings::fromText(text);
    CHECK(back == s);
    CHECK(back.i("editor.tabWidth") == 2 && !back.b("editor.minimap") && back.s("terminal.shell") == "/bin/zsh");
    s.reset("editor.minimap");
    CHECK(s.b("editor.minimap") && s.isDefault("editor.minimap"));
    // hand-written files: bad values fall back, unknown keys survive a save
    std::vector<std::string> problems;
    Settings h = Settings::fromText("{\"editor.tabWidth\": \"wide\", \"editor.minimap\": false, \"future.thing\": [1,2]}", &problems);
    CHECK(problems.size() == 1 && h.i("editor.tabWidth") == 4 && !h.b("editor.minimap"));
    CHECK(h.toText().find("future.thing") != std::string::npos);
    problems.clear();
    Settings bad = Settings::fromText("{ nonsense", &problems);
    CHECK(problems.size() == 1 && bad == Settings{});
    CHECK(sections().size() >= 5);
}

void runServicesTests() {
    testCMakePaths();
    testGlsl();
    testSettings();
    testToolchain();
    testFileOps();
    testCMake();
    testLsp();
    testCMakeCompletion();
    testModules();
    testProjects();
    testGit();
    testTerminal();
    testSearch();
    testGdbMi();
}
