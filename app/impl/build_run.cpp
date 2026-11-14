// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / build_run - part of the App implementation (see app.cppm for the class).
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.options;
import daemon.app.theme;
import daemon.ui.vulkan;
import daemon.ui.wayland;
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
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

void App::detectCMake() {
    cmakeOpen_ = false;
    if (buildProc_) cmakeStop();
    if (!explorer_.isOpen() || !hasCMakeLists(explorer_.rootPath())) { dirty_ = true; return; }
    cmakeOpen_ = true;
    cmakeRoot_ = explorer_.rootPath();
    profiles_ = defaultProfiles(cmakeRoot_);
    profile_ = 0;
    haveNinja_ = findExecutable("ninja").has_value();
    targets_.clear();
    target_ = -1;
    buildOut_.clear(); runOut_.clear(); problems_.clear();
    buildStatus_.clear();
    if (isConfigured(profile())) loadTargetsNow();
    if (targets_.empty()) cmakeReload();  // like CLion: load the project automatically
    else { buildStatus_ = "CMake ready"; restartLsp(); }  // pick up the project's compile database
    dirty_ = true;
}

void App::loadTargetsNow() {
    std::string keep = target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : std::string();
    targets_ = loadTargets(profile().dir);
    target_ = -1;
    for (std::size_t i = 0; i < targets_.size(); ++i)
        if (targets_[i].name == keep) target_ = static_cast<int>(i);
    if (target_ < 0 && !targets_.empty()) target_ = 0;  // executables sort first
    dirty_ = true;
}

void App::selectProfile(int i) {
    if (i == profile_ || i < 0 || i >= static_cast<int>(profiles_.size())) return;
    if (buildProc_) { setMessage("Stop the running build first", true); return; }
    profile_ = i;
    targets_.clear();
    target_ = -1;
    if (isConfigured(profile())) loadTargetsNow();
    if (targets_.empty()) cmakeReload(); else { buildStatus_ = "CMake ready"; restartLsp(); }
    dirty_ = true;
}

void App::startBuildProc(TaskKind kind, std::vector<std::string> argv, const std::string& title) {
    buildOut_.clear();
    problems_.clear();
    buildCur_ = buildTotal_ = 0;
    buildPartial_.clear();
    ProcessOptions o;
    o.argv = std::move(argv);
    o.cwd = cmakeRoot_.string();
    std::string cmd;
    for (const auto& a : o.argv) cmd += (cmd.empty() ? "" : " ") + a;
    buildOut_.push_back({"$ " + cmd, 4, -1});
    buildProc_ = std::make_unique<Process>();
    panelOpen_ = true;
    panelTab_ = 0;
    panelFollow_[0] = true;
    if (auto r = buildProc_->start(o); !r) {
        buildOut_.push_back({r.error(), 2, -1});
        buildProc_.reset();
        buildStatus_ = "Build failed to start";
        return;
    }
    buildKind_ = kind;
    buildEof_ = false;
    buildStatus_ = title;
    win_.watch(buildProc_->outFd(), [this] { pumpBuild(); });
    dirty_ = true;
}

void App::cmakeReload() {
    if (!cmakeOpen_) return;
    if (buildProc_) { setMessage("A build is already running", true); return; }
    saveAllForBuild();
    prepareFileApi(profile().dir);
    startBuildProc(TaskKind::Configure, configureArgv(cmakeRoot_, profile(), haveNinja_), "Configuring (" + profile().name + ")");
}

void App::cmakeBuild(bool thenRun, bool debug) {
    debugRun_ = thenRun && debug;
    if (!cmakeOpen_) { setMessage("No CMake project open (open a folder with a CMakeLists.txt)", true); return; }
    if (buildProc_ && buildKind_ == TaskKind::Configure) {  // CMake is still loading: build as soon as it is done
        pendingBuild_ = true;
        runAfterBuild_ = thenRun;
        setMessage("Waiting for CMake, then building...", false, 2.5f);
        return;
    }
    if (buildProc_) { setMessage("A build is already running", true); return; }
    if (thenRun) {
        if (target_ < 0 && !targets_.empty()) { setMessage("No target selected", true); return; }
        if (target_ < 0) { setMessage("No target selected", true); return; }
        if (!targets_[static_cast<std::size_t>(target_)].executable()) {
            setMessage("'" + targets_[static_cast<std::size_t>(target_)].name + "' is not an executable target", true);
            return;
        }
    }
    saveAllForBuild();
    runAfterBuild_ = thenRun;
    if (!isConfigured(profile()) || targets_.empty()) { pendingBuild_ = true; cmakeReload(); return; }
    const std::string tgt = target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : std::string();
    startBuildProc(TaskKind::Build, buildArgv(profile(), tgt), "Building " + (tgt.empty() ? profile().name : tgt));
}

