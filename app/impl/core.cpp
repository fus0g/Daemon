// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / core - part of the App implementation (see app.cppm for the class).
module;
#include <time.h>
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.image;
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

std::expected<void, std::string> App::init() {
    fontSize_ = opt_.fontSize;
    if (auto r = font_.load(); !r) return r;

    win_.onResize = [this](int w, int h, int s) { onResize(w, h, s); };
    win_.onKey = [this](const KeyEvent& k) { onKey(k); };
    win_.onPointerMove = [this](float x, float y) { onPointerMove(x, y); };
    win_.onPointerButton = [this](Button b, bool p, float x, float y) { onPointerButton(b, p, x, y); };
    win_.onScroll = [this](float dx, float dy, int sx, int sy) { onScroll(dx, dy, sx, sy); };
    win_.watch(fsw_.fd(), [this] { if (fsw_.drain()) { fsPending_ = true; fsEventAt_ = Clock::now(); dirty_ = true; } });
    win_.onFocus = [this](bool f) { focused_ = f; resetCaret(); dirty_ = true; if (f) gitNextPoll_ = {}; };
    win_.onClose = [this] { requestQuit(); };
    if (auto r = win_.create("Daemon", 1280, 800); !r) return r;
    win_.setIcon(appIconPixels(256), 256);   // compositors that support xdg-toplevel-icon (KDE, ...)
    installDesktopEntry();                   // the rest (GNOME) match the app id to a .desktop file
    if (opt_.screenshot.empty()) win_.setMaximized();   // start maximized (captures keep a fixed size)

    scale_ = win_.scale();
    wLog_ = win_.width();
    hLog_ = win_.height();
    focused_ = win_.focused();
    applyFonts();
    if (auto r = ren_.init(win_.display(), win_.surface()); !r) return r;
    ren_.setExtent(static_cast<std::uint32_t>(wLog_ * scale_), static_cast<std::uint32_t>(hLog_ * scale_));
    ren_.setPictures(&pics_);
    setSvgTextFont(font_.uiFontPath());
    rendererReady_ = true;
    std::println("Vulkan device: {}   font: {}   scale: {}x", ren_.deviceName(), font_.fontPath(), scale_);

    state_ = loadState();
    toolchain::init();   // shipped / managed tools first on PATH
    // Sessions: not for --screenshot runs (deterministic, must not touch the user's files); with files named on the
    // command line those files win, a single folder argument restores that project's session.
    sessionOn_ = (opt_.screenshot.empty() || std::getenv("DAEMON_SESSION")) && !std::getenv("DAEMON_NO_SESSION");
    std::error_code sec;
    if (opt_.files.size() > 1 || (opt_.files.size() == 1 && !fs::is_directory(expandPath(opt_.files[0]), sec))) sessionOn_ = false;
    for (const auto& f : opt_.files) openFile(f);
    if (opt_.files.empty() && sessionOn_ && !state_.lastProject.empty() && fs::is_directory(state_.lastProject, sec))
        openFolder(state_.lastProject);   // plain launch: continue where the last session ended
    if (docs_.empty()) newDoc();
    if (opt_.files.empty() && opt_.screenshot.empty() && !explorer_.isOpen()) welcomeOpen();   // nothing to open: show the project chooser
    if (!explorer_.isOpen()) gitDetect();
    toolsCheck(opt_.screenshot.empty());   // start-up check; the dialog only opens if something is missing
    resetCaret();
    feedKeys(opt_.keys);
    return {};
}

void App::feedKeys(std::string_view spec) {
    script_ = std::string(spec);
    scriptPos_ = 0;
    stepScript();
}

