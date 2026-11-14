// SPDX-License-Identifier: GPL-3.0-or-later
// cli - command line handling and the process entry point of Daemon (implementation unit of daemon.app).
module daemon.app;
import std;
import daemon.base.util;
import daemon.services.notebook;

namespace {

// daemon --nb-run notebook.dcxxnb [out.dcxxnb]: executes the C++ cells headlessly (also used by the tests).
int runNotebook(const std::string& in, const std::string& out) {
    auto text = ide::readFile(in);
    if (!text) { std::println(std::cerr, "daemon: cannot read {}", in); return 2; }
    auto nb = ide::nb::parseNotebook(*text);
    if (!nb) { std::println(std::cerr, "daemon: {}", nb.error()); return 2; }
    if (!nb->isCpp()) { std::println(std::cerr, "daemon: only C++ notebooks can be run"); return 2; }
    const int failed = ide::nb::runAll(*nb, std::filesystem::absolute(in).string(), &std::cout);
    const std::string target = out.empty() ? in : out;
    if (!ide::writeFile(target, ide::nb::serializeNotebook(*nb))) { std::println(std::cerr, "daemon: cannot write {}", target); return 2; }
    std::println(std::cerr, "{} cell(s) failed; written to {}", failed, target);
    return failed == 0 ? 0 : 1;
}

void usage() {
    std::println("daemon - a Wayland + Vulkan code editor\n\n"
                 "usage: daemon [options] [file|folder...]\n\n"
                 "  --font-size <px>        font size in logical pixels (default 15)\n"
                 "  --screenshot <out.png>  render a few frames, save a PNG and exit\n"
                 "  --keys <script>         replay keys, e.g. \"ctrl+f|t:main|enter\" (for testing)\n"
                 "  --nb-run <file.ipynb|.dcxxnb> [out]\n"
                 "                          run every cell of a C++ notebook (no window), print the output, write the results\n"
                 "  -v, --version           print the version and exit\n"
                 "  -h, --help              show this help\n\n"
                 "environment: IDE_FONT=<ttf/otf>  IDE_UI_FONT=<ttf/otf>  IDE_GPU=<index>  IDE_VALIDATE=1");
}

}  // namespace

// Called by main() (a plain translation unit that does not need to import any module).
extern "C" int daemon_main(int argc, char** argv) {
    ide::Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "-v" || a == "--version") { std::println("Daemon {}", ide::versionFull()); return 0; }
        if (a == "--nb-run" && i + 1 < argc) {
            const std::string in = argv[++i];
            return runNotebook(in, i + 1 < argc && argv[i + 1][0] != '-' ? std::string(argv[++i]) : std::string());
        }
        if (a == "--screenshot" && i + 1 < argc) { opts.screenshot = argv[++i]; continue; }
        if (a == "--keys" && i + 1 < argc) { opts.keys = argv[++i]; continue; }
        if (a == "--font-size" && i + 1 < argc) { opts.fontSize = std::stof(argv[++i]); continue; }
        if (a.starts_with("--")) { std::println(std::cerr, "unknown option: {}", a); usage(); return 2; }
        opts.files.emplace_back(a);
    }

    auto app = std::make_unique<ide::App>(std::move(opts));
    if (auto r = app->init(); !r) {
        std::println(std::cerr, "daemon: {}", r.error());
        return 1;
    }
    const int rc = app->run();
    const auto t0 = std::chrono::steady_clock::now();
    app.reset();
    ide::logLine(std::format("shutdown took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(rc);   // background jobs may still be running (a git fetch, a download): do not wait for them or race their statics
}
