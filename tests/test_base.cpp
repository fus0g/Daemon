// Tests for the base libraries.
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

static void testPty() {
    Process p;
    ProcessOptions o;
    o.argv = {"sh", "-c", "printf 'Name: '; read x; echo \"got $x\"; exit 3"};
    o.usePty = true;
    CHECK(p.start(o).has_value());
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (out.find("Name: ") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        pollfd pf{p.outFd(), POLLIN, 0};
        if (::poll(&pf, 1, 100) > 0) p.read(out);
    }
    CHECK(out.find("Name: ") != std::string::npos);   // the prompt arrives before any input (no pipe buffering)
    CHECK(p.write("bob\n"));
    while (out.find("got bob") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        pollfd pf{p.outFd(), POLLIN, 0};
        if (::poll(&pf, 1, 100) > 0 && p.read(out) == 0) break;
    }
    CHECK(out.find("got bob") != std::string::npos);
    for (int i = 0; i < 100 && !p.tryWait(); ++i) { p.read(out); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    CHECK(p.tryWait().value_or(-1) == 3);
}

void runBaseTests() {
    testUtf8();
    testJson();
    testProcess();
    testPty();
}