void App::stepScript() {
    const std::string_view spec = script_;
    std::size_t& pos = scriptPos_;
    static const std::unordered_map<std::string_view, Key> names = {
        {"enter", Key::Enter}, {"tab", Key::Tab}, {"esc", Key::Escape}, {"backspace", Key::Backspace},
        {"delete", Key::Delete}, {"left", Key::Left}, {"right", Key::Right}, {"up", Key::Up}, {"down", Key::Down},
        {"home", Key::Home}, {"end", Key::End}, {"pgup", Key::PageUp}, {"pgdn", Key::PageDown}, {"f2", Key::F2}, {"f3", Key::F3}, {"f5", Key::F5}, {"f9", Key::F9}, {"f10", Key::F10}, {"f12", Key::F12}};
    while (pos < spec.size()) {
        auto bar = spec.find('|', pos);
        const std::size_t tokStart = pos;
        std::string_view tok = spec.substr(pos, bar == std::string_view::npos ? bar : bar - pos);
        const std::size_t next = bar == std::string_view::npos ? spec.size() : bar + 1;
        if (tok.starts_with("sleep:")) {   // test hook: pause the script (and the capture) so asynchronous work can finish
            const auto now = Clock::now();
            if (!scriptSleeping_) { scriptSleeping_ = true; scriptSleepUntil_ = now + std::chrono::milliseconds(std::atoi(std::string(tok.substr(6)).c_str())); }
            if (now < scriptSleepUntil_) { settleUntil_ = scriptSleepUntil_; return; }
            scriptSleeping_ = false;
            pos = next;
            continue;
        }
        if (tok == "waitrun") {   // test hook: until the program is running (or nothing is going to run)
            if (!runProc_ && (buildProc_ || pendingBuild_)) return;
            pos = next;
            continue;
        }
        if (tok == "setup") { setupOpen(); pos = next; continue; }   // test hook: open the toolchain dialog
        if (tok == "copyprob" || tok == "copyerr") {   // test hook: the Problems tab's copy buttons; also prints what was copied
            copyProblems(tok == "copyerr");
            std::print(std::cerr, "--- copied ---\n{}--- end ---\n", problemsText(tok == "copyerr"));
            pos = next;
            continue;
        }
        if (tok.starts_with("toolget:")) { toolsDownload(std::string(tok.substr(8))); pos = next; continue; }   // test hook: managed download
        if (tok == "wait") {
            if (!scriptReady()) return;  // retried from update()
            pos = next;
            continue;
        }
        pos = next;
        if (tok.starts_with("bp:") && !docs_.empty()) { dbgToggleBpAt(doc().path, std::atoi(std::string(tok.substr(3)).c_str())); pos = next; continue; }   // test hook: breakpoint on a line
        if (tok.starts_with("bpedit:") && !docs_.empty()) {   // test hook: bpedit:LINE:TEXT sets condition / skip / logpoint of the breakpoint on LINE
            const std::string rest(tok.substr(7));
            const auto c = rest.find(':');
            if (Breakpoint* b = dbgBpAt(doc().path, std::atoi(rest.substr(0, c).c_str()))) dbgApplyBpEdit(b->uid, rest.substr(c + 1));
            pos = next; continue;
        }
        if (tok.starts_with("dbgwatch:")) { dbgAddWatch(std::string(tok.substr(9))); pos = next; continue; }
        if (tok == "debugview") { activityClick(6); pos = next; continue; }   // test hook: open the Debug sidebar + console
        if (tok == "dbgwait") {   // test hook: until the debugger is paused (or ended)
            if (buildProc_ || pendingBuild_ || runAfterBuild_ || (dbg_ && dbgState_ != DbgState::Stopped)) { pos = tokStart; return; }
            pos = next; continue;
        }
        if (tok.starts_with("dbgframe:")) { dbgSelectFrame(std::atoi(std::string(tok.substr(9)).c_str())); pos = next; continue; }
        if (tok.starts_with("panel:")) { panelOpen_ = true; panelTab_ = std::atoi(std::string(tok.substr(6)).c_str()); pos = next; continue; }
        if (tok.starts_with("dbgeval:")) { panelOpen_ = true; panelTab_ = 4; dbgEval(std::string(tok.substr(8))); pos = next; continue; }
        if (tok.starts_with("dbgexpand:")) { dbgExpanded_.insert(std::string(tok.substr(10))); if (dbgState_ == DbgState::Stopped) dbgRefreshVars(); pos = next; continue; }
        if (tok.starts_with("replaceall:")) { sReplace_ = {std::string(tok.substr(11)), tok.size() - 11}; searchReplace(-1, -1); pos = next; continue; }   // test hook
        if (tok.starts_with("search:")) { openSearch(false); sQuery_ = {std::string(tok.substr(7)), tok.size() - 7}; startSearch(); pos = next; continue; }   // test hook: run a project search
        if (tok == "side") { focusSidebar(); pos = next; continue; }   // test hook: focus the explorer
        if (tok == "filedrag") {   // test hook: show a drag of the 1st row over the 3rd
            auto& rows = explorer_.rows();
            if (rows.size() > 3) {
                fdrag_ = {}; fdrag_.path = rows[0].node->path; fdrag_.active = true;
                mx_ = L_.side.x + L_.side.w * 0.4f; my_ = L_.sideList.y + L_.sideRow * 2.5f;
                fdrag_.target = fileDropTarget();
            }
            pos = next; continue;
        }
        if (tok == "menu") {  // test hook: context menu on the selected tree row (or the root)
            layout();
            int i = explorer_.indexOfSelected();
            std::string path = explorer_.selected();
            float y = L_.sideList.y + (i < 0 ? 0 : static_cast<float>(i)) * L_.sideRow + L_.sideRow;
            if (path.empty() || i < 0) openMenu(L_.side.x + 40 * S(), y, nullptr);
            else openMenu(L_.side.x + 80 * S(), y, &path);
            continue;
        }
        if (tok.starts_with("dragdemo:") && !docs_.empty()) {  // test hook: show the drop overlay for the current tab
            layout();
            const auto at = tok.substr(9);
            const Rect& isl = L_.island;
            tabDrag_ = {};
            tabDrag_.doc = &doc();
            tabDrag_.from = paneOfDoc(&doc());
            tabDrag_.active = true;
            tabDrag_.grabX = 60 * S();
            mx_ = isl.x + isl.w * (at == "left" ? 0.1f : at == "right" ? 0.97f : 0.75f);
            my_ = isl.y + isl.h * (at == "up" ? 0.15f : at == "down" ? 0.9f : at == "tabs" ? 0.0f : 0.5f) + (at == "tabs" ? 20 * S() : 0.0f);
            updateDropTarget();
            continue;
        }
        if (tok.starts_with("tooltip:")) {   // test hook: show a toolbar tooltip (3 run, 8 debug, 4 build, 6 regenerate, 5 stop)
            hoverTool_ = std::atoi(std::string(tok.substr(8)).c_str());
            toolTipShown_ = true;
            continue;
        }
        if (tok.starts_with("gitsub:")) {   // test hook: open the Git tab on a sub-tab (0 changes, 1 history, 2 branches)
            sidebar_ = true; sideGit_ = true; sideSearch_ = false; sideDebug_ = false;
            gitSub_ = std::atoi(std::string(tok.substr(7)).c_str());
            gitScroll_ = 0;
            continue;
        }
        if (tok.starts_with("gitexpand:")) {   // test hook: expand the n-th commit
            const auto n = static_cast<std::size_t>(std::atoi(std::string(tok.substr(10)).c_str()));
            if (n < gitLog_.size()) gitToggleCommit(gitLog_[n].hash);
            continue;
        }
        if (tok == "blamehover" && blameRect_.w > 0) {   // test hook: hover the blame annotation
            mx_ = blameRect_.x + blameRect_.w / 2;
            my_ = blameRect_.y + blameRect_.h / 2;
            blameHover_ = true;
            showBlameCard(blameHash_, blameUncommitted_);
            continue;
        }
        if (tok == "panelsel") {   // test hook: select rows 1..2 of the output panel and copy
            psel_ = {}; psel_.active = psel_.moved = true; psel_.tab = panelTab_;
            psel_.ar = 1; psel_.ac = 2; psel_.cr = 2; psel_.cc = 12;
            copyOverlay();
            continue;
        }
        if (tok == "hoverfake") {   // test hook: show a sample error tooltip near the cursor
            layout();
            hover_ = {};
            hover_.show = true;
            hover_.px = L_.textX + 120 * S();
            hover_.py = L_.editor.y + 60 * S();
            hover_.lines = {{"Use of undeclared identifier 'undefined_fn'", 1}, {"in function 'main' at a.cpp:2:5", 0}};
            continue;
        }
        if (tok == "hoversel" && hover_.show) {   // test hook: select the tooltip's text
            hsel_ = {}; hsel_.active = hsel_.moved = true;
            hsel_.ar = 0; hsel_.ac = 0; hsel_.cr = static_cast<int>(hover_.lines.size()) - 1; hsel_.cc = 1000;
            continue;
        }
        if (tok.starts_with("dlg:")) {   // test hook: open a project dialog
            const auto which = tok.substr(4);
            if (which == "new") newProjectOpen();
            else if (which == "browse") browseOpen(false);
            else if (which == "browseloc") { newProjectOpen(); browseOpen(true); }
            else welcomeOpen();
            continue;
        }
        if (tok.starts_with("split:") && !docs_.empty()) {  // test hook: move the current tab into a new pane
            const auto side = tok.substr(6);
            syncPanes();
            if (Pane* ap = activePane_; ap && ap->tabs.size() > 1) {
                splitPane(ap, side == "left" ? Zone::Left : side == "up" ? Zone::Top : side == "down" ? Zone::Bottom : Zone::Right, &doc());
                layout();
            }
            continue;
        }
        if (tok == "hover" && !docs_.empty()) {  // test hook: hover the text cursor
            layout();
            Document& hd = doc();
            mx_ = L_.textX - std::round(hd.scrollX) + (static_cast<float>(displayCol(hd.buf.line(hd.cursor.line), hd.cursor.col, hd.tabWidth)) + 0.5f) * cw();
            my_ = L_.editor.y + (static_cast<float>(hd.cursor.line) + 0.5f) * lh() - std::round(hd.scrollY);
            hover_ = {};
            hover_.pos = hd.cursor;
            hover_.px = mx_;
            hover_.py = my_;
            hover_.pending = true;
            hover_.due = Clock::now() + std::chrono::milliseconds(400);  // let diagnostics arrive first
            continue;
        }
        if (tok.starts_with("t:")) {
            std::string_view text = tok.substr(2);
            for (std::size_t i = 0; i < text.size();) {
                std::size_t start = i;
                char32_t cp = decodeUtf8(text, i);
                KeyEvent k;
                k.key = Key::Char;
                k.ch = cp < 0x80 ? static_cast<char32_t>(std::tolower(static_cast<int>(cp))) : cp;
                k.text = std::string(text.substr(start, i - start));
                onKey(k);
            }
            continue;
        }
        KeyEvent k;
        while (true) {
            if (tok.starts_with("ctrl+")) { k.ctrl = true; tok.remove_prefix(5); }
            else if (tok.starts_with("shift+")) { k.shift = true; tok.remove_prefix(6); }
            else if (tok.starts_with("alt+")) { k.alt = true; tok.remove_prefix(4); }
            else break;
        }
        if (auto it = names.find(tok); it != names.end()) k.key = it->second;
        else if (!tok.empty()) { k.key = Key::Char; k.ch = static_cast<unsigned char>(tok[0]); }
        else continue;
        onKey(k);
    }
}