void App::cmakeStop() {
    debugRun_ = false;
    pendingBuild_ = false;
    runAfterBuild_ = false;
    if (buildProc_) buildProc_->terminate();
    if (dbg_) debugStop();
    if (runProc_) runProc_->terminate();
}

std::string App::cleanTerm(std::string_view raw) {
    const std::string t = stripAnsi(raw);
    std::string out;
    for (char c : t) {
        if (c == '\b') { if (!out.empty()) out.erase(utf8Prev(out, out.size())); }
        else if (c == '\t' || static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::vector<std::string> App::takeLines(std::string& partial, std::string_view chunk) {
    partial.append(chunk);
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < partial.size(); ++i) {
        if (partial[i] == '\n' || (partial[i] == '\r' && !(i + 1 < partial.size() && partial[i + 1] == '\n'))) {
            lines.push_back(partial.substr(start, i - start));
            start = i + 1;
        }
    }
    partial.erase(0, start);
    return lines;
}

void App::addBuildLine(const std::string& raw) {
    std::string text = stripAnsi(raw);
    const std::string rootPrefix = cmakeRoot_.string() + "/";
    for (std::size_t at = text.find(rootPrefix); at != std::string::npos; at = text.find(rootPrefix, at)) text.erase(at, rootPrefix.size());
    OutLine ol{text, 0, -1};
    if (auto pr = parseProgress(text)) {
        buildCur_ = pr->first;
        buildTotal_ = pr->second;
        buildStatus_ = std::format("{} {}/{}", buildKind_ == TaskKind::Configure ? "Configuring" : "Building", pr->first, pr->second);
        ol.sev = 4;
    } else if (auto p = parseProblem(stripAnsi(raw), profile().dir, cmakeRoot_)) {
        ol.problem = static_cast<int>(problems_.size());
        ol.sev = p->severity == Severity::Error ? 2 : p->severity == Severity::Warning ? 1 : 3;
        problems_.push_back(*p);
    } else if (text.starts_with("FAILED:") || text.starts_with("ninja: build stopped") || text.find("error:") != std::string::npos ||
               text.find("undefined reference") != std::string::npos) {
        ol.sev = 2;
    }
    buildOut_.push_back(std::move(ol));
    if (buildOut_.size() > 30000) buildOut_.erase(buildOut_.begin(), buildOut_.begin() + 5000);
}

void App::pumpBuild() {
    if (!buildProc_) return;
    std::string chunk;
    long n = buildProc_->read(chunk);
    if (!chunk.empty()) for (auto& l : takeLines(buildPartial_, chunk)) addBuildLine(l);
    dirty_ = true;
    if (n == 0) {
        win_.unwatch(buildProc_->outFd());
        buildProc_->closeOutput();
        buildEof_ = true;
    }
}

void App::finishBuild(int code) {
    if (!buildPartial_.empty()) { addBuildLine(buildPartial_); buildPartial_.clear(); }
    const TaskKind kind = buildKind_;
    buildProc_.reset();
    buildKind_ = TaskKind::None;
    buildEof_ = false;
    int errors = static_cast<int>(std::ranges::count_if(problems_, [](const Problem& p) { return p.severity == Severity::Error; }));
    if (kind == TaskKind::Configure) {
        if (code == 0) {
            buildOut_.push_back({"CMake configured successfully", 5, -1});
            buildStatus_ = "CMake ready";
            loadTargetsNow();
            restartLsp();
            if (pendingBuild_) { pendingBuild_ = false; cmakeBuild(runAfterBuild_, debugRun_); return; }
            setMessage("CMake configured: " + std::to_string(targets_.size()) + " targets", false, 2.5f);
        } else {
            buildOut_.push_back({std::format("CMake failed (exit code {})", code), 2, -1});
            buildStatus_ = "CMake failed";
            pendingBuild_ = runAfterBuild_ = false;
            if (!problems_.empty()) panelTab_ = 0;
            setMessage("CMake configuration failed - see the Build panel", true, 5);
        }
    } else if (kind == TaskKind::Build) {
        // The build may have generated headers / module files that earlier diagnostics were missing: look at the files again.
        // After a successful build no clangd error is believable (the compiler accepted everything), so all of them are re-checked.
        refreshStaleDiagnostics(code != 0);
        if (code == 0) {
            buildOut_.push_back({"Build finished", 5, -1});
            buildStatus_ = "Build succeeded";
            if (runAfterBuild_) { runAfterBuild_ = false; startRun(debugRun_); debugRun_ = false; return; }
            setMessage("Build succeeded", false, 2.5f);
        } else {
            buildOut_.push_back({std::format("Build failed (exit code {})", code), 2, -1});
            buildStatus_ = errors ? std::format("Build failed: {} error{}", errors, errors == 1 ? "" : "s") : "Build failed";
            runAfterBuild_ = false;
            setMessage(buildStatus_, true, 5);
        }
    }
    dirty_ = true;
}

void App::startRun(bool debug) {
    if (target_ < 0) return;
    const CMakeTarget& t = targets_[static_cast<std::size_t>(target_)];
    std::error_code ec;
    if (!fs::exists(t.artifact, ec)) { setMessage("Executable not found: " + t.artifact.string(), true); return; }
    if (debug) {   // gdb through its machine interface (breakpoints, stepping, variables ...)
        debugLaunch({t.artifact.string(), t.artifact.parent_path().string(), runArgs_, t.name});
        return;
    }
    if (runProc_) { runProc_->terminate(); runProc_.reset(); }
    runOut_.clear();
    runPartial_.clear();
    ProcessOptions o;
    o.usePty = true;   // the program runs on a terminal: prompts show immediately and it can read the keyboard
    o.argv = {t.artifact.string()};
    for (auto& a : splitArgs(runArgs_)) o.argv.push_back(a);
    o.cwd = t.artifact.parent_path().string();
    std::string cmd;
    for (const auto& a : o.argv) cmd += (cmd.empty() ? "" : " ") + a;
    runOut_.push_back({"$ " + cmd, 4, -1});
    runProc_ = std::make_unique<Process>();
    panelOpen_ = true;
    panelTab_ = 1;
    panelFollow_[1] = true;
    if (auto r = runProc_->start(o); !r) {
        runOut_.push_back({r.error(), 2, -1});
        runProc_.reset();
        return;
    }
    runEof_ = false;
    runFocus_ = true;   // keystrokes go to the program (Esc or a click elsewhere releases them)
    buildStatus_ = (debug ? "Debugging " : "Running ") + t.name;
    win_.watch(runProc_->outFd(), [this] { pumpRun(); });
    dirty_ = true;
}

void App::pumpRun() {
    if (!runProc_) return;
    std::string chunk;
    long n = runProc_->read(chunk);
    if (!chunk.empty()) {
        for (auto& l : takeLines(runPartial_, chunk)) runOut_.push_back({cleanTerm(l), 0, -1});
        if (runOut_.size() > 30000) runOut_.erase(runOut_.begin(), runOut_.begin() + 5000);
    }
    dirty_ = true;
    if (n == 0) {
        win_.unwatch(runProc_->outFd());
        runProc_->closeOutput();
        runEof_ = true;
    }
}

void App::finishRun(int code) {
    if (!runPartial_.empty()) { runOut_.push_back({cleanTerm(runPartial_), 0, -1}); runPartial_.clear(); }
    runFocus_ = false;
    runOut_.push_back({"", 0, -1});
    runOut_.push_back({std::format("Process finished with exit code {}", code), code == 0 ? 5 : 2, -1});
    runProc_.reset();
    runEof_ = false;
    buildStatus_ = code == 0 ? "Run finished" : std::format("Run exited with code {}", code);
    dirty_ = true;
}

void App::refreshProblems() {
    std::vector<Problem> out = problems_;
    std::unordered_set<std::string> seen;
    for (const auto& p : problems_) seen.insert(p.file.string() + ":" + std::to_string(p.line));
    std::vector<Problem> live;
    for (const auto& [path, ds] : diags_) {
        for (const auto& d : ds) {
            if (d.severity > 3) continue;
            Problem p;
            p.severity = d.severity == 1 ? Severity::Error : d.severity == 2 ? Severity::Warning : Severity::Note;
            p.file = path;
            p.line = d.line + 1;
            p.col = d.col + 1;
            p.message = d.message;
            if (!seen.contains(path + ":" + std::to_string(p.line))) live.push_back(std::move(p));
        }
    }
    std::ranges::stable_sort(live, [](const Problem& a, const Problem& b) {
        if (a.severity != b.severity) return a.severity < b.severity;   // errors first
        return a.file != b.file ? a.file < b.file : a.line < b.line;
    });
    out.insert(out.end(), live.begin(), live.end());
    const bool changed = out.size() != shownProblems_.size() ||
        !std::ranges::equal(out, shownProblems_, [](const Problem& a, const Problem& b) { return a.file == b.file && a.line == b.line && a.message == b.message && a.severity == b.severity; });
    if (changed) {
        shownProblems_ = std::move(out);
        problemCounts_.clear();
        for (const auto& pr : shownProblems_) {
            if (pr.severity == Severity::Note) continue;
            const bool err = pr.severity == Severity::Error;
            fs::path f = pr.file.lexically_normal();
            const fs::path root = explorer_.isOpen() ? explorer_.rootPath() : fs::path();
            for (fs::path q = f; !q.empty() && q != q.parent_path(); q = q.parent_path()) {
                auto& c = problemCounts_[q.string()];
                (err ? c.first : c.second)++;
                if (!root.empty() && q == root) break;
            }
        }
        dirty_ = true;
    }
}

void App::gotoProblem(const Problem& p) {
    openFile(p.file.string());
    if (docs_.empty()) return;
    Document& d = doc();
    d.gotoLine(p.line - 1, 0);
    d.setCursor({d.cursor.line, byteColAt(d.buf.line(d.cursor.line), std::max(0, p.col - 1), d.tabWidth)});
    scrollToCursor_ = true;
    sideFocus_ = false;
    dirty_ = true;
}

void App::toolbarClick() {
    const float x = mx_, y = my_;
    if (tool_.profile.contains(x, y)) {
        std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>> e;
        for (int i = 0; i < static_cast<int>(profiles_.size()); ++i)
            e.emplace_back(profiles_[static_cast<std::size_t>(i)].name, "", i == profile_, [this, i] { selectProfile(i); });
        openCustomMenu(tool_.profile.x, tool_.profile.bottom() + 4 * S(), e);
    } else if (tool_.target.contains(x, y)) {
        std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>> e;
        for (int i = 0; i < static_cast<int>(targets_.size()); ++i) {
            const auto& t = targets_[static_cast<std::size_t>(i)];
            std::string kind = t.type == "EXECUTABLE" ? "app" : t.type == "STATIC_LIBRARY" ? "static lib" : t.type == "SHARED_LIBRARY" ? "shared lib" : "";
            e.emplace_back(t.name, kind, i == target_, [this, i] { target_ = i; dirty_ = true; });
        }
        e.emplace_back("Program arguments...", "", false, [this] { openPrompt(PromptKind::RunArgs); });
        e.emplace_back("Reload CMake", "Ctrl+Shift+R", false, [this] { cmakeReload(); });
        openCustomMenu(tool_.target.x, tool_.target.bottom() + 4 * S(), e);
    } else if (tool_.run.contains(x, y)) cmakeBuild(true);
    else if (tool_.debug.contains(x, y)) cmakeBuild(true, true);
    else if (tool_.build.contains(x, y)) cmakeBuild(false);
    else if (tool_.stop.contains(x, y)) cmakeStop();
    else if (tool_.reload.contains(x, y)) cmakeReload();
    else if (tool_.panel.contains(x, y)) togglePanel();
}

int App::toolAt(float x, float y) const {
    if (!cmakeOpen_) return 0;
    const Rect* rs[] = {&tool_.profile, &tool_.target, &tool_.run, &tool_.build, &tool_.stop, &tool_.reload, &tool_.panel, &tool_.debug};
    for (int i = 0; i < 8; ++i)
        if (rs[i]->contains(x, y)) return i + 1;
    return 0;
}

void App::drawToolbar() {
    if (!cmakeOpen_) return;
    const float s = S(), m = std::round(8 * s);
    const bool busy = buildProc_ != nullptr;
    const bool anyRunning = busy || runProc_ != nullptr;
    const float hair = std::max(1.0f, std::round(0.8f * s));
    auto fade = [](Color c, float a) { return c.alpha(static_cast<std::uint8_t>(static_cast<float>(c.a) * a)); };

    // Configuration / target selectors: outlined pills with a chevron.
    auto chip = [&](const Rect& r, const std::string& text, bool hot) {
        fill(r, Color::rgb(0xffffff, hot ? 56 : 34), r.h / 2);
        fill(r.x + hair, r.y + hair, r.w - 2 * hair, r.h - 2 * hair, hot ? Color::rgb(0x1a1c21) : Color::rgb(0x0d0e11), r.h / 2 - hair);
        uiLabel(r.x + std::round(16 * s), r.y, r.h, text, theme::text, r.right() - std::round(30 * s));
        const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.6f)));
        Icons::chevron(dl_, font_, r.right() - static_cast<float>(cs) - std::round(12 * s), r.y + (r.h - static_cast<float>(cs)) / 2, cs, true, theme::dim);
    };
    chip(tool_.profile, profiles_.empty() ? "-" : profiles_[static_cast<std::size_t>(profile_)].name, hoverTool_ == 1);
    chip(tool_.target, target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : "No target", hoverTool_ == 2);

    // Action cluster.
    const Rect& cl = tool_.cluster;
    fill(cl, Color::rgb(0xffffff, 34), cl.h / 2);
    fill(cl.x + hair, cl.y + hair, cl.w - 2 * hair, cl.h - 2 * hair, Color::rgb(0x0d0e11), cl.h / 2 - hair);
    const int ip = std::max(14, static_cast<int>(std::round(cl.h * 0.6f)));
    struct Btn { const Rect* r; char32_t glyph; Color color; bool enabled; int id; bool primary; };
    const bool canRun = !busy && target_ >= 0;
    const Color white = Color::rgb(0xffffff);
    const Btn btns[] = {
        {&tool_.run, 0xF040A, white, canRun, 3, false},
        {&tool_.debug, 0xF00E4, white, canRun, 8, false},
        {&tool_.build, 0xF08EA, white, !busy, 4, false},
        {&tool_.reload, 0xF0450, white, !busy, 6, false},
        {&tool_.stop, 0xF04DB, white, anyRunning, 5, false},
    };
    for (const Btn& b : btns) {
        const Rect& r = *b.r;
        const bool hot = hoverTool_ == b.id && b.enabled;
        const float d = std::round(r.h - 6 * s);
        const Rect circle{r.x + (r.w - d) / 2, r.y + 3 * s, d, d};
        if (hot) fill(circle, Color::rgb(0xffffff, 30), d / 2);
        else if (b.id == 4 && busy) fill(circle, Color::rgb(0xffffff, 22), d / 2);
        Icons::glyph(dl_, font_, std::round(r.x + (r.w - static_cast<float>(ip)) / 2), std::round(r.y + (r.h - static_cast<float>(ip)) / 2), ip, b.glyph,
                     b.enabled ? (b.id == 5 ? theme::danger : b.color) : fade(b.color, 0.32f));   // Stop turns red while something runs
    }
    // Dividers between the groups (run | build | stop).
    for (const Rect* r : {&tool_.debug, &tool_.reload}) {
        const float dx = r->right() + std::round(4 * s);
        fill(dx, cl.y + std::round(8 * s), hair, cl.h - std::round(16 * s), Color::rgb(0xffffff, 30));
    }
    if (busy) animating_ = true;   // keeps the status bar / progress fresh

    // Status text with a slim progress bar while building.
    float rx = L_.toolbar.w - m - std::round(6 * s) - (L_.csd ? closePill().w + std::round(8 * s) : 0.0f);
    const float ry = tool_.run.y, rhh = tool_.run.h;
    if (!buildStatus_.empty()) {
        const bool bad = buildStatus_.find("failed") != std::string::npos || buildStatus_.find("Failed") != std::string::npos;
        const Color c = bad ? theme::danger : (busy ? theme::text : theme::dim);
        rx = uiLabelR(rx, ry, rhh, buildStatus_, c) - std::round(12 * s);
        const float bw = std::round(96 * s), bh = std::max(3.0f, std::round(4 * s)), by = ry + (rhh - bh) / 2;
        if (busy && buildTotal_ > 0) {
            fill(rx - bw, by, bw, bh, theme::border, bh / 2);
            fill(rx - bw, by, std::max(bh, bw * static_cast<float>(buildCur_) / static_cast<float>(buildTotal_)), bh, theme::accent, bh / 2);
        } else if (busy) {
            fill(rx - bw, by, bw, bh, theme::border, bh / 2);
            const float t = std::fmod(static_cast<float>(frames_) * 0.03f, 1.0f);
            fill(rx - bw + (bw - bw * 0.3f) * t, by, bw * 0.3f, bh, theme::accent, bh / 2);
            animating_ = true;
        }
    }
}

