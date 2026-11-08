// cli - command line handling and the process entry point of Daemon (implementation unit of daemon.app).
module daemon.app;
import std;

namespace {

void usage() {
    std::println("daemon - a Wayland + Vulkan code editor\n\n"
                 "usage: daemon [options] [file|folder...]\n\n"
                 "  --font-size <px>        font size in logical pixels (default 15)\n"
                 "  --screenshot <out.png>  render a few frames, save a PNG and exit\n"
                 "  --keys <script>         replay keys, e.g. \"ctrl+f|t:main|enter\" (for testing)\n"
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
        if (a == "--screenshot" && i + 1 < argc) { opts.screenshot = argv[++i]; continue; }
        if (a == "--keys" && i + 1 < argc) { opts.keys = argv[++i]; continue; }
        if (a == "--font-size" && i + 1 < argc) { opts.fontSize = std::stof(argv[++i]); continue; }
        if (a.starts_with("--")) { std::println(std::cerr, "unknown option: {}", a); usage(); return 2; }
        opts.files.emplace_back(a);
    }

    ide::App app(std::move(opts));
    if (auto r = app.init(); !r) {
        std::println(std::cerr, "daemon: {}", r.error());
        return 1;
    }
    
    return app.run();
}