// IDE_BENCH=N: draw + submit N frames back to back and print the average CPU cost of each phase.
static void benchFrames(App& app, int n, const std::function<void()>& draw, const std::function<RenderStatus()>& render) {
    using Ms = std::chrono::duration<double, std::milli>;
    double d = 0, r = 0;
    for (int i = 0; i < n; ++i) {
        auto a = Clock::now();
        draw();
        auto b = Clock::now();
        render();
        auto c = Clock::now();
        d += Ms(b - a).count();
        r += Ms(c - b).count();
    }
    std::println(std::cerr, "bench: {} frames, draw {:.3f} ms, render+present {:.3f} ms", n, d / n, r / n);
    (void)app;
}

int App::run() {
    if (const char* b = std::getenv("IDE_BENCH")) {
        for (int i = 0; i < 20; ++i) { win_.pump(5); update(Clock::now(), 0.016f); dirty_ = true; draw(); ren_.render(dl_, font_, frameC()); }
        benchFrames(*this, std::atoi(b), [&] { dirty_ = true; draw(); },
                    [&] { return ren_.render(dl_, font_, frameC()); });
        return 0;
    }
    auto last = Clock::now();
    int failures = 0;
    // CLOCK_MONOTONIC stops while the machine sleeps, CLOCK_BOOTTIME does not: their difference growing means a suspend.
    auto sleptNs = [] {
        timespec a{}, b{};
        ::clock_gettime(CLOCK_MONOTONIC, &a);
        ::clock_gettime(CLOCK_BOOTTIME, &b);
        return (b.tv_sec - a.tv_sec) * 1'000'000'000LL + (b.tv_nsec - a.tv_nsec);
    };
    long long sleptBefore = sleptNs();
    auto lastPresent = Clock::now(), lastStallLog = Clock::now();
    // Diagnostics thread: records where the main loop is, so a hang can be located from daemon.log.
    static constexpr const char* kPhase[] = {"idle", "pump", "update", "draw", "render"};
    std::atomic<int> phase{0};
    std::atomic<long long> loops{0}, presented{0}, skippedN{0}, failedN{0}, resumeAtMs{0};
    std::atomic<bool> dirtyFlag{false}, pendingFlag{false};
    std::jthread watchdog([&](std::stop_token st) {
        long long lastLoops = -1, lastPresented = -1;
        int stuck = 0, tick = 0;
        while (!st.stop_requested()) {
            for (int i = 0; i < 20 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++tick;
            long long l = loops.load(), p = presented.load();
            const bool afterResume = resumeAtMs.load() != 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count() - resumeAtMs.load() < 180'000;
            if (l == lastLoops && phase.load() != 1) {
                if (++stuck == 3) logLine(std::format("main loop stuck in phase '{}' for 6 s", kPhase[phase.load()]));
            } else stuck = 0;
            if (afterResume && tick % 5 == 0)
                logLine(std::format("state: loops+{} presented+{} total={} skipped={} failed={} phase={} dirty={} framePending={}",
                                    l - std::max(lastLoops, 0LL), p - std::max(lastPresented, 0LL), p, skippedN.load(), failedN.load(),
                                    kPhase[phase.load()], dirtyFlag.load(), pendingFlag.load()));
            lastLoops = l; lastPresented = p;
        }
    });
    logLine(std::format("daemon {} started on {}", versionFull(), ren_.deviceName()));
    while (running_) {
        auto now = Clock::now();
        if (long long s = sleptNs(); s - sleptBefore > 1'000'000'000LL) {
            logLine(std::format("resumed from suspend after ~{} s: resetting window and renderer", (s - sleptBefore) / 1'000'000'000LL));
            sleptBefore = s;
            win_.cancelFrame();
            ren_.rebuild();
            skipped_ = false; failures = 0; lastPresent = now;
            dirty_ = true;
            resumeAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        }
        ++loops; dirtyFlag = dirty_; pendingFlag = win_.framePending();
        if (dirty_ && now - lastPresent > std::chrono::seconds(3) && now - lastStallLog > std::chrono::seconds(5)) {
            lastStallLog = now;
            logLine(std::format("no frame presented for {} s (configured={} framePending={} waitMs={} skipped={} failures={} focused={})",
                                std::chrono::duration_cast<std::chrono::seconds>(now - lastPresent).count(), win_.configured(),
                                win_.framePending(), win_.frameWaitMs(), skipped_, failures, win_.focused()));
        }
        int timeout = -1;
        if (dirty_ || animating_) {
            timeout = (skipped_ || failures > 0) ? (failures > 0 ? (failures > 20 ? 1000 : 250) : 16) : (win_.framePending() ? 16 : 0);
        } else {
            auto wake = nextWake();
            if (wake) timeout = static_cast<int>(std::max<long long>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(*wake - now).count() + 1));
        }
        phase = 1;
        const bool pumped = win_.pump(timeout);
        phase = 2;
        if (!pumped) break;
        now = Clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        update(now, std::min(dt, 0.1f));

        // Frame pacing: if the compositor has not answered the previous frame callback the window is probably
        // hidden or occluded. Presenting then can stall the driver, so wait (still serving events), and only
        // force a frame occasionally.
        const bool throttled = win_.framePending() && opt_.screenshot.empty() && win_.frameWaitMs() < 250;
        if (dirty_ && win_.configured() && !throttled) {
            phase = 3;
            draw();
            if (opt_.screenshot.empty()) win_.requestFrame();
            phase = 4;
            switch (ren_.render(dl_, font_, frameC())) {
                case RenderStatus::Presented: dirty_ = false; skipped_ = false; failures = 0; ++frames_; lastPresent = now; ++presented; break;
                case RenderStatus::Skipped: skipped_ = true; ++skippedN; win_.cancelFrame(); break;
                case RenderStatus::Failed:
                    ++failedN;
                    win_.cancelFrame();
                    // Never exit on render failures: the GPU may take a while to come back after suspend and the
                    // user may have unsaved work. Retry with a growing delay; the renderer rebuilds itself.
                    if (failures < 1000) ++failures;
                    break;
            }
            if (!opt_.screenshot.empty()) screenshotStep();
        }
        phase = 0;
    }
    saveSession(true);
    return 0;
}

void App::setMessage(std::string m, bool error, float seconds) {
    message_ = std::move(m);
    messageError_ = error;
    messageUntil_ = Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
    dirty_ = true;
}

void App::resetCaret() {
    caretOn_ = true;
    caretReset_ = Clock::now();
    nextBlink_ = Clock::now() + std::chrono::milliseconds(530);
    dirty_ = true;
}

// The caret stops blinking after 30 s without input: an idle editor then sleeps completely (no wakeups, no frames).
bool App::blinking(Clock::time_point now) const { return now - caretReset_ < std::chrono::seconds(30); }

std::optional<Clock::time_point> App::nextWake() const {
    std::optional<Clock::time_point> t;
    auto consider = [&](Clock::time_point p) { if (!t || p < *t) t = p; };
    if (focused_ && opt_.screenshot.empty() && blinking(Clock::now())) consider(nextBlink_);
    if (!message_.empty()) consider(messageUntil_);
    if (buildEof_ || runEof_) consider(Clock::now() + std::chrono::milliseconds(30));
    if (lspFlushAt_) consider(*lspFlushAt_);
    if (!bgQueue_.empty() && lsp_) consider(bgNext_);
    if (!jobs_.empty()) consider(Clock::now() + std::chrono::milliseconds(40));
    if (toolDl_) consider(Clock::now() + std::chrono::milliseconds(100));   // progress bar
    if (scriptSleeping_) consider(scriptSleepUntil_);
    if (hoverTool_ >= 3 && !toolTipShown_) consider(toolHoverAt_ + std::chrono::milliseconds(390));
    if (gitRoot_ && focused_) consider(gitNextPoll_);
    if (fsPending_) consider(fsEventAt_ + std::chrono::milliseconds(150));
    if (sPending_) consider(sRunAt_);
    if (nextBlameWake_ > Clock::now()) consider(nextBlameWake_);
    if (lsp_ && cmakeOpen_) consider(nextDbCheck_);
    if (lsp_ && healTries_ < 3 && nextHeal_ != Clock::time_point{}) consider(nextHeal_);
    if (hover_.pending) consider(hover_.due);
    return t;
}

void App::update(Clock::time_point now, float dt) {
    if (toolDl_) dirty_ = true;   // download progress is polled while drawing
    sessionTick(now);
    if (fsPending_ && now >= fsEventAt_ + std::chrono::milliseconds(150)) { fsPending_ = false; handleFsChanges(); }
    if (sPending_ && now >= sRunAt_) startSearch();
    if (now >= fsSyncAt_) { fsSyncAt_ = now + std::chrono::seconds(1); syncFsWatches(); }
    if (focused_ && opt_.screenshot.empty() && !blinking(now) && !caretOn_) { caretOn_ = true; dirty_ = true; }   // settle solid
    if (focused_ && opt_.screenshot.empty() && blinking(now) && now >= nextBlink_) {
        caretOn_ = !caretOn_;
        nextBlink_ = now + std::chrono::milliseconds(530);
        dirty_ = true;
    }
    if (!message_.empty() && now >= messageUntil_) { message_.clear(); dirty_ = true; }
    if (lsp_ && cmakeOpen_ && now >= nextDbCheck_) {
        // The compile database changes when CMake reconfigures (from here, CLion, or the command line):
        // restart clangd so it picks up the new flags / files.
        nextDbCheck_ = now + std::chrono::seconds(2);
        std::error_code ec;
        const auto stamp = fs::last_write_time(compileCommands(profile()), ec);
        if (!ec && stamp != dbStamp_ && !buildProc_) { restartLsp(); }
    }

    layout();
    if (!docs_.empty()) {
        Document& d = doc();
        if (isPreview(d)) refreshPreview(d);
        if (scrollToCursor_) { ensureCursorVisible(d); scrollToCursor_ = false; }
        if (dragging_ || scrollbarDrag_) dragStep(d);
        clampScroll(d);
        animating_ = false;
        float diff = d.targetY - d.scrollY;
        if (!opt_.screenshot.empty()) d.scrollY = d.targetY;  // deterministic captures
        else if (std::abs(diff) > 0.5f) {
            d.scrollY += diff * (1.0f - std::exp(-dt * 22.0f));
            animating_ = true;
            dirty_ = true;
        } else if (diff != 0) {
            d.scrollY = d.targetY;
            dirty_ = true;
        }
        if (stepCaret(d, dt)) { animating_ = true; dirty_ = true; }
    }
    if (sidebar_ && explorer_.isOpen() && !sideGit_ && !sideSearch_ && !sideDebug_) {
        if (sideReveal_) { revealInSidebar(); sideReveal_ = false; }
        clampSide();
        float diff = sideTarget_ - sideScroll_;
        if (!opt_.screenshot.empty() || std::abs(diff) <= 0.5f) {
            if (diff != 0) dirty_ = true;
            sideScroll_ = sideTarget_;
        } else {
            sideScroll_ += diff * (1.0f - std::exp(-dt * 22.0f));
            animating_ = true;
            dirty_ = true;
        }
    }
    refreshProblems();
    if (docs_.empty()) animating_ = false;
    if (stepAnimations(dt)) { animating_ = true; dirty_ = true; }
    pollTasks();
    if (scriptPos_ < script_.size()) stepScript();
    lspSync(now);
    if (lspCrashed_) { lspCrashed_ = false; handleLspCrash(now); }
    if (hoverTool_ >= 3 && !toolTipShown_ && now - toolHoverAt_ > std::chrono::milliseconds(380)) { toolTipShown_ = true; dirty_ = true; }
    if (lsp_ && lsp_->ready() && !bgScanned_) { bgScanned_ = true; queueProjectDiagnostics(); }
    pumpProjectDiagnostics(now);
    healDiagnostics(now);
    gitTick(now);
    hoverTick(now);
    if (panelOpen_) {
        auto t = static_cast<std::size_t>(panelTab_);
        if (panelFollow_[t]) panelScroll_[t] = panelMaxScroll();
    }
    updateTitle();
}

bool App::chaseEdges(float& lo, float& hi, float tlo, float thi, float dt, float snapDist) {
    const float travel = (tlo + thi) - (lo + hi);
    if (std::abs(travel) * 0.5f > snapDist) { lo = tlo; hi = thi; return false; }
    constexpr float rate = 32.0f;  // 1/seconds: ~95% of the way in about 90 ms
    const float k = 1.0f - std::exp(-dt * rate);
    lo += (tlo - lo) * k;
    hi += (thi - hi) * k;
    if (std::abs(tlo - lo) < 0.2f && std::abs(thi - hi) < 0.2f) { lo = tlo; hi = thi; return false; }
    return true;
}

bool App::stepCaret(const Document& d, float dt) {
    const float cell = cw(), line = lh(), s = S();
    const float w = std::max(2.0f, std::round(1.6f * s));
    const float tx = static_cast<float>(displayCol(d.buf.line(d.cursor.line), d.cursor.col, d.tabWidth)) * cell;
    const float ty = static_cast<float>(d.cursor.line) * line + 1;
    const float tyb = ty + line - 2;
    const bool reset = !caret_.init || caret_.doc != &d || caret_.cell != cell || caret_.line != line ||
                       !opt_.screenshot.empty();
    if (reset) {
        caret_ = {tx, tx + w, ty, tyb, true, &d, cell, line};
        return false;
    }
    bool mx = chaseEdges(caret_.xl, caret_.xr, tx, tx + w, dt, std::max(400.0f, L_.textW * 1.5f));
    bool my = chaseEdges(caret_.yt, caret_.yb, ty, tyb, dt, std::max(200.0f, L_.editor.h * 1.2f));
    return mx || my;
}

void App::clampSide() {
    float contentH = static_cast<float>(explorer_.rows().size()) * L_.sideRow;
    float maxS = std::max(0.0f, contentH - L_.sideList.h * 0.5f);
    sideTarget_ = std::clamp(sideTarget_, 0.0f, maxS);
    sideScroll_ = std::clamp(sideScroll_, 0.0f, maxS);
}

void App::revealInSidebar() {
    int i = explorer_.indexOfSelected();
    if (i < 0) return;
    float top = static_cast<float>(i) * L_.sideRow, bottom = top + L_.sideRow;
    if (top < sideTarget_) sideTarget_ = top;
    else if (bottom > sideTarget_ + L_.sideList.h) sideTarget_ = bottom - L_.sideList.h;
}

void App::updateTitle() {
    if (docs_.empty()) return;
    std::string t = doc().name + (doc().modified() ? " *" : "") + " - Daemon";
    if (t != lastTitle_) { lastTitle_ = t; win_.setTitle(t); }
}

void App::screenshotStep() {
    const bool busy = tasksRunning() || pendingBuild_ || hover_.pending || !jobs_.empty() || Clock::now() < settleUntil_;
    if (frames_ >= opt_.screenshotFrames && !shotRequested_ && ((!busy && scriptPos_ >= script_.size()) || frames_ > 2400)) {
        shotRequested_ = true;
        ren_.requestScreenshot(opt_.screenshot);
        dirty_ = true;
    } else if (shotRequested_ && ren_.screenshotDone()) {
        running_ = false;
    } else {
        dirty_ = true;  // keep presenting until the capture has been taken
    }
}

void App::onResize(int w, int h, int s) {
    wLog_ = w;
    hLog_ = h;
    if (s != scale_) {
        float ratio = static_cast<float>(s) / static_cast<float>(scale_);
        scale_ = s;
        rescaleViews(ratio);
        applyFonts();
    }
    if (rendererReady_)
        ren_.setExtent(static_cast<std::uint32_t>(wLog_ * scale_), static_cast<std::uint32_t>(hLog_ * scale_));
    dirty_ = true;
}

void App::zoom(float delta, bool reset) {
    float next = reset ? opt_.fontSize : std::clamp(fontSize_ + delta, 8.0f, 48.0f);
    if (next == fontSize_) return;
    rescaleViews(next / fontSize_);
    fontSize_ = next;
    applyFonts();
    scrollToCursor_ = true;
    dirty_ = true;
    setMessage(std::format("Font size {:.0f}", fontSize_), false, 1.5f);
}

}  // namespace ide