void App::drawToolTip() {
    if (!cmakeOpen_ || !toolTipShown_ || menu_.open || dlg_.kind != DlgKind::None) return;
    struct Tip { int id; const Rect* r; const char* name; const char* keys; };
    const Tip tips[] = {{3, &tool_.run, "Run", "Shift+F10"}, {8, &tool_.debug, "Debug (gdb)", "Shift+F9"}, {4, &tool_.build, "Build", "Ctrl+F9"},
                        {6, &tool_.reload, "Regenerate CMake", "Ctrl+Shift+R"}, {5, &tool_.stop, "Stop", "Ctrl+F2"}};
    for (const auto& t : tips) {
        if (hoverTool_ != t.id) continue;
        const float s = S();
        const std::string name = t.name, keys = t.keys;
        const float w = uiW(name) + uiW(keys) + 44 * s, h = std::round(uiH() + 14 * s);
        const Rect r{std::min(t.r->x, L_.status.w - w - 8 * s), t.r->bottom() + 8 * s, w, h};
        glassPanel(r, 8 * s);
        uiLabel(r.x + 12 * s, r.y, r.h, name, theme::text);
        uiLabelR(r.right() - 12 * s, r.y, r.h, keys, theme::dim);
        return;
    }
}

Color App::outColor(int sev) {
    switch (sev) {
        case 1: return Color::rgb(0xe5c07b);
        case 2: return Color::rgb(0xf2777a);
        case 3: return Color::rgb(0x7fb7ff);
        case 4: return theme::dim;
        case 5: return Color::rgb(0x6aab73);
        default: return theme::text;
    }
}

