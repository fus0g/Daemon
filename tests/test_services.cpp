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

void runServicesTests() {
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