std::size_t App::panelRowCount() const {
    if (panelTab_ == 2) return shownProblems_.size();
    if (panelTab_ == 4) return dbgCon_.size();
    return panelLines().size() + (panelTab_ == 1 && runProc_ ? 1 : 0);   // + the line the program is still writing
}

std::string App::panelRowText(int i) const {
    if (panelTab_ == 2) {
        const Problem& pr = shownProblems_[static_cast<std::size_t>(i)];
        std::string rel = pr.file.lexically_relative(cmakeRoot_).string();
        if (rel.empty() || rel.starts_with("..")) rel = pr.file.string();
        return std::format("{}:{}:{}  {}", rel, pr.line, pr.col, pr.message);
    }
    if (panelTab_ == 1 && static_cast<std::size_t>(i) >= runOut_.size()) return cleanTerm(runPartial_);
    return panelLines()[static_cast<std::size_t>(i)].text;
}

std::pair<int, int> App::panelPosAt(float x, float y) const {
    const Rect body = panelBody();
    const int total = static_cast<int>(panelRowCount());
    if (total == 0) return {0, 0};
    int row = static_cast<int>(std::floor((y - body.y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
    row = std::clamp(row, 0, total - 1);
    const int cell = std::max(0, static_cast<int>(std::lround((x - (body.x + 14 * S())) / cw())));
    return {row, byteColAt(panelRowText(row), cell, 4)};
}

std::string App::selectedOf(const TextSel& t, const std::function<std::string(int)>& rowText, int rows) {
    int r0 = t.ar, c0 = t.ac, r1 = t.cr, c1 = t.cc;
    if (!selOrdered(t)) { std::swap(r0, r1); std::swap(c0, c1); }
    std::string out;
    for (int r = r0; r <= r1 && r < rows; ++r) {
        const std::string line = rowText(r);
        const std::size_t a = r == r0 ? std::min<std::size_t>(static_cast<std::size_t>(c0), line.size()) : 0;
        const std::size_t b = r == r1 ? std::min<std::size_t>(static_cast<std::size_t>(c1), line.size()) : line.size();
        if (r > r0) out += '\n';
        if (b > a) out += line.substr(a, b - a);
    }
    return out;
}

bool App::copyOverlay() {
    if (psel_.active && psel_.moved && panelOpen_) {
        const std::string t = selectedOf(psel_, [&](int r) { return panelRowText(r); }, static_cast<int>(panelRowCount()));
        if (!t.empty()) { win_.setClipboard(t); setMessage("Copied", false, 1.2f); return true; }
    }
    if (hover_.show && !hover_.lines.empty()) {
        std::string t;
        if (hsel_.active && hsel_.moved)
            t = selectedOf(hsel_, [&](int r) { return hover_.lines[static_cast<std::size_t>(r)].first; }, static_cast<int>(hover_.lines.size()));
        else if (docs_.empty() || !doc().hasSelection())
            for (std::size_t i = 0; i < hover_.lines.size(); ++i) t += (i ? "\n" : "") + hover_.lines[i].first;
        if (!t.empty()) { win_.setClipboard(t); setMessage("Copied", false, 1.2f); return true; }
    }
    return false;
}

void App::panelRowClick(int row) {
    if (panelTab_ == 2) {
        if (row >= 0 && row < static_cast<int>(shownProblems_.size())) gotoProblem(shownProblems_[static_cast<std::size_t>(row)]);
    } else {
        const auto& lines = panelLines();
        if (row >= 0 && row < static_cast<int>(lines.size()) && lines[static_cast<std::size_t>(row)].problem >= 0)
            gotoProblem(problems_[static_cast<std::size_t>(lines[static_cast<std::size_t>(row)].problem)]);
    }
}

std::string App::problemsText(bool errorsOnly) const {
    std::string out;
    for (const Problem& pr : shownProblems_) {
        if (errorsOnly && pr.severity != Severity::Error) continue;
        std::string rel = pr.file.lexically_relative(cmakeRoot_).string();
        if (rel.empty() || rel.starts_with("..")) rel = pr.file.string();
        const char* sev = pr.severity == Severity::Error ? "error" : pr.severity == Severity::Warning ? "warning" : "note";
        out += std::format("{}:{}:{}: {}: {}\n", rel, pr.line, pr.col, sev, pr.message);
    }
    return out;
}

void App::copyProblems(bool errorsOnly) {
    const std::string t = problemsText(errorsOnly);
    if (t.empty()) { setMessage(errorsOnly ? "No errors to copy" : "No problems to copy", false, 2); return; }
    win_.setClipboard(t);
    const auto n = std::ranges::count(t, '\n');
    setMessage(std::format("Copied {} {}", n, errorsOnly ? (n == 1 ? "error" : "errors") : (n == 1 ? "problem" : "problems")), false, 2);
}

void App::panelClick(Button b) {
    const Rect& p = L_.panel;
    if (b != Button::Left) return;
    runFocus_ = false;
    termFocus_ = false;
    dbgConFocus_ = false;
    psel_.active = false;
    if (my_ < p.y + 6 * S()) { panelDrag_ = true; return; }  // splitter at the top edge
    for (std::size_t i = 0; i < panelTabRects_.size(); ++i)
        if (panelTabRects_[i].contains(mx_, my_)) { panelTab_ = static_cast<int>(i); psel_.active = false; runFocus_ = panelTab_ == 1 && runProc_; termFocus_ = panelTab_ == 3; dbgConFocus_ = panelTab_ == 4; if (termFocus_ && !termProc_ && !termExited_) startTerminal(); dirty_ = true; return; }
    if (panelClose_.contains(mx_, my_)) { panelOpen_ = false; dirty_ = true; return; }
    if (panelTab_ == 2 && panelCopyAll_.contains(mx_, my_)) { copyProblems(false); dirty_ = true; return; }
    if (panelTab_ == 2 && panelCopyErrors_.contains(mx_, my_)) { copyProblems(true); dirty_ = true; return; }
    if (panelClear_.contains(mx_, my_)) {
        if (panelTab_ == 4) dbgCon_.clear();
        else if (panelTab_ == 3) { if (!termProc_) term_.reset(); }
        else if (panelTab_ == 1) runOut_.clear(); else if (panelTab_ == 0) { buildOut_.clear(); problems_.clear(); }
        else problems_.clear();
        dirty_ = true;
        return;
    }
    Rect body = panelBody();
    if (body.contains(mx_, my_) && panelTab_ == 3) { termClick(body); dirty_ = true; return; }
    if (body.contains(mx_, my_) && panelTab_ == 4) { dbgConFocus_ = dbgConIn_.contains(mx_, my_) || true; dbgIn_.cur = dbgConIn_.contains(mx_, my_) ? dbgIn_.text.size() : dbgIn_.cur; dirty_ = true; return; }
    if (body.contains(mx_, my_)) {
        runFocus_ = panelTab_ == 1 && runProc_;   // clicking the Run output gives the program the keyboard
        // Press starts a text selection; releasing without dragging counts as a click on the row.
        const auto [row, col] = panelPosAt(mx_, my_);
        psel_ = {};
        psel_.active = true;
        psel_.dragging = true;
        psel_.tab = panelTab_;
        psel_.ar = psel_.cr = row;
        psel_.ac = psel_.cc = col;
        psel_.sx = mx_; psel_.sy = my_;
    }
}

void App::drawPanel() {
    if (L_.panel.h <= 0.5f) return;
    const Rect& p = L_.panel;
    const float s = S(), hh = panelHeadH();
    fill(p, theme::panelBg);

    // Body first (rows may overflow the header; the header is painted over them).
    const Rect body = panelBody();
    const std::size_t tab = static_cast<std::size_t>(panelTab_);
    if (panelTab_ == 3) drawTerminalBody(body);
    else if (panelTab_ == 4) drawDebugConsole(body);
    else {
    panelScroll_[tab] = std::clamp(panelFollow_[tab] ? panelMaxScroll() : panelScroll_[tab], 0.0f, panelMaxScroll());
    const float sy = std::round(panelScroll_[tab]);
    const int total = static_cast<int>(panelRowCount());
    const int first = std::max(0, static_cast<int>(std::floor(sy / lh())));
    const int last = std::min(total - 1, static_cast<int>(std::floor((sy + body.h) / lh())));
    for (int i = first; i <= last; ++i) {
        const float y = body.y + static_cast<float>(i) * lh() - sy;
        std::string text;
        int sev = 0;
        bool clickable = false;
        if (panelTab_ == 2) {
            const Problem& pr = shownProblems_[static_cast<std::size_t>(i)];
            std::string rel = pr.file.lexically_relative(cmakeRoot_).string();
            if (rel.empty() || rel.starts_with("..")) rel = pr.file.string();
            text = std::format("{}:{}:{}  {}", rel, pr.line, pr.col, pr.message);
            sev = pr.severity == Severity::Error ? 2 : pr.severity == Severity::Warning ? 1 : 3;
            clickable = true;
        } else {
            if (panelTab_ == 1 && static_cast<std::size_t>(i) >= runOut_.size()) {   // live line: partial output + caret
                text = cleanTerm(runPartial_);
                if (runFocus_ && caretOn_ && focused_) {
                    const float cx = body.x + 14 * s + static_cast<float>(utf8Length(text)) * cw();
                    fill(std::round(cx), y + 2 * s, std::max(2.0f, std::round(cw() * 0.55f)), lh() - 4 * s, theme::caret.alpha(210));
                }
            } else {
                const OutLine& ol = panelLines()[static_cast<std::size_t>(i)];
                text = ol.text;
                sev = ol.sev;
                clickable = ol.problem >= 0;
            }
        }
        if (clickable && i == hoverPanelRow_ && !psel_.moved) fill(body.x + 4 * s, y, body.w - 8 * s, lh(), theme::tabHover, 4 * s);
        if (psel_.active && psel_.moved && psel_.tab == panelTab_) {   // selection highlight
            int r0 = psel_.ar, c0 = psel_.ac, r1 = psel_.cr, c1 = psel_.cc;
            if (!selOrdered(psel_)) { std::swap(r0, r1); std::swap(c0, c1); }
            if (i >= r0 && i <= r1) {
                const int a = i == r0 ? displayCol(text, std::min<int>(c0, static_cast<int>(text.size())), 4) : 0;
                const int e = i == r1 ? displayCol(text, std::min<int>(c1, static_cast<int>(text.size())), 4) : displayCol(text, static_cast<int>(text.size()), 4) + 1;
                if (e > a) fill(body.x + 14 * s + static_cast<float>(a) * cw(), y, static_cast<float>(e - a) * cw(), lh(), Color::rgb(0x3a4152));
            }
        }
        label(body.x + 14 * s, y, text, outColor(sev), body.right() - 10 * s);
    }
    if (total == 0) {
        const char* hint = panelTab_ == 0 ? "No build output yet - Ctrl+F9 builds, Shift+F10 builds and runs"
                                          : panelTab_ == 1 ? "Program output appears here" : "No problems";
        label(body.x + 14 * s, body.y + 0.5f * lh(), hint, theme::lineNum);
    }
    if (panelMaxScroll() > 0) {
        float thumbH = std::max(20 * s, body.h * body.h / (panelMaxScroll() + body.h));
        float thumbY = body.y + (body.h - thumbH) * (sy / panelMaxScroll());
        fill(body.right() - 9 * s, thumbY, 5 * s, thumbH, theme::scrollbar, 2.5f * s);
    }
    }   // (not the Git tab)

    // Header: darker band, text tabs with an accent underline (no pills, no borders).
    fill(p.x, p.y, p.w, hh, theme::panelHead);
    static constexpr std::array<std::string_view, 5> names = {"Build", "Run", "Problems", "Terminal", "Debug"};
    float x = p.x + 8 * s;
    for (std::size_t i = 0; i < 5; ++i) {
        std::string t = std::string(names[i]);
        if (i == 2 && !shownProblems_.empty()) t += std::format("  {}", shownProblems_.size());
        const float w = uiW(t) + 28 * s;
        panelTabRects_[i] = {x, p.y + 4 * s, w, hh - 8 * s};
        const bool active = static_cast<int>(i) == panelTab_;
        const bool hot = panelTabRects_[i].contains(mx_, my_);
        Color c = active ? theme::text : (hot ? Color::rgb(0xb4b8c1) : theme::dim);
        if (i == 2 && !shownProblems_.empty()) c = active ? Color::rgb(0xf2777a) : Color::rgb(0xc86a6c);
        uiLabel(x + 14 * s, p.y, hh, t, c);
        if (active) fill(x + 14 * s, p.y + hh - std::round(2 * s) - s, uiW(t), std::round(2 * s), i == 2 && !shownProblems_.empty() ? Color::rgb(0xf2777a) : theme::accent, std::round(s));
        x += w + 2 * s;
    }
    const std::string clear = "Clear";
    const float cwid = uiW(clear) + 24 * s, xs = std::round(hh * 0.72f);
    panelClose_ = {p.right() - 8 * s - xs, p.y + (hh - xs) / 2, xs, xs};
    panelClear_ = {panelClose_.x - 6 * s - cwid, p.y + 4 * s, cwid, hh - 8 * s};
    uiLabel(panelClear_.x + 12 * s, p.y, hh, clear, theme::dim);
    panelCopyAll_ = panelCopyErrors_ = {};
    if (panelTab_ == 2 && !shownProblems_.empty()) {   // copy everything listed (or just the errors) in one go
        const std::size_t errs = static_cast<std::size_t>(std::ranges::count_if(shownProblems_, [](const Problem& pr) { return pr.severity == Severity::Error; }));
        float bx = panelClear_.x - 4 * s;
        auto button = [&](Rect& r, const std::string& label) {
            const float w = uiW(label) + 24 * s;
            bx -= w;
            r = {bx, p.y + 4 * s, w, hh - 8 * s};
            if (r.contains(mx_, my_)) fill(r, theme::tabHover, 6 * s);
            uiLabel(r.x + 12 * s, p.y, hh, label, r.contains(mx_, my_) ? theme::text : theme::dim);
            bx -= 2 * s;
        };
        button(panelCopyAll_, "Copy all");
        if (errs > 0 && errs < shownProblems_.size()) button(panelCopyErrors_, "Copy errors");
    }
    if (runProc_ && panelTab_ == 1)
        uiLabelR(panelClear_.x - 14 * s, p.y, hh, runFocus_ ? "Typing goes to the program  \xC2\xB7  Esc to release" : "Click here to type input", runFocus_ ? Color::rgb(0xe6e8ec) : theme::lineNum);
    const int ci = std::max(8, static_cast<int>(std::round(uiH() * 0.7f)));
    Icons::tool(dl_, font_, std::round(panelClose_.x + (xs - static_cast<float>(ci)) / 2), std::round(panelClose_.y + (xs - static_cast<float>(ci)) / 2), ci,
                ToolIcon::Close, theme::dim);
}

}  // namespace ide
