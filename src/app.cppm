// ide.app — the application: tabs, editor view, prompts, input routing and the frame loop.
export module ide.app;
import std;
import ide.util;
import ide.input;
import ide.text;
import ide.syntax;
import ide.font;
import ide.draw;
import ide.vulkan;
import ide.wayland;
import ide.editor;
import ide.explorer;
import ide.icons;
import ide.json;
import ide.process;
import ide.cmake;
import ide.project;
import ide.git;
import ide.search;
import ide.gdbmi;
import ide.terminal;
import ide.lsp;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

export namespace ide {

struct Options {
    std::vector<std::string> files;
    std::string screenshot;   // write a PNG after a few frames, then exit
    std::string keys;         // scripted input, e.g. "ctrl+f|t:foo|enter" (for tests and screenshots)
    int screenshotFrames = 4;
    float fontSize = 14;      // code font, logical pixels (em size)
};

// "Island" theme: a dark frame with floating rounded panels. Cool neutrals and a blue accent
// (Visual Studio) with warm keywords and soft pastel syntax colours (JetBrains).
namespace theme {
inline constexpr Color frame = Color::rgb(0x111218, 214);    // translucent chrome between islands (blurred by the compositor)
inline constexpr Color editorBg = Color::rgb(0x0a0b0d);     // code editor: near-black
inline constexpr Color bg = Color::rgb(0x1a1b1f);           // island (side panels) surface
inline constexpr Color lineHl = Color::rgb(0x15171b);
inline constexpr Color selection = Color::rgb(0x4d171c);
inline constexpr Color selectionIdle = Color::rgb(0x34363d);
inline constexpr Color text = Color::rgb(0xd1d4da);
inline constexpr Color lineNum = Color::rgb(0x565962);
inline constexpr Color lineNumActive = Color::rgb(0xaeb2bb);
inline constexpr Color guide = Color::rgb(0x1f2126);
inline constexpr Color caret = Color::rgb(0xe0454d);
inline constexpr Color titleBg = frame;
inline constexpr Color tabBg = Color::rgb(0x121316);
inline constexpr Color stripBg = Color::rgb(0x060709);      // editor tab strip: darker than the editor
inline constexpr Color sideBg = Color::rgb(0x0d0e11);       // explorer island
inline constexpr Color sideSel = Color::rgb(0x1a1c21);
inline constexpr Color sideHover = Color::rgb(0x121317);
inline constexpr Color panelBg = Color::rgb(0x0b0c0f);      // output panel island
inline constexpr Color panelHead = Color::rgb(0x07080a);        // tab strip inside the island
inline constexpr Color tabHover = Color::rgb(0x27292e);
inline constexpr Color tabActive = Color::rgb(0x2f3138);
inline constexpr Color statusBg = frame;
inline constexpr Color promptBg = Color::rgb(0x26282d);
inline constexpr Color border = Color::rgb(0x2e3036);
inline constexpr Color accent = Color::rgb(0xb01e23);   // Targaryen / Caraxes crimson
inline constexpr Color dim = Color::rgb(0x8a8e98);
inline constexpr Color match = Color::rgb(0x5a4a22);
inline constexpr Color bracket = Color::rgb(0x7d8494);
inline constexpr Color danger = Color::rgb(0xe5484d);
inline constexpr Color scrollbar = Color::rgb(0x7a8090, 110);

inline constexpr Color syntax(Tok t) {
    switch (t) {
        case Tok::Keyword: return Color::rgb(0xcf8e6d);   // warm orange
        case Tok::Type: return Color::rgb(0x4ec9b0);      // teal
        case Tok::String: return Color::rgb(0x6aab73);    // soft green
        case Tok::Number: return Color::rgb(0xb5cea8);    // pale green
        case Tok::Comment: return Color::rgb(0x6c7280);
        case Tok::Preproc: return Color::rgb(0xc586c0);   // magenta
        case Tok::Function: return Color::rgb(0x56a8f5);  // blue
        case Tok::Operator: return text;
        case Tok::Constant: return Color::rgb(0xc77dbb);  // violet
        case Tok::Key: return Color::rgb(0x7fb7ff);
        default: return text;
    }
}
}  // namespace theme

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

class App {
public:
    explicit App(Options opts) : opt_(std::move(opts)) {}
    ~App() { if (dbg_) dbg_->kill(); lsp_.reset(); }  // while the members its callbacks touch are still alive

    std::expected<void, std::string> init() {
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
        if (opt_.screenshot.empty()) win_.setMaximized();   // start maximized (captures keep a fixed size)

        scale_ = win_.scale();
        wLog_ = win_.width();
        hLog_ = win_.height();
        focused_ = win_.focused();
        applyFonts();
        if (auto r = ren_.init(win_.display(), win_.surface()); !r) return r;
        ren_.setExtent(static_cast<std::uint32_t>(wLog_ * scale_), static_cast<std::uint32_t>(hLog_ * scale_));
        rendererReady_ = true;
        std::println("Vulkan device: {}   font: {}   scale: {}x", ren_.deviceName(), font_.fontPath(), scale_);

        state_ = loadState();
        for (const auto& f : opt_.files) openFile(f);
        if (docs_.empty()) newDoc();
        if (opt_.files.empty() && opt_.screenshot.empty()) welcomeOpen();   // nothing to open: show the project chooser
        if (!explorer_.isOpen()) gitDetect();
        resetCaret();
        feedKeys(opt_.keys);
        return {};
    }

    // Replays a '|'-separated list of key presses: "ctrl+shift+f", "enter", "left", or "t:literal text".
    void feedKeys(std::string_view spec) {
        script_ = std::string(spec);
        scriptPos_ = 0;
        stepScript();
    }

    // "wait" pauses the script until builds are done and the language server (if any) is ready.
    bool scriptReady() const {
        if (tasksRunning() || pendingBuild_) return false;
        return !lsp_ || lsp_->state() != LspClient::State::Starting;
    }

    void stepScript() {
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

    int run() {
        auto last = Clock::now();
        int failures = 0;
        while (running_) {
            auto now = Clock::now();
            int timeout = -1;
            if (dirty_ || animating_) {
                timeout = skipped_ ? 16 : 0;
            } else {
                auto wake = nextWake();
                if (wake) timeout = static_cast<int>(std::max<long long>(
                    0, std::chrono::duration_cast<std::chrono::milliseconds>(*wake - now).count() + 1));
            }
            if (!win_.pump(timeout)) break;
            now = Clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            update(now, std::min(dt, 0.1f));

            if (dirty_ && win_.configured()) {
                draw();
                switch (ren_.render(dl_, font_, frameC())) {
                    case RenderStatus::Presented: dirty_ = false; skipped_ = false; failures = 0; ++frames_; break;
                    case RenderStatus::Skipped: skipped_ = true; break;
                    case RenderStatus::Failed:
                        if (++failures >= 3) { std::println(std::cerr, "fatal: rendering keeps failing"); return 1; }
                        break;
                }
                if (!opt_.screenshot.empty()) screenshotStep();
            }
        }
        return 0;
    }

private:
    // ============================================================ state

    enum class PromptKind { None, Find, Replace, Goto, Open, OpenFolder, SaveAs, NewFile, NewFolder, Rename, Delete, RunArgs, DbgWatch, DbgSetValue, DbgBpEdit, DbgFuncBp };
    struct Field {
        std::string label, text;
        std::size_t cur = 0;
        bool fresh = false;  // prefilled: the first edit replaces it
    };
    struct Prompt {
        PromptKind kind = PromptKind::None;
        std::vector<Field> fields;
        int active = 0;
    };
    struct Layout {
        Rect title, island, tabs, editor, prompt, status;
        Rect side, split, sideList;   // sidebar island, draggable gap, scrolling file list
        float sideRow = 0;
        Rect toolbar, panel;          // CMake toolbar row and the build/run output panel
        Rect activity, crumbs, minimap;  // right-edge activity bar, breadcrumb row, code minimap
        float radius = 0;
        float gutterW = 0, textX = 0, textW = 0;
        bool csd = false;
        float rowH = 0;
    };
    struct TabHit {
        Rect tab, close;
    };
    struct Pane;
    struct Node;
    enum class Zone;
    enum class Act { Open, NewFile, NewFolder, Rename, Trash, DeleteForever, Copy, Cut, Paste, Duplicate, CopyPath,
                     CopyRelPath, Refresh, Sep, Custom };
    struct MenuItem {
        std::string label, hint;
        Act act;
        bool enabled = true;
        int arg = -1;   // Act::Custom: index into menuActions_
    };
    struct Menu {
        bool open = false;
        Rect rect;
        std::vector<MenuItem> items;
        int hover = -1;
        std::string target;   // path the menu was opened on ("" = empty area / root)
        float itemH = 0, sepH = 0;
    };
    struct OutLine {
        std::string text;
        int sev = 0;       // 0 normal, 1 warning, 2 error, 3 note, 4 dim, 5 success
        int problem = -1;  // index into problems_ when the line has a source location
    };
    struct ToolRects {
        Rect profile, target, run, build, stop, reload, panel, debug, cluster;
    };
    enum class TaskKind { None, Configure, Build };
    struct Completion {
        bool open = false;
        std::vector<LspCompletion> all;
        std::vector<int> shown;   // indices into all, filtered and ranked
        int sel = 0, scroll = 0;
        Pos anchor;               // start of the identifier being completed
        bool dotted = false;      // module names: "." and ":" belong to the word
    };
    struct Hover {
        bool pending = false, show = false;
        Pos pos;
        Clock::time_point due;
        float px = 0, py = 0;
        std::vector<std::pair<std::string, int>> lines;   // text, severity (0 = plain)
    };
    struct NavEntry {
        std::string path;
        Pos pos;
    };
    struct FileClip {
        std::vector<fs::path> paths;
        bool cut = false;
    };

    Document& doc() { return *docs_[static_cast<std::size_t>(cur_)]; }
    float lh() const { return font_.lineHeight(); }
    float cw() const { return font_.cellW(); }
    float S() const { return static_cast<float>(scale_); }

    // ============================================================ documents & files

    void newDoc() {
        docs_.push_back(std::make_unique<Document>());
        cur_ = static_cast<int>(docs_.size()) - 1;
        dirty_ = true;
    }

    static std::string expandPath(std::string p) {
        if (p.starts_with("~/") || p == "~") {
            if (const char* home = std::getenv("HOME")) p = std::string(home) + p.substr(1);
        }
        return p;
    }

    void openFile(const std::string& raw) {
        std::string path = expandPath(raw);
        std::error_code ec;
        if (fs::is_directory(path, ec)) { openFolder(path); return; }
        std::string abs = fs::absolute(path, ec).lexically_normal().string();
        for (std::size_t i = 0; i < docs_.size(); ++i)
            if (docs_[i]->path == abs) { switchTab(static_cast<int>(i)); return; }

        auto d = std::make_unique<Document>();
        if (fs::exists(path, ec)) {
            if (auto r = d->load(path); !r) { setMessage(r.error(), true); return; }
        } else {
            d->setPath(path);
            setMessage("New file: " + d->name);
        }
        // Replace a pristine untitled tab instead of stacking up empty ones.
        if (docs_.size() == 1 && docs_[0]->path.empty() && !docs_[0]->modified() && docs_[0]->buf.lineCount() == 1 &&
            docs_[0]->buf.line(0).empty())
            docs_.clear();
        docs_.push_back(std::move(d));
        cur_ = static_cast<int>(docs_.size()) - 1;
        revealCurrent();
        ensureLsp(*docs_.back());
        dirty_ = true;
    }

    // ---- folder / sidebar

    // ---- changes made outside the IDE (inotify): the explorer, open documents and git state follow along

    DirWatcher fsw_;
    bool fsPending_ = false;
    Clock::time_point fsEventAt_{}, fsSyncAt_{};

    void syncFsWatches() {
        std::set<std::string> dirs;
        if (explorer_.isOpen()) for (const auto& d : explorer_.loadedDirs()) dirs.insert(d.string());
        for (const auto& d : docs_) if (!d->path.empty()) dirs.insert(fs::path(d->path).parent_path().string());
        fsw_.sync(dirs);
    }

    void handleFsChanges() {
        if (explorer_.isOpen()) explorer_.refresh();
        checkDocsOnDisk();
        gitNextPoll_ = {};
        if (sideSearch_ && !sQuery_.text.empty()) scheduleSearch();   // results may be stale
        syncFsWatches();
        dirty_ = true;
    }

    void checkDocsOnDisk() {
        for (auto& dp : docs_) {
            Document& d = *dp;
            if (d.path.empty()) continue;
            std::error_code ec;
            if (!fs::exists(d.path, ec)) {
                if (!d.diskGone) { d.diskGone = true; setMessage(d.name + " was deleted on disk", true, 4); }
                continue;
            }
            d.diskGone = false;
            const auto t = fs::last_write_time(d.path, ec);
            if (ec || t == d.diskTime) continue;
            if (d.modified()) {   // keep the user's edits; just tell them once per change
                d.diskTime = t;
                setMessage(d.name + " changed on disk - you have unsaved edits", true, 5);
                continue;
            }
            const Pos c = d.cursor, a = d.anchor;
            const float sy = d.scrollY, ty = d.targetY;
            if (auto r = d.load(d.path); !r) { d.diskTime = t; continue; }
            d.cursor = d.buf.clamp(c);
            d.anchor = d.buf.clamp(a);
            d.scrollY = sy; d.targetY = ty;
            setMessage("Reloaded " + d.name + " (changed on disk)", false, 2.5f);
        }
    }

    void openFolder(const std::string& raw) {
        std::string err;
        if (!explorer_.open(expandPath(raw), err)) { setMessage(err, true); return; }
        sidebar_ = true;
        sideScroll_ = sideTarget_ = 0;
        sideFocus_ = false;
        revealCurrent();
        detectCMake();
        setMessage("Opened folder " + explorer_.rootPath().string());
        addRecent(state_, explorer_.rootPath().string());
        saveState(state_);
        gitReset();
        dbgLoad();
        dirty_ = true;
    }

    // Highlights the active document in the tree (expanding its parents).
    void revealCurrent() {
        if (!sidebar_ || !explorer_.isOpen() || docs_.empty() || doc().path.empty()) return;
        if (explorer_.reveal(doc().path)) sideReveal_ = true;
    }

    void toggleSidebar() {
        sidebar_ = !sidebar_;
        if (!sidebar_) sideFocus_ = false;
        else if (!explorer_.isOpen()) setMessage("No folder open - press Ctrl+Shift+O to open one", false, 3);
        else revealCurrent();
        dirty_ = true;
    }

    void focusSidebar() {
        if (!sidebar_) { sidebar_ = true; }
        if (!explorer_.isOpen()) { browseOpen(false); return; }
        sideFocus_ = true;
        if (explorer_.indexOfSelected() < 0 && !explorer_.rows().empty()) explorer_.select(explorer_.rows().front().node->path.string());
        sideReveal_ = true;
        dirty_ = true;
    }

    void switchTab(int i) {
        if (docs_.empty()) return;
        int n = static_cast<int>(docs_.size());
        cur_ = ((i % n) + n) % n;
        revealCurrent();
        resetCaret();
        scrollToCursor_ = true;
        dirty_ = true;
    }

    void closeTab(int i, bool force = false) {
        if (i < 0 || i >= static_cast<int>(docs_.size())) return;
        if (!force && docs_[static_cast<std::size_t>(i)]->modified()) {
            if (closeArmed_ != i) {
                closeArmed_ = i;
                setMessage("'" + docs_[static_cast<std::size_t>(i)]->name + "' has unsaved changes - close again to discard", true);
                return;
            }
        }
        closeArmed_ = -1;
        // Keep focus in the same pane: after closing the current tab, move to its neighbour there.
        Document* keep = nullptr;
        if (i == cur_) {
            Document* closing = docs_[static_cast<std::size_t>(i)].get();
            if (Pane* pp = paneOfDoc(closing); pp && pp->tabs.size() > 1) {
                const auto at = static_cast<std::size_t>(std::ranges::find(pp->tabs, closing) - pp->tabs.begin());
                keep = at + 1 < pp->tabs.size() ? pp->tabs[at + 1] : pp->tabs[at - 1];
            }
        }
        lspDetach(*docs_[static_cast<std::size_t>(i)]);
        closeCompletion();
        docs_.erase(docs_.begin() + i);
        if (docs_.empty()) newDoc();
        cur_ = std::clamp(cur_ > i ? cur_ - 1 : cur_, 0, static_cast<int>(docs_.size()) - 1);
        if (keep) if (int k = docIndex(keep); k >= 0) cur_ = k;
        scrollToCursor_ = true;
        dirty_ = true;
    }

    void saveCurrent() {
        Document& d = doc();
        flushDoc(d);
        if (d.path.empty()) { openPrompt(PromptKind::SaveAs); return; }
        if (auto r = d.save(); r) {
            setMessage("Saved " + d.name);
            gitNextPoll_ = {};   // refresh the change markers / status right away
            if (lsp_ && d.lspOpen) lsp_->didSave(d.path);
            if (isCppDoc(d) && !modules_.empty()) refreshModules();  // a module may have been renamed
            if (explorer_.isOpen()) explorer_.refresh();
            if (cmakeOpen_ && (d.name == "CMakeLists.txt" || d.name.ends_with(".cmake")) && !buildProc_) cmakeReload();
        } else setMessage(r.error(), true);
    }

    void requestQuit() {
        bool anyModified = std::ranges::any_of(docs_, [](const auto& d) { return d->modified(); });
        if (anyModified && !quitArmed_) {
            quitArmed_ = true;
            setMessage("Unsaved changes - press Ctrl+Q again to quit without saving", true);
            return;
        }
        running_ = false;
    }

    // ============================================================ messages / timing

    void setMessage(std::string m, bool error = false, float seconds = 4) {
        message_ = std::move(m);
        messageError_ = error;
        messageUntil_ = Clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
        dirty_ = true;
    }

    void resetCaret() {
        caretOn_ = true;
        nextBlink_ = Clock::now() + std::chrono::milliseconds(530);
        dirty_ = true;
    }

    std::optional<Clock::time_point> nextWake() const {
        std::optional<Clock::time_point> t;
        auto consider = [&](Clock::time_point p) { if (!t || p < *t) t = p; };
        if (focused_ && opt_.screenshot.empty()) consider(nextBlink_);
        if (!message_.empty()) consider(messageUntil_);
        if (buildEof_ || runEof_) consider(Clock::now() + std::chrono::milliseconds(30));
        if (lspFlushAt_) consider(*lspFlushAt_);
        if (!bgQueue_.empty() && lsp_) consider(bgNext_);
        if (!jobs_.empty()) consider(Clock::now() + std::chrono::milliseconds(40));
        if (scriptSleeping_) consider(scriptSleepUntil_);
        if (hoverTool_ >= 3 && !toolTipShown_) consider(toolHoverAt_ + std::chrono::milliseconds(390));
        if (gitRoot_ && focused_) consider(gitNextPoll_);
        if (fsPending_) consider(fsEventAt_ + std::chrono::milliseconds(150));
        if (sPending_) consider(sRunAt_);
        if (nextBlameWake_ > Clock::now()) consider(nextBlameWake_);
        if (lsp_ && cmakeOpen_) consider(nextDbCheck_);
        if (hover_.pending) consider(hover_.due);
        return t;
    }

    void update(Clock::time_point now, float dt) {
        if (fsPending_ && now >= fsEventAt_ + std::chrono::milliseconds(150)) { fsPending_ = false; handleFsChanges(); }
        if (sPending_ && now >= sRunAt_) startSearch();
        if (now >= fsSyncAt_) { fsSyncAt_ = now + std::chrono::seconds(1); syncFsWatches(); }
        if (focused_ && opt_.screenshot.empty() && now >= nextBlink_) {
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
        gitTick(now);
        hoverTick(now);
        if (panelOpen_) {
            auto t = static_cast<std::size_t>(panelTab_);
            if (panelFollow_[t]) panelScroll_[t] = panelMaxScroll();
        }
        updateTitle();
    }

    // Glides one axis of the caret toward its target with exponential easing. Both edges use the
    // same rate, so the caret keeps its exact size while moving. Returns true while still moving.
    static bool chaseEdges(float& lo, float& hi, float tlo, float thi, float dt, float snapDist) {
        const float travel = (tlo + thi) - (lo + hi);
        if (std::abs(travel) * 0.5f > snapDist) { lo = tlo; hi = thi; return false; }
        constexpr float rate = 32.0f;  // 1/seconds: ~95% of the way in about 90 ms
        const float k = 1.0f - std::exp(-dt * rate);
        lo += (tlo - lo) * k;
        hi += (thi - hi) * k;
        if (std::abs(tlo - lo) < 0.2f && std::abs(thi - hi) < 0.2f) { lo = tlo; hi = thi; return false; }
        return true;
    }

    // Advances the caret animation toward the document's cursor. Coordinates are in content space
    // (independent of scrolling, which animates separately).
    bool stepCaret(const Document& d, float dt) {
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

    void clampSide() {
        float contentH = static_cast<float>(explorer_.rows().size()) * L_.sideRow;
        float maxS = std::max(0.0f, contentH - L_.sideList.h * 0.5f);
        sideTarget_ = std::clamp(sideTarget_, 0.0f, maxS);
        sideScroll_ = std::clamp(sideScroll_, 0.0f, maxS);
    }

    // Scrolls the file list so the selected row is visible.
    void revealInSidebar() {
        int i = explorer_.indexOfSelected();
        if (i < 0) return;
        float top = static_cast<float>(i) * L_.sideRow, bottom = top + L_.sideRow;
        if (top < sideTarget_) sideTarget_ = top;
        else if (bottom > sideTarget_ + L_.sideList.h) sideTarget_ = bottom - L_.sideList.h;
    }

    void updateTitle() {
        if (docs_.empty()) return;
        std::string t = doc().name + (doc().modified() ? " *" : "") + " - IDE";
        if (t != lastTitle_) { lastTitle_ = t; win_.setTitle(t); }
    }

    void screenshotStep() {
        const bool busy = tasksRunning() || pendingBuild_ || hover_.pending || Clock::now() < settleUntil_;
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

    // ============================================================ resize / zoom

    void rescaleViews(float ratio) {
        for (auto& d : docs_) { d->scrollY *= ratio; d->targetY *= ratio; d->scrollX *= ratio; }
    }

    void onResize(int w, int h, int s) {
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

    void zoom(float delta, bool reset = false) {
        float next = reset ? opt_.fontSize : std::clamp(fontSize_ + delta, 8.0f, 48.0f);
        if (next == fontSize_) return;
        rescaleViews(next / fontSize_);
        fontSize_ = next;
        applyFonts();
        scrollToCursor_ = true;
        dirty_ = true;
        setMessage(std::format("Font size {:.0f}", fontSize_), false, 1.5f);
    }

    // ============================================================ activity bar (right edge)

    // Two vertical pills: the right-hand island's views (Explorer, Git), then the bottom panel's (Output, Problems).
    float activityCell() const { return std::round(L_.activity.w - 20 * S()); }
    float activityPadV() const { return std::round(7 * S()); }
    Rect activityPill(int group) const {
        const Rect& a = L_.activity;
        const float bs = activityCell(), pad = std::round(3.5f * S()), gap = std::round(2 * S()), between = std::round(10 * S());
        const float w = bs + 2 * pad, x = a.x + (a.w - w) / 2, y0 = a.y + std::round(6 * S());
        auto height = [&](int n) { return static_cast<float>(n) * bs + static_cast<float>(n - 1) * gap + 2 * pad + 2 * activityPadV(); };   // extra room above the first and below the last icon
        return {x, y0 + static_cast<float>(group) * (height(3) + between), w, group == 2 ? w : height(3)};   // the lone Debug button gets a circle
    }
    // Buttons by id (0 Explorer, 1 Git, 2 Output, 3 Terminal, 4 Problems, 5 Search, 6 Debug). Pills: Explorer / Search / Git,
    // Output / Terminal / Problems, and Debug on its own.
    static int activityPos(int id) { static constexpr int pos[7] = {0, 2, 3, 4, 5, 1, 6}; return pos[id]; }
    Rect activityRect(int i) const {
        const float bs = activityCell(), pad = std::round(3.5f * S()), gap = std::round(2 * S());
        const int p = activityPos(i);
        const int g = p < 3 ? 0 : p < 6 ? 1 : 2;
        const Rect pill = activityPill(g);
        return {pill.x + pad, pill.y + pad + (g == 2 ? 0.0f : activityPadV()) + static_cast<float>(p - g * 3) * (bs + gap), bs, bs};
    }
    int activityAt(float x, float y) const {
        if (!L_.activity.contains(x, y)) return 0;
        for (int i = 0; i < 7; ++i)
            if (activityRect(i).contains(x, y)) return i + 1;
        return 0;
    }
    bool activityActive(int i) const {
        switch (i) {
            case 0: return sidebar_ && !sideGit_ && !sideSearch_ && !sideDebug_ && !sideDebug_;
            case 6: return sidebar_ && sideDebug_;
            case 1: return sidebar_ && sideGit_;
            case 5: return sidebar_ && sideSearch_;
            case 2: return panelOpen_ && panelTab_ < 2;
            case 3: return panelOpen_ && panelTab_ == 3;
            default: return panelOpen_ && panelTab_ == 2;
        }
    }
    void activityClick(int i) {
        switch (i) {
            case 0:
                if (activityActive(0)) { sidebar_ = false; sideFocus_ = false; }
                else { sidebar_ = true; sideGit_ = false; sideSearch_ = false; sideDebug_ = false; revealCurrent(); }
                break;
            case 1:
                if (activityActive(1)) { sidebar_ = false; gitMsgFocus_ = false; }
                else { sidebar_ = true; sideGit_ = true; sideSearch_ = false; sideDebug_ = false; sideFocus_ = false; gitNextPoll_ = {}; }
                break;
            case 5:
                if (activityActive(5)) { sidebar_ = false; sFocus_ = -1; }
                else openSearch(false);
                break;
            case 6:   // the debugger lives in two places: the sidebar view and the Debug console; open or close both together
                if (activityActive(6) && panelOpen_ && panelTab_ == 4) { sidebar_ = false; panelOpen_ = false; }
                else {
                    sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; sideFocus_ = false; sFocus_ = -1;
                    panelOpen_ = true; panelTab_ = 4; panelFollow_[4] = true; runFocus_ = false; termFocus_ = false;
                }
                break;
            case 2:
                if (activityActive(2)) panelOpen_ = false;
                else { panelOpen_ = true; panelTab_ = runProc_ ? 1 : 0; }
                break;
            case 3:
                if (activityActive(3)) panelOpen_ = false;
                else openTerminal();
                break;
            default:
                if (activityActive(4)) panelOpen_ = false;
                else { panelOpen_ = true; panelTab_ = 2; termFocus_ = false; }
                break;
        }
        dirty_ = true;
    }

    void drawActivityBar() {
        const Rect& a = L_.activity;
        if (a.w <= 0) return;
        const float s = S(), hair = std::max(1.0f, std::round(0.8f * s));
        const int ip = std::max(15, static_cast<int>(std::round(activityCell() * 0.64f)));
        const bool hasErrors = !problems_.empty() ||
                               std::ranges::any_of(shownProblems_, [](const Problem& p) { return p.severity == Severity::Error; });
        for (int g = 0; g < 3; ++g) {   // white outline, dark fill (same as the toolbar pill)
            const Rect pill = activityPill(g);
            fill(pill, Color::rgb(0xffffff, 34), pill.w / 2);
            fill(pill.x + hair, pill.y + hair, pill.w - 2 * hair, pill.h - 2 * hair, Color::rgb(0x0d0e11), pill.w / 2 - hair);
        }
        for (int i = 0; i < 7; ++i) {
            const Rect r = activityRect(i);
            const bool active = activityActive(i), hot = hoverAct_ == i + 1;
            if (hot && !active) fill(r, Color::rgb(0xffffff, 26), r.w / 2);
            const float ix = std::round(r.x + (r.w - static_cast<float>(ip)) / 2), iy = std::round(r.y + (r.h - static_cast<float>(ip)) / 2);
            const Color col = active ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : Color::rgb(0xffffff);   // the active view's icon takes the accent
            // Material Design icons: file-tree, source-branch, script-text-play (output), console-line (terminal), alert.
            static constexpr std::array<char32_t, 7> glyphs = {0xF13D2, 0xF062C, 0xF1728, 0xF07B7, 0xF002A, 0xF0349, 0xF00E4};
            Icons::glyph(dl_, font_, ix, iy, ip, glyphs[static_cast<std::size_t>(i)], col);
            if (i == 4 && hasErrors) {
                const float d = std::round(7 * s);
                fill(ix + static_cast<float>(ip) - d * 0.6f, iy - d * 0.2f, d, d, theme::danger, d / 2);
            }
        }
    }

    // ============================================================ minimap

    struct MiniRun {
        std::uint16_t c0, c1;
        std::uint8_t tok;
    };
    struct MiniCache {
        const Document* doc = nullptr;
        std::uint64_t version = 0;
        int tab = 0;
        std::vector<std::vector<MiniRun>> lines;
    };
    struct MiniMetrics {
        float rowH, offset, content;
    };

    void rebuildMini(Document& d) {
        mini_.doc = &d;
        mini_.version = d.buf.version();
        mini_.tab = d.tabWidth;
        const int n = std::min(d.buf.lineCount(), 20000);
        mini_.lines.assign(static_cast<std::size_t>(n), {});
        for (int ln = 0; ln < n; ++ln) {
            const std::string& str = d.buf.line(ln);
            if (str.empty()) continue;
            d.hl.highlight(d.buf, ln, toks_);
            auto& runs = mini_.lines[static_cast<std::size_t>(ln)];
            int dcol = 0, runStart = -1;
            std::uint8_t runTok = 0;
            auto close = [&] {
                if (runStart >= 0) runs.push_back({static_cast<std::uint16_t>(runStart), static_cast<std::uint16_t>(dcol), runTok});
                runStart = -1;
            };
            for (std::size_t i = 0; i < str.size() && dcol < 160;) {
                const std::size_t start = i;
                const char32_t cp = decodeUtf8(str, i);
                if (cp == '\t') { close(); dcol += d.tabWidth - dcol % d.tabWidth; continue; }
                if (cp == ' ') { close(); ++dcol; continue; }
                const auto t = static_cast<std::uint8_t>(toks_[start]);
                if (runStart < 0) { runStart = dcol; runTok = t; }
                else if (t != runTok) { close(); runStart = dcol; runTok = t; }
                ++dcol;
            }
            close();
        }
    }

    MiniMetrics miniMetrics(const Document& d) const {
        const float rowH = std::max(2.0f, std::round(2.6f * S()));
        const float content = static_cast<float>(d.buf.lineCount()) * rowH;
        const float maxS = maxScroll(d);
        const float offset = (content > L_.minimap.h && maxS > 0) ? (d.scrollY / maxS) * (content - L_.minimap.h) : 0.0f;
        return {rowH, offset, content};
    }

    bool inMinimap(float x, float y) const {
        return L_.minimap.w > 0 && L_.minimap.contains(x, y) && x < L_.minimap.right() - 12 * S();
    }

    void miniJump(Document& d) {
        const MiniMetrics m = miniMetrics(d);
        const float line = (my_ - L_.minimap.y + m.offset) / m.rowH;
        d.targetY = std::clamp(line * lh() - L_.editor.h / 2, 0.0f, maxScroll(d));
        dirty_ = true;
    }

    void drawMinimap(Document& d) {
        const Rect& mm = L_.minimap;
        if (mm.w <= 0 || mm.h <= 0) return;
        if (mini_.doc != &d || mini_.version != d.buf.version() || mini_.tab != d.tabWidth) rebuildMini(d);
        const float s = S();
        fill(mm, theme::editorBg);   // opaque: also hides text that runs underneath
        fill(mm.x, mm.y, std::max(1.0f, std::round(s * 0.5f)), mm.h, theme::guide);
        const MiniMetrics m = miniMetrics(d);
        const float cs = std::max(1.0f, std::round(s)), barH = std::max(1.0f, std::round(m.rowH * 0.6f));
        const int n = static_cast<int>(mini_.lines.size());
        const int first = std::max(0, static_cast<int>(std::floor(m.offset / m.rowH)));
        const int last = std::min(n - 1, static_cast<int>(std::floor((m.offset + mm.h) / m.rowH)));
        const float x0 = mm.x + 8 * s, xMax = mm.right() - 8 * s;
        for (int ln = first; ln <= last; ++ln) {
            const float y = mm.y + static_cast<float>(ln) * m.rowH - m.offset + std::round((m.rowH - barH) / 2);
            for (const MiniRun& r : mini_.lines[static_cast<std::size_t>(ln)]) {
                const float x = x0 + static_cast<float>(r.c0) * cs;
                if (x >= xMax) break;
                const Tok t = static_cast<Tok>(r.tok);
                fill(x, y, std::min(static_cast<float>(r.c1 - r.c0) * cs, xMax - x), barH,
                     t == Tok::Normal ? theme::text.alpha(120) : theme::syntax(t).alpha(190));
            }
        }
        // Viewport slider.
        const float top = d.scrollY / lh() * m.rowH - m.offset;
        const float vh = std::min(mm.h, L_.editor.h / lh() * m.rowH);
        const float sy = std::clamp(mm.y + top, mm.y, mm.bottom() - vh);
        fill(mm.x + 1, sy, mm.w - 1, vh, Color{255, 255, 255, static_cast<std::uint8_t>(miniDrag_ ? 38 : 20)});
    }

    // ============================================================ UI font helpers

    static constexpr float kUiSize = 13.0f;   // interface font, logical px

    void applyFonts() { font_.setSizes(fontSize_ * S(), kUiSize * S()); }
    float uiH() { return font_.uiLineHeight(); }
    float uiW(std::string_view t) { return font_.uiWidth(t); }
    // Draws UI text vertically centred in the row [rowY, rowY + rowH); returns the end x.
    float uiLabel(float x, float rowY, float rowH, std::string_view t, Color c, float maxX = std::numeric_limits<float>::infinity()) {
        return dl_.uiText(font_, x, std::round(rowY + (rowH - uiH()) / 2), t, c, maxX);
    }
    // Text that ends at xRight; returns its left x.
    float uiLabelR(float xRight, float rowY, float rowH, std::string_view t, Color c) {
        const float w = uiW(t);
        uiLabel(xRight - w, rowY, rowH, t, c);
        return xRight - w;
    }
    float uiLabelC(float cx, float rowY, float rowH, std::string_view t, Color c) {
        const float w = uiW(t);
        uiLabel(cx - w / 2, rowY, rowH, t, c);
        return w;
    }

    // ============================================================ layout

    // ---- animations

    static float easeInOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

    float tabNaturalW(const std::string& name) {
        const float s = S();
        return std::clamp(14 * s + iconSize() + 8 * s + uiW(name) + 12 * s + 22 * s + 8 * s, 96 * s, 260 * s);
    }

    // Keeps one animated entry per tab of a pane, plus ghosts for tabs that were just closed or moved away.
    void syncTabs(Pane& p) {
        std::unordered_set<const Document*> live(p.tabs.begin(), p.tabs.end());
        std::unordered_map<const Document*, TabAnim> liveMap;
        std::vector<TabAnim> ghosts;
        const Document* lastLive = nullptr;
        for (auto& e : p.anim) {
            if (e.ghost) { if (e.w >= 0.6f) ghosts.push_back(std::move(e)); continue; }
            if (live.contains(e.doc)) { lastLive = e.doc; liveMap.emplace(e.doc, std::move(e)); }
            else { e.ghost = true; e.target = 0; e.anchor = lastLive; ghosts.push_back(std::move(e)); }
        }
        std::vector<TabAnim> next;
        auto emitGhosts = [&](const Document* anchor) {
            for (auto& g : ghosts)
                if (!g.used && g.anchor == anchor) { g.used = true; next.push_back(g); }
        };
        emitGhosts(nullptr);
        for (Document* d : p.tabs) {
            TabAnim e;
            if (auto it = liveMap.find(d); it != liveMap.end()) e = std::move(it->second);
            else { e.doc = d; e.w = 0; e.x = -1; }
            e.name = d->name;
            e.target = tabNaturalW(e.name);
            e.ghost = false;
            next.push_back(std::move(e));
            emitGhosts(d);
        }
        for (auto& g : ghosts) if (!g.used) next.push_back(g);
        p.anim = std::move(next);
        if (frames_ < 2 || (!opt_.screenshot.empty() && !std::getenv("DAEMON_ANIMATE")))
            for (auto& e : p.anim) { e.w = e.target; e.x = -1; }
    }

    // Advances tab / side bar / panel animations. Returns true while anything is still moving.
    bool stepAnimations(float dt) {
        bool moving = false;
        const bool snap = frames_ < 3 || (!opt_.screenshot.empty() && !std::getenv("DAEMON_ANIMATE"));
        auto chase = [&](float& v, float target, float rate) {
            if (snap || std::abs(target - v) < 0.002f * std::max(1.0f, std::abs(target))) { v = target; return; }
            v += (target - v) * (1.0f - std::exp(-dt * rate));
            moving = true;
        };
        chase(panelAnim_, panelOpen_ ? 1.0f : 0.0f, 14.0f);
        chase(sideAnim_, sidebar_ ? 1.0f : 0.0f, 14.0f);
        for (Node* n : leaves())
            for (auto& e : n->pane->anim) {
                if (snap || std::abs(e.target - e.w) < 0.4f) e.w = e.target;
                else { e.w += (e.target - e.w) * (1.0f - std::exp(-dt * 16.0f)); moving = true; }
                if (snap || std::abs(e.tx - e.x) < 0.4f) e.x = e.tx;
                else { e.x += (e.tx - e.x) * (1.0f - std::exp(-dt * 18.0f)); moving = true; }
            }
        return moving;
    }

    void layout() {
        const float W = static_cast<float>(wLog_ * scale_), H = static_cast<float>(hLog_ * scale_);
        const float s = S(), uh = uiH();
        L_.csd = !win_.serverDecorations() && !win_.fullscreen();
        const float titleH = L_.csd ? std::round(uh + 18 * s) : 0;
        const float tabH = std::round(uh + 20 * s);
        const float crumbH = (!docs_.empty() && !doc().path.empty()) ? std::round(uh + 8 * s) : 0;
        const float statusH = std::round(uh + 10 * s);
        const float m = std::round(5 * s);     // gap between the frame and the islands
        const float act = std::round(50 * s);  // activity bar width
        L_.rowH = std::round(uh + 14 * s);
        L_.radius = std::round(12 * s);
        const float promptH = (prompt_.kind == PromptKind::None || sidePrompt()) ? 0 : L_.rowH * static_cast<float>(prompt_.fields.size());
        const float tbH = cmakeOpen_ ? std::round(uh + 18 * s) : 0;
        // With client-side decorations the toolbar shares the title-bar row; otherwise it is its own row.
        L_.toolbar = L_.csd ? Rect{0, 0, W, titleH} : Rect{0, 0, W, tbH};
        const float chromeBottom = L_.csd ? titleH : tbH;
        const float top = chromeBottom > 0 ? chromeBottom + std::round(1 * s) : m;
        L_.title = {0, 0, W, titleH};
        L_.status = {0, H - statusH, W, statusH};
        const float h = std::max(0.0f, L_.status.y - top);
        // The output panel is its own island below the editor island.
        float panelH = 0;
        const bool panelShown = panelOpen_ || panelAnim_ > 0.002f;
        if (panelShown) panelH = std::round(std::clamp(std::round(panelH_ * s), 90 * s, std::max(90 * s, h * 0.6f)) * easeInOut(panelAnim_));
        const float panelTotal = panelH > 0 ? panelH + m * easeInOut(panelAnim_) : 0;
        const float contentRight = W - act;
        L_.activity = {contentRight, top, act, h};
        float sbW = 0;
        const bool sideShown = sidebar_ || sideAnim_ > 0.002f;
        if (sideShown) sbW = std::clamp(sidebarWidth_ * s, 150 * s, std::max(150 * s, W * 0.5f)) * easeInOut(sideAnim_);
        const float mainW = std::max(120 * s, contentRight - m - (sideShown ? sbW + m * easeInOut(sideAnim_) : 0));
        L_.island = {m, top, mainW, std::max(0.0f, h - panelTotal)};
        L_.side = sideShown ? Rect{L_.island.right() + m, top, std::max(0.0f, contentRight - (L_.island.right() + m)), h} : Rect{};
        L_.split = sideShown ? Rect{L_.island.right(), top, m, h} : Rect{};
        L_.sideRow = std::round(uh + 10 * s);
        L_.sideList = sideShown ? Rect{L_.side.x, L_.side.y + tabH, L_.side.w, std::max(0.0f, L_.side.h - tabH - sideCardH())} : Rect{};
        L_.tabs = {L_.island.x, L_.island.y, L_.island.w, tabH};
        L_.panel = {L_.island.x, L_.island.bottom() + (panelH > 0 ? m * easeInOut(panelAnim_) : 0), L_.island.w, panelH};
        (void)crumbH;

        // Toolbar hit rectangles.
        tool_ = {};
        if (cmakeOpen_) {
            const float cy = L_.toolbar.y + std::round(4 * s), ch = L_.toolbar.h - std::round(8 * s);
            auto chipW = [&](const std::string& t) { return uiW(t) + std::round(56 * s); };
            float x = L_.csd ? std::round(46 * s) : m;
            const std::string pn = profiles_.empty() ? "-" : profiles_[static_cast<std::size_t>(profile_)].name;
            const std::string tn = target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : "No target";
            tool_.profile = {x, cy, chipW(pn), ch};
            x += tool_.profile.w + std::round(6 * s);
            tool_.target = {x, cy, chipW(tn), ch};
            x += tool_.target.w + std::round(14 * s);
            // Action cluster: one pill holding Run | Debug | Build | Regenerate | Stop (groups separated by dividers).
            const float bw = std::round(ch * 1.08f), div = std::round(9 * s), pad = std::round(2 * s);
            const float cw_ = pad + bw * 5 + div * 2 + pad;
            tool_.cluster = {x, cy, cw_, ch};
            float bx = x + pad;
            tool_.run = {bx, cy, bw, ch}; bx += bw;
            tool_.debug = {bx, cy, bw, ch}; bx += bw + div;
            tool_.build = {bx, cy, bw, ch}; bx += bw;
            tool_.reload = {bx, cy, bw, ch}; bx += bw + div;
            tool_.stop = {bx, cy, bw, ch};
        }

        // Editor panes: split tree inside the island, each pane with its own tab strip, breadcrumbs, gutter and minimap.
        syncPanes();
        layoutTree(*root_, L_.island, m);
        for (Node* n : leaves()) layoutPane(*n->pane, n->pane.get() == activePane_, promptH);
        {
            Pane& ap = *activePane_;   // the rest of the app works on the active pane through L_
            L_.tabs = ap.tabsR; L_.crumbs = ap.crumbs; L_.editor = ap.editor; L_.minimap = ap.minimap;
            L_.gutterW = ap.gutterW; L_.textX = ap.textX; L_.textW = ap.textW;
            L_.prompt = {ap.rect.x, ap.editor.bottom(), ap.rect.w, promptH};
        }
    }

    // ---- panes

    std::vector<Node*> leaves() {
        std::vector<Node*> out;
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n) return;
            if (n->leaf()) out.push_back(n);
            else { walk(n->a.get()); walk(n->b.get()); }
        };
        walk(root_.get());
        return out;
    }
    std::vector<Node*> splits() {
        std::vector<Node*> out;
        std::function<void(Node*)> walk = [&](Node* n) {
            if (!n || n->leaf()) return;
            out.push_back(n);
            walk(n->a.get());
            walk(n->b.get());
        };
        walk(root_.get());
        return out;
    }
    Pane* paneOfDoc(const Document* d) {
        for (Node* n : leaves())
            if (std::ranges::find(n->pane->tabs, d) != n->pane->tabs.end()) return n->pane.get();
        return nullptr;
    }
    Node* nodeOfPane(const Pane* p) {
        for (Node* n : leaves()) if (n->pane.get() == p) return n;
        return nullptr;
    }
    int docIndex(const Document* d) const {
        for (std::size_t i = 0; i < docs_.size(); ++i) if (docs_[i].get() == d) return static_cast<int>(i);
        return -1;
    }
    Pane* paneAt(float x, float y) {
        for (Node* n : leaves()) if (n->pane->rect.contains(x, y)) return n->pane.get();
        return nullptr;
    }
    Node* gapAt(float x, float y) {
        const float pad = 3 * S();
        for (Node* n : splits()) {
            Rect g = n->gap;
            g.x -= pad; g.y -= pad; g.w += 2 * pad; g.h += 2 * pad;
            if (g.contains(x, y)) return n;
        }
        return nullptr;
    }

    static bool pruneNode(std::unique_ptr<Node>& n) {   // true when the subtree holds no tabs at all
        if (n->leaf()) return n->pane->tabs.empty();
        const bool ea = pruneNode(n->a), eb = pruneNode(n->b);
        if (ea && eb) return true;
        if (ea) { auto keep = std::move(n->b); n = std::move(keep); }
        else if (eb) { auto keep = std::move(n->a); n = std::move(keep); }
        return false;
    }

    // Reconciles panes with the document list: drops closed tabs, puts new documents into the active pane,
    // removes empty panes and makes the pane of the current document the active one.
    void syncPanes() {
        if (!root_) {
            root_ = std::make_unique<Node>();
            root_->pane = std::make_unique<Pane>();
            activePane_ = root_->pane.get();
        }
        std::unordered_set<Document*> alive;
        for (auto& d : docs_) alive.insert(d.get());
        std::unordered_set<Document*> assigned;
        for (Node* n : leaves())
            std::erase_if(n->pane->tabs, [&](Document* d) { return !alive.contains(d) || !assigned.insert(d).second; });
        if (!std::ranges::contains(leaves(), activePane_, [](Node* n) { return n->pane.get(); })) activePane_ = leaves().front()->pane.get();
        for (auto& d : docs_)
            if (!assigned.contains(d.get())) activePane_->tabs.push_back(d.get());
        if (pruneNode(root_)) { /* no documents: keep the (empty) root */ }
        if (!std::ranges::contains(leaves(), activePane_, [](Node* n) { return n->pane.get(); })) activePane_ = leaves().front()->pane.get();
        if (!docs_.empty()) {
            Document* c = docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))].get();
            if (Pane* p = paneOfDoc(c)) { activePane_ = p; p->cur = c; }
        }
        for (Node* n : leaves()) {
            Pane& p = *n->pane;
            if (p.cur && std::ranges::find(p.tabs, p.cur) == p.tabs.end()) p.cur = nullptr;
            if (!p.cur && !p.tabs.empty()) p.cur = p.tabs.back();
        }
    }

    void layoutTree(Node& n, Rect r, float gap) {
        n.rect = r;
        if (n.leaf()) { n.pane->rect = r; return; }
        const float s = S();
        if (n.sideBySide) {
            const float usable = std::max(0.0f, r.w - gap);
            n.ratio = std::clamp(n.ratio, std::min(0.5f, 160 * s / std::max(1.0f, usable)), std::max(0.5f, 1.0f - 160 * s / std::max(1.0f, usable)));
            const float wa = std::round(usable * n.ratio);
            n.gap = {r.x + wa, r.y, gap, r.h};
            layoutTree(*n.a, {r.x, r.y, wa, r.h}, gap);
            layoutTree(*n.b, {r.x + wa + gap, r.y, usable - wa, r.h}, gap);
        } else {
            const float usable = std::max(0.0f, r.h - gap);
            n.ratio = std::clamp(n.ratio, std::min(0.5f, 120 * s / std::max(1.0f, usable)), std::max(0.5f, 1.0f - 120 * s / std::max(1.0f, usable)));
            const float ha = std::round(usable * n.ratio);
            n.gap = {r.x, r.y + ha, r.w, gap};
            layoutTree(*n.a, {r.x, r.y, r.w, ha}, gap);
            layoutTree(*n.b, {r.x, r.y + ha + gap, r.w, usable - ha}, gap);
        }
    }

    void layoutPane(Pane& p, bool active, float promptH) {
        const float s = S(), uh = uiH();
        const float tabH = std::round(uh + 20 * s);
        // A file directly in the project root would only repeat its tab title, so it gets no breadcrumb row.
        const float crumbH = (p.cur && !p.cur->path.empty() && crumbParts(*p.cur).size() > 1) ? std::round(uh + 8 * s) : 0;
        const Rect& r = p.rect;
        p.tabsR = {r.x, r.y, r.w, std::min(tabH, r.h)};
        p.crumbs = {r.x, p.tabsR.bottom(), r.w, std::min(crumbH, std::max(0.0f, r.h - tabH))};
        float bottom = r.bottom();
        if (active) bottom -= promptH;
        p.editor = {r.x, p.crumbs.bottom(), r.w, std::max(0.0f, bottom - p.crumbs.bottom())};
        const float mapW = r.w > 640 * s ? std::round(92 * s) : 0;
        p.minimap = {p.editor.right() - mapW, p.editor.y, mapW, p.editor.h};
        const int lines = p.cur ? p.cur->buf.lineCount() : 1;
        const int digits = std::max(3, static_cast<int>(std::to_string(lines).size()));
        p.gutterW = (static_cast<float>(digits) + 3.4f) * cw();
        p.textX = p.editor.x + p.gutterW;
        p.textW = std::max(0.0f, p.editor.w - p.gutterW - mapW - 12 * s);

        // Tab strip: animated entries left to right (closed tabs shrink away as ghosts).
        syncTabs(p);
        p.draw.clear();
        p.hits.clear();
        float x = 8 * s;   // offsets are relative to the strip, so tabs move rigidly with their pane
        const float gap = 2 * s;
        int idx = 0;
        for (auto& e : p.anim) {
            e.tx = x;
            if (e.x < -0.5f) e.x = x;
            const float w = std::max(0.0f, e.w);
            Rect t{p.tabsR.x + e.x, p.tabsR.y + 5 * s, w, p.tabsR.h - 5 * s};
            Rect c{t.right() - 28 * s, t.y, 24 * s, t.h};
            const float alpha = e.target > 0 ? std::clamp(w / e.target, 0.0f, 1.0f) : std::clamp(w / (96 * s), 0.0f, 1.0f);
            p.draw.push_back({t, c, e.ghost ? -1 : idx, e.name, alpha, e.doc});
            if (!e.ghost) { p.hits.push_back({t, c}); ++idx; }
            x += w + gap;
        }
    }

    // Draws with a pane's geometry installed in L_ (and its document current).
    struct PaneCtx { Rect tabs, crumbs, editor, minimap; float g, tx, tw; int cur; };
    PaneCtx enterPane(Pane& p) {
        PaneCtx sv{L_.tabs, L_.crumbs, L_.editor, L_.minimap, L_.gutterW, L_.textX, L_.textW, cur_};
        L_.tabs = p.tabsR; L_.crumbs = p.crumbs; L_.editor = p.editor; L_.minimap = p.minimap;
        L_.gutterW = p.gutterW; L_.textX = p.textX; L_.textW = p.textW;
        if (p.cur) cur_ = std::max(0, docIndex(p.cur));
        std::swap(mini_, p.mini);
        return sv;
    }
    void leavePane(Pane& p, const PaneCtx& sv) {
        std::swap(mini_, p.mini);
        L_.tabs = sv.tabs; L_.crumbs = sv.crumbs; L_.editor = sv.editor; L_.minimap = sv.minimap;
        L_.gutterW = sv.g; L_.textX = sv.tx; L_.textW = sv.tw;
        cur_ = sv.cur;
    }

    float maxScroll(const Document& d) const {
        return std::max(0.0f, (static_cast<float>(d.buf.lineCount()) - 1.0f) * lh());
    }

    void clampScroll(Document& d) {
        float m = maxScroll(d);
        d.targetY = std::clamp(d.targetY, 0.0f, m);
        d.scrollY = std::clamp(d.scrollY, 0.0f, m);
        d.scrollX = std::max(0.0f, d.scrollX);
    }

    void ensureCursorVisible(Document& d) {
        const Rect& e = L_.editor;
        if (e.h <= 0) return;
        float margin = std::min(2 * lh(), e.h / 4);
        float cy = static_cast<float>(d.cursor.line) * lh();
        if (cy - margin < d.targetY) d.targetY = std::max(0.0f, cy - margin);
        else if (cy + lh() + margin > d.targetY + e.h) d.targetY = cy + lh() + margin - e.h;
        float cx = static_cast<float>(displayCol(d.buf.line(d.cursor.line), d.cursor.col, d.tabWidth)) * cw();
        if (cx < d.scrollX) d.scrollX = std::max(0.0f, cx - 6 * cw());
        else if (cx + 2 * cw() > d.scrollX + L_.textW) d.scrollX = cx + 2 * cw() - L_.textW + 6 * cw();
    }

    // Maps window coordinates to a position in the document.
    Pos posAt(Document& d, float mx, float my) const {
        float y = my - L_.editor.y + std::round(d.scrollY);
        int line = std::clamp(static_cast<int>(std::floor(y / lh())), 0, d.buf.lineCount() - 1);
        float x = mx - L_.textX + std::round(d.scrollX);
        int dcol = std::max(0, static_cast<int>(std::floor(x / cw() + 0.5f)));
        return {line, byteColAt(d.buf.line(line), dcol, d.tabWidth)};
    }

    // ============================================================ input: keyboard

    // Keystrokes for the running program (it sits on a pseudo-terminal, so the kernel echoes and line-edits).
    // Returns false for keys that should keep their normal (global) meaning.
    bool runKey(const KeyEvent& k) {
        if (k.alt || k.super) return false;
        if (k.key == Key::Escape) { runFocus_ = false; return true; }
        if (k.ctrl) {
            if (k.key != Key::Char) return false;
            if (k.ch == 'c' && !k.shift) { runProc_->write("\x03"); return true; }   // interrupt
            if (k.ch == 'd' && !k.shift) { runProc_->write("\x04"); return true; }   // end of input
            if (k.ch == 'v' && !k.shift) { std::string c = win_.clipboard(); std::erase(c, '\r'); runProc_->write(c); return true; }
            return false;
        }
        switch (k.key) {
            case Key::Enter: runProc_->write("\n"); return true;
            case Key::Backspace: runProc_->write("\x7f"); return true;
            case Key::Tab: runProc_->write("\t"); return true;
            case Key::Left: case Key::Right: case Key::Up: case Key::Down: case Key::Home: case Key::End:
            case Key::PageUp: case Key::PageDown: case Key::Delete: case Key::Insert: return true;
            case Key::Char: if (!k.text.empty()) runProc_->write(k.text); return true;
            default: return false;   // function keys (build / run / stop ...) keep working
        }
    }

    void onKey(const KeyEvent& k) {
        resetCaret();
        if (!(k.ctrl && k.key == Key::Char && k.ch == 'q')) quitArmed_ = false;
        if (!(k.ctrl && k.key == Key::Char && k.ch == 'w')) closeArmed_ = -1;
        if (dlg_.kind != DlgKind::None) { dlgKey(k); dirty_ = true; return; }
        if (k.ctrl && !k.shift && !k.alt && k.key == Key::Char && k.ch == 'c' && copyOverlay()) { dirty_ = true; return; }
        if (gitMsgFocus_ && gitView() && gitMsgKey(k)) { dirty_ = true; return; }
        if (sFocus_ >= 0 && searchView() && searchKey(k)) { dirty_ = true; return; }
        if (termFocus_ && panelOpen_ && panelTab_ == 3 && termKey(k)) { dirty_ = true; return; }
        if (dbgConFocus_ && panelOpen_ && panelTab_ == 4 && dbgConKey(k)) { dirty_ = true; return; }
        if (runFocus_ && runProc_ && panelOpen_ && panelTab_ == 1 && runKey(k)) { dirty_ = true; return; }
        if (tabDrag_.active) {  // a tab is being dragged: Esc cancels, other keys are ignored
            if (k.key == Key::Escape) { tabDrag_ = {}; dirty_ = true; }
            return;
        }
        if (menu_.open) { menuKey(k); dirty_ = true; return; }
        if (prompt_.kind != PromptKind::None) { promptKey(k); return; }
        if (sidebar_ && sideFocus_ && !sideGit_ && !sideSearch_ && !sideDebug_ && explorerKey(k)) { dirty_ = true; return; }
        editorKey(k);
        dirty_ = true;
    }

    // ============================================================ language server

    static bool isCMakeDoc(const Document& d) { return d.lang && d.lang->name == "CMake"; }
    static bool isCppDoc(const Document& d) { return d.lang && (d.lang->name == "C++" || d.lang->name == "C") && !d.path.empty(); }
    static bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
    static Color diagColor(int sev) {
        switch (sev) {
            case 1: return Color::rgb(0xf2777a);
            case 2: return Color::rgb(0xe5c07b);
            case 3: return Color::rgb(0x7fb7ff);
            default: return Color::rgb(0x8a8e98);
        }
    }

    const LspDiag* diagOnLine(const Document& d, int line) const {
        auto it = diags_.find(d.path);
        if (it == diags_.end()) return nullptr;
        const LspDiag* best = nullptr;
        for (const auto& dg : it->second)
            if (dg.line <= line && line <= dg.endLine && (!best || dg.severity < best->severity)) best = &dg;
        return best;
    }

    static std::string languageId(const Document& d) { return d.lang && d.lang->name == "C" && d.path.ends_with(".c") ? "c" : "cpp"; }

    void startLsp() {
        if (lsp_ && lsp_->running()) return;
        const char* env = std::getenv("IDE_CLANGD");
        auto exe = findExecutable(env && *env ? env : "clangd");
        if (!exe) {
            lspStatus_ = "clangd not found";
            if (!lspNoticeShown_) {
                lspNoticeShown_ = true;
                setMessage("clangd not found - install it (or set IDE_CLANGD) for completion and diagnostics", true, 7);
            }
            return;
        }
        std::vector<std::string> argv = {*exe, "--background-index", "--header-insertion=never", "--log=error",
                                         "--experimental-modules-support",
                                         "--completion-parse=always"};  // real AST completions, never the text-based fallback
        clangdModules_ = false;
        if (cmakeOpen_ && fs::exists(compileCommands(profile()))) {
            // GCC-style module builds get a Clang-flavoured database so clangd can build BMIs (import std; import foo;).
            {
                std::error_code ec;
                dbStamp_ = fs::last_write_time(compileCommands(profile()), ec);
            }
            auto db = prepareClangdDb(profile().dir);
            if (std::getenv("IDE_LSP_TRACE")) std::println(std::cerr, "clangd db: {} derived={}", db ? db->dir.string() : "none", db && db->derived);
            if (db) {
                std::error_code ec;
                fs::create_directories(profile().dir / ".daemon", ec);
                std::ofstream(profile().dir / ".daemon" / "clangd-start.log", std::ios::app)
                    << "clangd db: " << db->dir.string() << " derived=" << db->derived << " sources=" << db->sources << "\n";
                argv.push_back("--compile-commands-dir=" + db->dir.string());
                clangdModules_ = db->derived;
            } else {
                argv.push_back("--compile-commands-dir=" + profile().dir.string());
            }
            refreshModules();
        }
        std::string root = cmakeOpen_ ? cmakeRoot_.string() : (explorer_.isOpen() ? explorer_.rootPath().string() : fs::current_path().string());
        lsp_ = std::make_unique<LspClient>();
        lsp_->onDiagnostics = [this](const std::string& path, std::vector<LspDiag> d) {
            // clangd's "unused include" hints are noise next to real errors (CLion greys them out instead).
            std::erase_if(d, [](const LspDiag& x) { return x.code == "unused-includes" || x.message.starts_with("Included header "); });
            if (d.empty()) diags_.erase(path); else diags_[path] = std::move(d);
            dirty_ = true;
        };
        lsp_->onState = [this](LspClient::State st) {
            if (st == LspClient::State::Failed) lspCrashed_ = true;   // handled from update(): the client cannot be replaced from its own callback
            lspStatus_ = st == LspClient::State::Ready ? (clangdModules_ ? "clangd \xC2\xB7 modules" : "clangd") : st == LspClient::State::Starting ? "clangd starting" : "clangd stopped";
            dirty_ = true;
        };
        if (auto r = lsp_->start(argv, root); !r) {
            lspStatus_ = "clangd failed";
            setMessage("clangd: " + r.error(), true);
            lsp_.reset();
            return;
        }
        lspFd_ = lsp_->fd();
        win_.watch(lspFd_, [this] { pumpLsp(); });
        for (auto& d : docs_) attachLsp(*d);  // queued by the client until the handshake completes
    }

    // ---- C++20 modules: project module list (for `import` completion and navigation)

    void refreshModules() {
        modules_.clear();
        if (!cmakeOpen_) return;
        modules_ = scanModules(compileCommands(profile()));
    }

    const ModuleDecl* findModule(const std::string& name) const {
        for (const auto& m : modules_) if (m.interface && m.name == name) return &m;
        return nullptr;
    }

    // Completion inside `import <name>;`: module names from the project (and std).
    bool completeImport(Document& d) {
        const std::string& ln = d.buf.line(d.cursor.line);
        auto start = importNameStart(ln, static_cast<std::size_t>(d.cursor.col));
        if (!start || modules_.empty()) return false;
        std::vector<LspCompletion> items;
        for (const auto& m : modules_) {
            if (!m.interface) continue;
            LspCompletion it;
            it.label = m.name;
            it.kind = 9;
            const bool lib = m.name == "std" || m.name == "std.compat";
            it.detail = lib ? "standard library" : (m.name.find(':') != std::string::npos ? "partition" : "module");
            it.sortText = (lib ? "1" : "0") + m.name;
            items.push_back(std::move(it));
        }
        if (items.empty()) return false;
        showCompletion(std::move(items), d.cursor, static_cast<int>(*start));
        return true;
    }

    bool gotoModule(Document& d) {
        auto name = importNameAt(d.buf.line(d.cursor.line), static_cast<std::size_t>(d.cursor.col));
        if (!name) return false;
        std::string full = *name;
        if (full.starts_with(':')) {  // partition of the current module: ide.app + :part
            for (const auto& m : modules_)
                if (m.path == fs::path(d.path)) { full = m.name.substr(0, m.name.find(':')) + full; break; }
        }
        const ModuleDecl* m = findModule(full);
        if (!m) return false;
        navBack_.push_back({d.path, d.cursor});
        const std::string path = m->path.string();
        openFile(path);
        if (docs_.empty()) return true;
        Document& nd = doc();
        for (int i = 0; i < nd.buf.lineCount(); ++i) {
            const std::string& l = nd.buf.line(i);
            if (l.find("module") != std::string::npos && l.find(full) != std::string::npos && l.find(';') != std::string::npos) {
                nd.setCursor({i, 0});
                break;
            }
        }
        scrollToCursor_ = true;
        dirty_ = true;
        return true;
    }

    void pumpLsp() {
        if (!lsp_) return;
        lsp_->pump();
        dirty_ = true;
        if (lsp_->fd() < 0) { win_.unwatch(lspFd_); lspFd_ = -1; }
    }

    void stopLsp() {
        if (lspFd_ >= 0) win_.unwatch(lspFd_);
        lspFd_ = -1;
        lsp_.reset();
        compReq_ = -1;
        diags_.clear();
        bgQueue_.clear(); bgOpen_.clear(); bgSources_.clear(); bgScanned_ = false;
        closeCompletion();
        for (auto& d : docs_) d->lspOpen = false;
    }

    // clangd died on its own (crash / killed). Its persistent module cache can be what it chokes on (a stale BMI left
    // over from an older version of a module crashes clangd's module reader), so clear it and start again; give up
    // when it keeps happening.
    void handleLspCrash(Clock::time_point now) {
        if (!lsp_) return;
        while (!lspCrashTimes_.empty() && now - lspCrashTimes_.front() > std::chrono::minutes(5)) lspCrashTimes_.pop_front();
        lspCrashTimes_.push_back(now);
        if (lspCrashTimes_.size() > 3) {
            setMessage("clangd keeps crashing - code intelligence is off for now (restart Daemon to retry)", true, 10);
            return;
        }
        if (cmakeOpen_ && clangdModules_) {
            std::error_code ec;
            fs::remove_all(profile().dir / ".daemon" / "clangd-v2" / ".cache", ec);
        }
        setMessage("clangd crashed - restarting it with a clean module cache", true, 5);
        restartLsp();
    }

    void restartLsp() {
        const bool had = lsp_ != nullptr;
        stopLsp();
        lspStatus_.clear();
        if (had || std::ranges::any_of(docs_, [](const auto& d) { return isCppDoc(*d); })) {
            if (std::ranges::any_of(docs_, [](const auto& d) { return isCppDoc(*d); })) startLsp();
        }
    }

    void ensureLsp(Document& d) {
        if (!isCppDoc(d)) return;
        if (!lsp_) startLsp(); else attachLsp(d);
    }

    // ---- project-wide diagnostics: clangd only reports on open files, so every source file of the project is opened
    // in the language server in the background (no tab, text read from disk), a few per tick.

    static bool isSourcePath(const fs::path& p) {
        static const std::unordered_set<std::string> ext = {".c", ".cc", ".cpp", ".cxx", ".c++", ".cppm", ".ixx", ".mpp", ".ccm", ".cxxm"};
        return ext.contains(p.extension().string());
    }

    std::vector<std::string> projectSources() {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;
        std::error_code ec;
        auto inside = [&](const fs::path& root, const fs::path& p) {
            auto r = p.lexically_relative(root);
            return !r.empty() && *r.begin() != "..";
        };
        auto add = [&](const fs::path& f, const fs::path& root, const fs::path* skip) {
            const fs::path n = f.lexically_normal();
            if (!inside(root, n) || (skip && inside(*skip, n)) || !isSourcePath(n)) return;
            if (seen.insert(n.string()).second) out.push_back(n.string());
        };
        constexpr std::size_t kMax = 400;
        if (cmakeOpen_ && fs::exists(compileCommands(profile()), ec)) {
            if (auto db = readJson(compileCommands(profile())); db && db->array())
                for (const auto& e : *db->array()) { add(e["file"].str(), cmakeRoot_, &profile().dir); if (out.size() >= kMax) break; }
        }
        if (out.empty() && explorer_.isOpen()) {
            for (fs::recursive_directory_iterator it(explorer_.rootPath(), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (it->is_directory(ec) && (name.starts_with('.') || name.starts_with("cmake-build") || name == "build" || name == "node_modules" || name == "third_party")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec)) add(it->path(), explorer_.rootPath(), nullptr);
                if (out.size() >= kMax) break;
            }
        }
        return out;
    }

    bool openInEditor(const std::string& path) const {
        return std::ranges::any_of(docs_, [&](const auto& d) { return d->path == path; });
    }

    void queueProjectDiagnostics() {
        bgQueue_.clear();
        bgOpen_.clear();
        bgSources_.clear();
        for (auto& p : projectSources()) { bgQueue_.push_back(p); bgSources_.insert(p); }
        bgNext_ = Clock::now();
    }

    void pumpProjectDiagnostics(Clock::time_point now) {
        if (!lsp_ || !lsp_->ready() || bgQueue_.empty() || now < bgNext_) return;
        for (int n = 0; n < 3 && !bgQueue_.empty(); ++n) {
            const std::string path = bgQueue_.front();
            bgQueue_.pop_front();
            if (openInEditor(path) || bgOpen_.contains(path)) continue;
            auto text = readFile(path);
            if (!text) continue;
            lsp_->didOpen(path, path.ends_with(".c") ? "c" : "cpp", 1, *text);
            bgOpen_.insert(path);
        }
        bgNext_ = now + std::chrono::milliseconds(150);
    }

    void attachLsp(Document& d) {
        if (!lsp_ || !isCppDoc(d) || d.lspOpen) return;
        if (bgOpen_.erase(d.path)) lsp_->didClose(d.path);   // the editor takes over from the background copy
        lsp_->didOpen(d.path, languageId(d), ++d.lspVersion, d.buf.text());
        d.lspOpen = true;
        d.lspSent = d.buf.version();
    }

    void lspDetach(Document& d) {
        if (lsp_ && d.lspOpen) lsp_->didClose(d.path);
        d.lspOpen = false;
        diags_.erase(d.path);
        if (lsp_ && bgSources_.contains(d.path)) bgQueue_.push_back(d.path);   // keep watching it in the background
    }

    void flushDoc(Document& d) {
        if (lsp_ && d.lspOpen && d.buf.version() != d.lspSent) {
            lsp_->didChange(d.path, ++d.lspVersion, d.buf.text());
            d.lspSent = d.buf.version();
        }
    }

    // Sends edits to the server shortly after typing stops.
    void lspSync(Clock::time_point now) {
        if (!lsp_) { lspFlushAt_.reset(); return; }
        const bool pending = std::ranges::any_of(docs_, [](const auto& d) { return d->lspOpen && d->buf.version() != d->lspSent; });
        if (!pending) { lspFlushAt_.reset(); return; }
        if (!lspFlushAt_) lspFlushAt_ = now + std::chrono::milliseconds(120);
        if (now >= *lspFlushAt_) {
            for (auto& d : docs_) flushDoc(*d);
            lspFlushAt_.reset();
        }
    }

    static std::pair<int, int> lspPos(const Document& d, Pos p) { return {p.line, utf16Col(d.buf.line(p.line), p.col)}; }

    // ---- completion

    static int matchScore(std::string_view prefix, std::string_view text) {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        if (prefix.empty()) return 0;
        if (text.starts_with(prefix)) return 0;
        auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
        bool prefixCI = text.size() >= prefix.size();
        for (std::size_t i = 0; prefixCI && i < prefix.size(); ++i) prefixCI = lower(text[i]) == lower(prefix[i]);
        if (prefixCI) return 1;
        std::size_t pi = 0;  // subsequence
        for (char c : text)
            if (pi < prefix.size() && lower(c) == lower(prefix[pi])) ++pi;
        return pi == prefix.size() ? 2 : -1;
    }

    void closeCompletion() { if (comp_.open) { comp_ = {}; dirty_ = true; } }

    void requestCompletion(int triggerKind) {
        if (docs_.empty()) return;
        if (isCppDoc(doc()) && completeImport(doc())) return;
        if (!lsp_ || !lsp_->ready()) return;
        Document& d = doc();
        if (!isCppDoc(d) || !d.lspOpen) return;
        flushDoc(d);
        const Pos at = d.cursor;
        const Document* dp = &d;
        const std::uint64_t seq = ++compSeq_;
        auto [l, c] = lspPos(d, at);
        lsp_->cancel(compReq_);  // a newer keystroke supersedes the pending request
        compReq_ = lsp_->completion(d.path, l, c, triggerKind, [this, seq, at, dp](std::vector<LspCompletion> items) {
            if (seq != compSeq_ || docs_.empty() || &doc() != dp) return;
            Document& d = doc();
            const std::string& ln = d.buf.line(at.line);
            int a = at.col;
            while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
            if (d.cursor.line != at.line || d.cursor.col < a) return;  // cursor moved away meanwhile
            showCompletion(std::move(items), at);
        });
    }

    void showCompletion(std::vector<LspCompletion> items, Pos at, int anchorCol = -1) {
        Document& d = doc();
        const std::string& ln = d.buf.line(at.line);
        int a = at.col;
        if (anchorCol >= 0) a = anchorCol;
        else while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
        std::ranges::stable_sort(items, [](const LspCompletion& x, const LspCompletion& y) { return x.sortText < y.sortText; });
        comp_ = {};
        comp_.all = std::move(items);
        comp_.anchor = {at.line, a};
        comp_.dotted = anchorCol >= 0;
        comp_.open = true;
        refilterCompletion();
        dirty_ = true;
    }

    // CMake files: completion is computed locally from the installed cmake's own name lists.
    void requestCMakeCompletion() {
        if (docs_.empty()) return;
        Document& d = doc();
        if (!cmakeKb_.ready()) cmakeKb_.load();
        std::string before, full;
        for (int i = 0; i < d.buf.lineCount(); ++i) {
            const std::string& l = d.buf.line(i);
            if (i < d.cursor.line) { before += l; before += '\n'; }
            else if (i == d.cursor.line) before += l.substr(0, static_cast<std::size_t>(d.cursor.col));
            full += l; full += '\n';
        }
        std::vector<std::string> targets;
        for (const auto& t : targets_) targets.push_back(t.name);
        std::vector<LspCompletion> items;
        for (auto& c : cmakeCompletions(cmakeKb_, before, full, targets)) {
            LspCompletion it;
            it.label = std::move(c.label);
            it.detail = std::move(c.detail);
            it.insertText = std::move(c.insert);
            it.sortText = std::move(c.sort);
            it.kind = c.kind;
            items.push_back(std::move(it));
        }
        if (items.empty()) { closeCompletion(); return; }
        showCompletion(std::move(items), d.cursor);
    }

    void refilterCompletion() {
        if (!comp_.open || docs_.empty()) return;
        Document& d = doc();
        if (d.cursor.line != comp_.anchor.line || d.cursor.col < comp_.anchor.col) { closeCompletion(); return; }
        const std::string& ln = d.buf.line(comp_.anchor.line);
        const std::string prefix = ln.substr(static_cast<std::size_t>(comp_.anchor.col), static_cast<std::size_t>(d.cursor.col - comp_.anchor.col));
        const bool dotted = comp_.dotted;
        if (!std::ranges::all_of(prefix, [dotted](char c) { return identChar(c) || (dotted && (c == '.' || c == ':')); })) { closeCompletion(); return; }
        std::vector<std::pair<int, int>> scored;
        for (int i = 0; i < static_cast<int>(comp_.all.size()); ++i) {
            const auto& it = comp_.all[static_cast<std::size_t>(i)];
            int sc = matchScore(prefix, it.filterText.empty() ? it.label : it.filterText);
            if (sc >= 0) scored.emplace_back(sc, i);
        }
        std::ranges::stable_sort(scored, [](auto& a, auto& b) { return a.first < b.first; });
        comp_.shown.clear();
        for (auto& [sc, i] : scored) { if (comp_.shown.size() >= 200) break; comp_.shown.push_back(i); }
        comp_.sel = 0;
        comp_.scroll = 0;
        if (comp_.shown.empty()) closeCompletion();
        dirty_ = true;
    }

    bool completionKey(const KeyEvent& k) {
        if (k.ctrl || k.alt) return false;
        const int n = static_cast<int>(comp_.shown.size());
        auto move = [&](int to) {
            comp_.sel = std::clamp(to, 0, std::max(0, n - 1));
            if (comp_.sel < comp_.scroll) comp_.scroll = comp_.sel;
            if (comp_.sel >= comp_.scroll + kCompletionRows) comp_.scroll = comp_.sel - kCompletionRows + 1;
            dirty_ = true;
        };
        switch (k.key) {
            case Key::Up: move(comp_.sel - 1); return true;
            case Key::Down: move(comp_.sel + 1); return true;
            case Key::PageUp: move(comp_.sel - kCompletionRows); return true;
            case Key::PageDown: move(comp_.sel + kCompletionRows); return true;
            case Key::Enter: case Key::Tab: acceptCompletion(); return true;
            case Key::Escape: closeCompletion(); return true;
            default: return false;
        }
    }

    void acceptCompletion() {
        if (!comp_.open || comp_.shown.empty() || docs_.empty()) { closeCompletion(); return; }
        Document& d = doc();
        const LspCompletion item = comp_.all[static_cast<std::size_t>(comp_.shown[static_cast<std::size_t>(comp_.sel)])];
        std::string text = item.insertText.empty() ? item.label : item.insertText;
        while (!text.empty() && text.front() == ' ') text.erase(0, 1);
        const bool isCall = item.kind == 2 || item.kind == 3 || item.kind == 4;  // method, function, constructor
        const std::string& ln = d.buf.line(d.cursor.line);
        bool addParens = isCall && text.find('(') == std::string::npos && !(d.cursor.col < static_cast<int>(ln.size()) && ln[static_cast<std::size_t>(d.cursor.col)] == '(');
        const bool hasParams = addParens && item.label.find('(') != std::string::npos && item.label.find("()") == std::string::npos &&
                               item.label.find("(void)") == std::string::npos;
        if (addParens) text += "()";
        d.select(comp_.anchor, d.cursor);
        d.insertRaw(text);
        if (hasParams || ((item.detail.starts_with("command") || item.detail.starts_with("function (this")) && text.ends_with("()")))
            d.setCursor({d.cursor.line, d.cursor.col - 1});
        closeCompletion();
        scrollToCursor_ = true;
        dirty_ = true;
    }

    // Runs after every editor key: keeps the popup in sync and opens it when typing identifiers.
    void afterEditorKey(const KeyEvent& k, std::uint64_t verBefore) {
        Document& d = doc();
        if (comp_.open) refilterCompletion();
        if (isCMakeDoc(d)) {
            if (k.ctrl && !k.alt && k.key == Key::Char && k.ch == ' ') { requestCMakeCompletion(); return; }
            if (k.ctrl || k.alt || k.text.empty() || d.buf.version() == verBefore) return;
            const char c = k.text.back();
            const std::string& ln = d.buf.line(d.cursor.line);
            const int col = d.cursor.col;
            if (identChar(c)) { if (!comp_.open) requestCMakeCompletion(); }
            else if ((c == '{' || c == '<') && col >= 2 && ln[static_cast<std::size_t>(col - 2)] == '$') requestCMakeCompletion();
            else if (c == ' ' || c == '(') requestCMakeCompletion();
            else closeCompletion();
            return;
        }
        if (isCppDoc(d) && !modules_.empty() && !k.ctrl && !k.alt && !k.text.empty() && d.buf.version() != verBefore) {
            const char c = k.text.back();
            if ((identChar(c) || c == '.' || c == ':' || c == ' ') &&
                importNameStart(d.buf.line(d.cursor.line), static_cast<std::size_t>(d.cursor.col))) {
                if (!comp_.open) requestCompletion(1);
                return;
            }
        }
        if (!lsp_ || !lsp_->ready() || !isCppDoc(d)) return;
        if (k.ctrl && !k.alt && k.key == Key::Char && k.ch == ' ') { requestCompletion(1); return; }
        if (k.ctrl || k.alt || k.text.empty() || d.buf.version() == verBefore) return;
        const char c = k.text.back();
        const std::string& ln = d.buf.line(d.cursor.line);
        const int col = d.cursor.col;
        if (identChar(c)) {
            if (!comp_.open) requestCompletion(1);
        } else if (c == '.' || (c == '>' && col >= 2 && ln[static_cast<std::size_t>(col - 2)] == '-') ||
                   (c == ':' && col >= 2 && ln[static_cast<std::size_t>(col - 2)] == ':')) {
            requestCompletion(2);
        } else {
            closeCompletion();
        }
    }

    struct KindIcon { char32_t glyph; Color color; };
    // LSP CompletionItemKind -> Remix icon + VS Code-like colour.
    static KindIcon completionKindIcon(int kind) {
        const Color purple = Color::rgb(0xb98aea), blue = Color::rgb(0x6cb6ff), orange = Color::rgb(0xf0a04b),
                    teal = Color::rgb(0x4ec9b0), gray = Color::rgb(0x9a9ea9), green = Color::rgb(0x7bc47f);
        switch (kind) {
            case 2: case 4: return {0xF01A7, purple};  // method, constructor: cube-outline
            case 3: return {0xF0295, purple};          // function
            case 5: case 10: return {0xF04FC, blue};   // field, property: tag-outline
            case 6: return {0xF0AE7, blue};            // variable
            case 7: return {0xF0832, orange};          // class: shape-outline
            case 22: return {0xF0B84, orange};         // struct: cube-scan
            case 8: return {0xF0339, teal};            // interface: link-variant
            case 13: return {0xF027A, orange};         // enum: format-list-bulleted-type
            case 20: return {0xF0316, blue};           // enum member: label-outline
            case 21: return {0xF01FD, blue};           // constant: equal-box
            case 9: return {0xF03D6, gray};            // module / namespace: package-variant
            case 14: return {0xF0DD6, gray};           // keyword: key-outline
            case 15: return {0xF10D6, green};          // snippet: code-braces-box
            case 1: return {0xF09ED, gray};            // text
            case 25: return {0xF0C26, teal};           // type parameter
            case 24: return {0xF065E, gray};           // operator
            case 23: return {0xF06D5, orange};         // event: flash-outline
            case 16: return {0xF0E0C, orange};         // colour: palette-outline
            case 17: return {0xF0224, gray};           // file
            case 19: return {0xF0256, gray};           // folder
            case 18: return {0xF1178, gray};           // reference
            default: return {0xF0A66, gray};           // unit / value / other: puzzle-outline
        }
    }

    static constexpr int kCompletionRows = 10;

    void drawCompletion() {
        if (!comp_.open || comp_.shown.empty() || docs_.empty()) return;
        Document& d = doc();
        const float s = S(), cell = cw(), line = lh(), rowH = std::round(line + 4 * s);
        const int n = static_cast<int>(comp_.shown.size());
        const int rows = std::min(n, kCompletionRows);
        // Size to the longest visible entry.
        std::size_t maxLabel = 0, maxDetail = 0;
        for (int i = comp_.scroll; i < comp_.scroll + rows; ++i) {
            const auto& it = comp_.all[static_cast<std::size_t>(comp_.shown[static_cast<std::size_t>(i)])];
            std::string_view lab = it.label;
            while (!lab.empty() && lab.front() == ' ') lab.remove_prefix(1);
            maxLabel = std::max(maxLabel, utf8Length(lab));
            maxDetail = std::max(maxDetail, utf8Length(it.detail));
        }
        maxDetail = std::min<std::size_t>(maxDetail, 28);
        const float iconW = std::round(26 * s);
        float w = iconW + static_cast<float>(std::min<std::size_t>(maxLabel, 60) + maxDetail + 4) * cell + 22 * s;
        w = std::min(w, L_.editor.w * 0.9f);
        const float h = static_cast<float>(rows) * rowH + 8 * s;
        const float sx = std::round(d.scrollX), sy = std::round(d.scrollY);
        float x = L_.textX - sx + static_cast<float>(displayCol(d.buf.line(comp_.anchor.line), comp_.anchor.col, d.tabWidth)) * cell - iconW;
        float y = L_.editor.y + static_cast<float>(comp_.anchor.line + 1) * line - sy + 2 * s;
        if (y + h > L_.editor.bottom()) y = L_.editor.y + static_cast<float>(comp_.anchor.line) * line - sy - h - 2 * s;  // flip above
        x = std::clamp(x, L_.editor.x + 4 * s, std::max(L_.editor.x + 4 * s, L_.editor.right() - w - 4 * s));
        const Rect r{x, y, w, h};
        glassPanel(r, 10 * s);
        for (int i = 0; i < rows; ++i) {
            const int idx = comp_.scroll + i;
            const auto& it = comp_.all[static_cast<std::size_t>(comp_.shown[static_cast<std::size_t>(idx)])];
            const float ry = r.y + 4 * s + static_cast<float>(i) * rowH;
            if (idx == comp_.sel) fill(r.x + 4 * s, ry, r.w - 8 * s, rowH, theme::accent.alpha(110), 6 * s);
            {
                const KindIcon ki = completionKindIcon(it.kind);
                const int ip = std::max(12, static_cast<int>(std::round(16 * s)));
                const Glyph& g = font_.icon(ki.glyph, ip);
                dl_.glyph(g, r.x + 10 * s, ry + (rowH - static_cast<float>(ip)) / 2 + font_.iconBaseline(ip), ki.color);
            }
            std::string lab = it.label;
            while (!lab.empty() && lab.front() == ' ') lab.erase(0, 1);
            const float ty = ry + (rowH - line) / 2;
            label(r.x + 10 * s + iconW - 2 * s, ty, lab, theme::text, r.right() - 10 * s - static_cast<float>(maxDetail) * cell);
            if (!it.detail.empty()) {
                std::string det = it.detail;
                if (utf8Length(det) > maxDetail) { det.resize(maxDetail); }
                label(r.right() - 10 * s - static_cast<float>(utf8Length(det)) * cell, ty, det, Color::rgb(0x7a7e88));
            }
        }
        if (n > rows) {
            const float tx = r.right() - 5 * s, th = (h - 8 * s) * static_cast<float>(rows) / static_cast<float>(n);
            const float ty = r.y + 4 * s + (h - 8 * s - th) * static_cast<float>(comp_.scroll) / static_cast<float>(n - rows);
            fill(tx, ty, 2.5f * s, th, theme::scrollbar, 1.2f * s);
        }
    }

    // ---- hover (diagnostics + clangd documentation)

    void hideHover() {
        if (hover_.pending || hover_.show) { hover_.pending = hover_.show = false; ++hoverSeq_; hsel_ = {}; dirty_ = true; }
    }

    void hoverMoved() {
        Document& d = doc();
        Pos p = posAt(d, mx_, my_);
        if ((hover_.pending || hover_.show) && p == hover_.pos) return;
        hover_.show = false;
        hover_.pos = p;
        hover_.px = mx_;
        hover_.py = my_;
        hover_.pending = true;
        hover_.due = Clock::now() + std::chrono::milliseconds(450);
        ++hoverSeq_;
        dirty_ = true;
    }

    static std::string cleanMarkdown(const std::string& md) {
        std::string out;
        std::istringstream in(md);
        for (std::string l; std::getline(in, l);) {
            if (l.starts_with("```")) continue;
            if (l.starts_with("### ")) l.erase(0, 4);
            out += l + "\n";
        }
        while (!out.empty() && out.back() == '\n') out.pop_back();
        return out;
    }

    void addHoverText(const std::string& text, int sev) {
        std::istringstream in(text);
        int n = 0;
        for (std::string l; std::getline(in, l) && n < 14; ++n) {
            while (l.size() > 100) {  // hard wrap
                std::size_t cut = l.rfind(' ', 100);
                if (cut == std::string::npos || cut < 40) cut = 100;
                hover_.lines.emplace_back(l.substr(0, cut), sev);
                l.erase(0, cut + (l[cut] == ' ' ? 1 : 0));
            }
            hover_.lines.emplace_back(l, sev);
        }
    }

    void hoverTick(Clock::time_point now) {
        if (!hover_.pending || now < hover_.due || docs_.empty()) return;
        hover_.pending = false;
        settleUntil_ = now + std::chrono::milliseconds(250);  // (screenshots wait for the reply)
        if (menu_.open || prompt_.kind != PromptKind::None || comp_.open) return;
        Document& d = doc();
        hover_.lines.clear();
        if (auto it = diags_.find(d.path); it != diags_.end()) {
            for (const auto& dg : it->second) {
                const std::string& sl = d.buf.line(std::clamp(dg.line, 0, d.buf.lineCount() - 1));
                Pos a{dg.line, byteColFromUtf16(sl, dg.col)};
                Pos b{dg.endLine, byteColFromUtf16(d.buf.line(std::clamp(dg.endLine, 0, d.buf.lineCount() - 1)), dg.endCol)};
                if (b <= a) b.col = a.col + 1;
                if (hover_.pos >= a && hover_.pos <= b) addHoverText(dg.message, dg.severity);
            }
        }
        const std::uint64_t seq = hoverSeq_;
        const Pos at = hover_.pos;
        if (lsp_ && lsp_->ready() && isCppDoc(d) && d.lspOpen) {
            flushDoc(d);
            auto [l, c] = lspPos(d, at);
            lsp_->hover(d.path, l, c, [this, seq, at](std::string text) {
                if (seq != hoverSeq_ || hover_.pos != at || text.empty()) return;
                if (!hover_.lines.empty()) hover_.lines.emplace_back("", 0);
                addHoverText(cleanMarkdown(text), 0);
                hover_.show = !hover_.lines.empty();
                dirty_ = true;
            });
        }
        if (dbg_ && dbgState_ == DbgState::Stopped && isCppDoc(d)) {   // value of the expression under the pointer while paused
            std::string expr;
            if (d.hasSelection() && at >= d.selBegin() && at <= d.selEnd() && d.selectedText().find('\n') == std::string::npos) expr = d.selectedText();
            else { auto [wa, wb] = d.wordRange(at); if (wa.line == wb.line && wb.col > wa.col) expr = d.buf.slice(wa, wb); }
            if (!expr.empty() && (std::isalpha(static_cast<unsigned char>(expr[0])) || expr[0] == '_' || d.hasSelection()))
                dbg_->send("-data-evaluate-expression " + gdb::quote(expr), [this, seq, at, expr](const gdb::Record& r) {
                    if (seq != hoverSeq_ || hover_.pos != at || !r.ok()) return;
                    if (!hover_.lines.empty()) hover_.lines.emplace_back("", 0);
                    hover_.lines.emplace_back(expr + " = " + r.data["value"].str(), 0);
                    hover_.show = true;
                    dirty_ = true;
                });
        }
        hover_.show = !hover_.lines.empty();
        dirty_ = true;
    }

    // ---- tooltip text selection

    Rect hoverRect_, hoverCopyRect_;

    std::pair<int, int> hoverPosAt(float x, float y) {
        const float s = S(), line = std::round(uiH() + 3 * s);
        const int n = static_cast<int>(hover_.lines.size());
        const int row = std::clamp(static_cast<int>(std::floor((y - (hoverRect_.y + 8 * s)) / line)), 0, std::max(0, n - 1));
        return {row, static_cast<int>(caretFromX(hover_.lines[static_cast<std::size_t>(row)].first, x - (hoverRect_.x + 13 * s)))};
    }
    void hoverSelAnchor() {
        const auto [r, c] = hoverPosAt(mx_, my_);
        hsel_.ar = hsel_.cr = r;
        hsel_.ac = hsel_.cc = c;
        hsel_.sx = mx_; hsel_.sy = my_;
    }
    void hoverSelUpdate() {
        const auto [r, c] = hoverPosAt(mx_, my_);
        hsel_.cr = r; hsel_.cc = c;
        if (std::hypot(mx_ - hsel_.sx, my_ - hsel_.sy) > 3 * S()) hsel_.moved = true;
    }

    void drawHover() {
        if (!hover_.show || hover_.lines.empty() || menu_.open) return;
        const float s = S(), line = std::round(uiH() + 3 * s);
        float tw = 0;
        for (const auto& [t, sev] : hover_.lines) tw = std::max(tw, uiW(t));
        const float w = tw + 26 * s + 24 * s, h = static_cast<float>(hover_.lines.size()) * line + 16 * s;
        const float W = L_.status.w, H = L_.status.bottom();
        float x = std::min(hover_.px + 12 * s, W - w - 6 * s);
        float y = hover_.py + 22 * s;
        if (y + h > H - 6 * s) y = hover_.py - h - 10 * s;
        x = std::max(6 * s, x);
        if (blameHover_ && blameRect_.w > 0) {   // blame card: sits right above (or below) the annotation
            y = blameRect_.y - h - 6 * s;
            if (y < 6 * s) y = blameRect_.bottom() + 6 * s;
            x = std::min(blameRect_.x, W - w - 6 * s);
        }
        glassPanel({x, y, w, h}, 10 * s);
        hoverRect_ = {x, y, w, h};
        float ty = y + 8 * s;
        int row = 0;
        int r0 = hsel_.ar, c0 = hsel_.ac, r1 = hsel_.cr, c1 = hsel_.cc;
        if (!selOrdered(hsel_)) { std::swap(r0, r1); std::swap(c0, c1); }
        for (const auto& [t, sev] : hover_.lines) {
            if (hsel_.active && hsel_.moved && row >= r0 && row <= r1) {
                const std::size_t a = row == r0 ? std::min<std::size_t>(static_cast<std::size_t>(c0), t.size()) : 0;
                const std::size_t e = row == r1 ? std::min<std::size_t>(static_cast<std::size_t>(c1), t.size()) : t.size();
                const float x0 = x + 13 * s + uiW(std::string_view(t).substr(0, a)), x1 = x + 13 * s + uiW(std::string_view(t).substr(0, e));
                if (x1 > x0) fill(x0, ty, x1 - x0, line, Color::rgb(0x3a4152));
            }
            uiLabel(x + 13 * s, ty, line, t, sev == 0 ? theme::text : diagColor(sev));
            ty += line;
            ++row;
        }
        // Copy-all button in the corner.
        const float bs = std::round(22 * s);
        hoverCopyRect_ = {x + w - bs - 6 * s, y + 6 * s, bs, bs};
        const bool hot = hoverCopyRect_.contains(mx_, my_);
        if (hot) fill(hoverCopyRect_, Color::rgb(0xffffff, 40), 6 * s);
        const int ip = std::max(12, static_cast<int>(std::round(bs * 0.7f)));
        Icons::glyph(dl_, font_, hoverCopyRect_.x + (bs - static_cast<float>(ip)) / 2, hoverCopyRect_.y + (bs - static_cast<float>(ip)) / 2, ip, 0xF018F, hot ? Color::rgb(0xffffff) : theme::dim);
    }

    // ---- navigation

    void jumpTo(const std::string& path, int line, int u16col) {
        openFile(path);
        if (docs_.empty()) return;
        Document& d = doc();
        const int l = std::clamp(line, 0, d.buf.lineCount() - 1);
        d.setCursor({l, byteColFromUtf16(d.buf.line(l), u16col)});
        scrollToCursor_ = true;
        sideFocus_ = false;
        dirty_ = true;
    }

    void gotoDefinition() {
        if (docs_.empty()) return;
        Document& d = doc();
        if (isCppDoc(d) && gotoModule(d)) return;  // `import ide.json;` jumps to the module interface
        if (!lsp_ || !lsp_->ready() || !isCppDoc(d) || !d.lspOpen) { setMessage("Language server not ready", true, 2.5f); return; }
        flushDoc(d);
        auto [l, c] = lspPos(d, d.cursor);
        const NavEntry from{d.path, d.cursor};
        lsp_->definition(d.path, l, c, [this, from](std::vector<LspLocation> locs) {
            if (locs.empty()) { setMessage("No definition found", true, 2.5f); return; }
            navBack_.push_back(from);
            if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
            jumpTo(locs[0].path, locs[0].line, locs[0].col);
        });
    }

    void navBack() {
        if (navBack_.empty()) { setMessage("Nothing to go back to", false, 1.5f); return; }
        NavEntry e = navBack_.back();
        navBack_.pop_back();
        openFile(e.path);
        if (!docs_.empty()) { doc().setCursor(e.pos); scrollToCursor_ = true; }
    }

    void switchHeaderSource() {
        if (docs_.empty() || doc().path.empty()) return;
        const fs::path p(doc().path);
        static const std::array<std::string_view, 8> headers = {".h", ".hpp", ".hh", ".hxx", ".h++", ".inl", ".ipp", ".tpp"};
        static const std::array<std::string_view, 8> sources = {".cpp", ".cc", ".cxx", ".c", ".c++", ".cppm", ".ixx", ".mm"};
        const std::string ext = p.extension().string();
        const bool isHeader = std::ranges::find(headers, ext) != headers.end();
        std::vector<fs::path> dirs = {p.parent_path()};
        for (auto sib : {"src", "include", "source", "inc"}) dirs.push_back(p.parent_path().parent_path() / sib);
        std::error_code ec;
        for (const auto& dir : dirs)
            for (auto e : (isHeader ? std::span<const std::string_view>(sources) : std::span<const std::string_view>(headers))) {
                fs::path cand = dir / (p.stem().string() + std::string(e));
                if (fs::is_regular_file(cand, ec)) { openFile(cand.string()); return; }
            }
        if (lsp_ && lsp_->ready() && isCppDoc(doc())) {
            lsp_->switchSourceHeader(doc().path, [this](std::string other) {
                if (other.empty()) setMessage("No matching header/source file", true, 2.5f); else openFile(other);
            });
            return;
        }
        setMessage("No matching header/source file", true, 2.5f);
    }

    // ============================================================ CMake: configure / build / run

    CMakeProfile& profile() { return profiles_[static_cast<std::size_t>(profile_)]; }

    // Called when a folder is opened: enables the toolbar and loads (or creates) the build configuration.
    void detectCMake() {
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

    void loadTargetsNow() {
        std::string keep = target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : std::string();
        targets_ = loadTargets(profile().dir);
        target_ = -1;
        for (std::size_t i = 0; i < targets_.size(); ++i)
            if (targets_[i].name == keep) target_ = static_cast<int>(i);
        if (target_ < 0 && !targets_.empty()) target_ = 0;  // executables sort first
        dirty_ = true;
    }

    void selectProfile(int i) {
        if (i == profile_ || i < 0 || i >= static_cast<int>(profiles_.size())) return;
        if (buildProc_) { setMessage("Stop the running build first", true); return; }
        profile_ = i;
        targets_.clear();
        target_ = -1;
        if (isConfigured(profile())) loadTargetsNow();
        if (targets_.empty()) cmakeReload(); else { buildStatus_ = "CMake ready"; restartLsp(); }
        dirty_ = true;
    }

    void saveAllForBuild() {
        for (auto& d : docs_)
            if (d->modified() && !d->path.empty()) { flushDoc(*d); (void)d->save(); if (lsp_ && d->lspOpen) lsp_->didSave(d->path); }
    }

    void startBuildProc(TaskKind kind, std::vector<std::string> argv, const std::string& title) {
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

    void cmakeReload() {
        if (!cmakeOpen_) return;
        if (buildProc_) { setMessage("A build is already running", true); return; }
        saveAllForBuild();
        prepareFileApi(profile().dir);
        startBuildProc(TaskKind::Configure, configureArgv(cmakeRoot_, profile(), haveNinja_), "Configuring (" + profile().name + ")");
    }

    void cmakeBuild(bool thenRun, bool debug = false) {
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

    void cmakeStop() {
        debugRun_ = false;
        pendingBuild_ = false;
        runAfterBuild_ = false;
        if (buildProc_) buildProc_->terminate();
        if (dbg_) debugStop();
        if (runProc_) runProc_->terminate();
    }

    bool tasksRunning() const { return buildProc_ || runProc_; }

    // Terminal text -> plain text: strips ANSI sequences and applies backspaces (the echo of a deleted character
    // arrives as "\b \b"); other control characters are dropped.
    static std::string cleanTerm(std::string_view raw) {
        const std::string t = stripAnsi(raw);
        std::string out;
        for (char c : t) {
            if (c == '\b') { if (!out.empty()) out.erase(utf8Prev(out, out.size())); }
            else if (c == '\t' || static_cast<unsigned char>(c) >= 0x20) out += c;
        }
        return out;
    }

    static std::vector<std::string> takeLines(std::string& partial, std::string_view chunk) {
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

    void addBuildLine(const std::string& raw) {
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

    void pumpBuild() {
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

    void finishBuild(int code) {
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

    void startRun(bool debug = false) {
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

    void pumpRun() {
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

    void finishRun(int code) {
        if (!runPartial_.empty()) { runOut_.push_back({cleanTerm(runPartial_), 0, -1}); runPartial_.clear(); }
        runFocus_ = false;
        runOut_.push_back({"", 0, -1});
        runOut_.push_back({std::format("Process finished with exit code {}", code), code == 0 ? 5 : 2, -1});
        runProc_.reset();
        runEof_ = false;
        buildStatus_ = code == 0 ? "Run finished" : std::format("Run exited with code {}", code);
        dirty_ = true;
    }

    // Detects finished child processes (their output has reached EOF).
    void pollTasks() {
        if (buildProc_ && buildEof_) { if (auto c = buildProc_->tryWait()) finishBuild(*c); }
        if (runProc_ && runEof_) { if (auto c = runProc_->tryWait()) finishRun(*c); }
    }

    // Problems tab content: compiler / CMake problems from the last build plus clangd's live diagnostics
    // (errors, warnings and notes; hints are left out), ordered by file and line.
    void refreshProblems() {
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

    void gotoProblem(const Problem& p) {
        openFile(p.file.string());
        if (docs_.empty()) return;
        Document& d = doc();
        d.gotoLine(p.line - 1, 0);
        d.setCursor({d.cursor.line, byteColAt(d.buf.line(d.cursor.line), std::max(0, p.col - 1), d.tabWidth)});
        scrollToCursor_ = true;
        sideFocus_ = false;
        dirty_ = true;
    }

    // ---- toolbar

    void togglePanel() {
        panelOpen_ = !panelOpen_;
        dirty_ = true;
    }

    void toolbarClick() {
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

    int toolAt(float x, float y) const {
        if (!cmakeOpen_) return 0;
        const Rect* rs[] = {&tool_.profile, &tool_.target, &tool_.run, &tool_.build, &tool_.stop, &tool_.reload, &tool_.panel, &tool_.debug};
        for (int i = 0; i < 8; ++i)
            if (rs[i]->contains(x, y)) return i + 1;
        return 0;
    }

    void drawToolbar() {
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
        float rx = L_.toolbar.w - m - std::round(6 * s) - (L_.csd ? std::round(L_.title.h * 1.25f) * 3 + 6 * s : 0.0f);
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

    // Tooltip for the toolbar buttons (name and shortcut), shown after the pointer rests on one.
    void drawToolTip() {
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

    // ---- output panel

    const std::vector<OutLine>& panelLines() const { return panelTab_ == 1 ? runOut_ : buildOut_; }

    static Color outColor(int sev) {
        switch (sev) {
            case 1: return Color::rgb(0xe5c07b);
            case 2: return Color::rgb(0xf2777a);
            case 3: return Color::rgb(0x7fb7ff);
            case 4: return theme::dim;
            case 5: return Color::rgb(0x6aab73);
            default: return theme::text;
        }
    }

    std::size_t panelRowCount() const {
        if (panelTab_ == 2) return shownProblems_.size();
        if (panelTab_ == 4) return dbgCon_.size();
        return panelLines().size() + (panelTab_ == 1 && runProc_ ? 1 : 0);   // + the line the program is still writing
    }
    float panelHeadH() const { return std::round(font_.uiLineHeight() + 14 * S()); }
    Rect panelBody() const {
        const Rect& p = L_.panel;
        return {p.x, p.y + panelHeadH(), p.w, std::max(0.0f, p.h - panelHeadH())};
    }
    float panelMaxScroll() const { return std::max(0.0f, static_cast<float>(panelRowCount()) * lh() - panelBody().h); }

    // ---- selecting and copying output text

    struct TextSel {
        bool active = false, dragging = false, moved = false;
        int tab = -1;
        int ar = 0, ac = 0, cr = 0, cc = 0;   // anchor / cursor: row and byte column
        float sx = 0, sy = 0;
    };
    TextSel psel_, hsel_;

    std::string panelRowText(int i) const {
        if (panelTab_ == 2) {
            const Problem& pr = shownProblems_[static_cast<std::size_t>(i)];
            std::string rel = pr.file.lexically_relative(cmakeRoot_).string();
            if (rel.empty() || rel.starts_with("..")) rel = pr.file.string();
            return std::format("{}:{}:{}  {}", rel, pr.line, pr.col, pr.message);
        }
        if (panelTab_ == 1 && static_cast<std::size_t>(i) >= runOut_.size()) return cleanTerm(runPartial_);
        return panelLines()[static_cast<std::size_t>(i)].text;
    }

    // Row and byte column under a pointer position inside the panel body.
    std::pair<int, int> panelPosAt(float x, float y) const {
        const Rect body = panelBody();
        const int total = static_cast<int>(panelRowCount());
        if (total == 0) return {0, 0};
        int row = static_cast<int>(std::floor((y - body.y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
        row = std::clamp(row, 0, total - 1);
        const int cell = std::max(0, static_cast<int>(std::lround((x - (body.x + 14 * S())) / cw())));
        return {row, byteColAt(panelRowText(row), cell, 4)};
    }

    static bool selOrdered(const TextSel& t) { return t.ar < t.cr || (t.ar == t.cr && t.ac <= t.cc); }
    static std::string selectedOf(const TextSel& t, const std::function<std::string(int)>& rowText, int rows) {
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

    // Ctrl+C: output selection first, then the hover popup. Returns true when it handled the key.
    bool copyOverlay() {
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

    void panelRowClick(int row) {
        if (panelTab_ == 2) {
            if (row >= 0 && row < static_cast<int>(shownProblems_.size())) gotoProblem(shownProblems_[static_cast<std::size_t>(row)]);
        } else {
            const auto& lines = panelLines();
            if (row >= 0 && row < static_cast<int>(lines.size()) && lines[static_cast<std::size_t>(row)].problem >= 0)
                gotoProblem(problems_[static_cast<std::size_t>(lines[static_cast<std::size_t>(row)].problem)]);
        }
    }

    void panelClick(Button b) {
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

    void drawPanel() {
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
        if (runProc_ && panelTab_ == 1)
            uiLabelR(panelClear_.x - 14 * s, p.y, hh, runFocus_ ? "Typing goes to the program  \xC2\xB7  Esc to release" : "Click here to type input", runFocus_ ? Color::rgb(0xe6e8ec) : theme::lineNum);
        const int ci = std::max(8, static_cast<int>(std::round(uiH() * 0.7f)));
        Icons::tool(dl_, font_, std::round(panelClose_.x + (xs - static_cast<float>(ci)) / 2), std::round(panelClose_.y + (xs - static_cast<float>(ci)) / 2), ci,
                    ToolIcon::Close, theme::dim);
    }

    // ============================================================ file operations (sidebar)

    // Directory that new items / pastes go into: the selected folder, or the selected file's parent.
    fs::path targetDir() const {
        const std::string& sel = explorer_.selected();
        if (sel.empty()) return explorer_.rootPath();
        std::error_code ec;
        fs::path p(sel);
        return fs::is_directory(p, ec) ? p : p.parent_path();
    }

    bool isItem(const fs::path& p) const { return !p.empty() && p != explorer_.rootPath(); }

    std::string relToRoot(const fs::path& p) const {
        auto r = p.lexically_relative(explorer_.rootPath());
        return r.empty() || r == "." ? explorer_.rootName() : r.string();
    }

    // New file / new folder / rename / delete are asked for in a card at the bottom of the sidebar (not in the editor).
    bool sidePrompt() const {
        return prompt_.kind == PromptKind::NewFile || prompt_.kind == PromptKind::NewFolder || prompt_.kind == PromptKind::Rename || prompt_.kind == PromptKind::Delete ||
               prompt_.kind == PromptKind::DbgWatch || prompt_.kind == PromptKind::DbgSetValue || prompt_.kind == PromptKind::DbgBpEdit || prompt_.kind == PromptKind::DbgFuncBp;
    }
    float sideCardH() const { return sidePrompt() ? std::round(L_.rowH * 2 + 22 * S()) : 0; }

    void drawSidePrompt() {
        if (L_.side.w <= 0 || prompt_.fields.empty()) return;
        const float s = S(), rh = L_.rowH, hair = std::max(1.0f, std::round(0.8f * s));
        const Rect card{L_.side.x + 8 * s, L_.side.bottom() - sideCardH() + 4 * s, L_.side.w - 16 * s, sideCardH() - 12 * s};
        fill(card, theme::accent.alpha(90), 10 * s);
        fill(card.x + hair, card.y + hair, card.w - 2 * hair, card.h - 2 * hair, Color::rgb(0x0d0e11), 10 * s - hair);
        const Field& f = prompt_.fields[0];
        const float pad = 12 * s;
        if (prompt_.kind == PromptKind::Delete) {
            uiLabel(card.x + pad, card.y + 4 * s, rh, f.label, theme::text, card.right() - pad);
            uiLabel(card.x + pad, card.y + 4 * s + rh, rh, "Enter - confirm    Esc - cancel", theme::dim, card.right() - pad);
            return;
        }
        uiLabel(card.x + pad, card.y + 4 * s, rh, f.label, theme::dim, card.right() - pad);
        const Rect in{card.x + 8 * s, card.y + 4 * s + rh, card.w - 16 * s, rh - 6 * s};
        fill(in, Color::rgb(0xffffff, 18), 6 * s);
        const float x = in.x + 8 * s, maxX = in.right() - 8 * s;
        std::size_t startByte = 0;
        while (startByte < f.cur && uiW(std::string_view(f.text).substr(startByte, f.cur - startByte)) > maxX - x - 4 * s)
            startByte = utf8Next(f.text, startByte);
        const std::string_view shown = std::string_view(f.text).substr(startByte);
        if (f.fresh)   // prefilled name: selected until the first edit
            fill(x - 2 * s, in.y + 3 * s, std::min(uiW(shown), maxX - x) + 4 * s, in.h - 6 * s, theme::selection, 3 * s);
        uiLabel(x, in.y, in.h, shown, theme::text, maxX);
        if (caretOn_ && focused_) {
            const float cx = x + uiW(std::string_view(f.text).substr(startByte, f.cur - startByte));
            fill(std::round(cx), in.y + 5 * s, std::max(2.0f, std::round(1.4f * s)), in.h - 10 * s, theme::caret, s);
        }
    }

    // ---- dragging files inside the explorer to move them
    struct FileDrag {
        fs::path path;
        bool pending = false, active = false;
        float sx = 0, sy = 0;
        fs::path target;   // folder under the pointer (empty: not a valid drop)
    };
    FileDrag fdrag_;

    fs::path fileDropTarget() {
        if (!fdrag_.active || !L_.side.contains(mx_, my_) || !explorer_.isOpen()) return {};
        fs::path dir = explorer_.rootPath();
        if (L_.sideList.contains(mx_, my_)) {
            const auto& rows = explorer_.rows();
            const int row = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
            if (row >= 0 && row < static_cast<int>(rows.size())) {
                const FileNode& n = *rows[static_cast<std::size_t>(row)].node;
                dir = n.dir ? n.path : n.path.parent_path();
            }
        } else return {};
        if (dir == fdrag_.path.parent_path()) return {};
        if (dir == fdrag_.path || fsops::isInside(fdrag_.path, dir)) return {};
        return dir;
    }

    void finishFileDrag() {
        const fs::path src = fdrag_.path, dir = fdrag_.target;
        fdrag_ = {};
        dirty_ = true;
        if (dir.empty()) return;
        auto r = fsops::moveInto(src, dir);
        if (!r) { setMessage(r.error(), true); return; }
        retargetDocs(src, *r);
        afterFsChange(*r);
        setMessage("Moved " + src.filename().string() + " to " + relToRoot(dir) + "/");
    }

    void drawFileDrag() {
        if (!fdrag_.active) return;
        const float s = S();
        const std::string name = fdrag_.path.filename().string();
        const float h = std::round(uiH() + 12 * s), w = uiW(name) + iconSize() + 36 * s;
        const Rect r{mx_ + 12 * s, my_ + 8 * s, w, h};
        glassPanel(r, 9 * s);
        std::error_code ec;
        if (fs::is_directory(fdrag_.path, ec)) drawFolderIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name, false);
        else drawFileIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name);
        uiLabel(r.x + 12 * s + iconSize() + 8 * s, r.y, h, name, theme::text, r.right() - 10 * s);
    }

    void openFileOp(PromptKind kind, const fs::path& target, std::string label, std::string text = {}, bool fresh = false) {
        Prompt p;
        p.kind = kind;
        Field f;
        f.label = std::move(label);
        f.text = std::move(text);
        f.cur = f.text.size();
        f.fresh = fresh && !f.text.empty();
        p.fields.push_back(std::move(f));
        prompt_ = std::move(p);
        fileOpPath_ = target;
        overwriteArmed_.clear();
        const bool dbgPrompt = kind == PromptKind::DbgWatch || kind == PromptKind::DbgSetValue || kind == PromptKind::DbgBpEdit || kind == PromptKind::DbgFuncBp;
        if (dbgPrompt) { sidebar_ = true; }
        else if (!sidebar_ || sideGit_ || sideSearch_ || sideDebug_) { sidebar_ = true; sideGit_ = false; sideSearch_ = false; sideDebug_ = false; sideDebug_ = false; }
        sideFocus_ = false;
        dirty_ = true;
    }

    void beginNewFile(const fs::path& dir) { openFileOp(PromptKind::NewFile, dir, "New file in " + relToRoot(dir) + "/"); }
    void beginNewFolder(const fs::path& dir) { openFileOp(PromptKind::NewFolder, dir, "New folder in " + relToRoot(dir) + "/"); }
    void beginRename(const fs::path& p) {
        if (isItem(p)) openFileOp(PromptKind::Rename, p, "Rename", p.filename().string(), true);
    }
    void beginDelete(const fs::path& p, bool forever) {
        if (!isItem(p)) return;
        deleteForever_ = forever;
        openFileOp(PromptKind::Delete, p, std::string(forever ? "Permanently delete '" : "Move to Trash '") + p.filename().string() + "'? Enter = yes, Esc = no");
    }

    void stageClipboard(const fs::path& p, bool cut) {
        if (!isItem(p)) return;
        fileClip_ = {{p}, cut};
        setMessage(std::string(cut ? "Cut " : "Copied ") + p.filename().string(), false, 2);
    }

    // Refreshes the tree and (optionally) selects `focus`.
    void afterFsChange(const fs::path& focus) {
        explorer_.refresh();
        if (!focus.empty()) explorer_.reveal(focus);
        sideReveal_ = true;
        dirty_ = true;
    }

    // Keeps open documents attached to files that were renamed / moved.
    void retargetDocs(const fs::path& from, const fs::path& to) {
        for (auto& d : docs_) {
            if (d->path.empty()) continue;
            fs::path dp(d->path);
            if (dp == from) d->setPath(to.string());
            else if (fsops::isInside(from, dp)) d->setPath((to / dp.lexically_relative(from)).string());
        }
    }

    void pasteFiles(const fs::path& dir) {
        if (fileClip_.paths.empty()) { setMessage("Nothing to paste", true); return; }
        fs::path last;
        int done = 0;
        for (const auto& src : fileClip_.paths) {
            std::error_code ec;
            if (!fs::exists(src, ec)) continue;
            auto r = fileClip_.cut ? fsops::moveInto(src, dir) : fsops::copyInto(src, dir);
            if (!r) { setMessage(r.error(), true); break; }
            if (fileClip_.cut && *r != src) retargetDocs(src, *r);
            last = *r;
            ++done;
        }
        if (fileClip_.cut && done > 0) fileClip_ = {};
        afterFsChange(last);
        if (done > 0) setMessage(std::format("Pasted {} item{}", done, done == 1 ? "" : "s"));
    }

    void duplicateItem(const fs::path& p) {
        if (!isItem(p)) return;
        auto r = fsops::copyInto(p, p.parent_path());
        if (!r) { setMessage(r.error(), true); return; }
        afterFsChange(*r);
        setMessage("Created " + r->filename().string());
    }

    void finishFileOp(const std::string& text) {
        const PromptKind kind = prompt_.kind;
        const fs::path target = fileOpPath_;
        closePrompt();
        switch (kind) {
            case PromptKind::NewFile: {
                auto r = fsops::createFile(target, text);
                if (!r) { setMessage(r.error(), true); break; }
                afterFsChange(*r);
                openFile(r->string());
                sideFocus_ = false;
                break;
            }
            case PromptKind::NewFolder: {
                auto r = fsops::createFolder(target, text);
                if (!r) { setMessage(r.error(), true); break; }
                afterFsChange(*r);
                setMessage("Created folder " + r->filename().string());
                break;
            }
            case PromptKind::Rename: {
                auto r = fsops::rename(target, text);
                if (!r) { setMessage(r.error(), true); break; }
                retargetDocs(target, *r);
                afterFsChange(*r);
                setMessage("Renamed to " + r->filename().string());
                break;
            }
            case PromptKind::Delete: {
                auto r = deleteForever_ ? fsops::removeForever(target) : fsops::trash(target);
                if (!r) { setMessage(r.error(), true); break; }
                explorer_.select(target.parent_path().string());
                afterFsChange({});
                setMessage((deleteForever_ ? "Deleted " : "Moved to Trash: ") + target.filename().string());
                break;
            }
            default: break;
        }
    }

    // ---- context menu

    void closeMenu() { menu_.open = false; dirty_ = true; }

    void openMenu(float x, float y, const std::string* path) {
        menu_ = {};
        menu_.open = true;
        menu_.target = path ? *path : std::string();
        std::error_code ec;
        const bool has = path != nullptr, isDir = has && fs::is_directory(*path, ec);
        const bool canPaste = !fileClip_.paths.empty();
        auto add = [&](std::string l, std::string h, Act a, bool en = true) { menu_.items.push_back({std::move(l), std::move(h), a, en}); };
        auto sep = [&] { add("", "", Act::Sep); };
        if (has) {
            if (!isDir) { add("Open", "Enter", Act::Open); sep(); }
            else { add("New File...", "Ctrl+N", Act::NewFile); add("New Folder...", "Ctrl+Shift+N", Act::NewFolder); sep(); }
            add("Cut", "Ctrl+X", Act::Cut);
            add("Copy", "Ctrl+C", Act::Copy);
            add("Paste", "Ctrl+V", Act::Paste, canPaste);
            add("Duplicate", "Ctrl+D", Act::Duplicate);
            sep();
            add("Rename...", "F2", Act::Rename);
            add("Move to Trash", "Del", Act::Trash);
            add("Delete Permanently", "Shift+Del", Act::DeleteForever);
            sep();
            add("Copy Path", "", Act::CopyPath);
            add("Copy Relative Path", "", Act::CopyRelPath);
            sep();
            add("Refresh", "F5", Act::Refresh);
        } else {
            add("New File...", "Ctrl+N", Act::NewFile);
            add("New Folder...", "Ctrl+Shift+N", Act::NewFolder);
            sep();
            add("Paste", "Ctrl+V", Act::Paste, canPaste);
            sep();
            add("Refresh", "F5", Act::Refresh);
        }
        layoutMenu(x, y);
    }

    void layoutMenu(float x, float y) {
        const float s = S();
        menu_.itemH = std::round(uiH() + 12 * s);
        menu_.sepH = std::round(11 * s);
        float maxLabel = 0, maxHint = 0, h = 12 * s;
        for (const auto& it : menu_.items) {
            maxLabel = std::max(maxLabel, uiW(it.label));
            maxHint = std::max(maxHint, uiW(it.hint));
            h += it.act == Act::Sep ? menu_.sepH : menu_.itemH;
        }
        const float w = maxLabel + (maxHint > 0 ? maxHint + 36 * s : 0) + 44 * s;
        const float W = L_.status.w, H = L_.status.bottom();
        menu_.rect = {std::clamp(x, 4 * s, std::max(4 * s, W - w - 4 * s)), std::clamp(y, 4 * s, std::max(4 * s, H - h - 4 * s)), w, h};
        dirty_ = true;
    }

    // A pop-up list of arbitrary actions (profile / target selectors).
    void openCustomMenu(float x, float y, const std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>>& entries) {
        menu_ = {};
        menu_.open = true;
        menuActions_.clear();
        for (const auto& [label, hint, checked, fn] : entries) {
            menu_.items.push_back({(checked ? std::string("\xE2\x9C\x93  ") : std::string("    ")) + label, hint, Act::Custom, true,
                                   static_cast<int>(menuActions_.size())});
            menuActions_.push_back(fn);
        }
        layoutMenu(x, y);
    }

    int menuItemAt(float px, float py) const {
        if (!menu_.open || !menu_.rect.contains(px, py)) return -1;
        float y = menu_.rect.y + 6 * S();
        for (std::size_t i = 0; i < menu_.items.size(); ++i) {
            float h = menu_.items[i].act == Act::Sep ? menu_.sepH : menu_.itemH;
            if (py >= y && py < y + h) return menu_.items[i].act == Act::Sep ? -1 : static_cast<int>(i);
            y += h;
        }
        return -1;
    }

    void runAct(Act a, const std::string& target) {
        const fs::path t(target);
        std::error_code ec;
        const bool has = !target.empty();
        const fs::path dir = has ? (fs::is_directory(t, ec) ? t : t.parent_path()) : explorer_.rootPath();
        switch (a) {
            case Act::Open: openFile(target); sideFocus_ = false; break;
            case Act::NewFile: beginNewFile(dir); break;
            case Act::NewFolder: beginNewFolder(dir); break;
            case Act::Rename: beginRename(t); break;
            case Act::Trash: beginDelete(t, false); break;
            case Act::DeleteForever: beginDelete(t, true); break;
            case Act::Copy: stageClipboard(t, false); break;
            case Act::Cut: stageClipboard(t, true); break;
            case Act::Paste: pasteFiles(dir); break;
            case Act::Duplicate: duplicateItem(t); break;
            case Act::CopyPath: win_.setClipboard(target); setMessage("Copied path", false, 1.5f); break;
            case Act::CopyRelPath: win_.setClipboard(relToRoot(t)); setMessage("Copied relative path", false, 1.5f); break;
            case Act::Refresh: explorer_.refresh(); setMessage("Refreshed", false, 1.5f); break;
            case Act::Sep: case Act::Custom: break;
        }
        dirty_ = true;
    }

    void activateMenuItem(int i) {
        if (i < 0 || i >= static_cast<int>(menu_.items.size()) || !menu_.items[static_cast<std::size_t>(i)].enabled) return;
        const MenuItem item = menu_.items[static_cast<std::size_t>(i)];
        std::string target = menu_.target;
        closeMenu();
        if (item.act == Act::Custom) {
            if (item.arg >= 0 && item.arg < static_cast<int>(menuActions_.size())) { auto fn = menuActions_[static_cast<std::size_t>(item.arg)]; fn(); }
            return;
        }
        runAct(item.act, target);
    }

    void menuKey(const KeyEvent& k) {
        auto step = [&](int dir) {
            const int n = static_cast<int>(menu_.items.size());
            int i = menu_.hover;
            for (int tries = 0; tries < n; ++tries) {
                i = ((i + dir) % n + n) % n;
                if (menu_.items[static_cast<std::size_t>(i)].act != Act::Sep && menu_.items[static_cast<std::size_t>(i)].enabled) { menu_.hover = i; return; }
            }
        };
        switch (k.key) {
            case Key::Down: if (menu_.hover < 0) menu_.hover = -1; step(1); break;
            case Key::Up: if (menu_.hover < 0) menu_.hover = 0; step(-1); break;
            case Key::Enter: activateMenuItem(menu_.hover); break;
            case Key::Escape: closeMenu(); break;
            default: break;
        }
    }

    void drawMenu() {
        if (!menu_.open) return;
        const Rect& r = menu_.rect;
        const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
        glassPanel(r, 11 * s);
        float y = r.y + 6 * s;
        for (std::size_t i = 0; i < menu_.items.size(); ++i) {
            const MenuItem& it = menu_.items[i];
            if (it.act == Act::Sep) {
                fill(r.x + 10 * s, y + std::round(menu_.sepH / 2), r.w - 20 * s, hair, theme::border);
                y += menu_.sepH;
                continue;
            }
            if (static_cast<int>(i) == menu_.hover && it.enabled) fill(r.x + 5 * s, y, r.w - 10 * s, menu_.itemH, theme::selection, 6 * s);
            const Color tc = it.enabled ? (it.act == Act::DeleteForever || it.act == Act::Trash ? Color::rgb(0xe8a09f) : theme::text) : Color::rgb(0x5d606a);
            uiLabel(r.x + 16 * s, y, menu_.itemH, it.label, tc);
            if (!it.hint.empty()) uiLabelR(r.right() - 16 * s, y, menu_.itemH, it.hint, Color::rgb(0x7a7e88));
            y += menu_.itemH;
        }
    }

    // Keyboard navigation while the sidebar has focus. Returns false for keys it does not own
    // (Ctrl/Alt shortcuts), which then fall through to the normal handlers.
    bool explorerKey(const KeyEvent& k) {
        if (!explorer_.isOpen() || k.key >= Key::F6) return false;
        const fs::path sel = explorer_.selected().empty() ? fs::path() : fs::path(explorer_.selected());
        if (k.ctrl && !k.alt && k.key == Key::Char) {
            switch (k.ch) {
                case 'c': stageClipboard(sel, false); return true;
                case 'x': stageClipboard(sel, true); return true;
                case 'v': pasteFiles(targetDir()); return true;
                case 'd': duplicateItem(sel); return true;
                case 'n': if (k.shift) beginNewFolder(targetDir()); else beginNewFile(targetDir()); return true;
                default: return false;
            }
        }
        if (k.ctrl || k.alt) return false;
        const auto& rows = explorer_.rows();
        const int n = static_cast<int>(rows.size());
        int i = explorer_.indexOfSelected();
        auto go = [&](int to) {
            if (n == 0) return;
            to = std::clamp(to, 0, n - 1);
            explorer_.select(rows[static_cast<std::size_t>(to)].node->path.string());
            sideReveal_ = true;
        };
        switch (k.key) {
            case Key::Up: go(i < 0 ? 0 : i - 1); break;
            case Key::Down: go(i + 1); break;
            case Key::Home: go(0); break;
            case Key::End: go(n - 1); break;
            case Key::PageUp: go(i - 10); break;
            case Key::PageDown: go(i + 10); break;
            case Key::Right:
                if (i >= 0 && rows[static_cast<std::size_t>(i)].node->dir) {
                    FileNode* nd = rows[static_cast<std::size_t>(i)].node;
                    if (!nd->expanded) explorer_.setExpanded(*nd, true); else go(i + 1);
                }
                break;
            case Key::Left:
                if (i >= 0) {
                    FileNode* nd = rows[static_cast<std::size_t>(i)].node;
                    if (nd->dir && nd->expanded) explorer_.setExpanded(*nd, false);
                    else if (FileNode* parent = explorer_.parentOf(*nd)) { explorer_.select(parent->path.string()); sideReveal_ = true; }
                }
                break;
            case Key::Enter:
                if (i >= 0) activateRow(*rows[static_cast<std::size_t>(i)].node);
                break;
            case Key::F5: explorer_.refresh(); setMessage("Refreshed", false, 1.5f); break;
            case Key::F2: beginRename(sel); break;
            case Key::Delete: beginDelete(sel, k.shift); break;
            case Key::Escape: case Key::Tab: sideFocus_ = false; break;
            default: break;  // swallow everything else so typing does not leak into the editor
        }
        return true;
    }

    void activateRow(FileNode& node) {
        explorer_.select(node.path.string());
        if (node.dir) { explorer_.toggle(node); return; }
        openFile(node.path.string());
        sideFocus_ = false;
    }

    void editorKey(const KeyEvent& k) {
        if (comp_.open && completionKey(k)) { scrollToCursor_ = true; return; }
        hideHover();
        const Document* dp = &doc();
        const std::uint64_t verBefore = doc().buf.version();
        editorKeyImpl(k);
        if (docs_.empty() || &doc() != dp) { closeCompletion(); return; }
        afterEditorKey(k, verBefore);
    }

    void editorKeyImpl(const KeyEvent& k) {
        Document& d = doc();
        const bool shift = k.shift, ctrl = k.ctrl, alt = k.alt;
        scrollToCursor_ = true;

        if (k.key == Key::F12) { gotoDefinition(); scrollToCursor_ = false; return; }
        if (alt && !ctrl && k.key == Key::Char && k.ch == 'o') { switchHeaderSource(); scrollToCursor_ = false; return; }
        if (alt && !ctrl && k.key == Key::Left) { navBack(); return; }
        if (debugKey(k)) { scrollToCursor_ = false; return; }
        if (k.key == Key::F9 && ctrl) { cmakeBuild(false); scrollToCursor_ = false; return; }
        if (k.key == Key::F10 && shift) { cmakeBuild(true); scrollToCursor_ = false; return; }
        if (k.key == Key::F9 && shift) { cmakeBuild(true, true); scrollToCursor_ = false; return; }
        if (k.key == Key::F2 && ctrl) { cmakeStop(); scrollToCursor_ = false; return; }

        if (ctrl && !alt && k.key == Key::Char) {
            switch (k.ch) {
                case 'r': if (shift) cmakeReload(); scrollToCursor_ = false; break;
                case '`': if (panelOpen_ && panelTab_ == 3 && termFocus_) togglePanel(); else openTerminal(); scrollToCursor_ = false; break;
                case 'm': if (shift) { panelOpen_ = true; panelTab_ = 2; dirty_ = true; scrollToCursor_ = false; } break;   // Problems
                case 's': if (shift) openPrompt(PromptKind::SaveAs); else saveCurrent(); break;
                case 'o': if (shift) browseOpen(false); else openPrompt(PromptKind::Open); break;
                case 'p': if (shift) welcomeOpen(); break;
                case 'b': toggleSidebar(); scrollToCursor_ = false; break;
                case 'e': if (shift) focusSidebar(); scrollToCursor_ = false; break;
                case 'n': if (shift) newProjectOpen(); else newDoc(); break;
                case 'w': closeTab(cur_); break;
                case 'q': requestQuit(); break;
                case 'f': if (shift) openSearch(false); else openPrompt(PromptKind::Find); scrollToCursor_ = false; break;
                case 'h': if (shift) openSearch(true); else openPrompt(PromptKind::Replace); scrollToCursor_ = false; break;
                case 'g': if (shift) { sidebar_ = true; sideGit_ = true; sideSearch_ = false; sideDebug_ = false; sideFocus_ = false; gitNextPoll_ = {}; dirty_ = true; scrollToCursor_ = false; } else openPrompt(PromptKind::Goto); break;
                case 'z': if (shift) d.redo(); else d.undo(); break;
                case 'y': d.redo(); break;
                case 'a': d.selectAll(); break;
                case 'c': copy(false); scrollToCursor_ = false; break;
                case 'x': copy(true); break;
                case 'v': paste(); break;
                case 'l': d.selectLine(d.cursor.line); break;
                case '/': d.toggleComment(); break;
                case 'd': if (shift) d.duplicateLines(); break;
                case 'k': if (shift) d.deleteLines(); break;
                case ']': d.indentLines(false); break;
                case '[': d.indentLines(true); break;
                case '=': case '+': zoom(1); break;
                case '-': zoom(-1); break;
                case '0': zoom(0, true); break;
                default: scrollToCursor_ = false; break;
            }
            return;
        }

        switch (k.key) {
            case Key::Left: d.moveLeft(shift, ctrl); break;
            case Key::Right: d.moveRight(shift, ctrl); break;
            case Key::Up: if (alt) d.moveLines(-1); else d.moveVertical(-1, shift); break;
            case Key::Down: if (alt) d.moveLines(1); else d.moveVertical(1, shift); break;
            case Key::Home: if (ctrl) d.docStart(shift); else d.home(shift); break;
            case Key::End: if (ctrl) d.docEnd(shift); else d.end(shift); break;
            case Key::PageUp:
                if (ctrl) { switchTab(cur_ - 1); break; }
                pageMove(d, -1, shift);
                break;
            case Key::PageDown:
                if (ctrl) { switchTab(cur_ + 1); break; }
                pageMove(d, 1, shift);
                break;
            case Key::Backspace: if (ctrl) d.deleteWordBack(); else d.backspace(); break;
            case Key::Delete: if (ctrl) d.deleteWordForward(); else d.deleteForward(); break;
            case Key::Enter: d.newline(); break;
            case Key::Tab:
                if (ctrl) switchTab(cur_ + (shift ? -1 : 1));
                else if (shift) d.indentLines(true);
                else d.tab();
                break;
            case Key::Escape:
                if (d.hasSelection()) d.setCursor(d.cursor);
                break;
            case Key::F3:
                if (!findText_.empty()) {
                    if (!d.find(findText_, !shift, matchCase_)) setMessage("No results for '" + findText_ + "'", true);
                }
                break;
            default:
                if (!k.text.empty() && !k.alt) d.insertText(k.text);
                else scrollToCursor_ = false;
                break;
        }
    }

    void pageMove(Document& d, int dir, bool extend) {
        int n = std::max(1, static_cast<int>(L_.editor.h / lh()) - 1);
        d.moveVertical(dir * n, extend);
        d.targetY += static_cast<float>(dir * n) * lh();
    }

    void copy(bool cut) {
        Document& d = doc();
        std::string t;
        if (d.hasSelection()) t = d.selectedText();
        else t = d.lineAt(d.cursor.line) + "\n";
        win_.setClipboard(t);
        if (cut) {
            if (d.hasSelection()) { d.buf.breakGroup(); d.deleteSelection(); }
            else d.deleteLines();
        }
    }

    void paste() {
        std::string t = win_.clipboard();
        if (!t.empty()) doc().insertRaw(t);
    }

    // ============================================================ prompts

    void openPrompt(PromptKind kind) {
        Prompt p;
        p.kind = kind;
        Document& d = doc();
        auto field = [](std::string label, std::string text = {}, bool fresh = false) {
            Field f;
            f.label = std::move(label);
            f.text = std::move(text);
            f.cur = f.text.size();
            f.fresh = fresh && !f.text.empty();
            return f;
        };
        switch (kind) {
            case PromptKind::Find:
            case PromptKind::Replace: {
                std::string seed = findText_;
                if (d.hasSelection() && d.selBegin().line == d.selEnd().line) seed = d.selectedText();
                p.fields.push_back(field("Find", seed, true));
                if (kind == PromptKind::Replace) p.fields.push_back(field("Replace", replaceText_));
                findText_ = seed;
                break;
            }
            case PromptKind::Goto: p.fields.push_back(field("Go to line[:col]")); break;
            case PromptKind::RunArgs: p.fields.push_back(field("Program arguments", runArgs_, true)); break;
            case PromptKind::Open: {
                std::string base = d.path.empty() ? fs::current_path().string() : fs::path(d.path).parent_path().string();
                if (const char* home = std::getenv("HOME"); home && base.starts_with(home)) base = "~" + base.substr(std::strlen(home));
                p.fields.push_back(field("Open", base + "/"));
                break;
            }
            case PromptKind::OpenFolder: {
                std::string base = explorer_.isOpen() ? explorer_.rootPath().string() : fs::current_path().string();
                if (const char* home = std::getenv("HOME"); home && base.starts_with(home)) base = "~" + base.substr(std::strlen(home));
                p.fields.push_back(field("Open folder", base + "/"));
                break;
            }
            case PromptKind::SaveAs:
                p.fields.push_back(field("Save as", d.path.empty() ? fs::current_path().string() + "/" + d.name : d.path));
                break;
            default: break;
        }
        prompt_ = std::move(p);
        overwriteArmed_.clear();
        dirty_ = true;
    }

    void closePrompt() {
        prompt_ = {};
        dirty_ = true;
    }

    bool inFindMode() const { return prompt_.kind == PromptKind::Find || prompt_.kind == PromptKind::Replace; }

    void promptChanged() {
        Field& f = prompt_.fields[static_cast<std::size_t>(prompt_.active)];
        if (inFindMode() && prompt_.active == 0) {
            findText_ = f.text;
            Document& d = doc();
            if (!findText_.empty() && !d.find(findText_, true, matchCase_, true)) setMessage("No results", true, 1.5f);
            scrollToCursor_ = true;
        } else if (inFindMode()) {
            replaceText_ = f.text;
        }
    }

    void promptKey(const KeyEvent& k) {
        Field& f = prompt_.fields[static_cast<std::size_t>(prompt_.active)];
        Document& d = doc();
        auto edit = [&](auto&& fn) {
            if (f.fresh) { f.text.clear(); f.cur = 0; f.fresh = false; }
            fn();
            promptChanged();
        };

        if (k.ctrl && k.key == Key::Char) {
            switch (k.ch) {
                case 'v': {
                    std::string t = win_.clipboard();
                    std::erase(t, '\r');
                    if (auto nl = t.find('\n'); nl != std::string::npos) t.resize(nl);
                    edit([&] { f.text.insert(f.cur, t); f.cur += t.size(); });
                    break;
                }
                case 'f': case 'h': case 'g': case 'o': case 's': case 'b': case 'e':
                    closePrompt();
                    editorKey(k);
                    break;
                case 'q': requestQuit(); break;
                default: break;
            }
            return;
        }
        if (k.alt && k.key == Key::Char && k.ch == 'c' && inFindMode()) {
            matchCase_ = !matchCase_;
            if (!findText_.empty()) d.find(findText_, true, matchCase_, true);
            return;
        }

        switch (k.key) {
            case Key::Escape: closePrompt(); return;
            case Key::Enter: submitPrompt(k); return;
            case Key::F3:
                if (inFindMode() && !findText_.empty()) d.find(findText_, !k.shift, matchCase_);
                scrollToCursor_ = true;
                return;
            case Key::Tab:
                if (inFindMode()) {
                    prompt_.active = (prompt_.active + 1) % static_cast<int>(prompt_.fields.size());
                } else if (prompt_.kind == PromptKind::Open || prompt_.kind == PromptKind::OpenFolder || prompt_.kind == PromptKind::SaveAs) {
                    completePath(f);
                }
                return;
            case Key::Left: f.fresh = false; if (f.cur > 0) f.cur = utf8Prev(f.text, f.cur); return;
            case Key::Right: f.fresh = false; if (f.cur < f.text.size()) f.cur = utf8Next(f.text, f.cur); return;
            case Key::Home: f.fresh = false; f.cur = 0; return;
            case Key::End: f.fresh = false; f.cur = f.text.size(); return;
            case Key::Up:
                if (inFindMode() && !findText_.empty()) { d.find(findText_, false, matchCase_); scrollToCursor_ = true; }
                return;
            case Key::Down:
                if (inFindMode() && !findText_.empty()) { d.find(findText_, true, matchCase_); scrollToCursor_ = true; }
                return;
            case Key::Backspace:
                edit([&] {
                    if (f.cur > 0) { auto p = utf8Prev(f.text, f.cur); f.text.erase(p, f.cur - p); f.cur = p; }
                });
                return;
            case Key::Delete:
                edit([&] {
                    if (f.cur < f.text.size()) { auto n = utf8Next(f.text, f.cur); f.text.erase(f.cur, n - f.cur); }
                });
                return;
            default:
                if (!k.text.empty()) edit([&] { f.text.insert(f.cur, k.text); f.cur += k.text.size(); });
                return;
        }
    }

    void submitPrompt(const KeyEvent& k) {
        Document& d = doc();
        Field& f = prompt_.fields[static_cast<std::size_t>(prompt_.active)];
        switch (prompt_.kind) {
            case PromptKind::Find:
                if (!findText_.empty() && !d.find(findText_, !k.shift, matchCase_))
                    setMessage("No results for '" + findText_ + "'", true);
                scrollToCursor_ = true;
                break;
            case PromptKind::Replace:
                if (prompt_.active == 0) {
                    if (!findText_.empty() && !d.find(findText_, !k.shift, matchCase_)) setMessage("No results", true);
                } else if (k.ctrl) {
                    int n = d.replaceAll(findText_, replaceText_, matchCase_);
                    setMessage(std::format("Replaced {} occurrence{}", n, n == 1 ? "" : "s"));
                } else if (!findText_.empty()) {
                    if (!d.replaceCurrent(findText_, replaceText_, matchCase_)) setMessage("No more results", true);
                }
                scrollToCursor_ = true;
                break;
            case PromptKind::Goto: {
                int line = 0, col = 0;
                auto colon = f.text.find(':');
                std::from_chars(f.text.data(), f.text.data() + (colon == std::string::npos ? f.text.size() : colon), line);
                if (colon != std::string::npos) std::from_chars(f.text.data() + colon + 1, f.text.data() + f.text.size(), col);
                if (line > 0) {
                    d.gotoLine(line - 1, 0);
                    if (col > 0) d.setCursor({d.cursor.line, byteColAt(d.buf.line(d.cursor.line), col - 1, d.tabWidth)});
                }
                closePrompt();
                scrollToCursor_ = true;
                break;
            }
            case PromptKind::Open: {
                std::string p = f.text;
                closePrompt();
                if (!p.empty()) openFile(p);
                break;
            }
            case PromptKind::OpenFolder: {
                std::string p = f.text;
                closePrompt();
                if (!p.empty()) openFolder(p);
                break;
            }
            case PromptKind::RunArgs:
                runArgs_ = f.text;
                closePrompt();
                setMessage(runArgs_.empty() ? "Program arguments cleared" : "Program arguments: " + runArgs_, false, 2.5f);
                break;
            case PromptKind::NewFile:
            case PromptKind::NewFolder:
            case PromptKind::Rename:
            case PromptKind::Delete: {
                std::string text = f.text;
                finishFileOp(text);
                break;
            }
            case PromptKind::DbgWatch: case PromptKind::DbgSetValue: case PromptKind::DbgBpEdit: case PromptKind::DbgFuncBp: {
                const std::string text = f.text;
                const PromptKind kd = prompt_.kind;
                closePrompt();
                dbgFinishPrompt(kd, text);
                break;
            }
            case PromptKind::SaveAs: {
                std::string p = expandPath(f.text);
                if (p.empty()) break;
                std::error_code ec;
                std::string abs = fs::absolute(p, ec).lexically_normal().string();
                if (fs::exists(abs, ec) && abs != d.path && overwriteArmed_ != abs) {
                    overwriteArmed_ = abs;
                    setMessage("File exists - press Enter again to overwrite", true);
                    break;
                }
                closePrompt();
                d.setPath(p);
                if (auto r = d.save(); r) { setMessage("Saved " + d.path); if (explorer_.isOpen()) explorer_.refresh(); }
                else setMessage(r.error(), true);
                break;
            }
            default: break;
        }
    }

    // Tab completion for file prompts: extends to the longest common prefix.
    void completePath(Field& f) {
        std::string typed = expandPath(f.text);
        fs::path p(typed);
        fs::path dir = typed.ends_with('/') ? p : p.parent_path();
        std::string prefix = typed.ends_with('/') ? "" : p.filename().string();
        std::error_code ec;
        std::vector<std::string> hits;
        for (fs::directory_iterator it(dir.empty() ? "." : dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().string();
            if (n.starts_with(prefix) && (prefix.starts_with('.') || !n.starts_with('.'))) {
                if (it->is_directory(ec)) n += '/';
                hits.push_back(n);
            }
        }
        if (hits.empty()) { setMessage("No matches", true, 1.5f); return; }
        std::ranges::sort(hits);
        std::string common = hits.front();
        for (const auto& h : hits)
            while (!h.starts_with(common)) common.pop_back();
        std::string head = f.text.substr(0, f.text.size() - prefix.size());
        f.text = head + common;
        f.cur = f.text.size();
        if (hits.size() > 1) {
            std::string list;
            for (std::size_t i = 0; i < hits.size() && i < 8; ++i) list += hits[i] + "   ";
            if (hits.size() > 8) list += std::format("... (+{})", hits.size() - 8);
            setMessage(list, false, 6);
        }
    }

    // ============================================================ input: pointer

    Edge edgeAt(float x, float y) const {
        if (!L_.csd || win_.maximized() || win_.fullscreen()) return Edge::None;
        const float m = 6 * S(), W = L_.status.w, H = L_.status.bottom();
        bool l = x < m, r = x >= W - m, t = y < m, b = y >= H - m;
        if (t && l) return Edge::TopLeft;
        if (t && r) return Edge::TopRight;
        if (b && l) return Edge::BottomLeft;
        if (b && r) return Edge::BottomRight;
        if (t) return Edge::Top;
        if (b) return Edge::Bottom;
        if (l) return Edge::Left;
        if (r) return Edge::Right;
        return Edge::None;
    }

    static Cursor cursorForEdge(Edge e) {
        switch (e) {
            case Edge::Top: return Cursor::ResizeN;
            case Edge::Bottom: return Cursor::ResizeS;
            case Edge::Left: return Cursor::ResizeW;
            case Edge::Right: return Cursor::ResizeE;
            case Edge::TopLeft: return Cursor::ResizeNW;
            case Edge::TopRight: return Cursor::ResizeNE;
            case Edge::BottomLeft: return Cursor::ResizeSW;
            case Edge::BottomRight: return Cursor::ResizeSE;
            default: return Cursor::Default;
        }
    }

    // 1 = minimise, 2 = maximise, 3 = close
    int titleButtonAt(float x, float y) const {
        if (!L_.csd || !L_.title.contains(x, y)) return 0;
        float bw = L_.title.h * 1.25f;
        int idx = static_cast<int>((L_.title.right() - x) / bw);
        return idx >= 0 && idx < 3 ? 3 - idx : 0;
    }

    bool inScrollbar(const Document& d, float x, float y) const {
        return L_.editor.contains(x, y) && x >= L_.editor.right() - 14 * S() && maxScroll(d) > 0;
    }

    void onPointerMove(float lx, float ly) {
        mx_ = lx * S();
        my_ = ly * S();
        if (dlg_.kind != DlgKind::None && !(L_.csd && L_.title.contains(mx_, my_))) {
            int id = -1;
            for (auto it = dlgHits_.rbegin(); it != dlgHits_.rend(); ++it)
                if (it->r.contains(mx_, my_)) { id = it->id; break; }
            if (id < 0) id = -1;
            if (id != dlg_.hover) { dlg_.hover = id; dirty_ = true; }
            win_.setCursor(id == kDlgName || id == kDlgLoc || id == kDlgPath ? Cursor::Text : Cursor::Default);
            return;
        }
        if (menu_.open) {
            int i = menuItemAt(mx_, my_);
            if (i != menu_.hover) { menu_.hover = i; dirty_ = true; }
            win_.setCursor(Cursor::Default);
            return;
        }
        Edge edge = edgeAt(mx_, my_);
        Cursor c = Cursor::Default;
        if (edge != Edge::None) c = cursorForEdge(edge);
        else if (splitDragNode_) c = splitDragNode_->sideBySide ? Cursor::ResizeW : Cursor::ResizeN;
        else if (Node* g = gapAt(mx_, my_); g && !tabDrag_.active) c = g->sideBySide ? Cursor::ResizeW : Cursor::ResizeN;
        else if (splitDrag_ || (sidebar_ && L_.split.contains(mx_, my_))) c = Cursor::ResizeW;
        else if (panelDrag_ || (panelOpen_ && L_.panel.contains(mx_, my_) && my_ < L_.panel.y + 6 * S())) c = Cursor::ResizeN;
        else if (!docs_.empty() && L_.editor.contains(mx_, my_) && mx_ >= L_.textX && !inScrollbar(doc(), mx_, my_) && !inMinimap(mx_, my_)) c = Cursor::Text;
        win_.setCursor(c);

        if (splitDragNode_) {
            Node& n = *splitDragNode_;
            if (n.sideBySide) n.ratio = (mx_ - n.rect.x - n.gap.w / 2) / std::max(1.0f, n.rect.w - n.gap.w);
            else n.ratio = (my_ - n.rect.y - n.gap.h / 2) / std::max(1.0f, n.rect.h - n.gap.h);
            dirty_ = true;
        }
        if (fdrag_.pending && !fdrag_.active && std::hypot(mx_ - fdrag_.sx, my_ - fdrag_.sy) > 6 * S()) fdrag_.active = true;
        if (fdrag_.active) { fdrag_.target = fileDropTarget(); win_.setCursor(Cursor::Default); dirty_ = true; }
        if (tabDrag_.doc) {
            if (!tabDrag_.active && std::hypot(mx_ - tabDrag_.sx, my_ - tabDrag_.sy) > 7 * S()) tabDrag_.active = true;
            if (tabDrag_.active) { updateDropTarget(); win_.setCursor(Cursor::Default); dirty_ = true; }
        }
        if (splitDrag_) {
            const float m = std::round(8 * S());
            float w = L_.status.w - m - mx_ - m / 2;
            sidebarWidth_ = std::clamp(w / S(), 150.0f, L_.status.w / S() * 0.5f);
            dirty_ = true;
        }
        int hr = -1;
        if (sidebar_ && !sideGit_ && !sideSearch_ && !sideDebug_ && L_.sideList.contains(mx_, my_)) {
            hr = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
            if (hr < 0 || hr >= static_cast<int>(explorer_.rows().size())) hr = -1;
        }
        if (hr != hoverRow_) { hoverRow_ = hr; dirty_ = true; }

        int hb = titleButtonAt(mx_, my_), ht = -1, hc = -1;
        Pane* hp = nullptr;
        for (Node* n : leaves())
            for (std::size_t i = 0; i < n->pane->hits.size(); ++i)
                if (n->pane->hits[i].tab.contains(mx_, my_)) {
                    hp = n->pane.get();
                    ht = static_cast<int>(i);
                    if (n->pane->hits[i].close.contains(mx_, my_)) hc = ht;
                }
        if (hp != hoverPane_) { hoverPane_ = hp; dirty_ = true; }
        const int hAct = activityAt(mx_, my_);
        if (hAct != hoverAct_) { hoverAct_ = hAct; dirty_ = true; }
        const int hover = opt_.screenshot.empty() ? toolAt(mx_, my_) : hoverTool_;   // scripted captures drive the hover state themselves
        int hpr = -1;
        if (panelOpen_ && panelBody().contains(mx_, my_))
            hpr = static_cast<int>(std::floor((my_ - panelBody().y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
        if (gitView()) gitUpdateHover();
        if ((searchView() || debugView()) && L_.side.contains(mx_, my_)) dirty_ = true;
        if (hb != hoverBtn_ || ht != hoverTab_ || hc != hoverClose_ || hover != hoverTool_ || hpr != hoverPanelRow_) {
            if (hover != hoverTool_) { toolHoverAt_ = Clock::now(); toolTipShown_ = false; }
            hoverBtn_ = hb; hoverTab_ = ht; hoverClose_ = hc; hoverTool_ = hover; hoverPanelRow_ = hpr;
            dirty_ = true;
        }
        if (tsel_.dragging) {
            const auto [r, c] = termPosAt(mx_, my_, panelBody());
            tsel_.cr = r; tsel_.cc = c;
            if (std::hypot(mx_ - tsel_.sx, my_ - tsel_.sy) > 3 * S()) tsel_.moved = true;
            const Rect body = panelBody();
            if (my_ < body.y) termScroll_ = std::min(termScroll_ + 1, term_.scrollbackSize());
            else if (my_ > body.bottom()) termScroll_ = std::max(termScroll_ - 1, 0);
            dirty_ = true;
        }
        if (psel_.dragging) {
            const auto [row, col] = panelPosAt(mx_, my_);
            psel_.cr = row; psel_.cc = col;
            if (std::hypot(mx_ - psel_.sx, my_ - psel_.sy) > 3 * S()) psel_.moved = true;
            // keep the pointer inside the panel body while dragging: scroll when it leaves it
            const Rect body = panelBody();
            auto& sc = panelScroll_[static_cast<std::size_t>(panelTab_)];
            if (my_ < body.y) { sc = std::max(0.0f, sc - lh() * 0.6f); panelFollow_[static_cast<std::size_t>(panelTab_)] = false; }
            else if (my_ > body.bottom()) sc = std::min(panelMaxScroll(), sc + lh() * 0.6f);
            dirty_ = true;
        }
        if (hsel_.dragging) {
            hoverSelUpdate();
            dirty_ = true;
        }
        Rect keepZone = hoverRect_;   // the popup (plus the gap up to the text it belongs to) stays while the pointer is on it
        keepZone.y -= 30 * S(); keepZone.h += 38 * S(); keepZone.x -= 44 * S(); keepZone.w += 54 * S();
        const bool onPopup = hover_.show && !menu_.open && keepZone.contains(mx_, my_);
        const bool overBlame = blameRect_.w > 0 && blameRect_.contains(mx_, my_);
        if (!opt_.screenshot.empty()) {}  // scripted runs drive hover themselves
        else if (overBlame) {
            if (!(blameHover_ && hover_.show)) { blameHover_ = true; showBlameCard(blameHash_, blameUncommitted_); dirty_ = true; }
        }
        else if (hsel_.dragging || onPopup) {}
        else if (!docs_.empty() && !menu_.open && !dragging_ && L_.editor.contains(mx_, my_) && mx_ >= L_.textX && mx_ < L_.minimap.x) { blameHover_ = false; hoverMoved(); }
        else { blameHover_ = false; hideHover(); }
        if (panelDrag_) {
            panelH_ = std::clamp((L_.panel.bottom() - my_) / S(), 90.0f, std::max(90.0f, (L_.side.h > 0 ? L_.side.h : L_.island.h + L_.panel.h) / S() * 0.6f));
            dirty_ = true;
        }
        if (dragging_ || scrollbarDrag_ || miniDrag_) { dragStep(doc()); dirty_ = true; }
    }

    // Applies an in-progress mouse drag (selection or scrollbar) at the current pointer position.
    void dragStep(Document& d) {
        if (miniDrag_) { miniJump(d); return; }
        if (scrollbarDrag_) {
            float frac = std::clamp((my_ - L_.editor.y) / std::max(1.0f, L_.editor.h), 0.0f, 1.0f);
            d.targetY = frac * maxScroll(d);
            d.scrollY = d.targetY;
            return;
        }
        if (!dragging_) return;
        // Auto-scroll while dragging above / below the text area.
        if (my_ < L_.editor.y) d.targetY -= lh() * 0.5f;
        else if (my_ > L_.editor.bottom()) d.targetY += lh() * 0.5f;
        d.setCursor(posAt(d, mx_, std::clamp(my_, L_.editor.y, L_.editor.bottom() - 1)), true);
        dirty_ = true;
    }

    // ---- tab drag & drop (reorder, move between panes, split)

    void updateDropTarget() {
        TabDrag& d = tabDrag_;
        d.zone = Zone::None;
        d.target = nullptr;
        Pane* p = paneAt(mx_, my_);
        if (!p) return;
        d.target = p;
        if (p->tabsR.contains(mx_, my_)) {
            d.zone = Zone::Tabs;
            int idx = 0;
            for (std::size_t i = 0; i < p->tabs.size() && i < p->hits.size(); ++i) {
                if (p->tabs[i] == d.doc) continue;
                const Rect& t = p->hits[i].tab;
                if (mx_ > t.x + t.w / 2) ++idx;
            }
            d.insertAt = idx;
            return;
        }
        const Rect body{p->rect.x, p->tabsR.bottom(), p->rect.w, std::max(1.0f, p->rect.h - p->tabsR.h)};
        const float fx = (mx_ - body.x) / body.w, fy = (my_ - body.y) / body.h;
        Zone z = Zone::Center;
        if (fx < 0.28f) z = Zone::Left;
        else if (fx > 0.72f) z = Zone::Right;
        else if (fy < 0.28f) z = Zone::Top;
        else if (fy > 0.72f) z = Zone::Bottom;
        if (p == d.from && p->tabs.size() == 1) z = Zone::Center;  // nothing to split off
        d.zone = z;
    }

    void moveTab(Document* doc, Pane* to, int at) {
        if (Pane* from = paneOfDoc(doc)) std::erase(from->tabs, doc);
        at = std::clamp(at, 0, static_cast<int>(to->tabs.size()));
        to->tabs.insert(to->tabs.begin() + at, doc);
        to->cur = doc;
    }

    // Moves `doc` into a new pane created next to `target` (on the side given by `z`).
    void splitPane(Pane* target, Zone z, Document* doc) {
        Node* leafNode = nodeOfPane(target);
        if (!leafNode) return;
        if (Pane* from = paneOfDoc(doc)) std::erase(from->tabs, doc);
        auto fresh = std::make_unique<Pane>();
        fresh->tabs = {doc};
        fresh->cur = doc;
        auto oldLeaf = std::make_unique<Node>();
        oldLeaf->pane = std::move(leafNode->pane);
        auto newLeaf = std::make_unique<Node>();
        newLeaf->pane = std::move(fresh);
        leafNode->sideBySide = z == Zone::Left || z == Zone::Right;
        leafNode->ratio = 0.5f;
        if (z == Zone::Left || z == Zone::Top) { leafNode->a = std::move(newLeaf); leafNode->b = std::move(oldLeaf); }
        else { leafNode->a = std::move(oldLeaf); leafNode->b = std::move(newLeaf); }
    }

    void performDrop() {
        const TabDrag d = tabDrag_;
        if (!d.doc || !d.target || d.zone == Zone::None) return;
        Pane* from = paneOfDoc(d.doc);
        if (!from) return;
        switch (d.zone) {
            case Zone::Tabs: moveTab(d.doc, d.target, d.insertAt); break;
            case Zone::Center: if (d.target != from) moveTab(d.doc, d.target, static_cast<int>(d.target->tabs.size())); break;
            case Zone::Left: case Zone::Right: case Zone::Top: case Zone::Bottom: splitPane(d.target, d.zone, d.doc); break;
            default: break;
        }
        const int idx = docIndex(d.doc);
        if (idx >= 0) cur_ = idx;
        resetCaret();
        scrollToCursor_ = true;
        dirty_ = true;
    }

    // Floating tab under the pointer plus the drop indicator of the target pane.
    void drawTabDrag() {
        if (!tabDrag_.active || !tabDrag_.doc) return;
        const float s = S();
        if (Pane* p = tabDrag_.target; p && tabDrag_.zone != Zone::None) {
            const Rect body{p->rect.x, p->tabsR.bottom(), p->rect.w, std::max(1.0f, p->rect.h - p->tabsR.h)};
            Rect hl = body;
            switch (tabDrag_.zone) {
                case Zone::Left: hl = {body.x, body.y, body.w / 2, body.h}; break;
                case Zone::Right: hl = {body.x + body.w / 2, body.y, body.w / 2, body.h}; break;
                case Zone::Top: hl = {body.x, body.y, body.w, body.h / 2}; break;
                case Zone::Bottom: hl = {body.x, body.y + body.h / 2, body.w, body.h / 2}; break;
                case Zone::Center: hl = {body.x + 6 * s, body.y + 6 * s, body.w - 12 * s, body.h - 12 * s}; break;
                default: break;
            }
            if (tabDrag_.zone == Zone::Tabs) {
                // Insertion marker between tabs.
                float x = p->tabsR.x + 8 * s;
                int seen = 0;
                for (std::size_t i = 0; i < p->tabs.size() && i < p->hits.size(); ++i) {
                    if (p->tabs[i] == tabDrag_.doc) continue;
                    if (seen == tabDrag_.insertAt) break;
                    x = p->hits[i].tab.right() + 1 * s;
                    ++seen;
                }
                fill(x - s, p->tabsR.y + 6 * s, std::round(2.5f * s), p->tabsR.h - 10 * s, theme::accent, std::round(s));
            } else {
                fill(hl, theme::accent.alpha(46), 10 * s);
                const float t = std::max(1.5f, std::round(1.5f * s));
                fill(hl.x, hl.y, hl.w, t, theme::accent.alpha(150)); fill(hl.x, hl.bottom() - t, hl.w, t, theme::accent.alpha(150));
                fill(hl.x, hl.y, t, hl.h, theme::accent.alpha(150)); fill(hl.right() - t, hl.y, t, hl.h, theme::accent.alpha(150));
            }
        }
        const std::string name = tabDrag_.doc->name;
        const float w = tabNaturalW(name), h = std::round(uiH() + 12 * s);
        Rect r{mx_ - tabDrag_.grabX, my_ - h / 2, w, h};
        glassPanel(r, 9 * s);
        drawFileIcon(r.x + 12 * s, r.y + (h - iconSize()) / 2, name);
        uiLabel(r.x + 12 * s + iconSize() + 8 * s, r.y, h, name, theme::text, r.right() - 10 * s);
    }

    void onPointerButton(Button b, bool pressed, float lx, float ly) {
        mx_ = lx * S();
        my_ = ly * S();
        dirty_ = true;
        if (dlg_.kind != DlgKind::None && !(L_.csd && L_.title.contains(mx_, my_))) {   // dialogs are modal (window controls stay live)
            if (pressed) dlgClick(b, mx_, my_);
            return;
        }
        if (pressed && !(panelOpen_ && L_.panel.contains(mx_, my_))) { runFocus_ = false; termFocus_ = false; dbgConFocus_ = false; psel_.active = false; gitMsgFocus_ = false; if (!(sidebar_ && L_.side.contains(mx_, my_))) sFocus_ = -1; }
        if (hover_.show && !hover_.lines.empty() && !menu_.open && hoverRect_.contains(mx_, my_)) {   // the tooltip is selectable
            if (pressed && b == Button::Left) {
                if (hoverCopyRect_.contains(mx_, my_)) { std::string t; for (std::size_t i = 0; i < hover_.lines.size(); ++i) t += (i ? "\n" : "") + hover_.lines[i].first; win_.setClipboard(t); setMessage("Copied", false, 1.2f); }
                else { hsel_ = {}; hsel_.active = true; hsel_.dragging = true; hoverSelAnchor(); }
            }
            return;
        }
        if (pressed && b == Button::Left && !menu_.open && dbgBarClick()) return;
        if (menu_.open) {  // the menu captures the click; clicking outside just dismisses it
            if (pressed) {
                int i = menuItemAt(mx_, my_);
                if (i >= 0) activateMenuItem(i); else if (!menu_.rect.contains(mx_, my_)) closeMenu();
            }
            return;
        }
        if (!pressed) {
            if (fdrag_.pending || fdrag_.active) {
                const bool wasDrag = fdrag_.active;
                if (wasDrag) { fdrag_.target = fileDropTarget(); finishFileDrag(); }
                else {
                    fdrag_ = {};
                    const auto& rows = explorer_.rows();
                    if (L_.sideList.contains(mx_, my_) && dragRow_ >= 0 && dragRow_ < static_cast<int>(rows.size())
                        && static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow)) == dragRow_)
                        activateRow(*rows[static_cast<std::size_t>(dragRow_)].node);
                }
                dragRow_ = -1;
                return;
            }
            if (tsel_.dragging) { tsel_.dragging = false; if (!tsel_.moved) tsel_.active = false; }
            if (psel_.dragging) {
                psel_.dragging = false;
                if (!psel_.moved) { const int row = psel_.ar; psel_.active = false; panelRowClick(row); }
            }
            hsel_.dragging = false;
            if (tabDrag_.doc && tabDrag_.active) performDrop();
            tabDrag_ = {};
            splitDragNode_ = nullptr;
            dragging_ = scrollbarDrag_ = splitDrag_ = panelDrag_ = miniDrag_ = false;
            return;
        }
        resetCaret();
        sideFocus_ = sidebar_ && L_.side.contains(mx_, my_);
        if (sidebar_ && b == Button::Left && L_.split.contains(mx_, my_)) { splitDrag_ = true; return; }
        if (sidebar_ && L_.side.contains(mx_, my_)) {
            if (sideDebug_) { debugClick(b); return; }
            if (sideSearch_) { searchClick(b); return; }
            if (sideGit_) {   // the Git view handles its own hits
                if (b == Button::Left && L_.sideList.contains(mx_, my_)) gitPanelClick(); else gitMsgFocus_ = false;
                return;
            }
            if (!explorer_.isOpen()) { if (b == Button::Left) browseOpen(false); return; }
            const auto& rows = explorer_.rows();
            int row = -1;
            if (L_.sideList.contains(mx_, my_)) {
                row = static_cast<int>(std::floor((my_ - L_.sideList.y + sideScroll_) / L_.sideRow));
                if (row < 0 || row >= static_cast<int>(rows.size())) row = -1;
            }
            if (b == Button::Right) {
                sideFocus_ = true;
                if (row >= 0) {
                    std::string p = rows[static_cast<std::size_t>(row)].node->path.string();
                    explorer_.select(p);
                    openMenu(mx_, my_, &p);
                } else {
                    explorer_.select(explorer_.rootPath().string());
                    openMenu(mx_, my_, nullptr);
                }
            } else if (b == Button::Left) {
                if (row >= 0) {   // opens / toggles on release, unless the press turns into a drag
                    FileNode& nd = *rows[static_cast<std::size_t>(row)].node;
                    explorer_.select(nd.path.string());
                    fdrag_ = {};
                    fdrag_.path = nd.path; fdrag_.pending = true; fdrag_.sx = mx_; fdrag_.sy = my_;
                    dragRow_ = row;
                }
                else explorer_.select(explorer_.rootPath().string());  // header / empty area targets the root
            }
            return;
        }

        if (Edge e = edgeAt(mx_, my_); e != Edge::None && b == Button::Left) { win_.startResize(e); return; }

        if (L_.title.contains(mx_, my_) && L_.csd) {
            if (cmakeOpen_ && toolAt(mx_, my_) > 0) {  // the toolbar shares this row
                if (b == Button::Left) toolbarClick();
                return;
            }
            if (b != Button::Left) return;
            switch (titleButtonAt(mx_, my_)) {
                case 1: win_.minimize(); return;
                case 2: win_.toggleMaximize(); return;
                case 3: requestQuit(); return;
                default: break;
            }
            if (mx_ < std::round(14 * S()) + std::max(14.0f, std::round(L_.title.h * 0.5f)) + 10 * S()) { welcomeOpen(); return; }   // logo: projects
            auto now = Clock::now();
            if (now - lastTitleClick_ < std::chrono::milliseconds(400)) { win_.toggleMaximize(); lastTitleClick_ = {}; }
            else { lastTitleClick_ = now; win_.startMove(); }
            return;
        }

        if (int act = activityAt(mx_, my_); act > 0) {
            if (b == Button::Left) activityClick(act - 1);
            return;
        }
        if (cmakeOpen_ && L_.toolbar.contains(mx_, my_)) {
            if (b == Button::Left) toolbarClick();
            return;
        }
        if (panelOpen_ && L_.panel.contains(mx_, my_)) { panelClick(b); return; }

        if (b == Button::Left)
            if (Node* g = gapAt(mx_, my_)) { splitDragNode_ = g; return; }
        if (Pane* pp = paneAt(mx_, my_)) {
            if (pp != activePane_ && pp->cur) {  // clicking another editor pane focuses it
                cur_ = std::max(0, docIndex(pp->cur));
                revealCurrent();
                resetCaret();
                layout();
            }
            if (pp->tabsR.contains(mx_, my_)) {
                for (std::size_t i = 0; i < pp->hits.size() && i < pp->tabs.size(); ++i) {
                    if (!pp->hits[i].tab.contains(mx_, my_)) continue;
                    Document* dd = pp->tabs[i];
                    const int idx = docIndex(dd);
                    if (b == Button::Middle || (b == Button::Left && pp->hits[i].close.contains(mx_, my_))) closeTab(idx);
                    else if (b == Button::Left) {
                        switchTab(idx);
                        tabDrag_ = {};
                        tabDrag_.doc = dd;
                        tabDrag_.from = pp;
                        tabDrag_.sx = mx_; tabDrag_.sy = my_;
                        tabDrag_.grabX = mx_ - pp->hits[i].tab.x;
                    }
                    return;
                }
                return;
            }
        }

        if (docs_.empty() || !L_.editor.contains(mx_, my_)) return;
        Document& d = doc();
        if (b == Button::Right && mx_ < L_.textX + 0 && L_.editor.contains(mx_, my_)) { openEditorMenu(posAt(d, mx_, my_)); return; }
        if (b == Button::Right) {
            const Pos pp = posAt(d, mx_, my_);
            if (!d.hasSelection() || pp < d.selBegin() || pp > d.selEnd()) d.setCursor(pp);
            openEditorMenu(pp);
            return;
        }
        if (b != Button::Left) return;
        if (inScrollbar(d, mx_, my_)) { scrollbarDrag_ = true; dragStep(d); return; }
        if (inMinimap(mx_, my_)) { miniDrag_ = true; miniJump(d); return; }
        if (mx_ < L_.textX) {  // gutter: breakpoint toggle in the left strip, otherwise select the whole line
            int line = posAt(d, mx_, my_).line;
            if (mx_ < L_.editor.x + 1.9f * cw()) { dbgToggleBpAt(d.path, line + 1); return; }
            d.selectLine(line);
            dragging_ = false;
            return;
        }

        if (win_.ctrlDown() && lsp_ && lsp_->ready()) {  // Ctrl+click: go to definition
            d.setCursor(posAt(d, mx_, my_));
            gotoDefinition();
            return;
        }
        auto now = Clock::now();
        Pos p = posAt(d, mx_, my_);
        bool near = std::abs(mx_ - lastClickX_) < 4 * S() && std::abs(my_ - lastClickY_) < 4 * S();
        clickCount_ = (near && now - lastClick_ < std::chrono::milliseconds(450)) ? clickCount_ % 3 + 1 : 1;
        lastClick_ = now;
        lastClickX_ = mx_;
        lastClickY_ = my_;
        switch (clickCount_) {
            case 1: d.setCursor(p, win_.shiftDown()); dragging_ = true; break;
            case 2: { auto [a, e] = d.wordRange(p); d.select(a, e); break; }
            default: d.selectLine(p.line); break;
        }
    }

    void onScroll(float dxPx, float dyPx, int stepsX, int stepsY) {
        if (dlg_.kind != DlgKind::None) { dlgScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * 34 * S() : dyPx * S()); dirty_ = true; return; }
        if (menu_.open) closeMenu();
        hideHover();
        if (panelOpen_ && panelTab_ == 4 && L_.panel.contains(mx_, my_)) {
            panelScroll_[4] = std::max(0.0f, panelScroll_[4] + (stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S()));
            panelFollow_[4] = false;
            dirty_ = true;
            return;
        }
        if (panelOpen_ && panelTab_ == 3 && L_.panel.contains(mx_, my_)) {
            const int n = stepsY != 0 ? stepsY * 3 : static_cast<int>(std::round(dyPx * S() / lh()));
            if (term_.altScreen() && termProc_) {   // full-screen programs get arrow keys instead of scrollback
                const std::string key = n > 0 ? "\x1b[A" : "\x1b[B";
                std::string out;
                for (int i = 0; i < std::min(std::abs(n), 10); ++i) out += key;
                termProc_->write(out);
            }
            else termScroll_ = std::clamp(termScroll_ - n, 0, term_.scrollbackSize());
            dirty_ = true;
            return;
        }
        if (panelOpen_ && L_.panel.contains(mx_, my_)) {
            auto t = static_cast<std::size_t>(panelTab_);
            float dy = stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S();
            panelScroll_[t] = std::clamp(panelScroll_[t] + dy, 0.0f, panelMaxScroll());
            panelFollow_[t] = panelScroll_[t] >= panelMaxScroll() - 1;
            dirty_ = true;
            return;
        }
        if (debugView() && L_.side.contains(mx_, my_)) { debugScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S()); return; }
        if (searchView() && L_.side.contains(mx_, my_)) { searchScroll(stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S()); return; }
        if (gitView() && L_.side.contains(mx_, my_)) {
            gitScroll_ = std::clamp(gitScroll_ + (stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S()), 0.0f, gitMax_);
            dirty_ = true;
            return;
        }
        if (sidebar_ && L_.side.contains(mx_, my_)) {
            sideTarget_ += stepsY != 0 ? static_cast<float>(stepsY) * 3 * L_.sideRow : dyPx * S();
            clampSide();
            dirty_ = true;
            return;
        }
        if (Pane* pp = paneAt(mx_, my_); pp && pp != activePane_ && pp->cur) { cur_ = std::max(0, docIndex(pp->cur)); layout(); }
        if (docs_.empty()) return;
        Document& d = doc();
        if (win_.ctrlDown()) {
            float v = static_cast<float>(stepsY ? stepsY : (dyPx > 0 ? 1 : dyPx < 0 ? -1 : 0));
            if (v != 0) zoom(-v);
            return;
        }
        if (win_.shiftDown() && dyPx != 0 && dxPx == 0) { dxPx = dyPx; dyPx = 0; stepsX = stepsY; stepsY = 0; }
        float dy = stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S();
        float dx = stepsX != 0 ? static_cast<float>(stepsX) * 6 * cw() : dxPx * S();
        d.targetY += dy;
        d.scrollX += dx;
        d.scrollX = std::max(0.0f, d.scrollX);
        clampScroll(d);
        dirty_ = true;
    }

    // ============================================================ git: status, gutter diff, inline blame

    struct GitDoc {
        std::optional<std::string> head;      // content at HEAD (nullopt: untracked / no commit)
        bool headAsked = false, headKnown = false;
        std::vector<git::Hunk> hunks;
        std::uint64_t hunksVersion = ~0ull;
        std::vector<std::uint8_t> kinds;      // per line: 0 unchanged, 1 added, 2 modified
        std::vector<int> deletedAt;           // lines before which text was removed
        git::Blame blame;
        std::uint64_t blameVersion = ~0ull;
        bool blameValid = false, blameBusy = false;
        Clock::time_point blameDue{};
    };
    std::unordered_map<const Document*, GitDoc> gitDocs_;
    std::optional<fs::path> gitRoot_;
    std::string gitBranch_, gitHead_, gitUpstream_;
    int gitAhead_ = 0, gitBehind_ = 0;
    std::vector<git::Entry> gitEntries_;
    std::unordered_map<std::string, git::Entry> gitFiles_;   // absolute path -> status
    std::unordered_set<std::string> gitDirs_;                // absolute folders that contain changes
    bool gitStatusBusy_ = false, gitDetecting_ = false;
    Clock::time_point gitNextPoll_{};
    std::vector<std::future<std::function<void()>>> jobs_;
    std::unordered_map<std::string, std::vector<std::string>> cardCache_;
    std::string cardPending_;
    Rect blameRect_;
    std::string blameHash_;
    bool blameHover_ = false, blameUncommitted_ = false;

    // Runs `work` on a worker thread; the function it returns is applied on the main thread when it is done.
    void spawn(std::function<std::function<void()>()> work) {
        jobs_.push_back(std::async(std::launch::async, std::move(work)));
    }
    void pollJobs() {
        for (std::size_t i = 0; i < jobs_.size();) {
            if (jobs_[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto apply = jobs_[i].get();
                jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(i));
                if (apply) apply();
                dirty_ = true;
            } else {
                ++i;
            }
        }
    }
    bool docAlive(const Document* d) const {
        return std::ranges::any_of(docs_, [&](const auto& x) { return x.get() == d; });
    }

    void gitDetect() {
        if (gitDetecting_) return;
        fs::path dir;
        if (explorer_.isOpen()) dir = explorer_.rootPath();
        else if (!docs_.empty() && !docs_.front()->path.empty()) dir = fs::path(docs_.front()->path).parent_path();
        if (dir.empty()) return;
        gitDetecting_ = true;
        spawn([this, dir] {
            auto root = git::repoRoot(dir);
            return std::function<void()>([this, root] {
                gitDetecting_ = false;
                gitRoot_ = root;
                gitNextPoll_ = {};
                if (root) gitRefresh();
                else { gitEntries_.clear(); gitFiles_.clear(); gitDirs_.clear(); gitBranch_.clear(); }
            });
        });
    }

    void gitReset() {   // a different project was opened
        gitRoot_.reset();
        gitDocs_.clear();
        gitEntries_.clear(); gitFiles_.clear(); gitDirs_.clear();
        gitBranch_.clear(); gitHead_.clear(); gitUpstream_.clear();
        gitAhead_ = gitBehind_ = 0;
        cardCache_.clear();
        gitDetect();
    }

    std::string gitRel(const std::string& absPath) const {
        if (!gitRoot_) return {};
        auto r = fs::path(absPath).lexically_relative(*gitRoot_);
        if (r.empty() || *r.begin() == "..") return {};
        return r.string();
    }

    void gitRefresh() {
        if (!gitRoot_ || gitStatusBusy_) return;
        gitStatusBusy_ = true;
        const fs::path root = *gitRoot_;
        spawn([this, root] {
            auto st = git::status(root);
            return std::function<void()>([this, root, st = std::move(st)] {
                gitStatusBusy_ = false;
                if (!st) return;
                const bool headChanged = st->head != gitHead_;
                gitBranch_ = st->branch; gitHead_ = st->head; gitUpstream_ = st->upstream;
                gitAhead_ = st->ahead; gitBehind_ = st->behind;
                gitEntries_ = st->entries;
                gitFiles_.clear();
                gitDirs_.clear();
                for (const auto& e : st->entries) {
                    std::string absStr = (root / e.path).lexically_normal().string();
                    while (absStr.size() > 1 && absStr.back() == '/') absStr.pop_back();   // untracked folders come with a trailing slash
                    const fs::path abs = absStr;
                    gitFiles_[abs.string()] = e;
                    for (fs::path q = abs.parent_path(); q.string().size() >= root.string().size() && q != q.parent_path(); q = q.parent_path()) {
                        gitDirs_.insert(q.string());
                        if (q == root) break;
                    }
                }
                if (headChanged) { for (auto& [d, g] : gitDocs_) { g.headAsked = false; g.headKnown = false; g.blameValid = false; } gitInvalidateLists(); }
                for (auto it = gitDocs_.begin(); it != gitDocs_.end();) { if (!docAlive(it->first)) it = gitDocs_.erase(it); else ++it; }
                dirty_ = true;
            });
        });
    }

    // Per visible document: HEAD content, change markers and blame (all computed off the UI thread or cheaply here).
    void gitUpdateDoc(Document& d, Clock::time_point now) {
        if (!gitRoot_ || d.path.empty()) return;
        const std::string rel = gitRel(d.path);
        if (rel.empty()) return;
        GitDoc& g = gitDocs_[&d];
        const fs::path root = *gitRoot_;
        const Document* key = &d;
        if (!g.headAsked) {
            g.headAsked = true;
            spawn([this, root, rel, key] {
                auto head = git::headContent(root, rel);
                return std::function<void()>([this, key, head = std::move(head)]() mutable {
                    if (!docAlive(key)) return;
                    GitDoc& gg = gitDocs_[key];
                    gg.head = std::move(head);
                    gg.headKnown = true;
                    gg.hunksVersion = ~0ull;
                    gg.blameVersion = ~0ull;
                    gg.blameValid = false;
                });
            });
        }
        if (!g.headKnown) return;
        const std::uint64_t ver = d.buf.version();
        if (g.hunksVersion != ver) {
            const std::string text = d.buf.text();
            const int n = d.buf.lineCount();
            if (!g.head) g.hunks = {{0, n, 0, 0}};
            else g.hunks = git::diffLines(git::splitLines(*g.head), git::splitLines(text));
            g.kinds.assign(static_cast<std::size_t>(n), 0);
            g.deletedAt.clear();
            for (const auto& h : g.hunks) {
                if (h.removed()) g.deletedAt.push_back(std::min(h.newStart, n));
                else for (int l = h.newStart; l < h.newStart + h.newCount && l < n; ++l) g.kinds[static_cast<std::size_t>(l)] = h.oldCount == 0 ? 1 : 2;
            }
            g.hunksVersion = ver;
            if (g.head) g.blameDue = now + (g.blameVersion == ~0ull ? std::chrono::milliseconds(0) : std::chrono::milliseconds(650));
            dirty_ = true;
        }
        if (g.head && !g.blameBusy && g.blameVersion != ver && now >= g.blameDue) {
            g.blameBusy = true;
            const fs::path tmp = fs::temp_directory_path() / std::format("daemon-blame-{}-{}.txt", reinterpret_cast<std::uintptr_t>(&d), ver);
            std::ofstream(tmp, std::ios::binary | std::ios::trunc) << d.buf.text();
            spawn([this, root, rel, key, ver, tmp] {
                auto b = git::blame(root, rel, tmp);
                std::error_code ec;
                fs::remove(tmp, ec);
                return std::function<void()>([this, key, ver, b = std::move(b)]() mutable {
                    if (!docAlive(key)) return;
                    GitDoc& gg = gitDocs_[key];
                    gg.blameBusy = false;
                    gg.blameVersion = ver;
                    gg.blameValid = b.has_value();
                    if (b) gg.blame = std::move(*b);
                });
            });
        } else if (g.head && !g.blameBusy && g.blameVersion != ver) {
            nextBlameWake_ = g.blameDue;
        }
    }
    Clock::time_point nextBlameWake_{};

    void gitTick(Clock::time_point now) {
        if (jobs_.size() > 0) pollJobs();
        if (!gitRoot_) return;
        if (focused_ && now >= gitNextPoll_) { gitNextPoll_ = now + std::chrono::seconds(3); gitRefresh(); }
        for (Node* n : leaves()) if (n->pane->cur) gitUpdateDoc(*n->pane->cur, now);
    }

    static std::string blameText(const git::Commit& c) {
        if (c.uncommitted) return "Uncommitted changes";
        return c.author + ", " + git::relativeTime(c.time, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
               "  \xE2\x80\xA2  " + c.summary;
    }

    // The commit that last changed line `ln` of `d` (null when blame is not available for the current text).
    const git::Commit* blameOf(const Document& d, int ln) const {
        auto it = gitDocs_.find(&d);
        if (it == gitDocs_.end()) return nullptr;
        const GitDoc& g = it->second;
        if (!g.blameValid || g.blameVersion != d.buf.version() || ln < 0 || ln >= static_cast<int>(g.blame.lineCommit.size())) return nullptr;
        const int ci = g.blame.lineCommit[static_cast<std::size_t>(ln)];
        return ci >= 0 && ci < static_cast<int>(g.blame.commits.size()) ? &g.blame.commits[static_cast<std::size_t>(ci)] : nullptr;
    }

    // Hover card for the commit behind the blame annotation (fetched once, then cached).
    void showBlameCard(const std::string& hash, bool uncommitted) {
        hover_ = {};
        hover_.show = true;
        hover_.px = mx_;
        hover_.py = my_ - 90 * S();
        if (uncommitted) { hover_.lines = {{"Not committed yet", 0}, {"This line has local changes that are not in any commit.", 0}}; return; }
        if (auto it = cardCache_.find(hash); it != cardCache_.end()) {
            for (const auto& l : it->second) hover_.lines.emplace_back(l, 0);
            hover_.lines.back().second = 0;
            return;
        }
        hover_.lines = {{"Loading commit...", 0}};
        if (cardPending_ == hash || !gitRoot_) return;
        cardPending_ = hash;
        const fs::path root = *gitRoot_;
        spawn([this, root, hash] {
            auto card = git::commitCard(root, hash);
            return std::function<void()>([this, hash, card = std::move(card)]() mutable {
                cardPending_.clear();
                cardCache_[hash] = std::move(card);
                if (blameHover_ && blameHash_ == hash && hover_.show) showBlameCard(hash, false);
            });
        });
    }

    // ============================================================ git panel (bottom panel, "Git" tab)

    struct TextBox { std::string text; std::size_t cur = 0; };
    struct GitHit { Rect r; int kind = 0, sec = 0, idx = 0; };   // kinds: 1 field, 2 commit, 3 stage all, 4 unstage all, 10 open, 11 stage, 12 unstage, 13 discard
    std::vector<GitHit> gitHits_;
    GitHit gitHover_;
    TextBox gitMsg_;
    bool gitMsgFocus_ = false;
    float gitScroll_ = 0, gitMax_ = 0;
    std::string gitDiscardArm_;
    Clock::time_point gitDiscardAt_{};

    bool gitHoverSame(const GitHit& a, const GitHit& b) const { return a.kind == b.kind && a.sec == b.sec && a.idx == b.idx; }

    void gitUpdateHover() {
        GitHit h;
        for (auto it = gitHits_.rbegin(); it != gitHits_.rend(); ++it)
            if (it->r.contains(mx_, my_)) { h = *it; break; }
        if (!gitHoverSame(h, gitHover_)) { gitHover_ = h; dirty_ = true; }
    }

    // Runs a git action on a worker thread, then refreshes the status and shows the outcome.
    void gitAction(std::function<git::Result(const fs::path&)> fn, std::string okMsg = {}) {
        if (!gitRoot_) return;
        const fs::path root = *gitRoot_;
        spawn([this, root, fn = std::move(fn), okMsg = std::move(okMsg)] {
            auto r = fn(root);
            return std::function<void()>([this, r = std::move(r), okMsg] {
                if (!r.ok()) {
                    std::string first = r.out.substr(0, r.out.find('\n'));
                    setMessage(first.empty() ? "git command failed" : first, true, 5);
                } else if (!okMsg.empty()) {
                    setMessage(okMsg, false, 3);
                }
                gitNextPoll_ = {};
                gitRefresh();
            });
        });
    }

    void gitCommit() {
        if (!gitRoot_) return;
        const std::string msg = git::trim(gitMsg_.text);
        const bool anyStaged = std::ranges::any_of(gitEntries_, [](const git::Entry& e) { return e.staged(); });
        if (msg.empty()) { setMessage("Enter a commit message first", true, 2.5f); return; }
        if (!anyStaged) { setMessage("Nothing staged - stage some files first", true, 2.5f); return; }
        gitMsg_ = {};
        const std::string summary = msg.substr(0, msg.find('\n'));
        gitAction([msg](const fs::path& root) { return git::commit(root, msg); }, "Committed: " + summary);
    }

    bool gitMsgKey(const KeyEvent& k) {
        if (k.key == Key::Escape) { gitMsgFocus_ = false; return true; }
        if (k.key == Key::Enter) { gitCommit(); return true; }
        if (k.ctrl && k.key != Key::Char) return false;
        if (k.key >= Key::F1) return false;
        return editText(gitMsg_, k);
    }

    void gitOpenEntry(const git::Entry& e) {
        if (!gitRoot_) return;
        const fs::path abs = *gitRoot_ / e.path;
        std::error_code ec;
        if (fs::is_regular_file(abs, ec)) openFile(abs.string());
        else setMessage("File no longer exists: " + e.path, true, 2.5f);
    }

    void gitPanelClick() {
        gitMsgFocus_ = false;
        const GitHit* hit = nullptr;
        for (auto it = gitHits_.rbegin(); it != gitHits_.rend(); ++it)
            if (it->r.contains(mx_, my_)) { hit = &*it; break; }
        if (!hit) return;
        auto entryOf = [&](const GitHit& h) -> const git::Entry* {
            int n = 0;
            for (const auto& e : gitEntries_) {
                if ((h.sec == 0 && !e.staged()) || (h.sec == 1 && !e.unstaged())) continue;
                if (n++ == h.idx) return &e;
            }
            return nullptr;
        };
        switch (hit->kind) {
            case 1: gitMsgFocus_ = true; gitMsg_.cur = caretFromX(gitMsg_.text, mx_ - (hit->r.x + 10 * S())); break;
            case 2: gitCommit(); break;
            case 20: case 21: case 22: gitSub_ = hit->kind - 20; gitScroll_ = 0; gitMsgFocus_ = false; break;
            case 30: gitAction([](const fs::path& r) { return git::fetch(r); }, "Fetched"); gitInvalidateLists(); break;
            case 31: gitAction([](const fs::path& r) { return git::pull(r); }, "Pulled"); gitInvalidateLists(); break;
            case 32: gitAction([](const fs::path& r) { return git::push(r); }, "Pushed"); break;
            case 40: if (hit->idx >= 0 && hit->idx < static_cast<int>(gitLog_.size())) gitToggleCommit(gitLog_[static_cast<std::size_t>(hit->idx)].hash); break;
            case 41:
                if (hit->sec >= 0 && hit->sec < static_cast<int>(gitLog_.size())) {
                    auto it = gitDetails_.find(gitLog_[static_cast<std::size_t>(hit->sec)].hash);
                    if (it != gitDetails_.end() && hit->idx < static_cast<int>(it->second.files.size())) {
                        git::Entry e; e.path = it->second.files[static_cast<std::size_t>(hit->idx)].path;
                        gitOpenEntry(e);
                    }
                }
                break;
            case 50:
                if (hit->idx >= 0 && hit->idx < static_cast<int>(gitBranches_.size())) {
                    const auto& b = gitBranches_[static_cast<std::size_t>(hit->idx)];
                    std::string name = b.name;
                    if (b.remote) name = name.substr(name.find('/') + 1);   // origin/feature -> feature (git creates the tracking branch)
                    gitAction([name](const fs::path& r) { return git::checkout(r, name); }, "Switched to " + name);
                    gitInvalidateLists();
                }
                break;
            case 3: gitAction([](const fs::path& r) { return git::stageAll(r); }); break;
            case 4: gitAction([](const fs::path& r) { return git::unstageAll(r); }); break;
            case 10: if (auto* e = entryOf(*hit)) gitOpenEntry(*e); break;
            case 11: if (auto* e = entryOf(*hit)) { const std::string p = e->path; gitAction([p](const fs::path& r) { return git::stage(r, {p}); }); } break;
            case 12: if (auto* e = entryOf(*hit)) { const std::string p = e->path; gitAction([p](const fs::path& r) { return git::unstage(r, {p}); }); } break;
            case 13:
                if (auto* e = entryOf(*hit)) {
                    const auto now = Clock::now();
                    if (gitDiscardArm_ == e->path && now - gitDiscardAt_ < std::chrono::seconds(4)) {
                        gitDiscardArm_.clear();
                        const git::Entry copy = *e;
                        gitAction([copy](const fs::path& r) { return git::discard(r, copy); }, "Discarded changes to " + copy.path);
                    } else {
                        gitDiscardArm_ = e->path;
                        gitDiscardAt_ = now;
                        setMessage("Click again to discard the changes to " + e->path, true, 4);
                    }
                }
                break;
            default: break;
        }
        dirty_ = true;
    }

    static Color gitLetterColor(char ch) {
        return (ch == 'U' || ch == 'A') ? Color::rgb(0x73c990) : (ch == 'D' || ch == '!') ? Color::rgb(0xe06c75) : ch == 'R' ? Color::rgb(0x61afef) : Color::rgb(0xe5c07b);
    }

    void drawGitChanges(const Rect& body) {
        const float s = S();
        if (!gitRoot_) {
            label(body.x + 14 * s, body.y + 0.5f * lh(), "Not a git repository", theme::lineNum);
            label(body.x + 14 * s, body.y + 1.5f * lh(), "Run `git init` in the project folder to track changes here.", theme::lineNum);
            return;
        }
        std::vector<const git::Entry*> staged, changes;
        for (const auto& e : gitEntries_) {
            if (e.staged()) staged.push_back(&e);
            if (e.unstaged()) changes.push_back(&e);
        }
        const float top = std::round(58 * s), rowH = std::round(lh() + 8 * s), headH = std::round(lh() + 12 * s);
        const float listH = std::max(0.0f, body.h - top);
        const float contentH = (staged.empty() ? 0 : headH + static_cast<float>(staged.size()) * rowH) +
                               (changes.empty() ? 0 : headH + static_cast<float>(changes.size()) * rowH);
        gitMax_ = std::max(0.0f, contentH - listH + 8 * s);
        gitScroll_ = std::clamp(gitScroll_, 0.0f, gitMax_);
        float y = body.y + top - std::round(gitScroll_);

        auto section = [&](const char* title, const std::vector<const git::Entry*>& list, int sec) {
            if (list.empty()) return;
            const float hy = y;
            uiLabel(body.x + 14 * s, hy, headH, std::format("{}  {}", title, list.size()), theme::dim);
            const std::string btn = sec == 0 ? "Unstage All" : "Stage All";
            const float bw = uiW(btn) + 18 * s;
            const Rect br{body.right() - bw - 14 * s, hy + 3 * s, bw, headH - 6 * s};
            const bool bh = hy + headH > body.y + top && br.contains(mx_, my_);
            if (hy >= body.y + top - 1) { gitHits_.push_back({br, sec == 0 ? 4 : 3, sec, 0}); if (bh) fill(br, Color::rgb(0xffffff, 26), 6 * s); }
            uiLabelC(br.x + br.w / 2, br.y, br.h, btn, bh ? theme::text : theme::dim);
            y += headH;
            int idx = 0;
            for (const git::Entry* e : list) {
                const Rect row{body.x + 6 * s, y, body.w - 12 * s, rowH - 2 * s};
                const bool visible = y + rowH > body.y + top && y < body.bottom();
                if (visible) {
                    const bool inside = y >= body.y + top - 1;
                    const bool hot = gitHover_.sec == sec && gitHover_.idx == idx && (gitHover_.kind == 10 || gitHover_.kind >= 11);
                    if (inside) gitHits_.push_back({row, 10, sec, idx});
                    if (hot) fill(row, Color::rgb(0xffffff, 16), 7 * s);
                    const fs::path pth(e->path);
                    drawFileIcon(row.x + 10 * s, row.y + (row.h - iconSize()) / 2, pth.filename().string());
                    float x = row.x + 10 * s + iconSize() + 10 * s;
                    const char ch = e->x == '?' ? 'U' : (e->conflicted() ? '!' : (sec == 0 ? e->x : (e->y != ' ' ? e->y : e->x)));
                    const Color lc = gitLetterColor(ch);
                    x = uiLabel(x, row.y, row.h, pth.filename().string(), lc.mix(theme::text, 0.55f));
                    if (pth.has_parent_path()) uiLabel(x + 10 * s, row.y, row.h, pth.parent_path().string(), theme::lineNum, row.right() - 110 * s);
                    float rx = row.right() - 12 * s;
                    const std::string letter(1, ch);
                    rx -= uiW(letter);
                    uiLabel(rx, row.y, row.h, letter, lc);
                    if (hot && inside) {   // row actions
                        const float bs = std::round(row.h - 4 * s);
                        auto act = [&](int kind, char32_t glyph) {
                            rx -= bs + 6 * s;
                            const Rect ar{rx, row.y + 2 * s, bs, bs};
                            gitHits_.push_back({ar, kind, sec, idx});
                            const bool ah = ar.contains(mx_, my_);
                            if (ah) fill(ar, Color::rgb(0xffffff, 40), 6 * s);
                            const int ip = std::max(12, static_cast<int>(std::round(bs * 0.62f)));
                            Icons::glyph(dl_, font_, ar.x + (bs - static_cast<float>(ip)) / 2, ar.y + (bs - static_cast<float>(ip)) / 2, ip, glyph, ah ? Color::rgb(0xffffff) : theme::dim);
                        };
                        if (sec == 0) act(12, 0xF0374);
                        else { act(11, 0xF0415); act(13, 0xF054C); }
                    }
                }
                y += rowH;
                ++idx;
            }
        };
        section("Staged Changes", staged, 0);
        section("Changes", changes, 1);
        if (gitEntries_.empty()) label(body.x + 14 * s, body.y + top + 0.5f * lh(), "No changes - the working tree is clean", theme::lineNum);

        // Commit box (drawn last so scrolled rows disappear beneath it).
        fill(body.x, body.y, body.w, top - 2 * s, theme::panelBg);
        const float bw = std::round(112 * s);
        const Rect field{body.x + 12 * s, body.y + 10 * s, body.w - 24 * s - bw - 10 * s, std::round(34 * s)};
        gitHits_.push_back({field, 1, 0, 0});
        fill(field, gitMsgFocus_ ? theme::accent : Color::rgb(0xffffff, 34), 8 * s);
        fill(field.x + 1, field.y + 1, field.w - 2, field.h - 2, Color::rgb(0x07080a), 7.5f * s);
        const float fx0 = field.x + 10 * s, fmax = field.right() - 10 * s;
        const std::size_t cur = std::min(gitMsg_.cur, gitMsg_.text.size());
        std::size_t start = 0;
        while (start < cur && uiW(std::string_view(gitMsg_.text).substr(start, cur - start)) > fmax - fx0 - 4 * s) start = utf8Next(gitMsg_.text, start);
        if (gitMsg_.text.empty()) uiLabel(fx0, field.y, field.h, gitBranch_.empty() || body.w < 520 * s ? "Commit message" : "Commit message  (Enter to commit to " + gitBranch_ + ")", theme::lineNum, fmax);
        else uiLabel(fx0, field.y, field.h, std::string_view(gitMsg_.text).substr(start), theme::text, fmax);
        if (gitMsgFocus_ && caretOn_ && focused_) {
            const float cx = fx0 + uiW(std::string_view(gitMsg_.text).substr(start, cur - start));
            fill(std::round(cx), field.y + 8 * s, std::max(2.0f, std::round(1.4f * s)), field.h - 16 * s, theme::caret, s);
        }
        const Rect cb{field.right() + 10 * s, field.y, bw, field.h};
        gitHits_.push_back({cb, 2, 0, 0});
        const bool ready = !staged.empty() && !git::trim(gitMsg_.text).empty();
        const bool chot = cb.contains(mx_, my_);
        fill(cb, ready ? (chot ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : theme::accent) : Color::rgb(0xffffff, chot ? 40 : 26), 9 * s);
        uiLabelC(cb.x + cb.w / 2, cb.y, cb.h, "Commit", ready ? Color::rgb(0xffffff) : theme::dim);
    }

    // ---- git tab: header, history (commit graph) and branches

    int gitSub_ = 0;   // 0 changes, 1 history, 2 branches
    std::vector<git::LogEntry> gitLog_;
    std::vector<git::Branch> gitBranches_;
    bool gitLogLoaded_ = false, gitLogBusy_ = false, gitBranchesLoaded_ = false, gitBranchesBusy_ = false;
    struct GraphRow { int lane = 0; bool hasTop = false; std::vector<int> through, converge, parentLanes; };
    std::vector<GraphRow> gitGraph_;
    int gitGraphWidth_ = 1;
    std::string gitExpanded_;
    struct GitDetail { std::vector<std::string> card; std::vector<git::FileChange> files; bool ready = false; };
    std::unordered_map<std::string, GitDetail> gitDetails_;

    void gitInvalidateLists() { gitLogLoaded_ = false; gitBranchesLoaded_ = false; gitDetails_.clear(); }

    // Assigns every commit a lane and the edges to draw in its row (the classic "git log --graph" layout).
    void buildGraph() {
        gitGraph_.clear();
        gitGraphWidth_ = 1;
        std::vector<std::string> lanes;
        for (const auto& c : gitLog_) {
            GraphRow gr;
            std::vector<int> expecting;
            for (std::size_t j = 0; j < lanes.size(); ++j) if (lanes[j] == c.hash) expecting.push_back(static_cast<int>(j));
            int lane = -1;
            if (!expecting.empty()) lane = expecting.front();
            else {
                for (std::size_t j = 0; j < lanes.size(); ++j) if (lanes[j].empty()) { lane = static_cast<int>(j); break; }
                if (lane < 0) { lane = static_cast<int>(lanes.size()); lanes.emplace_back(); }
            }
            gr.lane = lane;
            gr.hasTop = !expecting.empty();
            for (std::size_t j = 0; j < lanes.size(); ++j)
                if (!lanes[j].empty() && lanes[j] != c.hash) gr.through.push_back(static_cast<int>(j));
            for (std::size_t k = 1; k < expecting.size(); ++k) gr.converge.push_back(expecting[k]);
            for (int j : expecting) lanes[static_cast<std::size_t>(j)].clear();
            for (std::size_t pi = 0; pi < c.parents.size(); ++pi) {
                const std::string& p = c.parents[pi];
                int k = -1;
                for (std::size_t j = 0; j < lanes.size(); ++j) if (lanes[j] == p) { k = static_cast<int>(j); break; }
                if (k < 0) {
                    if (pi == 0) k = lane;
                    else {
                        for (std::size_t j = 0; j < lanes.size(); ++j) if (lanes[j].empty() && static_cast<int>(j) != lane) { k = static_cast<int>(j); break; }
                        if (k < 0) { k = static_cast<int>(lanes.size()); lanes.emplace_back(); }
                    }
                    if (static_cast<std::size_t>(k) >= lanes.size()) lanes.resize(static_cast<std::size_t>(k) + 1);
                    lanes[static_cast<std::size_t>(k)] = p;
                }
                gr.parentLanes.push_back(k);
            }
            while (!lanes.empty() && lanes.back().empty()) lanes.pop_back();
            int w = std::max(static_cast<int>(lanes.size()), lane + 1);
            for (int j : gr.through) w = std::max(w, j + 1);
            for (int j : gr.parentLanes) w = std::max(w, j + 1);
            gitGraphWidth_ = std::max(gitGraphWidth_, w);
            gitGraph_.push_back(std::move(gr));
        }
    }

    // Loads the lists the current sub-tab needs (off the UI thread).
    void gitEnsureLoaded() {
        if (!gitRoot_) return;
        const fs::path root = *gitRoot_;
        if (gitSub_ == 1 && !gitLogLoaded_ && !gitLogBusy_) {
            gitLogBusy_ = true;
            spawn([this, root] {
                auto lg = git::log(root, 400);
                return std::function<void()>([this, lg = std::move(lg)]() mutable {
                    gitLogBusy_ = false;
                    gitLog_ = std::move(lg);
                    gitLogLoaded_ = true;
                    buildGraph();
                });
            });
        }
        if (gitSub_ == 2 && !gitBranchesLoaded_ && !gitBranchesBusy_) {
            gitBranchesBusy_ = true;
            spawn([this, root] {
                auto br = git::branches(root);
                return std::function<void()>([this, br = std::move(br)]() mutable {
                    gitBranchesBusy_ = false;
                    gitBranches_ = std::move(br);
                    gitBranchesLoaded_ = true;
                });
            });
        }
    }

    void gitToggleCommit(const std::string& hash) {
        if (gitExpanded_ == hash) { gitExpanded_.clear(); return; }
        gitExpanded_ = hash;
        if (gitDetails_.contains(hash) || !gitRoot_) return;
        gitDetails_[hash] = {};
        const fs::path root = *gitRoot_;
        spawn([this, root, hash] {
            auto card = git::commitCard(root, hash);
            auto files = git::commitFiles(root, hash);
            return std::function<void()>([this, hash, card = std::move(card), files = std::move(files)]() mutable {
                auto& d = gitDetails_[hash];
                d.card = std::move(card);
                d.files = std::move(files);
                d.ready = true;
            });
        });
    }

    static Color laneColor(int lane) {
        static constexpr std::array<std::uint32_t, 7> pal = {0xe5484d, 0x5b9bff, 0x73c990, 0xe5c07b, 0xc586c0, 0x4ec9b0, 0xf0a04b};
        return Color::rgb(pal[static_cast<std::size_t>(lane) % pal.size()]);
    }

    // A straight or slanted line drawn from small squares (the renderer only has rects / rounded rects).
    void gLine(float x0, float y0, float x1, float y1, Color c, float w) {
        if (x0 == x1) { fill(x0 - w / 2, std::min(y0, y1), w, std::abs(y1 - y0) + 0.5f, c); return; }
        const float len = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        const int n = std::max(2, static_cast<int>(len / (w * 0.6f)));
        for (int i = 0; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            fill(x0 + (x1 - x0) * t - w / 2, y0 + (y1 - y0) * t - w / 2, w, w, c, w / 2);
        }
    }

    void drawChip(float& x, float y, float h, const std::string& text, Color c) {
        const float s = S();
        const float w = uiW(text) + 14 * s;
        fill(x, y + 2 * s, w, h - 4 * s, c.alpha(60), (h - 4 * s) / 2);
        uiLabelC(x + w / 2, y, h, text, c.mix(Color::rgb(0xffffff), 0.35f));
        x += w + 6 * s;
    }

    void drawGitHeader(const Rect& body, float headH) {
        const float s = S();
        static constexpr std::array<std::string_view, 3> names = {"Changes", "History", "Branches"};
        float x = body.x + 10 * s;
        for (std::size_t i = 0; i < 3; ++i) {
            std::string t(names[i]);
            const float w = uiW(t) + (body.w < 560 * s ? 12 : 24) * s;
            const Rect r{x, body.y + 5 * s, w, headH - 10 * s};
            gitHits_.push_back({r, 20 + static_cast<int>(i), 0, 0});
            const bool on = gitSub_ == static_cast<int>(i), hot = r.contains(mx_, my_);
            if (on) fill(r, Color::rgb(0xffffff, 26), 8 * s);
            else if (hot) fill(r, Color::rgb(0xffffff, 12), 8 * s);
            uiLabelC(r.x + r.w / 2, r.y, r.h, t, on ? theme::text : theme::dim);
            x += w + 4 * s;
        }
        // Sync buttons on the right: fetch / pull / push with ahead-behind counts.
        float rx = body.right() - 10 * s;
        struct B { int kind; char32_t icon; std::string label; };
        const std::array<B, 3> bs = {{{32, 0xF005D, gitAhead_ ? std::format("Push {}", gitAhead_) : std::string("Push")},
                                      {31, 0xF0045, gitBehind_ ? std::format("Pull {}", gitBehind_) : std::string("Pull")},
                                      {30, 0xF04E6, "Fetch"}}};
        const bool compact = body.w < 560 * s;   // the sidebar is narrow: icons only
        for (const auto& b : bs) {
            const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.95f)));
            const float w = compact ? static_cast<float>(ip) + 14 * s : uiW(b.label) + static_cast<float>(ip) + 28 * s;
            const Rect r{rx - w, body.y + 5 * s, w, headH - 10 * s};
            gitHits_.push_back({r, b.kind, 0, 0});
            const bool hot = r.contains(mx_, my_);
            fill(r, Color::rgb(0xffffff, hot ? 34 : 18), 8 * s);
            Icons::glyph(dl_, font_, r.x + (compact ? (r.w - static_cast<float>(ip)) / 2 : 10 * s), r.y + (r.h - static_cast<float>(ip)) / 2, ip, b.icon, hot ? theme::text : theme::dim);
            if (!compact) uiLabel(r.x + 10 * s + static_cast<float>(ip) + 6 * s, r.y, r.h, b.label, hot ? theme::text : theme::dim);
            rx -= w + 6 * s;
        }
    }

    void drawGitHistory(const Rect& body) {
        const float s = S();
        if (!gitLogLoaded_) { label(body.x + 14 * s, body.y + 0.5f * lh(), gitLogBusy_ ? "Loading history..." : "No commits yet", theme::lineNum); return; }
        if (gitLog_.empty()) { label(body.x + 14 * s, body.y + 0.5f * lh(), "No commits yet", theme::lineNum); return; }
        const float rowH = std::round(lh() + 10 * s), lane = std::round(14 * s);
        const bool narrow = body.w < 520 * s;
        const int showLanes = std::min(gitGraphWidth_, narrow ? 5 : 9);
        const float gw = static_cast<float>(showLanes) * lane + 12 * s;
        const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const float detailLine = std::round(lh() + 2 * s);

        // Total height (expanded commits take extra rows).
        float total = 0;
        for (const auto& c : gitLog_) {
            total += rowH;
            if (c.hash == gitExpanded_) {
                auto it = gitDetails_.find(c.hash);
                total += (it == gitDetails_.end() || !it->second.ready ? 1.0f : static_cast<float>(it->second.card.size() + it->second.files.size())) * detailLine + 8 * s;
            }
        }
        gitMax_ = std::max(0.0f, total - body.h + 8 * s);
        gitScroll_ = std::clamp(gitScroll_, 0.0f, gitMax_);

        float y = body.y - std::round(gitScroll_);
        const auto laneX = [&](int l) { return body.x + 16 * s + static_cast<float>(std::min(l, showLanes - 1)) * lane; };
        for (std::size_t i = 0; i < gitLog_.size(); ++i) {
            const auto& c = gitLog_[i];
            const GraphRow& gr = gitGraph_[i];
            const bool expanded = c.hash == gitExpanded_;
            float extra = 0;
            const GitDetail* det = nullptr;
            if (expanded) {
                auto it = gitDetails_.find(c.hash);
                if (it != gitDetails_.end()) det = &it->second;
                extra = (det && det->ready ? static_cast<float>(det->card.size() + det->files.size()) : 1.0f) * detailLine + 8 * s;
            }
            const float rowTotal = rowH + extra;
            if (y + rowTotal < body.y) { y += rowTotal; continue; }
            if (y > body.bottom()) break;
            const Rect row{body.x + 4 * s, y, body.w - 8 * s, rowH - 1};
            const bool inside = y >= body.y - 1;
            if (inside) gitHits_.push_back({row, 40, 0, static_cast<int>(i)});
            const bool hot = row.contains(mx_, my_) || expanded;
            if (hot) fill(row, Color::rgb(0xffffff, expanded ? 20 : 12), 7 * s);

            // Graph.
            const float yc = y + rowH / 2, yb = y + rowTotal;
            const float lw = std::max(1.5f, std::round(1.8f * s));
            for (int j : gr.through) gLine(laneX(j), y, laneX(j), yb, laneColor(j), lw);
            if (gr.hasTop) gLine(laneX(gr.lane), y, laneX(gr.lane), yc, laneColor(gr.lane), lw);
            for (int j : gr.converge) gLine(laneX(j), y, laneX(gr.lane), yc, laneColor(j), lw);
            for (int pl : gr.parentLanes) {
                gLine(laneX(gr.lane), yc, laneX(pl), y + rowH, laneColor(pl), lw);
                if (extra > 0) gLine(laneX(pl), y + rowH, laneX(pl), yb, laneColor(pl), lw);
            }
            const bool isHead = c.hash == gitHead_;
            const float nr = std::round((isHead ? 5.5f : 4.5f) * s);
            fill(laneX(gr.lane) - nr - (isHead ? s : 0), yc - nr - (isHead ? s : 0), 2 * (nr + (isHead ? s : 0)), 2 * (nr + (isHead ? s : 0)), isHead ? Color::rgb(0xffffff) : theme::panelBg, nr + s);
            fill(laneX(gr.lane) - nr + s, yc - nr + s, 2 * nr - 2 * s, 2 * nr - 2 * s, laneColor(gr.lane), nr);

            // Text: refs, subject, then author / time / hash on the right.
            float x = body.x + gw + 8 * s;
            const float right = row.right() - 12 * s;
            float rx = right;
            if (!narrow) {
                const std::string hashTxt = c.shortHash;
                rx -= uiW(hashTxt);
                uiLabel(rx, y, rowH, hashTxt, theme::lineNum);
            }
            std::string when = git::relativeTime(c.time, now);
            if (narrow) {   // "3 days ago" -> "3d"
                const auto sp = when.find(' ');
                when = when == "just now" ? "now" : when.substr(0, sp) + when.substr(sp + 1, 1);
            }
            rx -= uiW(when) + (narrow ? 0.0f : 16 * s);
            uiLabel(rx, y, rowH, when, theme::lineNum);
            if (!narrow) {
                const std::string who = c.author;
                const float ww = std::min(uiW(who), 150 * s);
                rx -= ww + 16 * s;
                uiLabel(rx, y, rowH, who, theme::dim, rx + ww);
            }
            const float textMax = rx - 12 * s;
            if (!c.refs.empty()) {
                for (std::size_t p = 0; p < c.refs.size();) {
                    auto q = c.refs.find(", ", p);
                    if (q == std::string::npos) q = c.refs.size();
                    std::string ref = c.refs.substr(p, q - p);
                    p = q + 2;
                    Color rc = Color::rgb(0x61afef);   // remote branch
                    bool head = false;
                    if (ref.starts_with("HEAD -> ")) { ref = ref.substr(8); head = true; rc = theme::accent.mix(Color::rgb(0xffffff), 0.1f); }
                    else if (ref.starts_with("tag: ")) { ref = ref.substr(5); rc = Color::rgb(0xe5c07b); }
                    else if (ref == "HEAD") { rc = Color::rgb(0xffffff); }
                    else if (ref.find('/') == std::string::npos) rc = Color::rgb(0x73c990);   // local branch
                    if (x + uiW(ref) + 20 * s < textMax) drawChip(x, y, rowH, (head ? "\xE2\x97\x8F " : "") + ref, rc);
                }
            }
            uiLabel(x, y, rowH, c.subject, expanded ? theme::text : Color::rgb(0xc4c8d0), textMax);

            // Expanded: commit card + changed files.
            if (expanded) {
                float dy = y + rowH + 4 * s;
                const float dx = body.x + gw + 8 * s;
                if (!det || !det->ready) {
                    uiLabel(dx, dy, detailLine, "Loading...", theme::lineNum);
                } else {
                    for (const auto& l : det->card) { uiLabel(dx, dy, detailLine, l, l.starts_with("commit ") ? theme::lineNum : theme::dim, right); dy += detailLine; }
                    int fi = 0;
                    for (const auto& f : det->files) {
                        const Rect fr{dx - 4 * s, dy, right - dx + 8 * s, detailLine};
                        if (dy >= body.y - 1 && dy + detailLine <= body.bottom() + 1) gitHits_.push_back({fr, 41, static_cast<int>(i), fi});
                        if (fr.contains(mx_, my_)) fill(fr, Color::rgb(0xffffff, 14), 5 * s);
                        const Color fc = gitLetterColor(f.status == 'A' ? 'A' : f.status == 'D' ? 'D' : f.status == 'R' ? 'R' : 'M');
                        const std::string st(1, f.status);
                        uiLabel(dx, dy, detailLine, st, fc);
                        uiLabel(dx + 22 * s, dy, detailLine, f.path, theme::text, right);
                        dy += detailLine;
                        ++fi;
                    }
                }
            }
            y += rowTotal;
        }
    }

    void drawGitBranches(const Rect& body) {
        const float s = S();
        if (!gitBranchesLoaded_) { label(body.x + 14 * s, body.y + 0.5f * lh(), "Loading branches...", theme::lineNum); return; }
        const float rowH = std::round(lh() + 10 * s), headH = std::round(lh() + 12 * s);
        std::vector<int> local, remote;
        for (std::size_t i = 0; i < gitBranches_.size(); ++i) (gitBranches_[i].remote ? remote : local).push_back(static_cast<int>(i));
        const float total = (local.empty() ? 0 : headH + static_cast<float>(local.size()) * rowH) + (remote.empty() ? 0 : headH + static_cast<float>(remote.size()) * rowH);
        gitMax_ = std::max(0.0f, total - body.h + 8 * s);
        gitScroll_ = std::clamp(gitScroll_, 0.0f, gitMax_);
        float y = body.y - std::round(gitScroll_);
        auto section = [&](const char* title, const std::vector<int>& list) {
            if (list.empty()) return;
            uiLabel(body.x + 14 * s, y, headH, std::format("{}  {}", title, list.size()), theme::dim);
            y += headH;
            for (int bi : list) {
                const auto& b = gitBranches_[static_cast<std::size_t>(bi)];
                const Rect row{body.x + 6 * s, y, body.w - 12 * s, rowH - 2 * s};
                const bool visible = y + rowH > body.y && y < body.bottom();
                if (visible) {
                    const bool inside = y >= body.y - 1;
                    const bool hot = row.contains(mx_, my_);
                    if (hot) fill(row, Color::rgb(0xffffff, 16), 7 * s);
                    const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.95f)));
                    Icons::glyph(dl_, font_, row.x + 10 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, 0xF062C, b.current ? Color::rgb(0x73c990) : theme::dim);
                    float x = uiLabel(row.x + 10 * s + static_cast<float>(ip) + 8 * s, row.y, row.h, b.name, b.current ? Color::rgb(0x73c990) : theme::text);
                    if (b.current) { float cx = x + 10 * s; drawChip(cx, row.y, row.h, "current", Color::rgb(0x73c990)); x = cx; }
                    else if (!b.upstream.empty()) { x += 10 * s; uiLabel(x, row.y, row.h, "\xE2\x86\x92 " + b.upstream, theme::lineNum); }
                    float rx = row.right() - 12 * s;
                    if (hot && !b.current && inside) {
                        const float bw = uiW("Checkout") + 20 * s;
                        const Rect br{rx - bw, row.y + 2 * s, bw, row.h - 4 * s};
                        gitHits_.push_back({br, 50, 0, bi});
                        const bool bh = br.contains(mx_, my_);
                        fill(br, bh ? theme::accent.mix(Color::rgb(0xffffff), 0.12f) : theme::accent, 7 * s);
                        uiLabelC(br.x + br.w / 2, br.y, br.h, "Checkout", Color::rgb(0xffffff));
                        rx = br.x - 12 * s;
                    }
                    if (body.w >= 520 * s) {
                        const float sx = std::max(x + 40 * s, row.x + row.w * 0.45f);
                        uiLabel(rx - uiW(b.hash), row.y, row.h, b.hash, theme::lineNum);
                        rx -= uiW(b.hash) + 14 * s;
                        uiLabel(sx, row.y, row.h, b.subject, theme::dim, rx);
                    }
                }
                y += rowH;
            }
        };
        section("Local", local);
        section("Remote", remote);
        if (gitBranches_.empty()) label(body.x + 14 * s, body.y + 0.5f * lh(), "No branches", theme::lineNum);
    }

    void drawGitBody(const Rect& body) {
        const float s = S();
        gitHits_.clear();
        if (!gitRoot_) {
            label(body.x + 14 * s, body.y + 0.5f * lh(), "Not a git repository", theme::lineNum);
            label(body.x + 14 * s, body.y + 1.5f * lh(), "Run `git init` in the project folder to track changes here.", theme::lineNum);
            return;
        }
        gitEnsureLoaded();
        const float headH = std::round(uiH() + 20 * s);
        const Rect content{body.x, body.y + headH, body.w, std::max(0.0f, body.h - headH)};
        if (gitSub_ == 0) drawGitChanges(content);
        else if (gitSub_ == 1) drawGitHistory(content);
        else drawGitBranches(content);
        fill(body.x, body.y, body.w, headH, theme::panelBg);   // the header covers rows scrolled beneath it
        drawGitHeader(body, headH);
    }

    // ============================================================ integrated terminal (bottom panel, "Terminal" tab)

    Terminal term_{80, 24};
    std::unique_ptr<Process> termProc_;
    bool termFocus_ = false, termExited_ = false;
    int termScroll_ = 0;           // lines scrolled back from the live screen
    TextSel tsel_;                 // selection in absolute terminal rows (see Terminal::absRow)
    int termDrawCols_ = 0, termDrawRows_ = 0;

    void startTerminal() {
        if (termProc_) return;
        std::string shell = std::getenv("SHELL") ? std::getenv("SHELL") : "";
        if (shell.empty() || !findExecutable(shell)) shell = findExecutable("bash") ? "bash" : "sh";
        ProcessOptions o;
        o.usePty = true;
        o.argv = {shell};
        o.cwd = cmakeOpen_ ? cmakeRoot_.string() : (explorer_.isOpen() ? explorer_.rootPath().string() : homeDir());
        o.env = {{"TERM", "xterm-256color"}, {"COLORTERM", "truecolor"}, {"TERM_PROGRAM", "daemon"}};
        termProc_ = std::make_unique<Process>();
        term_.reset();
        if (auto r = termProc_->start(o); !r) {
            term_.feed("Cannot start the shell: " + r.error() + "\r\n");
            termProc_.reset();
            termExited_ = true;
            return;
        }
        termExited_ = false;
        termScroll_ = 0;
        tsel_ = {};
        termProc_->resizePty(term_.cols(), term_.rows());
        win_.watch(termProc_->outFd(), [this] { pumpTerminal(); });
        dirty_ = true;
    }

    void pumpTerminal() {
        if (!termProc_) return;
        std::string chunk;
        const long n = termProc_->read(chunk);
        if (!chunk.empty()) {
            const int before = term_.scrollbackSize();
            term_.feed(chunk);
            if (std::string reply = term_.takeReply(); !reply.empty()) termProc_->write(reply);
            if (termScroll_ > 0) termScroll_ = std::min(termScroll_ + (term_.scrollbackSize() - before), term_.scrollbackSize());   // keep the view still
        }
        dirty_ = true;
        if (n == 0) {   // shell ended
            win_.unwatch(termProc_->outFd());
            termProc_->closeOutput();
            const int code = termProc_->tryWait().value_or(0);
            termProc_.reset();
            termExited_ = true;
            term_.feed(std::format("\r\n\x1b[2m[shell exited with code {} - press Enter to start a new one]\x1b[0m\r\n", code));
        }
    }

    void openTerminal() {
        panelOpen_ = true;
        panelTab_ = 3;
        termFocus_ = true;
        if (!termProc_ && !termExited_) startTerminal();
        dirty_ = true;
    }

    static std::string termMods(const KeyEvent& k) {
        const int m = 1 + (k.shift ? 1 : 0) + (k.alt ? 2 : 0) + (k.ctrl ? 4 : 0);
        return std::to_string(m);
    }

    // Translates a key press into what a terminal sends. Returns false for keys that keep their IDE meaning.
    bool termKey(const KeyEvent& k) {
        if (k.super) return false;
        if (k.ctrl && k.key == Key::Char && k.ch == '`') return false;   // panel toggle
        if (!termProc_) {
            if (k.key == Key::Enter) { startTerminal(); return true; }
            return k.key != Key::None && k.key < Key::F1;   // swallow typing while there is no shell
        }
        if (k.ctrl && k.shift && k.key == Key::Char) {
            if (k.ch == 'c') { termCopy(); return true; }
            if (k.ch == 'v') {
                std::string c = win_.clipboard();
                std::erase(c, '\r');
                if (term_.bracketedPaste()) c = "\x1b[200~" + c + "\x1b[201~";
                termProc_->write(c);
                return true;
            }
            return false;
        }
        if (k.ctrl && !k.shift && k.key == Key::Char && k.ch == 'c' && tsel_.active && tsel_.moved) { termCopy(); tsel_.active = false; return true; }
        std::string out;
        const bool mod = k.shift || k.alt || k.ctrl;
        auto csi = [&](char fin) { return mod ? "\x1b[1;" + termMods(k) + fin : (term_.appCursorKeys() ? std::string("\x1bO") + fin : std::string("\x1b[") + fin); };
        auto tilde = [&](int n) { return mod ? "\x1b[" + std::to_string(n) + ";" + termMods(k) + "~" : "\x1b[" + std::to_string(n) + "~"; };
        switch (k.key) {
            case Key::Enter: out = "\r"; break;
            case Key::Backspace: out = k.alt ? "\x1b\x7f" : "\x7f"; break;
            case Key::Tab: out = k.shift ? "\x1b[Z" : "\t"; break;
            case Key::Escape: out = "\x1b"; break;
            case Key::Up: out = csi('A'); break;
            case Key::Down: out = csi('B'); break;
            case Key::Right: out = csi('C'); break;
            case Key::Left: out = csi('D'); break;
            case Key::Home: out = csi('H'); break;
            case Key::End: out = csi('F'); break;
            case Key::PageUp: out = tilde(5); break;
            case Key::PageDown: out = tilde(6); break;
            case Key::Delete: out = tilde(3); break;
            case Key::Insert: out = tilde(2); break;
            case Key::F1: case Key::F2: case Key::F3: case Key::F4:
                if (k.ctrl || k.shift) return false;
                out = std::string("\x1bO") + static_cast<char>('P' + (static_cast<int>(k.key) - static_cast<int>(Key::F1)));
                break;
            case Key::F5: case Key::F6: case Key::F7: case Key::F8: case Key::F9: case Key::F10: case Key::F11: case Key::F12: {
                if (k.ctrl || k.shift) return false;   // Ctrl+F9 / Shift+F10 ... stay build / run shortcuts
                static constexpr int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
                out = "\x1b[" + std::to_string(codes[static_cast<int>(k.key) - static_cast<int>(Key::F5)]) + "~";
                break;
            }
            case Key::Char:
                if (k.ctrl && !k.alt) {
                    const char32_t c = k.ch;
                    if (c >= 'a' && c <= 'z') out = std::string(1, static_cast<char>(c - 'a' + 1));
                    else if (c == ' ' || c == '@' || c == '2') out = std::string(1, '\0');
                    else if (c == '[') out = "\x1b";
                    else if (c == '\\') out = "\x1c";
                    else if (c == ']') out = "\x1d";
                    else if (c == '/' || c == '_') out = "\x1f";
                    else return false;
                } else if (!k.text.empty()) {
                    out = (k.alt ? "\x1b" : "") + k.text;
                } else if (k.alt && k.ch < 0x80) {
                    out = std::string("\x1b") + static_cast<char>(k.ch);
                } else {
                    return false;
                }
                break;
            default: return false;
        }
        termScroll_ = 0;   // typing jumps back to the live screen
        tsel_.active = false;
        termProc_->write(out);
        return true;
    }

    std::pair<int, int> termPosAt(float x, float y, const Rect& body) const {
        const float pad = 10 * S();
        const int c = std::clamp(static_cast<int>(std::floor((x - (body.x + pad)) / cw())), 0, term_.cols() - 1);
        const int r = std::clamp(static_cast<int>(std::floor((y - body.y) / lh())), 0, term_.rows() - 1);
        return {term_.scrollbackSize() + r - termScroll_, c};
    }

    std::string termSelectedText() const {
        if (!tsel_.active || !tsel_.moved) return {};
        int r0 = tsel_.ar, c0 = tsel_.ac, r1 = tsel_.cr, c1 = tsel_.cc;
        if (r0 > r1 || (r0 == r1 && c0 > c1)) { std::swap(r0, r1); std::swap(c0, c1); }
        std::string out;
        for (int r = r0; r <= r1; ++r) {
            auto row = term_.absRow(r);
            const int a = r == r0 ? c0 : 0;
            int b = r == r1 ? c1 : static_cast<int>(row.size()) - 1;
            std::string line;
            for (int c = a; c <= b && c < static_cast<int>(row.size()); ++c) appendUtf8(line, row[static_cast<std::size_t>(c)].ch);
            while (!line.empty() && line.back() == ' ') line.pop_back();
            if (r > r0) out += '\n';
            out += line;
        }
        return out;
    }

    void termCopy() {
        const std::string t = termSelectedText();
        if (t.empty()) return;
        win_.setClipboard(t);
        setMessage("Copied", false, 1.2f);
    }

    void termClick(const Rect& body) {
        termFocus_ = true;
        if (!termProc_ && termExited_) return;
        const auto [r, c] = termPosAt(mx_, my_, body);
        tsel_ = {};
        tsel_.active = true;
        tsel_.dragging = true;
        tsel_.ar = tsel_.cr = r;
        tsel_.ac = tsel_.cc = c;
        tsel_.sx = mx_; tsel_.sy = my_;
    }

    static Color termColor(std::uint32_t c, Color def) { return c ? Color::rgb(c & 0xFFFFFF) : def; }

    void drawTerminalBody(const Rect& body) {
        const float s = S(), pad = 10 * s;
        const int cols = std::max(10, static_cast<int>((body.w - 2 * pad - 8 * s) / cw()));
        const int rows = std::max(2, static_cast<int>(body.h / lh()));
        if (cols != term_.cols() || rows != term_.rows()) {
            term_.resize(cols, rows);
            if (termProc_) termProc_->resizePty(cols, rows);
        }
        if (!termProc_ && !termExited_) startTerminal();
        termScroll_ = std::clamp(termScroll_, 0, term_.altScreen() ? 0 : term_.scrollbackSize());
        const float cwid = cw(), lineH = lh();
        int s0r = 0, s0c = 0, s1r = -1, s1c = 0;
        const bool haveSel = tsel_.active && tsel_.moved;
        if (haveSel) {
            s0r = tsel_.ar; s0c = tsel_.ac; s1r = tsel_.cr; s1c = tsel_.cc;
            if (s0r > s1r || (s0r == s1r && s0c > s1c)) { std::swap(s0r, s1r); std::swap(s0c, s1c); }
        }
        const Color defFg = theme::text, defBg = theme::panelBg;
        for (int r = 0; r < term_.rows(); ++r) {
            const float y = body.y + static_cast<float>(r) * lineH;
            auto row = term_.viewRow(r, termScroll_);
            const int absr = term_.scrollbackSize() + r - termScroll_;
            const int n = std::min(static_cast<int>(row.size()), term_.cols());
            // Backgrounds in runs.
            for (int c = 0; c < n;) {
                const TermCell& cell = row[static_cast<std::size_t>(c)];
                const bool inv = cell.attr & TermCell::Inverse;
                std::uint32_t bgv = inv ? (cell.fg ? cell.fg : 0xFF000000u | 0xcdd0d6u) : cell.bg;
                bool sel = haveSel && ((absr > s0r || (absr == s0r && c >= s0c)) && (absr < s1r || (absr == s1r && c <= s1c)));
                int e = c + 1;
                while (e < n) {
                    const TermCell& o = row[static_cast<std::size_t>(e)];
                    const bool oi = o.attr & TermCell::Inverse;
                    const std::uint32_t ob = oi ? (o.fg ? o.fg : 0xFF000000u | 0xcdd0d6u) : o.bg;
                    const bool os = haveSel && ((absr > s0r || (absr == s0r && e >= s0c)) && (absr < s1r || (absr == s1r && e <= s1c)));
                    if (ob != bgv || os != sel) break;
                    ++e;
                }
                if (sel) fill(body.x + pad + static_cast<float>(c) * cwid, y, static_cast<float>(e - c) * cwid, lineH, Color::rgb(0x3a4152));
                else if (bgv) fill(body.x + pad + static_cast<float>(c) * cwid, y, static_cast<float>(e - c) * cwid, lineH, termColor(bgv, defBg));
                c = e;
            }
            for (int c = 0; c < n; ++c) {
                const TermCell& cell = row[static_cast<std::size_t>(c)];
                const bool inv = cell.attr & TermCell::Inverse;
                Color fg = inv ? (cell.bg ? termColor(cell.bg, defBg) : defBg) : termColor(cell.fg, defFg);
                if (cell.attr & TermCell::Dim) fg = fg.alpha(150);
                if (cell.attr & TermCell::Bold && !cell.fg && !inv) fg = Color::rgb(0xffffff);
                const float x = body.x + pad + static_cast<float>(c) * cwid;
                if (cell.ch != U' ' && cell.ch != 0) dl_.glyph(font_.glyph(cell.ch), x, y + font_.baseline(), fg);
                if (cell.attr & TermCell::Underline) fill(x, y + lineH - std::max(2.0f, std::round(2 * s)), cwid, std::max(1.0f, std::round(s)), fg);
                if (cell.attr & TermCell::Strike) fill(x, y + lineH / 2, cwid, std::max(1.0f, std::round(s)), fg);
            }
        }
        // Cursor.
        if (termScroll_ == 0 && term_.cursorVisible() && termProc_) {
            const float cx = body.x + pad + static_cast<float>(term_.cursorCol()) * cwid, cy = body.y + static_cast<float>(term_.cursorRow()) * lineH;
            if (termFocus_ && focused_) {
                if (caretOn_) {
                    fill(cx, cy, cwid, lineH, theme::caret.alpha(220));
                    const TermCell& cell = term_.cell(term_.cursorRow(), term_.cursorCol());
                    if (cell.ch != U' ') dl_.glyph(font_.glyph(cell.ch), cx, cy + font_.baseline(), defBg);
                }
            } else {
                const float t = std::max(1.0f, std::round(s));
                fill(cx, cy, cwid, t, theme::caret); fill(cx, cy + lineH - t, cwid, t, theme::caret);
                fill(cx, cy, t, lineH, theme::caret); fill(cx + cwid - t, cy, t, lineH, theme::caret);
            }
        }
        // Scrollback indicator.
        if (termScroll_ > 0) {
            const float frac = static_cast<float>(termScroll_) / static_cast<float>(std::max(1, term_.scrollbackSize()));
            const float th = std::max(20 * s, body.h * 0.15f);
            fill(body.right() - 8 * s, body.y + (body.h - th) * (1.0f - frac), 4 * s, th, theme::scrollbar, 2 * s);
        }
    }

    // ============================================================ project search & replace (sidebar "Search" view)

    struct SHit { Rect r; int kind = 0; };   // 1-4 fields, 5 case, 6 word, 7 regex, 8 replace all, 9 filters
    std::vector<SHit> sHits_;
    TextBox sQuery_, sReplace_, sInclude_, sExclude_;
    int sFocus_ = -1;                        // focused field: 0 query, 1 replace, 2 include, 3 exclude
    bool sCase_ = false, sWord_ = false, sRegex_ = false, sFilters_ = false;
    std::vector<search::FileResult> sRes_;
    int sTotal_ = 0;
    bool sTrunc_ = false, sBusy_ = false, sPending_ = false, sSearched_ = false;
    std::string sError_;
    Clock::time_point sRunAt_{}, sReplaceArmAt_{};
    std::shared_ptr<std::atomic<int>> sGen_ = std::make_shared<std::atomic<int>>(0);
    struct SRow { int file = 0, match = -1; };   // match < 0: the file header row
    std::vector<SRow> sRows_;
    bool sRowsDirty_ = true;
    float sScroll_ = 0, sMax_ = 0, sResultsTop_ = 0;
    bool searchView() const { return sidebar_ && sideSearch_; }

    search::Options searchOptions() const {
        search::Options o;
        o.query = sQuery_.text; o.matchCase = sCase_; o.wholeWord = sWord_; o.regex = sRegex_;
        o.include = sInclude_.text; o.exclude = sExclude_.text;
        return o;
    }

    void openSearch(bool replace) {
        if (!explorer_.isOpen()) { setMessage("Open a folder to search in it", false, 2.5f); return; }
        sidebar_ = true; sideSearch_ = true; sideGit_ = false; sideDebug_ = false;
        sideFocus_ = false; gitMsgFocus_ = false;
        if (!docs_.empty() && doc().hasSelection()) {
            std::string t = doc().selectedText();
            if (t.find('\n') == std::string::npos && !t.empty() && t != sQuery_.text) { sQuery_ = {t, t.size()}; sPending_ = true; sRunAt_ = Clock::now(); }
        }
        sFocus_ = replace ? 1 : 0;
        sQuery_.cur = sQuery_.text.size();
        if (replace) sReplace_.cur = sReplace_.text.size();
        dirty_ = true;
    }

    void scheduleSearch() { sPending_ = true; sRunAt_ = Clock::now() + std::chrono::milliseconds(280); dirty_ = true; }

    void rebuildSearchRows() {
        sRows_.clear();
        for (int i = 0; i < static_cast<int>(sRes_.size()); ++i) {
            sRows_.push_back({i, -1});
            if (sRes_[static_cast<std::size_t>(i)].collapsed) continue;
            for (int m = 0; m < static_cast<int>(sRes_[static_cast<std::size_t>(i)].matches.size()); ++m) sRows_.push_back({i, m});
        }
        sRowsDirty_ = false;
    }

    void recountSearch() {
        sTotal_ = 0;
        for (const auto& f : sRes_) sTotal_ += static_cast<int>(f.matches.size());
        sRowsDirty_ = true;
    }

    void startSearch() {
        sPending_ = false;
        const int gen = ++*sGen_;
        sSearched_ = false;
        if (sQuery_.text.empty() || !explorer_.isOpen()) {
            sRes_.clear(); sTotal_ = 0; sError_.clear(); sBusy_ = false; sTrunc_ = false; sRowsDirty_ = true; sScroll_ = 0;
            dirty_ = true;
            return;
        }
        const search::Options o = searchOptions();
        if (search::Matcher m(o); !m.ok()) {   // typos in a regex are reported, not searched
            sRes_.clear(); sTotal_ = 0; sError_ = m.error(); sBusy_ = false; sTrunc_ = false; sRowsDirty_ = true;
            dirty_ = true;
            return;
        }
        std::map<std::string, std::string> overrides;   // unsaved edits are searched too
        for (const auto& d : docs_) if (!d->path.empty() && d->modified()) overrides[d->path] = d->buf.text();
        sBusy_ = true;
        sError_.clear();
        const fs::path root = explorer_.rootPath();
        auto g = sGen_;
        spawn([this, g, gen, root, o, overrides]() -> std::function<void()> {
            auto r = search::run(root, o, overrides, [g, gen] { return g->load() != gen; });
            return [this, g, gen, r = std::move(r)]() mutable {
                if (g->load() != gen) return;   // a newer search superseded this one
                sRes_ = std::move(r.files);
                sTotal_ = r.total; sTrunc_ = r.truncated; sError_ = r.error;
                sBusy_ = false; sSearched_ = true; sRowsDirty_ = true; sScroll_ = 0;
            };
        });
        dirty_ = true;
    }

    void searchOpenMatch(int fi, int mi) {
        if (fi < 0 || fi >= static_cast<int>(sRes_.size())) return;
        const auto& f = sRes_[static_cast<std::size_t>(fi)];
        openFile(f.path);
        if (docs_.empty() || doc().path != f.path) return;
        if (mi >= 0 && mi < static_cast<int>(f.matches.size())) {
            const auto& m = f.matches[static_cast<std::size_t>(mi)];
            doc().select({m.line, m.col}, {m.line, m.col + m.len});
            scrollToCursor_ = true;
        }
        sFocus_ = -1;
        sideFocus_ = false;
        dirty_ = true;
    }

    // fi/mi: -1/-1 everything, fi/-1 one file, fi/mi a single occurrence.
    void searchReplace(int fi, int mi) {
        const search::Options o = searchOptions();
        const std::string with = sReplace_.text;
        int total = 0, files = 0, skipped = 0;
        for (int i = 0; i < static_cast<int>(sRes_.size()); ++i) {
            if (fi >= 0 && i != fi) continue;
            const auto& fr = sRes_[static_cast<std::size_t>(i)];
            const bool unsaved = std::ranges::any_of(docs_, [&](const auto& d) { return d->path == fr.path && d->modified(); });
            if (unsaved) { ++skipped; continue; }   // never overwrite the user's unsaved edits
            std::set<std::pair<int, int>> only;
            if (mi >= 0) { const auto& m = fr.matches[static_cast<std::size_t>(mi)]; only.insert({m.line, m.col}); }
            else for (const auto& m : fr.matches) only.insert({m.line, m.col});
            const int n = search::replaceInFile(fr.path, o, with, &only);
            if (n > 0) { total += n; ++files; } else if (n < 0) ++skipped;
        }
        checkDocsOnDisk();   // open, unmodified files reload right away
        explorer_.refresh();
        gitNextPoll_ = {};
        std::string msg = std::format("Replaced {} occurrence{} in {} file{}", total, total == 1 ? "" : "s", files, files == 1 ? "" : "s");
        if (skipped > 0) msg += std::format(" ({} skipped: unsaved changes or unreadable)", skipped);
        setMessage(msg, skipped > 0 && total == 0, 4);
        startSearch();
    }

    void searchDismiss(int fi, int mi) {
        if (fi < 0 || fi >= static_cast<int>(sRes_.size())) return;
        auto& f = sRes_[static_cast<std::size_t>(fi)];
        if (mi >= 0 && mi < static_cast<int>(f.matches.size())) f.matches.erase(f.matches.begin() + mi);
        if (mi < 0 || f.matches.empty()) sRes_.erase(sRes_.begin() + fi);
        recountSearch();
        dirty_ = true;
    }

    bool searchKey(const KeyEvent& k) {
        TextBox* boxes[4] = {&sQuery_, &sReplace_, &sInclude_, &sExclude_};
        auto visible = [&](int i) { return i < 2 || sFilters_; };
        if (k.key == Key::Escape) { sFocus_ = -1; return true; }
        if (k.key == Key::Tab) {
            int n = sFocus_;
            for (int step = 0; step < 4; ++step) { n = (n + (k.shift ? 3 : 1)) % 4; if (visible(n)) break; }
            sFocus_ = n;
            return true;
        }
        if (k.key == Key::Enter) {
            if (k.ctrl && sFocus_ == 1) { searchReplace(-1, -1); return true; }
            startSearch();
            return true;
        }
        if (k.alt && !k.ctrl && k.key == Key::Char) {
            if (k.ch == 'c') { sCase_ = !sCase_; startSearch(); return true; }
            if (k.ch == 'w') { sWord_ = !sWord_; startSearch(); return true; }
            if (k.ch == 'r') { sRegex_ = !sRegex_; startSearch(); return true; }
        }
        if (k.ctrl && k.key != Key::Char) return false;
        if (k.key >= Key::F1) return false;
        if (k.ctrl && k.key == Key::Char && k.ch != 'v' && k.ch != 'a') return false;   // IDE shortcuts keep working
        if (sFocus_ < 0 || sFocus_ > 3) return false;
        TextBox& t = *boxes[sFocus_];
        const std::string before = t.text;
        const bool used = editText(t, k);
        if (used && t.text != before && sFocus_ != 1) scheduleSearch();
        return used;
    }

    void searchClick(Button b) {
        if (b != Button::Left) return;
        sFocus_ = -1;
        for (auto it = sHits_.rbegin(); it != sHits_.rend(); ++it) {
            if (!it->r.contains(mx_, my_)) continue;
            switch (it->kind) {
                case 1: case 2: case 3: case 4: {
                    sFocus_ = it->kind - 1;
                    TextBox& t = sFocus_ == 0 ? sQuery_ : sFocus_ == 1 ? sReplace_ : sFocus_ == 2 ? sInclude_ : sExclude_;
                    t.cur = caretFromX(t.text, mx_ - (it->r.x + 10 * S()));   // approximate: ignores horizontal scroll of long text
                    break;
                }
                case 5: sCase_ = !sCase_; startSearch(); sFocus_ = 0; break;
                case 6: sWord_ = !sWord_; startSearch(); sFocus_ = 0; break;
                case 7: sRegex_ = !sRegex_; startSearch(); sFocus_ = 0; break;
                case 8: {
                    if (sTotal_ == 0) break;
                    const auto now = Clock::now();
                    if (now - sReplaceArmAt_ < std::chrono::seconds(4)) { sReplaceArmAt_ = {}; searchReplace(-1, -1); }
                    else {
                        sReplaceArmAt_ = now;
                        setMessage(std::format("Replace {} occurrence{} with \"{}\"? Click again to confirm", sTotal_, sTotal_ == 1 ? "" : "s", sReplace_.text), false, 4);
                    }
                    break;
                }
                case 9: sFilters_ = !sFilters_; break;
                default: break;
            }
            dirty_ = true;
            return;
        }
        if (!L_.sideList.contains(mx_, my_) || my_ < sResultsTop_) return;
        if (sRowsDirty_) rebuildSearchRows();
        const float rh = L_.sideRow;
        const int row = static_cast<int>(std::floor((my_ - sResultsTop_ + sScroll_) / rh));
        if (row < 0 || row >= static_cast<int>(sRows_.size())) return;
        const SRow r = sRows_[static_cast<std::size_t>(row)];
        const float s = S(), bs = std::round(rh - 8 * s);
        const Rect dismiss{L_.side.right() - 10 * s - bs, sResultsTop_ + static_cast<float>(row) * rh - sScroll_ + 4 * s, bs, bs};
        const Rect repl{dismiss.x - 2 * s - bs, dismiss.y, bs, bs};
        if (dismiss.contains(mx_, my_)) { searchDismiss(r.file, r.match); return; }
        if (repl.contains(mx_, my_)) { searchReplace(r.file, r.match); return; }
        if (r.match < 0) { auto& f = sRes_[static_cast<std::size_t>(r.file)]; f.collapsed = !f.collapsed; sRowsDirty_ = true; }
        else searchOpenMatch(r.file, r.match);
        dirty_ = true;
    }

    void searchScroll(float dy) {
        sScroll_ = std::clamp(sScroll_ + dy, 0.0f, sMax_);
        dirty_ = true;
    }

    void searchField(const Rect& r, TextBox& t, bool focused, int kind, std::string_view placeholder, float rightPad) {
        const float s = S();
        sHits_.push_back({r, kind});
        fill(r, focused ? theme::accent : Color::rgb(0xffffff, 34), 7 * s);
        fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, Color::rgb(0x07080a), 6 * s);
        const float x0 = r.x + 10 * s, maxX = r.right() - rightPad;
        std::size_t start = 0;
        const std::size_t cur = std::min(t.cur, t.text.size());
        while (start < cur && uiW(std::string_view(t.text).substr(start, cur - start)) > maxX - x0 - 4 * s) start = utf8Next(t.text, start);
        if (t.text.empty()) uiLabel(x0, r.y, r.h, placeholder, theme::lineNum, maxX);
        else uiLabel(x0, r.y, r.h, std::string_view(t.text).substr(start), theme::text, maxX);
        if (focused && caretOn_ && focused_) {
            const float cx = x0 + uiW(std::string_view(t.text).substr(start, cur - start));
            fill(std::round(cx), r.y + 7 * s, std::max(2.0f, std::round(1.4f * s)), r.h - 14 * s, theme::caret, s);
        }
    }

    // Small toggle ("Aa", "ab", ".*") docked inside the right edge of a field; returns its left edge.
    float searchChip(float right, const Rect& field, std::string_view label, bool on, int kind) {
        const float s = S();
        const float w = std::max(uiW(label) + 12 * s, std::round(26 * s));
        const Rect c{right - w, field.y + 4 * s, w, field.h - 8 * s};
        sHits_.push_back({c, kind});
        const bool hot = c.contains(mx_, my_);
        if (on) fill(c, theme::accent.alpha(80), 5 * s);
        else if (hot) fill(c, Color::rgb(0xffffff, 24), 5 * s);
        uiLabelC(c.x + c.w / 2, c.y, c.h, label, on ? Color::rgb(0xffffff) : theme::dim);
        return c.x - 2 * s;
    }

    void drawSearchBody() {
        const Rect& sd = L_.side;
        const float s = S(), rh = L_.sideRow, fh = std::round(uiH() + 12 * s), gap = std::round(6 * s);
        sHits_.clear();
        const float x0 = sd.x + 10 * s, w = sd.w - 20 * s;
        float y = L_.sideList.y + 6 * s;
        const Rect qR{x0, y, w, fh};
        y += fh + gap;
        const Rect rR{x0, y, w, fh};
        y += fh + gap;
        Rect iR{}, eR{};
        if (sFilters_) { iR = {x0, y, w, fh}; y += fh + gap; eR = {x0, y, w, fh}; y += fh + gap; }
        const Rect sumR{sd.x, y, sd.w, rh};
        sResultsTop_ = std::round(sumR.bottom() + 2 * s);

        // ---- results (drawn first; the field block is painted over rows that scroll beneath it)
        if (sRowsDirty_) rebuildSearchRows();
        const float listBottom = L_.sideList.bottom();
        const float viewH = std::max(0.0f, listBottom - sResultsTop_);
        sMax_ = std::max(0.0f, static_cast<float>(sRows_.size()) * rh - viewH * 0.5f);
        sScroll_ = std::clamp(sScroll_, 0.0f, sMax_);
        const float sy = std::round(sScroll_);
        const int first = std::max(0, static_cast<int>(std::floor(sy / rh)));
        const int last = std::min(static_cast<int>(sRows_.size()) - 1, static_cast<int>(std::floor((sy + viewH) / rh)));
        const bool hasRepl = !sReplace_.text.empty();
        const float indent = std::round(16 * s);
        for (int i = first; i <= last; ++i) {
            const SRow r = sRows_[static_cast<std::size_t>(i)];
            const auto& f = sRes_[static_cast<std::size_t>(r.file)];
            const float ry = sResultsTop_ + static_cast<float>(i) * rh - sy;
            const Rect rr{sd.x + 6 * s, ry + 1, sd.w - 12 * s, rh - 2};
            const bool hot = L_.sideList.contains(mx_, my_) && my_ >= sResultsTop_ && rr.contains(mx_, my_);
            if (hot) fill(rr, theme::sideHover, 6 * s);
            const float bs = std::round(rh - 8 * s);
            const Rect dismiss{sd.right() - 10 * s - bs, ry + 4 * s, bs, bs};
            const Rect repl{dismiss.x - 2 * s - bs, dismiss.y, bs, bs};
            auto actionIcon = [&](const Rect& b, char32_t g) {
                if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 36), 5 * s);
                const int ip = std::max(10, static_cast<int>(std::round(bs * 0.62f)));
                Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, g, Color::rgb(0xffffff));
            };
            if (r.match < 0) {
                const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                Icons::chevron(dl_, font_, sd.x + 12 * s + (indent - static_cast<float>(cs)) / 2, ry + (rh - static_cast<float>(cs)) / 2, cs, !f.collapsed, theme::dim);
                const float ix = sd.x + 12 * s + indent, iy = ry + (rh - iconSize()) / 2;
                const std::string name = fs::path(f.rel).filename().string();
                drawFileIcon(ix, iy, name);
                const float nx = ix + iconSize() + 8 * s;
                const float rightEdge = hot ? repl.x - 6 * s : sd.right() - 14 * s;
                float after = uiLabel(nx, ry, rh, name, theme::text, rightEdge - 30 * s);
                const std::string dir = fs::path(f.rel).parent_path().generic_string();
                if (!dir.empty()) uiLabel(after + 8 * s, ry, rh, dir, theme::lineNum, rightEdge - 30 * s);
                if (hot) { actionIcon(repl, 0xF06D4); actionIcon(dismiss, 0xF0156); }
                else {
                    const std::string cnt = std::to_string(f.matches.size());
                    const float bw = std::max(uiW(cnt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                    const Rect badge{sd.right() - 14 * s - bw, ry + (rh - bh) / 2, bw, bh};
                    fill(badge, Color::rgb(0xffffff, 26), bh / 2);
                    uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, cnt, theme::dim);
                }
            } else {
                const auto& m = f.matches[static_cast<std::size_t>(r.match)];
                float x = sd.x + 12 * s + indent * 2;
                const float maxX = (hot ? repl.x - 6 * s : sd.right() - 14 * s);
                const std::string_view pv = m.preview;
                const std::size_t pc = std::min(static_cast<std::size_t>(std::max(0, m.pcol)), pv.size());
                const std::size_t pl = std::min(static_cast<std::size_t>(m.len), pv.size() - pc);
                const Color base = Color::rgb(0xb9bdc6);
                x = uiLabel(x, ry, rh, pv.substr(0, pc), base, maxX);
                const float mw = uiW(pv.substr(pc, pl));
                if (hasRepl) {
                    fill(x, ry + 4 * s, std::min(mw, maxX - x), rh - 8 * s, Color::rgb(0xe06c75, 60), 3 * s);
                    const float xe = uiLabel(x, ry, rh, pv.substr(pc, pl), Color::rgb(0xe9a3a8), maxX);
                    fill(x, ry + rh / 2, std::max(0.0f, std::min(mw, maxX - x)), std::max(1.0f, std::round(s)), Color::rgb(0xe9a3a8));
                    const float nw = uiW(sReplace_.text);
                    fill(xe, ry + 4 * s, std::max(0.0f, std::min(nw, maxX - xe)), rh - 8 * s, Color::rgb(0x73c990, 60), 3 * s);
                    x = uiLabel(xe, ry, rh, sReplace_.text, Color::rgb(0xa5e0b8), maxX);
                } else {
                    fill(x, ry + 4 * s, std::min(mw, maxX - x), rh - 8 * s, Color::rgb(0xe5c07b, 70), 3 * s);
                    x = uiLabel(x, ry, rh, pv.substr(pc, pl), Color::rgb(0xffffff), maxX);
                }
                if (pc + pl < pv.size()) uiLabel(x, ry, rh, pv.substr(pc + pl), base, maxX);
                if (hot) { actionIcon(repl, 0xF06D4); actionIcon(dismiss, 0xF0156); }
            }
        }
        if (static_cast<float>(sRows_.size()) * rh > viewH) {
            const float contentH = static_cast<float>(sRows_.size()) * rh;
            const float thumbH = std::max(24 * s, viewH * viewH / (contentH + viewH * 0.5f));
            const float thumbY = sResultsTop_ + (viewH - thumbH) * std::clamp(sy / sMax_, 0.0f, 1.0f);
            fill(sd.right() - 9 * s, thumbY, 5 * s, thumbH, theme::scrollbar, 2.5f * s);
        }

        // ---- inputs and summary (opaque, so results scrolling underneath disappear)
        fill(sd.x, L_.sideList.y, sd.w, sResultsTop_ - L_.sideList.y, theme::sideBg);
        sHits_.clear();
        {
            auto chipW = [&](std::string_view l) { return std::max(uiW(l) + 12 * s, std::round(26 * s)) + 2 * s; };
            const float reserve = chipW(".*") + chipW("ab") + chipW("Aa") + 10 * s;
            searchField(qR, sQuery_, sFocus_ == 0, 1, "Search", reserve);
            float rp = qR.right() - 6 * s;
            rp = searchChip(rp, qR, ".*", sRegex_, 7);
            rp = searchChip(rp, qR, "ab", sWord_, 6);
            searchChip(rp, qR, "Aa", sCase_, 5);
        }
        {   // replace-all button docked in the replace field
            const float bw = std::round(fh - 6 * s);
            const Rect b{rR.right() - 3 * s - bw, rR.y + 3 * s, bw, rR.h - 6 * s};
            searchField(rR, sReplace_, sFocus_ == 1, 2, "Replace", bw + 12 * s);
            sHits_.push_back({b, 8});
            const bool armed = Clock::now() - sReplaceArmAt_ < std::chrono::seconds(4);
            if (armed) fill(b, theme::accent.alpha(120), 5 * s);
            else if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 28), 5 * s);
            const int ip = std::max(12, static_cast<int>(std::round(b.h * 0.7f)));
            Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, 0xF06D4,
                         sTotal_ > 0 ? Color::rgb(0xffffff) : theme::lineNum);
        }
        if (sFilters_) {
            searchField(iR, sInclude_, sFocus_ == 2, 3, "files to include (*.cpp, src/**)", 10 * s);
            searchField(eR, sExclude_, sFocus_ == 3, 4, "files to exclude", 10 * s);
        }
        // summary line
        std::string sum;
        Color sc = theme::dim;
        if (!sError_.empty()) { sum = sError_; sc = sError_ == "Type to search" ? theme::lineNum : Color::rgb(0xf2777a); }
        else if (sBusy_ || sPending_) sum = "Searching...";
        else if (sSearched_ && sTotal_ == 0) sum = "No results";
        else if (sTotal_ > 0) sum = std::format("{}{} result{} in {} file{}", sTotal_, sTrunc_ ? "+" : "", sTotal_ == 1 ? "" : "s", sRes_.size(), sRes_.size() == 1 ? "" : "s");
        const std::string flt = "Filters";
        const float fw = uiW(flt) + 16 * s;
        const Rect fR{sd.right() - 10 * s - fw, sumR.y + 3 * s, fw, rh - 6 * s};
        sHits_.push_back({fR, 9});
        if (sFilters_) fill(fR, theme::accent.alpha(60), 5 * s);
        else if (fR.contains(mx_, my_)) fill(fR, Color::rgb(0xffffff, 22), 5 * s);
        uiLabelC(fR.x + fR.w / 2, fR.y, fR.h, flt, sFilters_ ? Color::rgb(0xffffff) : theme::dim);
        uiLabel(sd.x + 14 * s, sumR.y, rh, sum, sc, fR.x - 6 * s);
    }

    // ============================================================ debugger (gdb through its machine interface)

    enum class DbgState { Off, Starting, Running, Stopped };
    struct Breakpoint {
        int uid = 0;
        std::string file; int line = 0;   // line breakpoint: absolute path, 1-based line
        std::string func;                  // function breakpoint
        std::string watch;                 // data breakpoint (expression)
        bool exceptions = false;           // C++ throw
        std::string cond, log;             // condition; logpoint message with {expr} parts
        int ignore = 0;
        bool enabled = true, verified = false;
        int id = -1, hits = 0;             // gdb's number / hit count while a session runs
    };
    struct DbgFrame { int level = 0, line = 0; std::string func, file, full, addr; };
    struct DbgThread { int id = 0; std::string name, state, func; };
    struct DbgVar {
        std::string exp, path, vo, type, value;
        int numchild = 0;
        bool expanded = false, loaded = false, changed = false;
        std::vector<std::shared_ptr<DbgVar>> kids;
    };
    using VarP = std::shared_ptr<DbgVar>;
    struct DbgLaunch { std::string exe, cwd, args, name; };
    struct ConLine { std::string text; int kind = 0; };   // kinds: 0 output, 1 error, 2 input echo, 3 logpoint, 4 info

    std::unique_ptr<gdb::Session> dbg_;
    DbgState dbgState_ = DbgState::Off;
    DbgLaunch dbgLast_;
    int dbgGen_ = 0, dbgVarGen_ = 0, dbgUid_ = 1, dbgSel_ = 0, dbgThread_ = 0;
    bool dbgForceMain_ = false, dbgInterrupting_ = false, dbgEndPending_ = false;
    int dbgEndCode_ = 0;
    std::string dbgEndWhy_, dbgStopText_;
    std::vector<std::function<void()>> dbgOps_;
    std::vector<Breakpoint> bps_;
    std::vector<std::string> watches_;
    std::vector<DbgFrame> dbgFrames_;
    std::vector<DbgThread> dbgThreads_;
    std::vector<VarP> dbgLocals_, dbgWatchVars_;
    std::set<std::string> dbgExpanded_;
    std::map<std::string, std::string> dbgPrev_;
    struct DbgPos { std::string file; int line = 0; } dbgLine_;   // execution line shown in the editor
    std::vector<ConLine> dbgCon_;
    std::string dbgConPartial_;
    TextBox dbgIn_;
    std::vector<std::string> dbgHist_;
    int dbgHistAt_ = -1;
    bool dbgConFocus_ = false;
    bool dbgSec_[4] = {true, true, true, true};
    float dbgScroll_ = 0, dbgMax_ = 0, dbgTop_ = 0;
    VarP dbgEditVar_;
    int dbgPromptUid_ = -1;
    bool sideDebug_ = false;
    bool debugView() const { return sidebar_ && sideDebug_; }
    bool debugging() const { return dbg_ != nullptr; }

    // ---- console

    void dbgConsole(std::string text, int kind = 0) {
        dbgCon_.push_back({std::move(text), kind});
        if (dbgCon_.size() > 5000) dbgCon_.erase(dbgCon_.begin(), dbgCon_.begin() + 1000);
        panelFollow_[4] = true;
        dirty_ = true;
    }
    void dbgConsoleStream(const std::string& t) {
        dbgConPartial_ += t;
        for (std::size_t nl; (nl = dbgConPartial_.find('\n')) != std::string::npos;) {
            std::string line = dbgConPartial_.substr(0, nl);
            dbgConPartial_.erase(0, nl + 1);
            dbgConsole(std::move(line), 0);
        }
    }

    // ---- breakpoints

    fs::path dbgFile() const { return explorer_.isOpen() ? explorer_.rootPath() / ".daemon" / "debug.json" : fs::path(); }

    void dbgSave() {
        const fs::path f = dbgFile();
        if (f.empty()) return;
        auto j = json::object();
        auto arr = json::array();
        for (const auto& b : bps_) {
            auto o = json::object();
            o.set("file", b.file.empty() ? std::string() : fs::path(b.file).lexically_relative(explorer_.rootPath()).string());
            o.set("line", b.line); o.set("func", b.func); o.set("watch", b.watch); o.set("exceptions", b.exceptions);
            o.set("cond", b.cond); o.set("log", b.log); o.set("ignore", b.ignore); o.set("enabled", b.enabled);
            arr.push(std::move(o));
        }
        j.set("breakpoints", std::move(arr));
        auto w = json::array();
        for (const auto& e : watches_) w.push(e);
        j.set("watches", std::move(w));
        std::error_code ec;
        fs::create_directories(f.parent_path(), ec);
        std::ofstream(f, std::ios::binary | std::ios::trunc) << json::dump(j);
    }

    void dbgLoad() {
        bps_.clear(); watches_.clear();
        const fs::path f = dbgFile();
        if (f.empty()) return;
        auto text = readFile(f);
        if (!text) return;
        auto j = json::parse(*text);
        if (!j) return;
        if (auto* a = (*j)["breakpoints"].array())
            for (const auto& v : *a) {
                Breakpoint b;
                b.uid = dbgUid_++;
                const std::string rel = v["file"].str();
                b.file = rel.empty() ? std::string() : (explorer_.rootPath() / rel).lexically_normal().string();
                b.line = v["line"].integer(); b.func = v["func"].str(); b.watch = v["watch"].str();
                b.exceptions = v["exceptions"].boolean(); b.cond = v["cond"].str(); b.log = v["log"].str();
                b.ignore = v["ignore"].integer(); b.enabled = v["enabled"].boolean(true);
                bps_.push_back(std::move(b));
            }
        if (auto* a = (*j)["watches"].array()) for (const auto& v : *a) if (v.isString()) watches_.push_back(v.str());
    }

    Breakpoint* dbgBpByUid(int uid) {
        for (auto& b : bps_) if (b.uid == uid) return &b;
        return nullptr;
    }
    Breakpoint* dbgBpById(int id) {
        for (auto& b : bps_) if (b.id == id) return &b;
        return nullptr;
    }
    Breakpoint* dbgBpAt(const std::string& file, int line) {
        for (auto& b : bps_) if (b.func.empty() && b.watch.empty() && !b.exceptions && b.file == file && b.line == line) return &b;
        return nullptr;
    }

    std::string dbgBpTitle(const Breakpoint& b) const {
        if (b.exceptions) return "C++ exceptions (throw)";
        if (!b.watch.empty()) return "watch " + b.watch;
        if (!b.func.empty()) return b.func + "()";
        return fs::path(b.file).filename().string() + ":" + std::to_string(b.line);
    }

    // gdb refuses breakpoint changes while the program runs: interrupt, apply, continue.
    void dbgWhenPaused(std::function<void()> fn) {
        if (!dbg_) return;
        if (dbgState_ != DbgState::Running) { fn(); return; }
        dbgOps_.push_back(std::move(fn));
        if (!dbgInterrupting_) { dbgInterrupting_ = true; dbg_->send("-exec-interrupt --all"); }
    }

    static std::string dbgBpCommand(const Breakpoint& b) {
        if (b.exceptions) return "-catch-throw";
        if (!b.watch.empty()) return "-break-watch " + gdb::quote(b.watch);
        std::string c = "-break-insert -f";
        if (!b.cond.empty()) c += " -c " + gdb::quote(b.cond);
        if (b.ignore > 0) c += " -i " + std::to_string(b.ignore);
        if (!b.func.empty()) return c + " " + gdb::quote(b.func);
        return c + " --source " + gdb::quote(b.file) + " --line " + std::to_string(b.line);
    }

    void dbgInsert(int uid) {
        Breakpoint* b = dbgBpByUid(uid);
        if (!dbg_ || !b) return;
        const bool enabled = b->enabled, isLine = b->func.empty() && b->watch.empty() && !b->exceptions;
        dbg_->send(dbgBpCommand(*b), [this, uid, enabled, isLine](const gdb::Record& r) {
            Breakpoint* p = dbgBpByUid(uid);
            if (!r.ok()) { if (p) p->verified = false; dbgConsole("Breakpoint: " + r.msg(), 1); return; }
            const json::Value& bk = r.data["bkpt"].isNull() ? r.data["wpt"] : r.data["bkpt"];
            const int id = std::atoi(bk["number"].str().c_str());
            if (!p) { if (dbg_) dbg_->send("-break-delete " + std::to_string(id)); return; }   // removed while it was being inserted
            p->id = id;
            p->verified = bk["addr"].str() != "<PENDING>";
            if (isLine) {
                const int l = std::atoi(bk["line"].str().c_str());
                if (l > 0 && l != p->line && p->verified) { p->line = l; dbgSave(); }   // gdb moved it to the next line with code
            }
            if (!enabled && dbg_) dbg_->send("-break-disable " + std::to_string(id));
            dirty_ = true;
        });
    }

    void dbgAddBp(Breakpoint b) {
        b.uid = dbgUid_++;
        bps_.push_back(std::move(b));
        const int uid = bps_.back().uid;
        dbgSave();
        dbgWhenPaused([this, uid] { dbgInsert(uid); });
        dirty_ = true;
    }

    void dbgRemoveBp(int uid) {
        auto it = std::ranges::find_if(bps_, [&](const Breakpoint& b) { return b.uid == uid; });
        if (it == bps_.end()) return;
        const int id = it->id;
        bps_.erase(it);
        if (id >= 0) dbgWhenPaused([this, id] { if (dbg_) dbg_->send("-break-delete " + std::to_string(id)); });
        dbgSave();
        dirty_ = true;
    }

    void dbgToggleBpAt(const std::string& file, int line) {
        if (file.empty()) { setMessage("Save the file first to set breakpoints", true, 2.5f); return; }
        if (Breakpoint* b = dbgBpAt(file, line)) { dbgRemoveBp(b->uid); return; }
        Breakpoint nb;
        nb.file = file; nb.line = line;
        dbgAddBp(std::move(nb));
    }

    void dbgSetEnabled(int uid, bool on) {
        Breakpoint* b = dbgBpByUid(uid);
        if (!b) return;
        b->enabled = on;
        const int id = b->id;
        if (id >= 0) dbgWhenPaused([this, id, on] { if (dbg_) dbg_->send((on ? "-break-enable " : "-break-disable ") + std::to_string(id)); });
        dbgSave();
        dirty_ = true;
    }

    // Edit text: "expr" = condition, "#N" = ignore the first N hits, "log: text {expr}" = logpoint, empty = plain breakpoint.
    static std::string dbgBpEditText(const Breakpoint& b) {
        if (!b.log.empty()) return "log: " + b.log;
        if (b.ignore > 0 && b.cond.empty()) return "#" + std::to_string(b.ignore);
        return b.cond;
    }
    void dbgApplyBpEdit(int uid, std::string t) {
        Breakpoint* b = dbgBpByUid(uid);
        if (!b) return;
        while (!t.empty() && t.front() == ' ') t.erase(0, 1);
        b->cond.clear(); b->log.clear(); b->ignore = 0;
        if (t.starts_with("log:")) { b->log = t.substr(4); while (!b->log.empty() && b->log.front() == ' ') b->log.erase(0, 1); }
        else if (t.starts_with("#")) b->ignore = std::max(0, std::atoi(t.c_str() + 1));
        else b->cond = t;
        const int id = b->id, ign = b->ignore;
        const std::string cond = b->cond;
        if (id >= 0)
            dbgWhenPaused([this, id, cond, ign] {
                if (!dbg_) return;
                dbg_->send("-break-condition " + std::to_string(id) + (cond.empty() ? "" : " " + cond));
                dbg_->send("-break-after " + std::to_string(id) + " " + std::to_string(ign));
            });
        dbgSave();
        dirty_ = true;
    }

    // ---- starting and ending a session

    void debugLaunch(const DbgLaunch& L) {
        if (!findExecutable("gdb")) { setMessage("gdb was not found - install it to debug", true, 4); return; }
        if (dbg_) { dbgEnd("Restarting", 0, true); }
        dbgLast_ = L;
        if (runProc_) { win_.unwatch(runProc_->outFd()); runProc_->terminate(); runProc_.reset(); }
        runOut_.clear(); runPartial_.clear();
        runProc_ = std::make_unique<Process>();
        auto tty = runProc_->openPtyOnly();
        if (!tty) { setMessage("Cannot open a terminal for the program: " + tty.error(), true, 4); runProc_.reset(); return; }
        dbg_ = std::make_unique<gdb::Session>();
        if (auto st = dbg_->start({"gdb", "--interpreter=mi3", "-q", "-nx"}, L.cwd); !st) {
            setMessage("Cannot start gdb: " + st.error(), true, 4);
            dbg_.reset(); runProc_.reset();
            return;
        }
        runOut_.push_back({"$ " + L.exe + (L.args.empty() ? "" : " " + L.args), 4, -1});
        runEof_ = false;
        runFocus_ = true;
        win_.watch(runProc_->outFd(), [this] { pumpRun(); });
        win_.watch(dbg_->fd(), [this] { pumpDebug(); });
        dbg_->onAsync = [this](const gdb::Record& r) { dbgAsync(r); };
        dbg_->onStream = [this](char k, const std::string& t) { if (k == '~') dbgConsoleStream(t); };
        dbgState_ = DbgState::Starting;
        ++dbgGen_;
        dbgFrames_.clear(); dbgThreads_.clear(); dbgLocals_.clear(); dbgWatchVars_.clear(); dbgLine_ = {}; dbgSel_ = 0;
        dbgOps_.clear(); dbgInterrupting_ = false; dbgEndPending_ = false;
        for (auto& b : bps_) { b.id = -1; b.verified = false; b.hits = 0; }
        dbgConsole("--- Debugging " + L.name + " ---", 4);
        panelOpen_ = true; panelTab_ = 1; panelFollow_[1] = true;
        sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; sideFocus_ = false;
        buildStatus_ = "Debugging " + L.name;

        auto& g = *dbg_;
        g.send("-gdb-set confirm off");
        g.send("-gdb-set pagination off");
        g.send("-gdb-set breakpoint pending on");
        g.send("-gdb-set debuginfod enabled off");
        g.send("-gdb-set print elements 200");
        g.send("-gdb-set inferior-tty " + *tty);
        g.send("-gdb-set cwd " + L.cwd);
        if (!L.args.empty()) g.send("-gdb-set args " + L.args);
        g.send("-enable-pretty-printing");
        g.send("-file-exec-and-symbols " + gdb::quote(L.exe), [this](const gdb::Record& r) { if (!r.ok()) dbgConsole(r.msg(), 1); });
        bool anyStop = false;
        for (const auto& b : bps_) if (b.enabled && b.watch.empty()) anyStop = true;
        for (const auto& b : bps_) dbgInsert(b.uid);
        const bool stopAtMain = dbgForceMain_ || !anyStop;
        dbgForceMain_ = false;
        g.send(stopAtMain ? "-exec-run --start" : "-exec-run", [this](const gdb::Record& r) {
            if (!r.ok()) { dbgConsole("Cannot run: " + r.msg(), 1); dbgScheduleEnd("Failed to start the program", -1); }
        });
        dirty_ = true;
    }

    void pumpDebug() {
        if (!dbg_) return;
        const bool alive = dbg_->pump();
        dirty_ = true;
        if (!alive && dbg_ && !dbgEndPending_) dbgScheduleEnd("gdb exited", -1);
        if (dbgEndPending_) dbgEnd(dbgEndWhy_, dbgEndCode_, false);
    }

    void dbgScheduleEnd(std::string why, int code) { dbgEndPending_ = true; dbgEndWhy_ = std::move(why); dbgEndCode_ = code; }

    void dbgEnd(const std::string& why, int code, bool quiet) {
        dbgEndPending_ = false;
        if (!dbg_) return;
        win_.unwatch(dbg_->fd());
        dbg_->kill();
        dbg_.reset();   // (the destructor reaps gdb; the program dies with it)
        dbgState_ = DbgState::Off;
        ++dbgGen_;
        dbgFrames_.clear(); dbgThreads_.clear(); dbgLocals_.clear(); dbgWatchVars_.clear(); dbgLine_ = {}; dbgOps_.clear(); dbgInterrupting_ = false;
        for (auto& b : bps_) { b.id = -1; b.verified = false; }
        if (!dbgConPartial_.empty()) { dbgConsole(dbgConPartial_, 0); dbgConPartial_.clear(); }
        dbgConsole("--- " + why + " ---", 4);
        if (runProc_) {
            win_.unwatch(runProc_->outFd());
            std::string rest;
            runProc_->read(rest);
            if (!rest.empty()) for (auto& l : takeLines(runPartial_, rest)) runOut_.push_back({cleanTerm(l), 0, -1});
            if (!runPartial_.empty()) { runOut_.push_back({cleanTerm(runPartial_), 0, -1}); runPartial_.clear(); }
            runOut_.push_back({"", 0, -1});
            runOut_.push_back({why, code == 0 ? 5 : 2, -1});
            runProc_.reset();
            runEof_ = false;
        }
        runFocus_ = false;
        if (!quiet) { buildStatus_ = why; setMessage(why, code != 0, 3.5f); }
        dirty_ = true;
    }

    void debugStop() { if (dbg_) dbgEnd("Debugging stopped", 0, false); }

    // ---- events from gdb

    void dbgAsync(const gdb::Record& r) {
        if (r.kind == '*' && r.cls == "running") {
            dbgState_ = DbgState::Running;
            ++dbgGen_; ++dbgVarGen_;
            dbgLine_ = {};
            dbgFrames_.clear();
            dbgThreads_.clear();
            dirty_ = true;
        } else if (r.kind == '*' && r.cls == "stopped") {
            dbgOnStopped(r.data);
        } else if (r.kind == '=' && (r.cls == "breakpoint-modified" || r.cls == "breakpoint-created")) {
            const auto& bk = r.data["bkpt"];
            if (Breakpoint* b = dbgBpById(std::atoi(bk["number"].str().c_str()))) {
                b->hits = std::atoi(bk["times"].str().c_str());
                if (r.cls == "breakpoint-modified") b->verified = bk["addr"].str() != "<PENDING>";
                dirty_ = true;
            }
        }
    }

    void dbgOnStopped(const json::Value& d) {
        const std::string reason = d["reason"].str();
        if (reason.starts_with("exited")) {
            if (reason == "exited-signalled") dbgScheduleEnd("Program terminated by signal " + d["signal-name"].str() + " (" + d["signal-meaning"].str() + ")", 128);
            else {
                const int code = std::strtol(d["exit-code"].str("0").c_str(), nullptr, 8);   // gdb prints the code in octal
                dbgScheduleEnd(reason == "exited-normally" ? "Program exited normally" : std::format("Program exited with code {}", code), code);
            }
            return;
        }
        if (dbgInterrupting_ && reason == "signal-received" && d["signal-name"].str() == "SIGINT") {   // we paused it to edit breakpoints
            dbgInterrupting_ = false;
            dbgState_ = DbgState::Stopped;
            auto ops = std::move(dbgOps_);
            dbgOps_.clear();
            for (auto& op : ops) op();
            if (dbg_) dbg_->send("-exec-continue");
            return;
        }
        dbgState_ = DbgState::Stopped;
        ++dbgGen_;
        if (reason == "breakpoint-hit") {
            if (Breakpoint* b = dbgBpById(std::atoi(d["bkptno"].str().c_str()))) {
                b->hits = std::max(b->hits, 0) + 0;
                if (!b->log.empty()) { dbgRunLogpoint(b->log); return; }
            }
        }
        dbgThread_ = std::atoi(d["thread-id"].str().c_str());
        dbgSel_ = 0;
        std::string text;
        if (reason == "breakpoint-hit") text = "Paused on breakpoint";
        else if (reason == "end-stepping-range" || reason == "function-finished" || reason == "location-reached") text = "Paused";
        else if (reason == "signal-received") {
            text = "Received signal " + d["signal-name"].str() + " (" + d["signal-meaning"].str() + ")";
            setMessage(text, true, 5);
            dbgConsole(text, 1);
        } else if (reason.find("watchpoint") != std::string::npos) {
            const auto& w = d["wpt"].isNull() ? d["hw-wpt"] : d["wpt"];
            text = "Watchpoint " + w["exp"].str() + ": " + d["value"]["old"].str() + " -> " + d["value"]["new"].str();
            if (d["value"]["old"].isNull() && !d["value"]["value"].isNull()) text = "Watchpoint " + w["exp"].str() + " = " + d["value"]["value"].str();
            dbgConsole(text, 4);
        } else text = "Paused (" + reason + ")";
        dbgStopText_ = text;
        const auto& fr = d["frame"];
        DbgFrame f;
        f.func = fr["func"].str(); f.file = fr["file"].str(); f.full = fr["fullname"].str(); f.line = std::atoi(fr["line"].str().c_str()); f.addr = fr["addr"].str();
        dbgShowFrame(f);
        dbgRefreshStack();
        dirty_ = true;
    }

    void dbgRunLogpoint(const std::string& msg) {
        std::vector<std::string> parts, exprs;
        std::string cur;
        for (std::size_t i = 0; i < msg.size(); ++i) {
            if (msg[i] == '{') {
                const auto e = msg.find('}', i);
                if (e == std::string::npos) { cur += msg.substr(i); break; }
                parts.push_back(cur); cur.clear();
                exprs.push_back(msg.substr(i + 1, e - i - 1));
                i = e;
            } else cur += msg[i];
        }
        parts.push_back(cur);
        auto vals = std::make_shared<std::vector<std::string>>(exprs.size());
        auto left = std::make_shared<int>(static_cast<int>(exprs.size()));
        auto finish = [this, parts, vals] {
            std::string out;
            for (std::size_t i = 0; i < parts.size(); ++i) { out += parts[i]; if (i < vals->size()) out += (*vals)[i]; }
            dbgConsole(out, 3);
            if (dbg_) dbg_->send("-exec-continue");
        };
        if (exprs.empty()) { finish(); return; }
        for (std::size_t i = 0; i < exprs.size(); ++i)
            dbg_->send("-data-evaluate-expression " + gdb::quote(exprs[i]), [vals, left, i, finish](const gdb::Record& r) {
                (*vals)[i] = r.ok() ? r.data["value"].str() : "<" + r.msg() + ">";
                if (--*left == 0) finish();
            });
    }

    // Shows the source line of a frame in the editor and marks it as the execution line.
    void dbgShowFrame(const DbgFrame& f) {
        dbgLine_ = {};
        std::error_code ec;
        std::string path = !f.full.empty() ? f.full : f.file;
        if (path.empty() || f.line <= 0 || !fs::is_regular_file(path, ec)) return;
        path = fs::path(path).lexically_normal().string();
        dbgLine_ = {path, f.line};
        openFile(path);
        if (!docs_.empty() && doc().path == path) {
            doc().setCursor({f.line - 1, 0});
            scrollToCursor_ = true;
        }
        dirty_ = true;
    }

    void dbgRefreshStack() {
        if (!dbg_) return;
        const int gen = dbgGen_;
        dbg_->send("-stack-list-frames 0 255", [this, gen](const gdb::Record& r) {
            if (gen != dbgGen_ || !r.ok()) return;
            dbgFrames_.clear();
            if (const auto* a = r.data["stack"].array())
                for (const auto& v : *a) {
                    DbgFrame f;
                    f.level = std::atoi(v["level"].str().c_str()); f.func = v["func"].str(); f.file = v["file"].str();
                    f.full = v["fullname"].str(); f.line = std::atoi(v["line"].str().c_str()); f.addr = v["addr"].str();
                    dbgFrames_.push_back(std::move(f));
                }
            dirty_ = true;
        });
        dbg_->send("-thread-info", [this, gen](const gdb::Record& r) {
            if (gen != dbgGen_ || !r.ok()) return;
            dbgThreads_.clear();
            if (const auto* a = r.data["threads"].array())
                for (const auto& v : *a) {
                    DbgThread t;
                    t.id = std::atoi(v["id"].str().c_str()); t.name = v["name"].str(); t.state = v["state"].str(); t.func = v["frame"]["func"].str();
                    dbgThreads_.push_back(std::move(t));
                }
            dirty_ = true;
        });
        dbgRefreshVars();
    }

    void dbgSelectFrame(int level) {
        if (!dbg_ || dbgState_ != DbgState::Stopped) return;
        dbg_->send("-stack-select-frame " + std::to_string(level), [this, level](const gdb::Record& r) {
            if (!r.ok()) return;
            dbgSel_ = level;
            for (const auto& f : dbgFrames_) if (f.level == level) dbgShowFrame(f);
            dbgRefreshVars();
        });
    }

    void dbgSelectThread(int id) {
        if (!dbg_ || dbgState_ != DbgState::Stopped) return;
        dbg_->send("-thread-select " + std::to_string(id), [this, id](const gdb::Record& r) {
            if (!r.ok()) return;
            dbgThread_ = id;
            dbgSel_ = 0;
            dbgRefreshStack();
            dbg_->send("-stack-info-frame", [this](const gdb::Record& fr) {
                const auto& f = fr.data["frame"];
                DbgFrame d;
                d.func = f["func"].str(); d.file = f["file"].str(); d.full = f["fullname"].str(); d.line = std::atoi(f["line"].str().c_str());
                dbgShowFrame(d);
            });
        });
    }

    // ---- run control

    void dbgContinueOrStart() {
        if (!dbg_) { cmakeBuild(true, true); return; }
        if (dbgState_ == DbgState::Stopped) dbg_->send("-exec-continue");
    }
    void dbgStep(const char* cmd) {
        if (!dbg_) { dbgForceMain_ = true; cmakeBuild(true, true); return; }   // start and stop at main
        if (dbgState_ == DbgState::Stopped) dbg_->send(cmd);
    }
    void dbgPause() { if (dbg_ && dbgState_ == DbgState::Running) dbg_->send("-exec-interrupt --all"); }
    void dbgRestart() {
        if (dbgLast_.exe.empty()) { cmakeBuild(true, true); return; }
        const DbgLaunch l = dbgLast_;
        debugLaunch(l);
    }
    void dbgRunToCursor() {
        if (!dbg_ || dbgState_ != DbgState::Stopped || docs_.empty() || doc().path.empty()) return;
        dbg_->send("-break-insert -t -f --source " + gdb::quote(doc().path) + " --line " + std::to_string(doc().cursor.line + 1), [this](const gdb::Record& r) {
            if (!r.ok()) { setMessage(r.msg(), true); return; }
            dbg_->send("-exec-continue");
        });
    }
    void dbgSetNextStatement() {
        if (!dbg_ || dbgState_ != DbgState::Stopped || docs_.empty() || doc().path.empty()) return;
        dbg_->send("-exec-jump " + gdb::quote(doc().path + ":" + std::to_string(doc().cursor.line + 1)), [this](const gdb::Record& r) {
            if (!r.ok()) setMessage(r.msg(), true);
        });
    }

    // ---- variables and watches

    void dbgCollect(const std::vector<VarP>& v, std::map<std::string, std::string>& out) {
        for (const auto& n : v) { out[n->path] = n->value; dbgCollect(n->kids, out); }
    }

    void dbgRefreshVars() {
        if (!dbg_ || dbgState_ != DbgState::Stopped) return;
        const int gen = ++dbgVarGen_;
        dbgPrev_.clear();
        dbgCollect(dbgLocals_, dbgPrev_);
        dbgCollect(dbgWatchVars_, dbgPrev_);
        for (const auto& v : dbgLocals_) if (!v->vo.empty()) dbg_->send("-var-delete " + v->vo);
        for (const auto& v : dbgWatchVars_) if (!v->vo.empty()) dbg_->send("-var-delete " + v->vo);
        dbgLocals_.clear();
        dbgWatchVars_.clear();
        for (std::size_t i = 0; i < watches_.size(); ++i) {
            auto v = std::make_shared<DbgVar>();
            v->exp = watches_[i];
            v->path = "watch/" + std::to_string(i) + ":" + v->exp;
            v->value = "...";
            dbgWatchVars_.push_back(v);
            dbgCreateVar(v, gen);
        }
        dbg_->send("-stack-list-variables --no-values", [this, gen](const gdb::Record& r) {
            if (gen != dbgVarGen_ || !r.ok()) return;
            if (const auto* a = r.data["variables"].array())
                for (const auto& e : *a) {
                    auto v = std::make_shared<DbgVar>();
                    v->exp = e["name"].str();
                    if (v->exp.starts_with("__")) continue;   // compiler-generated (range-for temporaries ...)
                    v->path = "var/" + v->exp;
                    v->value = "...";
                    dbgLocals_.push_back(v);
                    dbgCreateVar(v, gen);
                }
            dirty_ = true;
        });
    }

    void dbgCreateVar(const VarP& v, int gen) {
        if (!dbg_) return;
        dbg_->send("-var-create - * " + gdb::quote(v->exp), [this, v, gen](const gdb::Record& r) {
            if (gen != dbgVarGen_) { if (r.ok() && dbg_) dbg_->send("-var-delete " + r.data["name"].str()); return; }
            if (!r.ok()) { v->value = r.msg().find("unable to create") != std::string::npos ? "<not available in this scope>" : "<" + r.msg() + ">"; v->numchild = 0; dirty_ = true; return; }
            v->vo = r.data["name"].str();
            v->type = r.data["type"].str();
            v->value = r.data["value"].str();
            v->numchild = std::atoi(r.data["numchild"].str().c_str());
            if (r.data["dynamic"].str() == "1") v->numchild = std::max(v->numchild, 1);   // pretty-printed containers list their elements lazily
            if (auto it = dbgPrev_.find(v->path); it != dbgPrev_.end()) v->changed = it->second != v->value;
            if (v->numchild > 0 && dbgExpanded_.contains(v->path)) dbgExpand(v, gen);
            dirty_ = true;
        });
    }

    // Children of a varobj, with C++ access-specifier pseudo nodes (public / private / protected) spliced into the list.
    void dbgFetchKids(const std::string& vo, const std::string& parentPath, int gen, std::function<void(std::vector<VarP>, bool)> done) {
        if (!dbg_) return;
        dbg_->send("-var-list-children --simple-values " + vo + " 0 300", [this, vo, parentPath, gen, done](const gdb::Record& r) {
            if (gen != dbgVarGen_) return;
            if (!r.ok()) { dbgConsole("Cannot list members: " + r.msg(), 1); done({}, false); return; }
            auto slots = std::make_shared<std::vector<std::vector<VarP>>>();
            auto left = std::make_shared<int>(0);
            auto fin = std::make_shared<std::function<void()>>();
            const bool more = r.data["has_more"].str() == "1";
            std::vector<std::pair<VarP, bool>> order;   // (child, pseudo)
            if (const auto* a = r.data["children"].array())
                for (const auto& c : *a) {
                    auto k = std::make_shared<DbgVar>();
                    k->exp = c["exp"].str(); k->vo = c["name"].str(); k->type = c["type"].str(); k->value = c["value"].str();
                    k->numchild = std::atoi(c["numchild"].str().c_str());
                    if (c["dynamic"].str() == "1") k->numchild = std::max(k->numchild, 1);
                    k->path = parentPath + "/" + k->exp;
                    if (auto it = dbgPrev_.find(k->path); it != dbgPrev_.end()) k->changed = it->second != k->value;
                    const bool pseudo = (k->exp == "public" || k->exp == "private" || k->exp == "protected") && k->type.empty();
                    order.push_back({k, pseudo});
                }
            slots->resize(order.size());
            *fin = [this, order, slots, done, more] {
                std::vector<VarP> out;
                for (std::size_t i = 0; i < order.size(); ++i) {
                    if (order[i].second) out.insert(out.end(), (*slots)[i].begin(), (*slots)[i].end());
                    else out.push_back(order[i].first);
                }
                done(std::move(out), more);
            };
            for (std::size_t i = 0; i < order.size(); ++i)
                if (order[i].second) {
                    ++*left;
                    dbgFetchKids(order[i].first->vo, parentPath, gen, [slots, left, fin, i](std::vector<VarP> ks, bool) {
                        (*slots)[i] = std::move(ks);
                        if (--*left == 0) (*fin)();
                    });
                }
            if (*left == 0) (*fin)();
        });
    }

    void dbgExpand(const VarP& v, int gen) {
        if (!dbg_ || v->vo.empty()) return;
        dbgFetchKids(v->vo, v->path, gen, [this, v, gen](std::vector<VarP> kids, bool more) {
            if (gen != dbgVarGen_) return;
            v->kids = std::move(kids);
            if (more) {
                auto m = std::make_shared<DbgVar>();
                m->exp = "..."; m->path = v->path + "/..."; m->value = "more items not shown";
                v->kids.push_back(m);
            }
            v->loaded = true;
            v->expanded = true;
            for (const auto& k : v->kids) if (k->numchild > 0 && dbgExpanded_.contains(k->path)) dbgExpand(k, gen);
            dirty_ = true;
        });
    }

    void dbgToggleVar(const VarP& v) {
        if (v->numchild <= 0) return;
        if (v->expanded) { v->expanded = false; dbgExpanded_.erase(v->path); return; }
        dbgExpanded_.insert(v->path);
        if (v->loaded) v->expanded = true; else dbgExpand(v, dbgVarGen_);
    }

    void dbgAddWatch(const std::string& e) {
        if (e.empty() || std::ranges::find(watches_, e) != watches_.end()) return;
        watches_.push_back(e);
        dbgSave();
        dbgSec_[1] = true;
        if (dbgState_ == DbgState::Stopped) dbgRefreshVars();
        dirty_ = true;
    }

    void dbgSetValue(const VarP& v, const std::string& value) {
        if (!dbg_ || dbgState_ != DbgState::Stopped || v->vo.empty()) return;
        dbg_->send("-var-assign " + v->vo + " " + gdb::quote(value), [this](const gdb::Record& r) {
            if (!r.ok()) { setMessage(r.msg(), true, 4); return; }
            dbgRefreshVars();
        });
    }

    // ---- console input

    std::deque<std::string> dbgEvalQ_;
    bool dbgEvalBusy_ = false;

    void dbgEval(const std::string& text) {
        if (text.empty()) return;
        dbgEvalQ_.push_back(text);
        if (dbgHist_.empty() || dbgHist_.back() != text) dbgHist_.push_back(text);
        dbgHistAt_ = -1;
        if (!dbgEvalBusy_) dbgEvalNext();
    }

    void dbgEvalNext() {
        if (dbgEvalQ_.empty()) { dbgEvalBusy_ = false; return; }
        dbgEvalBusy_ = true;
        const std::string text = dbgEvalQ_.front();
        dbgEvalQ_.pop_front();
        dbgConsole(text, 2);
        if (!dbg_) { dbgConsole("Not debugging - press F5 to start", 1); dbgEvalNext(); return; }
        if (dbgState_ != DbgState::Stopped) { dbgConsole("The program is running; pause it first (F6)", 1); dbgEvalNext(); return; }
        dbg_->send("-data-evaluate-expression " + gdb::quote(text), [this, text](const gdb::Record& r) {
            if (r.ok()) { dbgConsole(r.data["value"].str(), 5); dbgEvalNext(); return; }
            if (!dbg_) { dbgEvalBusy_ = false; return; }
            const std::string evalError = r.msg();
            dbg_->send("-interpreter-exec console " + gdb::quote(text), [this, evalError](const gdb::Record& r2) {   // gdb commands: bt, info locals, p x, ...
                if (!r2.ok()) dbgConsole(r2.msg().starts_with("Undefined command") ? evalError : r2.msg(), 1);   // not a command: report the expression's error
                dirty_ = true;
                dbgEvalNext();
            });
        });
    }

    bool dbgConKey(const KeyEvent& k) {
        if (k.key == Key::Escape) { dbgConFocus_ = false; return true; }
        if (k.key == Key::Enter) { const std::string t = dbgIn_.text; dbgIn_ = {}; dbgEval(t); return true; }
        if (k.key == Key::Up) {
            if (dbgHist_.empty()) return true;
            dbgHistAt_ = dbgHistAt_ < 0 ? static_cast<int>(dbgHist_.size()) - 1 : std::max(0, dbgHistAt_ - 1);
            dbgIn_.text = dbgHist_[static_cast<std::size_t>(dbgHistAt_)]; dbgIn_.cur = dbgIn_.text.size();
            return true;
        }
        if (k.key == Key::Down) {
            if (dbgHistAt_ < 0) return true;
            ++dbgHistAt_;
            if (dbgHistAt_ >= static_cast<int>(dbgHist_.size())) { dbgHistAt_ = -1; dbgIn_ = {}; }
            else { dbgIn_.text = dbgHist_[static_cast<std::size_t>(dbgHistAt_)]; dbgIn_.cur = dbgIn_.text.size(); }
            return true;
        }
        if (k.ctrl && k.key == Key::Char && k.ch == 'l') { dbgCon_.clear(); return true; }
        if (k.ctrl && k.key != Key::Char) return false;
        if (k.key >= Key::F1) return false;
        if (k.ctrl && k.key == Key::Char && k.ch != 'v' && k.ch != 'a') return false;
        return editText(dbgIn_, k);
    }

    void drawDebugConsole(const Rect& body) {
        const float s = S(), lineH = std::round(lh() + 3 * s), inH = std::round(lineH + 14 * s), pad = 12 * s, headH = std::round(uiH() + 14 * s);
        // status strip
        {
            const Color dot = !dbg_ ? theme::lineNum : dbgState_ == DbgState::Stopped ? Color::rgb(0xe5c07b) : Color::rgb(0x73c990);
            const float d = std::round(8 * s);
            fill(body.x + pad, body.y + (headH - d) / 2, d, d, dot, d / 2);
            std::string t = !dbg_ ? "Not debugging" : dbgState_ == DbgState::Stopped ? dbgStopText_ : dbgState_ == DbgState::Running ? "Running" : "Starting";
            if (dbg_ && dbgState_ == DbgState::Stopped && dbgLine_.line > 0) t += "  -  " + fs::path(dbgLine_.file).filename().string() + ":" + std::to_string(dbgLine_.line);
            uiLabel(body.x + pad + d + 10 * s, body.y, headH, t, dbg_ ? theme::text : theme::dim, body.right() - pad);
            fill(body.x, body.y + headH - 1, body.w, std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 14));
        }
        const Rect list{body.x, body.y + headH, body.w, std::max(0.0f, body.h - headH - inH)};
        const float total = static_cast<float>(dbgCon_.size()) * lineH + 8 * s;
        const float maxS = std::max(0.0f, total - list.h);
        auto& sc = panelScroll_[4];
        sc = std::clamp(panelFollow_[4] ? maxS : sc, 0.0f, maxS);
        const float sy = std::round(sc);
        const int first = std::max(0, static_cast<int>(std::floor((sy - 4 * s) / lineH)));
        const int last = std::min(static_cast<int>(dbgCon_.size()) - 1, static_cast<int>(std::floor((sy + list.h) / lineH)));
        const int ip = std::max(10, static_cast<int>(std::round(lineH * 0.6f)));
        for (int i = first; i <= last; ++i) {
            const auto& l = dbgCon_[static_cast<std::size_t>(i)];
            const float y = list.y + 4 * s + static_cast<float>(i) * lineH - sy;
            if (y < list.y || y + lineH > list.bottom() + 1) continue;
            Color c = Color::rgb(0xc9ccd3);
            char32_t g = 0;
            switch (l.kind) {
                case 1: c = Color::rgb(0xf2777a); g = 0xF05D6; break;                       // error
                case 2: c = Color::rgb(0xffffff); g = 0xF0142; break;                       // what you typed
                case 3: c = Color::rgb(0xe5c07b); g = 0xF0B8A; break;                       // logpoint
                case 5: c = Color::rgb(0x9fe0b4); g = 0xF0141; break;                       // evaluation result
                default: break;
            }
            if (l.kind == 4) {   // session / info markers: a centred rule
                const float tw = uiW(l.text);
                const float cx = list.x + list.w / 2, ly = y + lineH / 2;
                fill(list.x + pad, ly, std::max(0.0f, cx - tw / 2 - 14 * s - list.x - pad), std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 22));
                fill(cx + tw / 2 + 14 * s, ly, std::max(0.0f, list.right() - pad - (cx + tw / 2 + 14 * s)), std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 22));
                uiLabelC(cx, y, lineH, l.text, theme::dim);
                continue;
            }
            if (l.kind == 2) fill(list.x + 6 * s, y, list.w - 12 * s, lineH, Color::rgb(0xffffff, 10), 5 * s);
            if (g) Icons::glyph(dl_, font_, list.x + pad, y + (lineH - static_cast<float>(ip)) / 2, ip, g, c.alpha(200));
            label(list.x + pad + static_cast<float>(ip) + 8 * s, y + (lineH - lh()) / 2, cleanTerm(l.text), c, list.right() - pad);
        }
        if (dbgCon_.empty()) {
            const float cy = list.y + list.h / 2;
            uiLabelC(list.x + list.w / 2, cy - uiH(), uiH() + 8 * s, "Debug console", theme::dim);
            uiLabelC(list.x + list.w / 2, cy + 2 * s, uiH() + 8 * s, "Evaluate expressions (x + 1) or run gdb commands (bt, info locals) while paused", theme::lineNum);
        }
        // input row
        const Rect in{body.x + 8 * s, body.bottom() - inH + 5 * s, body.w - 16 * s, inH - 10 * s};
        fill(in, dbgConFocus_ ? theme::accent : Color::rgb(0xffffff, 30), in.h / 2);
        fill(in.x + 1, in.y + 1, in.w - 2, in.h - 2, Color::rgb(0x07080a), in.h / 2 - 1);
        const float x0 = in.x + 14 * s;
        const int cp = std::max(10, static_cast<int>(std::round(in.h * 0.5f)));
        Icons::glyph(dl_, font_, x0, in.y + (in.h - static_cast<float>(cp)) / 2, cp, 0xF0142, dbgConFocus_ ? theme::accent.mix(Color::rgb(0xffffff), 0.3f) : theme::dim);
        const float tx = x0 + static_cast<float>(cp) + 8 * s;
        std::size_t start = 0;
        const std::size_t cur = std::min(dbgIn_.cur, dbgIn_.text.size());
        while (start < cur && uiW(std::string_view(dbgIn_.text).substr(start, cur - start)) > in.right() - 14 * s - tx) start = utf8Next(dbgIn_.text, start);
        if (dbgIn_.text.empty()) uiLabel(tx, in.y, in.h, dbg_ ? "Evaluate an expression or run a gdb command" : "Start debugging to use the console", theme::lineNum, in.right() - 14 * s);
        else uiLabel(tx, in.y, in.h, std::string_view(dbgIn_.text).substr(start), theme::text, in.right() - 14 * s);
        if (dbgConFocus_ && caretOn_ && focused_) {
            const float cx = tx + uiW(std::string_view(dbgIn_.text).substr(start, cur - start));
            fill(std::round(cx), in.y + 7 * s, std::max(2.0f, std::round(1.4f * s)), in.h - 14 * s, theme::caret, s);
        }
        dbgConIn_ = in;
        if (maxS > 0) {
            const float th = std::max(24 * s, list.h * list.h / (total + list.h * 0.5f));
            fill(list.right() - 9 * s, list.y + (list.h - th) * std::clamp(sy / maxS, 0.0f, 1.0f), 5 * s, th, theme::scrollbar, 2.5f * s);
        }
    }
    Rect dbgConIn_;

    // ---- the Run and Debug sidebar view

    struct DRow { int kind = 0, idx = 0, depth = 0; VarP var; };   // 1 section, 2 var, 3 hint, 4 thread, 5 frame, 6 breakpoint, 7 status/start
    std::vector<DRow> dbgRows_;
    struct DBtn { Rect r; int id; };

    void dbgBuildRows() {
        dbgRows_.clear();
        dbgRows_.push_back({7, 0, 0, nullptr});
        auto addVars = [&](auto&& self, const std::vector<VarP>& vs, int depth) -> void {
            for (const auto& v : vs) {
                dbgRows_.push_back({2, 0, depth, v});
                if (v->expanded) self(self, v->kids, depth + 1);
            }
        };
        dbgRows_.push_back({1, 0, 0, nullptr});
        if (dbgSec_[0]) {
            if (dbgState_ != DbgState::Stopped) dbgRows_.push_back({3, 0, 1, nullptr});
            else addVars(addVars, dbgLocals_, 0);
        }
        dbgRows_.push_back({1, 1, 0, nullptr});
        if (dbgSec_[1]) {
            if (watches_.empty()) dbgRows_.push_back({3, 1, 1, nullptr});
            else if (dbgState_ == DbgState::Stopped) addVars(addVars, dbgWatchVars_, 0);
            else for (std::size_t i = 0; i < watches_.size(); ++i) dbgRows_.push_back({3, 10 + static_cast<int>(i), 1, nullptr});
        }
        dbgRows_.push_back({1, 2, 0, nullptr});
        if (dbgSec_[2]) {
            if (dbgState_ != DbgState::Stopped) dbgRows_.push_back({3, 2, 1, nullptr});
            else {
                if (dbgThreads_.size() > 1) for (std::size_t i = 0; i < dbgThreads_.size(); ++i) dbgRows_.push_back({4, static_cast<int>(i), 0, nullptr});
                for (std::size_t i = 0; i < dbgFrames_.size(); ++i) dbgRows_.push_back({5, static_cast<int>(i), 0, nullptr});
            }
        }
        dbgRows_.push_back({1, 3, 0, nullptr});
        if (dbgSec_[3]) {
            if (bps_.empty()) dbgRows_.push_back({3, 3, 1, nullptr});
            for (std::size_t i = 0; i < bps_.size(); ++i) dbgRows_.push_back({6, static_cast<int>(i), 0, nullptr});
        }
    }

    // Hover buttons of a row (right-aligned), by action id.
    std::vector<std::pair<int, char32_t>> dbgRowActions(const DRow& r) const {
        switch (r.kind) {
            case 1:
                if (r.idx == 1) return {{10, 0xF0415}};
                if (r.idx == 3) return {{11, 0xF0CE6}, {8, 0xF0295}, {9, 0xF0C62}};
                return {};
            case 2: {
                const bool isWatchRoot = r.var && r.var->path.starts_with("watch/") && r.var->path.find('/', 6) == std::string::npos;
                std::vector<std::pair<int, char32_t>> a = {{1, 0xF0CB6}, {7, 0xF018F}, {3, 0xF04FE}};
                if (isWatchRoot) a.push_back({4, 0xF09E7}); else a.insert(a.begin() + 2, {2, 0xF086C});
                return a;
            }
            case 6: return {{5, 0xF0CB6}, {6, 0xF09E7}};
            default: return {};
        }
    }

    std::vector<float> dbgY_;   // top of every row (relative to the list), filled by dbgLayoutRows

    float dbgRowH(const DRow& r) const {
        const float rh = L_.sideRow, s = S();
        switch (r.kind) {
            case 7: return std::round(rh + 30 * s);                        // status / start card
            case 1: return std::round(rh + (r.idx == 0 ? 4 : 12) * s);     // section header with breathing room above
            default: return rh;
        }
    }
    void dbgLayoutRows() {
        dbgY_.clear();
        float y = 0;
        for (const auto& r : dbgRows_) { dbgY_.push_back(y); y += dbgRowH(r); }
        dbgY_.push_back(y + 12 * S());   // total height incl. bottom padding
    }
    int dbgRowAt(float yRel) const {
        for (std::size_t i = 0; i + 1 < dbgY_.size(); ++i) if (yRel >= dbgY_[i] && yRel < dbgY_[i + 1] - (i + 2 == dbgY_.size() ? 12 * S() : 0)) return static_cast<int>(i);
        return -1;
    }

    void drawDebugBody() {
        const Rect& sd = L_.side;
        const float s = S(), rh = L_.sideRow, hair = std::max(1.0f, std::round(0.8f * s));
        dbgBuildRows();
        dbgLayoutRows();
        dbgTop_ = L_.sideList.y + 4 * s;
        const float viewH = std::max(0.0f, L_.sideList.bottom() - dbgTop_);
        const float contentH = dbgY_.back();
        dbgMax_ = std::max(0.0f, contentH - viewH);
        dbgScroll_ = std::clamp(dbgScroll_, 0.0f, dbgMax_);
        const float sy = std::round(dbgScroll_);
        const float indent = std::round(14 * s);
        static const char* secNames[4] = {"Variables", "Watch", "Call stack", "Breakpoints"};
        const int nStack = static_cast<int>(dbgFrames_.size());
        const int counts[4] = {static_cast<int>(dbgLocals_.size()), static_cast<int>(watches_.size()), nStack, static_cast<int>(bps_.size())};
        for (std::size_t i = 0; i < dbgRows_.size(); ++i) {
            const DRow& r = dbgRows_[i];
            const float h = dbgRowH(r), y = dbgTop_ + dbgY_[i] - sy;
            if (y + h < L_.sideList.y || y > L_.sideList.bottom()) continue;
            const float top = r.kind == 1 && r.idx != 0 ? std::round(12 * s) : 0.0f;   // header rows keep their padding on top
            const float ry = y + top, rowH = h - top;
            const Rect rr{sd.x + 6 * s, ry + 1, sd.w - 12 * s, rowH - 2};
            const bool hot = L_.sideList.contains(mx_, my_) && my_ >= ry && my_ < ry + rowH && mx_ >= sd.x && mx_ < sd.right();
            if (hot && r.kind != 3 && r.kind != 7) fill(rr, r.kind == 1 ? Color::rgb(0xffffff, 12) : theme::sideHover, 6 * s);
            const float bs = std::round(rowH - 8 * s);
            float rightEdge = sd.right() - 14 * s;
            if (hot) {
                float bx = sd.right() - 10 * s - bs;
                for (const auto& [id, glyph] : dbgRowActions(r)) {
                    const Rect b{bx, ry + 4 * s, bs, bs};
                    if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 36), 5 * s);
                    const int ip = std::max(10, static_cast<int>(std::round(bs * 0.62f)));
                    Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, glyph, Color::rgb(0xffffff));
                    bx -= bs + 2 * s;
                }
                rightEdge = bx + bs;
            }
            switch (r.kind) {
                case 7: {   // status / start card
                    const Rect card{sd.x + 10 * s, y + 4 * s, sd.w - 20 * s, h - 8 * s};
                    if (!dbg_) {
                        const Rect b{card.x, card.y, card.w, std::round(rh + 8 * s)};
                        const bool h2 = b.contains(mx_, my_);
                        fill(b, theme::accent.alpha(h2 ? 235 : 175), b.h / 2);
                        fill(b.x + hair, b.y + hair, b.w - 2 * hair, b.h - 2 * hair, h2 ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), b.h / 2 - hair);
                        const int ip = std::max(14, static_cast<int>(std::round(b.h * 0.5f)));
                        Icons::glyph(dl_, font_, b.x + 16 * s, b.y + (b.h - static_cast<float>(ip)) / 2, ip, 0xF040A, theme::accent.mix(Color::rgb(0xffffff), 0.14f));
                        uiLabel(b.x + 16 * s + static_cast<float>(ip) + 10 * s, b.y, b.h, "Start Debugging", theme::text);
                        uiLabelR(b.right() - 16 * s, b.y, b.h, "F5", theme::lineNum);
                        uiLabel(card.x + 6 * s, b.bottom() + 4 * s, uiH() + 6 * s, "Builds the active target and runs it under gdb", theme::lineNum, card.right());
                    } else {
                        const bool stopped = dbgState_ == DbgState::Stopped;
                        fill(card, Color::rgb(0xffffff, 12), 9 * s);
                        fill(card.x + hair, card.y + hair, card.w - 2 * hair, card.h - 2 * hair, Color::rgb(0x0a0b0d), 9 * s - hair);
                        const Color dot = stopped ? Color::rgb(0xe5c07b) : Color::rgb(0x73c990);
                        const float d = std::round(9 * s);
                        fill(card.x + 14 * s, card.y + (rh - d) / 2 + 4 * s, d, d, dot, d / 2);
                        const std::string t = dbgState_ == DbgState::Running ? "Running" : dbgState_ == DbgState::Starting ? "Starting gdb" : dbgStopText_;
                        uiLabel(card.x + 14 * s + d + 10 * s, card.y + 4 * s, rh, t, stopped ? theme::text : theme::dim, card.right() - 10 * s);
                        std::string sub = dbgLast_.name;
                        if (stopped && dbgLine_.line > 0) sub += "  -  " + fs::path(dbgLine_.file).filename().string() + ":" + std::to_string(dbgLine_.line);
                        uiLabel(card.x + 14 * s + d + 10 * s, card.y + 4 * s + rh - 4 * s, rh, sub, theme::lineNum, card.right() - 10 * s);
                    }
                    break;
                }
                case 1: {
                    if (r.idx != 0) fill(sd.x + 12 * s, y + 4 * s, sd.w - 24 * s, hair, Color::rgb(0xffffff, 14));
                    const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                    Icons::chevron(dl_, font_, sd.x + 12 * s, ry + (rowH - static_cast<float>(cs)) / 2, cs, dbgSec_[r.idx], theme::dim);
                    std::string title = secNames[r.idx];
                    for (auto& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    uiLabel(sd.x + 12 * s + static_cast<float>(cs) + 8 * s, ry, rowH, title, Color::rgb(0xd4d7dd), rightEdge);
                    if (!hot && counts[r.idx] > 0) {
                        const std::string cnt = std::to_string(counts[r.idx]);
                        const float bw = std::max(uiW(cnt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                        const Rect badge{sd.right() - 14 * s - bw, ry + (rowH - bh) / 2, bw, bh};
                        fill(badge, Color::rgb(0xffffff, 22), bh / 2);
                        uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, cnt, theme::dim);
                    }
                    break;
                }
                case 3: {
                    static const char* hints[4] = {"Not paused", "Add an expression with +", "Not paused", "Click the editor gutter or press F9"};
                    const std::string t = r.idx >= 10 ? watches_[static_cast<std::size_t>(r.idx - 10)] : hints[r.idx];
                    uiLabel(sd.x + 32 * s, ry, rowH, t, theme::lineNum, sd.right() - 12 * s);
                    break;
                }
                case 2: {
                    const VarP& v = r.var;
                    float x = sd.x + 14 * s + static_cast<float>(r.depth) * indent;
                    if (v->numchild > 0) {
                        const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                        Icons::chevron(dl_, font_, x + (indent - static_cast<float>(cs)) / 2, ry + (rowH - static_cast<float>(cs)) / 2, cs, v->expanded, theme::dim);
                    }
                    x += indent;
                    const bool isW = v->path.starts_with("watch/") && v->path.find('/', 6) == std::string::npos;
                    if (v->changed) fill(rr.x, rr.y, std::round(2.5f * s), rr.h, Color::rgb(0xe5c07b), 1.5f * s);
                    x = uiLabel(x, ry, rowH, v->exp, isW ? Color::rgb(0xe5c07b) : Color::rgb(0x8cc4ff), rightEdge - 20 * s);
                    x = uiLabel(x, ry, rowH, "  ", theme::dim, rightEdge);
                    std::string val = v->value;
                    if (val.empty() && v->numchild > 0) val = "{...}";
                    const bool err = val.starts_with("<");
                    const bool str = !val.empty() && (val.front() == '"' || val.front() == '\'');
                    Color vc = err ? Color::rgb(0xf2777a) : v->changed ? Color::rgb(0xe5c07b) : str ? Color::rgb(0xce9178) : (!val.empty() && (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-')) ? Color::rgb(0xb5cea8) : Color::rgb(0xd4d7dd);
                    x = uiLabel(x, ry, rowH, val, vc, rightEdge);
                    if (!v->type.empty() && !hot && rightEdge - x > 70 * s) uiLabel(x + 12 * s, ry, rowH, v->type, theme::lineNum, rightEdge);
                    break;
                }
                case 4: {
                    const DbgThread& t = dbgThreads_[static_cast<std::size_t>(r.idx)];
                    const bool cur = t.id == dbgThread_;
                    if (cur) fill(rr, theme::sideSel, 6 * s);
                    const int ip = std::max(10, static_cast<int>(std::round(rowH * 0.55f)));
                    Icons::glyph(dl_, font_, sd.x + 14 * s, ry + (rowH - static_cast<float>(ip)) / 2, ip, 0xF0A2E, cur ? Color::rgb(0xe5c07b) : theme::dim);   // chart-timeline-variant: thread
                    uiLabel(sd.x + 14 * s + static_cast<float>(ip) + 8 * s, ry, rowH, std::format("Thread {}{}  {}", t.id, t.name.empty() ? "" : " " + t.name, t.func), cur ? theme::text : theme::dim, rightEdge);
                    break;
                }
                case 5: {
                    const DbgFrame& f = dbgFrames_[static_cast<std::size_t>(r.idx)];
                    const bool cur = f.level == dbgSel_;
                    if (cur) { fill(rr, theme::accent.alpha(40), 6 * s); fill(rr.x, rr.y + 4 * s, std::round(2.5f * s), rr.h - 8 * s, theme::accent, 1.5f * s); }
                    const bool hasSrc = f.line > 0 && !(f.full.empty() && f.file.empty());
                    const float x = sd.x + 16 * s;
                    float xe = uiLabel(x, ry, rowH, f.func.empty() ? f.addr : f.func, cur ? Color::rgb(0xffffff) : hasSrc ? theme::text : theme::dim, rightEdge - 40 * s);
                    if (hasSrc) uiLabel(xe + 10 * s, ry, rowH, fs::path(!f.full.empty() ? f.full : f.file).filename().string() + ":" + std::to_string(f.line), theme::lineNum, rightEdge);
                    break;
                }
                case 6: {
                    const Breakpoint& b = bps_[static_cast<std::size_t>(r.idx)];
                    const float d = std::round(11 * s), cx = sd.x + 16 * s;
                    const bool pending = dbg_ && !b.verified && b.id < 0 && dbgState_ != DbgState::Off;
                    const Color bc = b.enabled ? Color::rgb(0xe5484d) : Color::rgb(0x6b6f78);
                    if (!b.log.empty()) Icons::glyph(dl_, font_, cx - 1, ry + (rowH - d * 1.3f) / 2, static_cast<int>(std::round(d * 1.3f)), 0xF0B8A, bc);
                    else if (b.enabled && !pending) fill(cx, ry + (rowH - d) / 2, d, d, bc, d / 2);
                    else { fill(cx, ry + (rowH - d) / 2, d, d, bc, d / 2); fill(cx + 2 * s, ry + (rowH - d) / 2 + 2 * s, d - 4 * s, d - 4 * s, theme::sideBg, (d - 4 * s) / 2); }
                    float x = uiLabel(cx + d + 10 * s, ry, rowH, dbgBpTitle(b), b.enabled ? theme::text : theme::dim, rightEdge - 20 * s);
                    auto chip = [&](const std::string& txt, Color c) {
                        const float cw2 = uiW(txt) + 12 * s, ch = std::round(uiH() + 2 * s);
                        if (x + 8 * s + cw2 > rightEdge) return;
                        const Rect cr{x + 8 * s, ry + (rowH - ch) / 2, cw2, ch};
                        fill(cr, c.alpha(40), ch / 2);
                        uiLabelC(cr.x + cr.w / 2, cr.y, cr.h, txt, c);
                        x = cr.right();
                    };
                    if (!b.log.empty()) chip("log", Color::rgb(0xe5c07b));
                    else if (!b.cond.empty()) chip("if " + b.cond, Color::rgb(0x8cc4ff));
                    else if (b.ignore > 0) chip("skip " + std::to_string(b.ignore), Color::rgb(0x8cc4ff));
                    if (b.hits > 0) chip(std::to_string(b.hits) + "x", theme::dim);
                    break;
                }
                default: break;
            }
        }
        if (contentH > viewH) {
            const float thumbH = std::max(24 * s, viewH * viewH / (contentH + viewH * 0.5f));
            fill(sd.right() - 9 * s, dbgTop_ + (viewH - thumbH) * std::clamp(sy / std::max(1.0f, dbgMax_), 0.0f, 1.0f), 5 * s, thumbH, theme::scrollbar, 2.5f * s);
        }
        fill(sd.x, L_.sideList.y, sd.w, 4 * s, theme::sideBg);
        (void)rh;
    }

    void dbgPromptFor(PromptKind k, const std::string& label, const std::string& text = {}) {
        openFileOp(k, {}, label, text, !text.empty());
    }

    void debugClick(Button b) {
        if (b != Button::Left) return;
        dbgBuildRows();
        dbgLayoutRows();
        const float rh = L_.sideRow, s = S();
        if (!L_.sideList.contains(mx_, my_) || my_ < dbgTop_) return;
        const int row = dbgRowAt(my_ - dbgTop_ + dbgScroll_);
        if (row < 0) return;
        const DRow r = dbgRows_[static_cast<std::size_t>(row)];
        const float h = dbgRowH(r), top = r.kind == 1 && r.idx != 0 ? std::round(12 * s) : 0.0f;
        const float y = dbgTop_ + dbgY_[static_cast<std::size_t>(row)] - dbgScroll_ + top, rowH = h - top, bs = std::round(rowH - 8 * s);
        (void)rh;
        float bx = L_.side.right() - 10 * s - bs;
        for (const auto& [id, glyph] : dbgRowActions(r)) {
            (void)glyph;
            if (Rect{bx, y + 4 * s, bs, bs}.contains(mx_, my_)) { dbgAction(id, r); return; }
            bx -= bs + 2 * s;
        }
        switch (r.kind) {
            case 7: if (!dbg_) dbgContinueOrStart(); break;
            case 1: if (my_ >= y) dbgSec_[r.idx] = !dbgSec_[r.idx]; break;
            case 2: dbgToggleVar(r.var); break;
            case 4: dbgSelectThread(dbgThreads_[static_cast<std::size_t>(r.idx)].id); break;
            case 5: dbgSelectFrame(dbgFrames_[static_cast<std::size_t>(r.idx)].level); break;
            case 6: {
                Breakpoint& bp = bps_[static_cast<std::size_t>(r.idx)];
                if (mx_ < L_.side.x + 16 * s + std::round(11 * s) + 8 * s) { dbgSetEnabled(bp.uid, !bp.enabled); break; }
                if (!bp.file.empty() && bp.line > 0) {
                    openFile(bp.file);
                    if (!docs_.empty() && doc().path == bp.file) { doc().setCursor({bp.line - 1, 0}); scrollToCursor_ = true; }
                }
                break;
            }
            default: break;
        }
        dirty_ = true;
    }

    // The full C expression of a variable (members of members, array elements...), asked from gdb for children.
    void dbgVarExpr(const VarP& v, std::function<void(const std::string&)> done) {
        const bool root = v->path.find('/', v->path.find('/') + 1) == std::string::npos;
        if (root || !dbg_ || v->vo.empty()) { done(v->exp); return; }
        dbg_->send("-var-info-path-expression " + v->vo, [this, done, v](const gdb::Record& r) {
            done(r.ok() ? r.data["path_expr"].str() : v->exp);
            dirty_ = true;
        });
    }

    void dbgAction(int id, const DRow& r) {
        switch (id) {
            case 1: if (r.var && dbgState_ == DbgState::Stopped) { dbgEditVar_ = r.var; dbgPromptFor(PromptKind::DbgSetValue, "New value of " + r.var->exp, r.var->value); } break;
            case 2: if (r.var) dbgVarExpr(r.var, [this](const std::string& e) { dbgAddWatch(e); }); break;
            case 3: if (r.var) dbgVarExpr(r.var, [this](const std::string& e) { Breakpoint nb; nb.watch = e; dbgAddBp(std::move(nb)); setMessage("Data breakpoint on " + e + ": the program stops when it changes", false, 3); }); break;
            case 4: if (r.var) {
                const auto at = r.var->path.find(':');
                const int i = std::atoi(r.var->path.c_str() + 6);
                (void)at;
                if (i >= 0 && i < static_cast<int>(watches_.size())) { watches_.erase(watches_.begin() + i); dbgSave(); if (dbgState_ == DbgState::Stopped) dbgRefreshVars(); }
            } break;
            case 5: if (r.idx >= 0 && r.idx < static_cast<int>(bps_.size())) {
                const Breakpoint& b = bps_[static_cast<std::size_t>(r.idx)];
                dbgPromptUid_ = b.uid;
                dbgPromptFor(PromptKind::DbgBpEdit, "Condition, #N skip count, or log: message {expr}  -  " + dbgBpTitle(b), dbgBpEditText(b));
            } break;
            case 6: if (r.idx >= 0 && r.idx < static_cast<int>(bps_.size())) dbgRemoveBp(bps_[static_cast<std::size_t>(r.idx)].uid); break;
            case 7: if (r.var) { win_.setClipboard(r.var->value); setMessage("Copied value", false, 1.2f); } break;
            case 8: dbgPromptFor(PromptKind::DbgFuncBp, "Break on function (e.g. main, ns::fn)"); break;
            case 9: { auto uids = std::vector<int>(); for (const auto& b : bps_) uids.push_back(b.uid); for (int u : uids) dbgRemoveBp(u); break; }
            case 10: dbgPromptFor(PromptKind::DbgWatch, "Watch expression"); break;
            case 11: {   // break when a C++ exception is thrown
                auto it = std::ranges::find_if(bps_, [](const Breakpoint& b) { return b.exceptions; });
                if (it != bps_.end()) dbgRemoveBp(it->uid);
                else { Breakpoint nb; nb.exceptions = true; dbgAddBp(std::move(nb)); }
                break;
            }
            default: break;
        }
        dirty_ = true;
    }

    void dbgFinishPrompt(PromptKind k, const std::string& t) {
        switch (k) {
            case PromptKind::DbgWatch: dbgAddWatch(t); break;
            case PromptKind::DbgSetValue: if (dbgEditVar_) dbgSetValue(dbgEditVar_, t); break;
            case PromptKind::DbgBpEdit: dbgApplyBpEdit(dbgPromptUid_, t); break;
            case PromptKind::DbgFuncBp: if (!t.empty()) { Breakpoint nb; nb.func = t; dbgAddBp(std::move(nb)); } break;
            default: break;
        }
    }

    void debugScroll(float dy) {
        dbgScroll_ = std::clamp(dbgScroll_ + dy, 0.0f, dbgMax_);
        dirty_ = true;
    }

    // ---- floating control bar over the editor

    struct DbgBarBtn { Rect r; int cmd; };
    std::vector<DbgBarBtn> dbgBar_;

    void drawDebugBar() {
        dbgBar_.clear();
        if (!dbg_ || L_.island.w < 200 * S()) return;
        const float s = S(), bs = std::round(uiH() + 14 * s), pad = std::round(5 * s), gap = std::round(2 * s);
        struct B { int cmd; char32_t glyph; const char* tip; bool enabled; Color col; };
        const bool st = dbgState_ == DbgState::Stopped, run = dbgState_ == DbgState::Running;
        const B btns[] = {
            {run ? 1 : 0, run ? 0xF03E4u : 0xF040Au, run ? "Pause (F6)" : "Continue (F5)", st || run, Color::rgb(0xffffff)},
            {2, 0xF01B7, "Step Over (F10)", st, Color::rgb(0xffffff)},
            {3, 0xF01B9, "Step Into (F11)", st, Color::rgb(0xffffff)},
            {4, 0xF01B8, "Step Out (Shift+F11)", st, Color::rgb(0xffffff)},
            {5, 0xF0709, "Restart (Ctrl+Shift+F5)", true, Color::rgb(0xffffff)},
            {6, 0xF04DB, "Stop (Shift+F5)", true, Color::rgb(0xf2777a)},
        };
        const float w = pad * 2 + 6 * bs + 5 * gap;
        const Rect bar{std::round(L_.island.x + (L_.island.w - w) / 2), L_.island.y + L_.tabs.h + 8 * s, w, bs + pad * 2};
        glassPanel(bar, bar.h / 2);
        float x = bar.x + pad;
        for (const auto& b : btns) {
            const Rect r{x, bar.y + pad, bs, bs};
            dbgBar_.push_back({r, b.cmd});
            const bool hot = r.contains(mx_, my_) && b.enabled;
            if (hot) fill(r, Color::rgb(0xffffff, 34), bs / 2);
            const int ip = std::max(14, static_cast<int>(std::round(bs * 0.58f)));
            Color c = b.col;
            if (!b.enabled) c = c.alpha(70);
            Icons::glyph(dl_, font_, std::round(r.x + (bs - static_cast<float>(ip)) / 2), std::round(r.y + (bs - static_cast<float>(ip)) / 2), ip, b.glyph, c);
            if (hot) dbgBarTip_ = b.tip, dbgBarTipRect_ = r;
            x += bs + gap;
        }
        if (!dbgBarTip_.empty() && dbgBarTipRect_.contains(mx_, my_)) {
            const float tw = uiW(dbgBarTip_) + 20 * s, th = std::round(uiH() + 10 * s);
            const Rect t{std::clamp(dbgBarTipRect_.x + dbgBarTipRect_.w / 2 - tw / 2, 4 * s, L_.status.w - tw - 4 * s), bar.bottom() + 6 * s, tw, th};
            glassPanel(t, 7 * s);
            uiLabelC(t.x + t.w / 2, t.y, t.h, dbgBarTip_, theme::text);
        } else dbgBarTip_.clear();
    }
    std::string dbgBarTip_;
    Rect dbgBarTipRect_;

    void dbgCommand(int cmd) {
        switch (cmd) {
            case 0: dbgContinueOrStart(); break;
            case 1: dbgPause(); break;
            case 2: dbgStep("-exec-next"); break;
            case 3: dbgStep("-exec-step"); break;
            case 4: dbgStep("-exec-finish"); break;
            case 5: dbgRestart(); break;
            case 6: debugStop(); break;
            default: break;
        }
        dirty_ = true;
    }

    bool dbgBarClick() {
        for (const auto& b : dbgBar_)
            if (b.r.contains(mx_, my_)) { dbgCommand(b.cmd); return true; }
        return false;
    }

    // Debugger keys: F5 continue / start, Shift+F5 stop, Ctrl+Shift+F5 restart, F6 pause, F9 breakpoint, F10 over, F11 into,
    // Shift+F11 out, Ctrl+F10 run to cursor, Ctrl+Shift+F10 set next statement.
    bool debugKey(const KeyEvent& k) {
        const bool ctrl = k.ctrl, shift = k.shift, alt = k.alt;
        if (alt) return false;
        switch (k.key) {
            case Key::F5:
                if (ctrl && shift) { dbgRestart(); return true; }
                if (shift) { debugStop(); return true; }
                if (ctrl) return false;
                dbgContinueOrStart(); return true;
            case Key::F6: if (ctrl || shift) return false; dbgPause(); return true;
            case Key::F9:
                if (ctrl || shift || docs_.empty()) return false;
                dbgToggleBpAt(doc().path, doc().cursor.line + 1); return true;
            case Key::F10:
                if (ctrl && shift) { dbgSetNextStatement(); return true; }
                if (ctrl) { dbgRunToCursor(); return true; }
                if (shift) return false;   // Shift+F10 stays "build and run"
                dbgStep("-exec-next"); return true;
            case Key::F11:
                if (ctrl) return false;
                dbgStep(shift ? "-exec-finish" : "-exec-step"); return true;
            default: return false;
        }
    }

    // Editor context menu (right click): breakpoints, debugger actions and the usual clipboard entries.
    void openEditorMenu(Pos p) {
        if (docs_.empty()) return;
        Document& d = doc();
        const bool sel = d.hasSelection(), stopped = dbgState_ == DbgState::Stopped;
        const std::string path = d.path;
        const int line = p.line + 1;
        std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>> items;
        items.emplace_back("Toggle Breakpoint", "F9", false, [this, path, line] { dbgToggleBpAt(path, line); });
        items.emplace_back("Add Conditional Breakpoint...", "", false, [this, path, line] {
            if (path.empty()) return;
            Breakpoint* b = dbgBpAt(path, line);
            if (!b) { Breakpoint nb; nb.file = path; nb.line = line; dbgAddBp(std::move(nb)); b = dbgBpAt(path, line); }
            if (b) { dbgPromptUid_ = b->uid; dbgPromptFor(PromptKind::DbgBpEdit, "Condition, #N skip count, or log: message {expr}", dbgBpEditText(*b)); }
        });
        items.emplace_back("Add Logpoint...", "", false, [this, path, line] {
            if (path.empty()) return;
            Breakpoint* b = dbgBpAt(path, line);
            if (!b) { Breakpoint nb; nb.file = path; nb.line = line; dbgAddBp(std::move(nb)); b = dbgBpAt(path, line); }
            if (b) { dbgPromptUid_ = b->uid; dbgPromptFor(PromptKind::DbgBpEdit, "Logpoint message, e.g. x = {x}", "log: "); }
        });
        if (stopped) {
            items.emplace_back("Run to Cursor", "Ctrl+F10", false, [this, p] { if (!docs_.empty()) { doc().setCursor(p); dbgRunToCursor(); } });
            items.emplace_back("Set Next Statement", "Ctrl+Shift+F10", false, [this, p] { if (!docs_.empty()) { doc().setCursor(p); dbgSetNextStatement(); } });
        }
        if (sel && d.selectedText().find('\n') == std::string::npos) {
            const std::string t = d.selectedText();
            items.emplace_back("Add to Watch", "", false, [this, t] { dbgAddWatch(t); sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; });
            if (stopped) items.emplace_back("Evaluate in Debug Console", "", false, [this, t] { panelOpen_ = true; panelTab_ = 4; dbgEval(t); });
        }
        items.emplace_back("Cut", "Ctrl+X", false, [this] { if (!docs_.empty()) copy(true); });
        items.emplace_back("Copy", "Ctrl+C", false, [this] { if (!docs_.empty()) copy(false); });
        items.emplace_back("Paste", "Ctrl+V", false, [this] { if (!docs_.empty()) paste(); });
        items.emplace_back("Select All", "Ctrl+A", false, [this] { if (!docs_.empty()) doc().selectAll(); });
        openCustomMenu(mx_, my_, items);
    }

    // Breakpoint dots and the execution marker in the gutter of `d`, plus the execution line highlight in the text area.
    void drawDebugGutter(const Document& d, int ln, float y, float lineH) {
        const float s = S(), cell = cw();
        const Rect& e = L_.editor;
        for (const auto& b : bps_) {
            if (b.file != d.path || b.line - 1 != ln || !b.func.empty() || !b.watch.empty() || b.exceptions) continue;
            const float dsz = std::round(std::min(lineH - 6 * s, 1.1f * cell));
            const float cx = e.x + std::round(0.5f * cell), cy = y + (lineH - dsz) / 2;
            const bool pending = dbg_ && !b.verified && dbgState_ != DbgState::Off;
            const Color c = b.enabled ? Color::rgb(0xe5484d) : Color::rgb(0x6b6f78);
            if (!b.log.empty()) {   // logpoint: diamond
                const int ip = std::max(10, static_cast<int>(std::round(dsz * 1.25f)));
                Icons::glyph(dl_, font_, cx - 1, y + (lineH - static_cast<float>(ip)) / 2, ip, 0xF0B8A, c);
            } else if (pending || !b.cond.empty()) {
                fill(cx, cy, dsz, dsz, c, dsz / 2);
                fill(cx + 2 * s, cy + 2 * s, dsz - 4 * s, dsz - 4 * s, b.cond.empty() ? theme::editorBg : theme::editorBg.mix(c, 0.35f), (dsz - 4 * s) / 2);
            } else fill(cx, cy, dsz, dsz, c, dsz / 2);
        }
        if (dbgLine_.line > 0 && dbgLine_.file == d.path && dbgLine_.line - 1 == ln) {
            const int ip = std::max(12, static_cast<int>(std::round(lineH * 0.8f)));
            Icons::glyph(dl_, font_, e.x + std::round(0.2f * cell), y + (lineH - static_cast<float>(ip)) / 2 - 1, ip, 0xF0734, Color::rgb(0xe5c07b));
        }
    }

    // ============================================================ project dialogs (welcome / new project / folder browser)

    enum DlgId : int {
        kDlgNew = 1, kDlgOpen = 2, kDlgRecent = 100, kDlgRecentDel = 200, kDlgTmpl = 300,
        kDlgName = 400, kDlgLoc = 401, kDlgBrowseBtn = 402, kDlgStd = 410, kDlgGit = 420, kDlgCreate = 430, kDlgCancel = 431,
        kDlgBm = 500, kDlgPath = 550, kDlgUp = 551, kDlgHidden = 552, kDlgOk = 553, kDlgBrowseCancel = 554, kDlgEntry = 600,
    };
    enum class DlgKind { None, Welcome, NewProject, Browse };
    struct BrowseEntry { std::string name; bool cmake = false; };
    struct Dialog {
        DlgKind kind = DlgKind::None;
        float recentScroll = 0;
        // new project
        int tmpl = 0, std_ = 1, focus = 0;
        bool git = true;
        TextBox name, location;
        std::string error;
        // folder browser
        bool forLocation = false, pathFocus = true, showHidden = false;
        TextBox path;
        std::string dir;
        std::vector<BrowseEntry> entries;
        int sel = -1;
        float scroll = 0;
        int hover = -1;
    };
    struct DlgHit { Rect r; int id; };
    Dialog dlg_;
    std::vector<DlgHit> dlgHits_;
    DaemonState state_;
    Clock::time_point dlgLastClick_{};
    int dlgLastId_ = -1;

    static std::string homeDir() { const char* h = std::getenv("HOME"); return h && *h ? h : "/"; }

    void dlgClose() { dlg_.kind = DlgKind::None; dlgHits_.clear(); dirty_ = true; }
    void welcomeOpen() { dlg_ = {}; dlg_.kind = DlgKind::Welcome; dirty_ = true; }

    std::string uniqueProjectName(const std::string& parent, const std::string& base) {
        std::error_code ec;
        for (int i = 0; i < 1000; ++i) {
            const std::string n = i == 0 ? base : base + std::to_string(i);
            if (!fs::exists(fs::path(parent) / n, ec)) return n;
        }
        return base;
    }

    void newProjectOpen() {
        const std::string keepLoc = dlg_.location.text;
        dlg_ = {};
        dlg_.kind = DlgKind::NewProject;
        std::string loc = !state_.lastLocation.empty() ? state_.lastLocation : (fs::path(homeDir()) / "Projects").string();
        std::error_code ec;
        if (!fs::is_directory(loc, ec)) loc = fs::is_directory(fs::path(homeDir()) / "Projects", ec) ? (fs::path(homeDir()) / "Projects").string() : homeDir();
        (void)keepLoc;
        dlg_.location = {loc, loc.size()};
        const std::string n = uniqueProjectName(loc, "untitled");
        dlg_.name = {n, n.size()};
        dlg_.std_ = 1;   // C++20
        dlg_.focus = 0;
        dirty_ = true;
    }

    void browseRefresh() {
        dlg_.entries.clear();
        dlg_.sel = -1;
        dlg_.scroll = 0;
        std::error_code ec;
        for (fs::directory_iterator it(dlg_.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2)) continue;
            const std::string n = it->path().filename().string();
            if (!dlg_.showHidden && n.starts_with('.')) continue;
            dlg_.entries.push_back({n, fs::exists(it->path() / "CMakeLists.txt", e2)});
        }
        std::ranges::sort(dlg_.entries, [](const BrowseEntry& a, const BrowseEntry& b) {
            auto lower = [](std::string s) { std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; };
            return lower(a.name) < lower(b.name);
        });
        dlg_.path = {dlg_.dir, dlg_.dir.size()};
    }

    // Folder choosers: the desktop's own dialog (zenity talks to GTK / the portal, so it is GNOME's file manager dialog);
    // the built-in browser is only the fallback when no system dialog is available.
    bool pickerBusy_ = false;
    void browseOpen(bool forLocation) {
        if (pickerBusy_) return;
        if (!findExecutable("zenity")) { browseOpenBuiltin(forLocation); return; }
        std::error_code ec;
        std::string start = forLocation ? dlg_.location.text : (explorer_.isOpen() ? explorer_.rootPath().parent_path().string() : state_.lastLocation);
        start = expandPath(start);
        if (start.empty() || !fs::is_directory(start, ec)) start = homeDir();
        if (start.back() != '/') start += '/';
        const std::string title = forLocation ? "Choose Location" : "Open Folder";
        pickerBusy_ = true;
        setMessage("Choose a folder in the file dialog...", false, 4);
        spawn([this, start, title, forLocation]() -> std::function<void()> {
            ProcessOptions o;
            o.argv = {"zenity", "--file-selection", "--directory", "--title=" + title, "--filename=" + start};
            auto r = Process::runCapture(o, 30 * 60 * 1000);
            std::string chosen;
            std::istringstream in(r.output);
            for (std::string line; std::getline(in, line);) if (!line.empty() && line.front() == '/') chosen = line;
            const bool failed = !r.error.empty();
            return [this, chosen, forLocation, failed] {
                pickerBusy_ = false;
                if (failed) { browseOpenBuiltin(forLocation); return; }
                if (chosen.empty()) return;   // cancelled
                if (forLocation) {
                    if (dlg_.kind != DlgKind::NewProject) newProjectOpen();
                    dlg_.location = {chosen, chosen.size()};
                    dlg_.error.clear();
                    dlg_.focus = 0;
                } else {
                    openProject(chosen);
                }
            };
        });
    }

    void browseOpenBuiltin(bool forLocation) {
        const DlgKind from = dlg_.kind;
        if (from != DlgKind::NewProject) dlg_ = {};
        dlg_.kind = DlgKind::Browse;
        dlg_.forLocation = forLocation;
        dlg_.pathFocus = true;
        std::error_code ec;
        std::string start = forLocation ? dlg_.location.text : (explorer_.isOpen() ? explorer_.rootPath().parent_path().string() : state_.lastLocation);
        if (start.empty() || !fs::is_directory(start, ec)) start = homeDir();
        dlg_.dir = fs::path(start).lexically_normal().string();
        while (dlg_.dir.size() > 1 && dlg_.dir.back() == '/') dlg_.dir.pop_back();
        browseRefresh();
        dirty_ = true;
    }

    void browseGo(const std::string& dir) {
        std::error_code ec;
        fs::path p = fs::path(expandPath(dir)).lexically_normal();
        if (!fs::is_directory(p, ec)) { setMessage("Not a folder: " + dir, true, 2); return; }
        dlg_.dir = p.string();
        while (dlg_.dir.size() > 1 && dlg_.dir.back() == '/') dlg_.dir.pop_back();
        browseRefresh();
    }

    void browseConfirm() {
        std::string target = dlg_.dir;
        if (dlg_.sel >= 0 && dlg_.sel < static_cast<int>(dlg_.entries.size()))
            target = (fs::path(dlg_.dir) / dlg_.entries[static_cast<std::size_t>(dlg_.sel)].name).string();
        if (dlg_.forLocation) {
            dlg_.location = {target, target.size()};
            dlg_.kind = DlgKind::NewProject;
            dlg_.error.clear();
            dlg_.focus = 0;
            dirty_ = true;
        } else {
            openProject(target);
        }
    }

    void openProject(const std::string& path) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) { setMessage("Not a folder: " + path, true, 3); return; }
        dlgClose();
        openFolder(path);
    }

    void createProjectFromDialog() {
        NewProjectSpec spec;
        const auto& t = projectTemplates()[static_cast<std::size_t>(dlg_.tmpl)];
        spec.templateId = t.id;
        spec.name = dlg_.name.text;
        spec.parent = expandPath(dlg_.location.text);
        static const char* cxx[] = {"17", "20", "23"};
        static const char* cc[] = {"11", "17"};
        spec.standard = t.isC ? cc[std::clamp(dlg_.std_, 0, 1)] : cxx[std::clamp(dlg_.std_, 0, 2)];
        spec.initGit = dlg_.git;
        auto r = createProject(spec);
        if (!r) { dlg_.error = r.error(); dirty_ = true; return; }
        state_.lastLocation = spec.parent.string();
        saveState(state_);
        dlgClose();
        openFolder(r->dir.string());
        openFile(r->mainFile.string());
        if (!r->note.empty()) setMessage("Created " + r->dir.string() + " (" + r->note + ")", false, 4);
        else setMessage("Created " + r->dir.string(), false, 3);
    }

    // ---- text boxes

    bool editText(TextBox& t, const KeyEvent& k) {
        t.cur = std::min(t.cur, t.text.size());
        if (k.ctrl && k.key == Key::Char) {
            if (k.ch == 'v') {
                std::string c = win_.clipboard();
                std::erase(c, '\r');
                if (auto nl = c.find('\n'); nl != std::string::npos) c.resize(nl);
                t.text.insert(t.cur, c);
                t.cur += c.size();
                return true;
            }
            if (k.ch == 'a') { t.cur = t.text.size(); return true; }
            return false;
        }
        switch (k.key) {
            case Key::Left: if (t.cur > 0) t.cur = utf8Prev(t.text, t.cur); return true;
            case Key::Right: if (t.cur < t.text.size()) t.cur = utf8Next(t.text, t.cur); return true;
            case Key::Home: t.cur = 0; return true;
            case Key::End: t.cur = t.text.size(); return true;
            case Key::Backspace:
                if (t.cur > 0) { auto p = utf8Prev(t.text, t.cur); t.text.erase(p, t.cur - p); t.cur = p; }
                return true;
            case Key::Delete:
                if (t.cur < t.text.size()) { auto n = utf8Next(t.text, t.cur); t.text.erase(t.cur, n - t.cur); }
                return true;
            default:
                if (!k.text.empty() && !k.alt) { t.text.insert(t.cur, k.text); t.cur += k.text.size(); return true; }
                return false;
        }
    }

    std::size_t caretFromX(const std::string& t, float relX) {
        std::size_t best = 0;
        float bestD = std::abs(relX);
        for (std::size_t i = 0; i < t.size();) {
            i = utf8Next(t, i);
            const float d = std::abs(relX - uiW(std::string_view(t).substr(0, i)));
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    // ---- input

    void dlgKey(const KeyEvent& k) {
        Dialog& d = dlg_;
        if (k.key == Key::Escape) {
            if (d.kind == DlgKind::Browse && !d.forLocation) { dlgClose(); return; }
            if (d.kind == DlgKind::Browse) { d.kind = DlgKind::NewProject; return; }
            if (d.kind == DlgKind::NewProject) { welcomeOpen(); return; }
            dlgClose();
            return;
        }
        if (k.ctrl && k.key == Key::Char && k.ch == 'q') { requestQuit(); return; }
        switch (d.kind) {
            case DlgKind::Welcome:
                if (k.key == Key::Enter) { newProjectOpen(); return; }
                if (k.ctrl && k.shift && k.key == Key::Char && k.ch == 'o') { browseOpen(false); return; }
                if (k.ctrl && k.shift && k.key == Key::Char && k.ch == 'n') { newProjectOpen(); return; }
                break;
            case DlgKind::NewProject: {
                if (k.key == Key::Tab) { d.focus = (d.focus + 1) % 2; return; }
                if (k.key == Key::Enter) { createProjectFromDialog(); return; }
                if (k.key == Key::Up || k.key == Key::Down) {
                    const int n = static_cast<int>(projectTemplates().size());
                    d.tmpl = (d.tmpl + (k.key == Key::Down ? 1 : n - 1)) % n;
                    d.std_ = std::min(d.std_, projectTemplates()[static_cast<std::size_t>(d.tmpl)].isC ? 1 : 2);
                    return;
                }
                TextBox& t = d.focus == 0 ? d.name : d.location;
                if (editText(t, k)) d.error.clear();
                break;
            }
            case DlgKind::Browse: {
                if (k.key == Key::Tab) { d.pathFocus = !d.pathFocus; return; }
                const int n = static_cast<int>(d.entries.size());
                if (d.pathFocus) {
                    if (k.key == Key::Enter) { browseGo(d.path.text); d.pathFocus = false; return; }
                    if (k.key == Key::Down && n > 0) { d.pathFocus = false; d.sel = 0; return; }
                    editText(d.path, k);
                    return;
                }
                switch (k.key) {
                    case Key::Up: d.sel = std::max(0, d.sel - 1); dlgScrollToSel(); return;
                    case Key::Down: d.sel = std::min(n - 1, d.sel + 1); dlgScrollToSel(); return;
                    case Key::Right: if (d.sel >= 0 && d.sel < n) browseGo((fs::path(d.dir) / d.entries[static_cast<std::size_t>(d.sel)].name).string()); return;
                    case Key::Left: case Key::Backspace: browseGo(fs::path(d.dir).parent_path().string()); return;
                    case Key::Enter: browseConfirm(); return;
                    default: break;
                }
                break;
            }
            default: break;
        }
    }

    void dlgScrollToSel() {
        const float rowH = std::round(34 * S());
        const float top = dlg_.sel * rowH;
        if (top < dlg_.scroll) dlg_.scroll = top;
        else if (top + rowH > dlg_.scroll + browseListH_) dlg_.scroll = top + rowH - browseListH_;
    }

    void dlgClick(Button b, float x, float y) {
        if (b != Button::Left) return;
        int id = -1;
        for (auto it = dlgHits_.rbegin(); it != dlgHits_.rend(); ++it)
            if (it->r.contains(x, y)) { id = it->id; break; }
        const auto now = Clock::now();
        const bool dbl = id >= 0 && id == dlgLastId_ && now - dlgLastClick_ < std::chrono::milliseconds(420);
        dlgLastClick_ = now;
        dlgLastId_ = id;
        Dialog& d = dlg_;
        if (id < 0) {
            if (!dlgPanel_.contains(x, y)) { if (d.kind == DlgKind::Welcome) dlgClose(); }
            return;
        }
        auto hitRect = [&](int want) { for (const auto& h : dlgHits_) if (h.id == want) return h.r; return Rect{}; };
        if (id == kDlgNew) { newProjectOpen(); return; }
        if (id == kDlgOpen) { browseOpen(false); return; }
        if (id >= kDlgRecentDel && id < kDlgRecentDel + 100) {
            const auto idx = static_cast<std::size_t>(id - kDlgRecentDel);
            if (idx < dlgRecentShown_.size()) { std::erase(state_.recent, dlgRecentShown_[idx]); saveState(state_); }
            return;
        }
        if (id >= kDlgRecent && id < kDlgRecent + 100) {
            const auto idx = static_cast<std::size_t>(id - kDlgRecent);
            if (idx < dlgRecentShown_.size()) openProject(dlgRecentShown_[idx]);
            return;
        }
        if (id >= kDlgTmpl && id < kDlgTmpl + 50) {
            d.tmpl = id - kDlgTmpl;
            d.std_ = std::min(d.std_, projectTemplates()[static_cast<std::size_t>(d.tmpl)].isC ? 1 : 2);
            return;
        }
        if (id == kDlgName || id == kDlgLoc) {
            d.focus = id == kDlgName ? 0 : 1;
            TextBox& t = d.focus == 0 ? d.name : d.location;
            t.cur = caretFromX(t.text, x - (hitRect(id).x + 10 * S()));
            return;
        }
        if (id == kDlgBrowseBtn) { browseOpen(true); return; }
        if (id >= kDlgStd && id < kDlgStd + 4) { d.std_ = id - kDlgStd; return; }
        if (id == kDlgGit) { d.git = !d.git; return; }
        if (id == kDlgCreate) { createProjectFromDialog(); return; }
        if (id == kDlgCancel) { welcomeOpen(); return; }
        if (id >= kDlgBm && id < kDlgBm + 20) {
            if (static_cast<std::size_t>(id - kDlgBm) < dlgPlaces_.size()) browseGo(dlgPlaces_[static_cast<std::size_t>(id - kDlgBm)]);
            return;
        }
        if (id == kDlgPath) { d.pathFocus = true; d.path.cur = caretFromX(d.path.text, x - (hitRect(id).x + 10 * S())); return; }
        if (id == kDlgUp) { browseGo(fs::path(d.dir).parent_path().string()); return; }
        if (id == kDlgHidden) { d.showHidden = !d.showHidden; browseRefresh(); return; }
        if (id == kDlgOk) { browseConfirm(); return; }
        if (id == kDlgBrowseCancel) { if (d.forLocation) d.kind = DlgKind::NewProject; else dlgClose(); return; }
        if (id >= kDlgEntry && id < kDlgEntry + 400) {
            const int i = id - kDlgEntry;
            d.pathFocus = false;
            d.sel = i;
            if (dbl && i < static_cast<int>(d.entries.size())) browseGo((fs::path(d.dir) / d.entries[static_cast<std::size_t>(i)].name).string());
            return;
        }
    }

    void dlgScroll(float dy) {
        Dialog& d = dlg_;
        if (d.kind == DlgKind::Welcome) d.recentScroll = std::clamp(d.recentScroll + dy, 0.0f, std::max(0.0f, dlgRecentMax_));
        else if (d.kind == DlgKind::Browse) d.scroll = std::clamp(d.scroll + dy, 0.0f, std::max(0.0f, static_cast<float>(d.entries.size()) * std::round(34 * S()) - browseListH_));
    }

    // ---- drawing helpers

    void dlgHit(const Rect& r, int id) { dlgHits_.push_back({r, id}); }
    bool dlgHot(int id) const { return dlg_.hover == id; }

    void dlgButton(const Rect& r, std::string_view label, int id, bool primary, char32_t icon = 0) {
        const float s = S();
        dlgHit(r, id);
        const bool hot = dlgHot(id);
        if (primary) fill(r, hot ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : theme::accent, r.h / 2);
        else {
            fill(r, Color::rgb(0xffffff, hot ? 60 : 34), r.h / 2);
            fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, hot ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), r.h / 2 - 1);
        }
        float x = r.x + 16 * s;
        const Color tc = primary ? Color::rgb(0xffffff) : theme::text;
        if (icon) {
            const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.15f)));
            Icons::glyph(dl_, font_, x, r.y + (r.h - static_cast<float>(ip)) / 2, ip, icon, primary ? Color::rgb(0xffffff) : theme::dim);
            x += static_cast<float>(ip) + 10 * s;
            uiLabel(x, r.y, r.h, label, tc, r.right() - 8 * s);
        } else {
            uiLabelC(r.x + r.w / 2, r.y, r.h, label, tc);
        }
    }

    void dlgField(const Rect& r, const TextBox& t, bool focused, int id, std::string_view placeholder) {
        const float s = S();
        dlgHit(r, id);
        fill(r, focused ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 56 : 34), 8 * s);
        fill(r.x + 1, r.y + 1, r.w - 2, r.h - 2, Color::rgb(0x07080a), 7.5f * s);
        const float x0 = r.x + 10 * s, maxX = r.right() - 10 * s;
        std::size_t start = 0;
        const std::size_t cur = std::min(t.cur, t.text.size());
        while (start < cur && uiW(std::string_view(t.text).substr(start, cur - start)) > maxX - x0 - 4 * s) start = utf8Next(t.text, start);
        const std::string_view shown = std::string_view(t.text).substr(start);
        if (t.text.empty()) uiLabel(x0, r.y, r.h, placeholder, theme::lineNum, maxX);
        else uiLabel(x0, r.y, r.h, shown, theme::text, maxX);
        if (focused && caretOn_) {
            const float cx = x0 + uiW(std::string_view(t.text).substr(start, cur - start));
            fill(std::round(cx), r.y + 8 * s, std::max(2.0f, std::round(1.4f * s)), r.h - 16 * s, theme::caret, s);
        }
    }

    void drawDialog() {
        if (dlg_.kind == DlgKind::None) return;
        const float s = S(), W = L_.status.w, H = L_.status.bottom();
        dlgHits_.clear();
        dlgRecentShown_.clear();
        const float top0 = L_.csd ? L_.title.h : 0;
        fill(0, top0, W, H - top0, Color{0, 0, 0, 150});   // dim everything behind (part of the scene the glass blurs)
        const bool welcome = dlg_.kind == DlgKind::Welcome;
        const float pw = std::min((welcome ? 840.0f : 940.0f) * s, W - 60 * s), ph = std::min((welcome ? 500.0f : 580.0f) * s, H - 80 * s);
        const Rect r{std::round((W - pw) / 2), std::round((H - ph) / 2), std::round(pw), std::round(ph)};
        dlgPanel_ = r;
        dlgHit(r, 0);   // swallows clicks inside the panel
        dlgHits_.back().id = -2;
        switch (dlg_.kind) {
            case DlgKind::Welcome: drawWelcome(r); break;
            case DlgKind::NewProject: drawNewProject(r); break;
            case DlgKind::Browse: drawBrowse(r); break;
            default: break;
        }
    }

    // The dialogs are built from islands like the main window: a narrow one on the left, the main one on the right.
    void dlgSide(const Rect& r, float w) {
        const float s = S(), gap = std::round(6 * s), rad = std::round(12 * s);
        fill(r.x, r.y, w, r.h, theme::sideBg, rad);
        fill(r.x + w + gap, r.y, r.w - w - gap, r.h, theme::editorBg, rad);
    }

    // One clickable row of the welcome screen's action list: icon tile, label, shortcut.
    void welcomeAction(const Rect& row, std::string_view label, std::string_view keys, int id, char32_t icon, bool primary) {
        const float s = S(), hair = std::max(1.0f, std::round(0.8f * s));
        dlgHit(row, id);
        const bool hot = dlgHot(id);
        const Color edge = primary ? theme::accent.alpha(hot ? 230 : 170) : Color::rgb(0xffffff, hot ? 60 : 34);
        fill(row, edge, row.h / 2);   // same outlined pill as the toolbar and the activity bar
        fill(row.x + hair, row.y + hair, row.w - 2 * hair, row.h - 2 * hair, hot ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), row.h / 2 - hair);
        const int ip = std::max(15, static_cast<int>(std::round(row.h * 0.42f)));
        Icons::glyph(dl_, font_, row.x + 18 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, icon, primary ? theme::accent.mix(Color::rgb(0xffffff), 0.14f) : Color::rgb(0xffffff));
        const float kx = row.right() - 18 * s;
        uiLabelR(kx, row.y, row.h, keys, theme::lineNum);
        uiLabel(row.x + 18 * s + static_cast<float>(ip) + 12 * s, row.y, row.h, label, theme::text, kx - uiW(keys) - 10 * s);
    }

    void drawWelcome(const Rect& r) {
        const float s = S(), sideW = std::round(320 * s);
        dlgSide(r, sideW);
        const float lx = r.x + 40 * s, cw0 = sideW - 64 * s;
        const int ls = static_cast<int>(std::round(44 * s));
        Icons::logo(dl_, font_, lx, r.y + 48 * s, ls, theme::accent);
        const float ty = r.y + 48 * s + static_cast<float>(ls) + 20 * s;
        dl_.titleText(font_, lx, ty, "Daemon", theme::text);
        const float sub = ty + font_.titleLineHeight() + 2 * s;
        uiLabel(lx, sub, uiH() + 6 * s, "C++ IDE for Wayland", theme::dim);
        const float rowH = std::round(44 * s);
        float by = sub + uiH() + 44 * s;
        welcomeAction({lx, by, cw0, rowH}, "New Project", "Ctrl+Shift+N", kDlgNew, 0xF0EAB, true);
        by += rowH + 10 * s;
        welcomeAction({lx, by, cw0, rowH}, "Open Folder", "Ctrl+Shift+O", kDlgOpen, 0xF0770, false);

        // Recent projects.
        const float rx = r.x + sideW + 40 * s, rw = r.right() - rx - 30 * s;
        std::vector<std::string> recent;
        std::error_code ec;
        for (const auto& p : state_.recent) if (fs::is_directory(p, ec)) recent.push_back(p);
        dlgRecentShown_ = recent;
        uiLabel(rx, r.y + 36 * s, uiH() + 8 * s, "RECENT PROJECTS", theme::dim);
        if (!recent.empty()) uiLabelR(rx + rw, r.y + 36 * s, uiH() + 8 * s, std::to_string(recent.size()), theme::lineNum);
        const float top = r.y + 80 * s, rowH2 = std::round(54 * s), listH = r.bottom() - top - 24 * s;
        if (recent.empty()) {
            const float cy = r.y + r.h / 2 - 20 * s, cx = rx + rw / 2;
            const int ip = static_cast<int>(std::round(40 * s));
            Icons::glyph(dl_, font_, cx - static_cast<float>(ip) / 2, cy - static_cast<float>(ip), ip, 0xF0DCF, Color::rgb(0xffffff, 50));
            uiLabelC(cx, cy + 8 * s, uiH() + 10 * s, "No recent projects", theme::dim);
            uiLabelC(cx, cy + 8 * s + uiH() + 10 * s, uiH() + 10 * s, "Create a project or open a folder to get started", theme::lineNum);
            return;
        }
        dlgRecentMax_ = static_cast<float>(recent.size()) * rowH2 - listH;
        dlg_.recentScroll = std::clamp(dlg_.recentScroll, 0.0f, std::max(0.0f, dlgRecentMax_));
        const std::string home = homeDir();
        for (std::size_t i = 0; i < recent.size() && i < 99; ++i) {
            const float y = top + static_cast<float>(i) * rowH2 - dlg_.recentScroll;
            if (y + rowH2 < top || y > top + listH) continue;
            const Rect row{rx - 12 * s, y, rw + 24 * s, rowH2 - 4 * s};
            const bool hot = dlgHot(kDlgRecent + static_cast<int>(i)) || dlgHot(kDlgRecentDel + static_cast<int>(i));
            const bool inside = y >= top - 1 && y + rowH2 <= top + listH + 1;   // partly visible rows get no hit area
            if (inside) dlgHit(row, kDlgRecent + static_cast<int>(i));
            if (hot) fill(row, theme::sideSel, 8 * s);
            const fs::path p(recent[i]);
            const int ip = static_cast<int>(std::round(iconSize() * 1.4f));
            Icons::folder(dl_, font_, rx, y + (rowH2 - 4 * s - static_cast<float>(ip)) / 2, ip, p.filename().string(), false);
            const float tx = rx + static_cast<float>(ip) + 14 * s;
            std::string where = p.parent_path().string();
            if (!home.empty() && where.starts_with(home)) where = "~" + where.substr(home.size());
            uiLabel(tx, y + 6 * s, uiH() + 8 * s, p.filename().string(), theme::text, rx + rw - 40 * s);
            uiLabel(tx, y + 6 * s + uiH() + 6 * s, uiH() + 6 * s, where, theme::dim, rx + rw - 40 * s);
            if (hot && inside) {
                const float bs = std::round(24 * s);
                const Rect del{rx + rw - bs, y + (rowH2 - 4 * s - bs) / 2, bs, bs};
                dlgHit(del, kDlgRecentDel + static_cast<int>(i));
                if (dlgHot(kDlgRecentDel + static_cast<int>(i))) fill(del, Color::rgb(0xffffff, 40), 6 * s);
                const int cs = std::max(10, static_cast<int>(std::round(uiH() * 0.8f)));
                Icons::tool(dl_, font_, del.x + (bs - static_cast<float>(cs)) / 2, del.y + (bs - static_cast<float>(cs)) / 2, cs, ToolIcon::Close, theme::dim);
            }
        }
    }

    void dlgCheckbox(const Rect& r, bool on, std::string_view label, int id) {
        const float s = S();
        dlgHit(r, id);
        const float bs = std::round(20 * s);
        const Rect box{r.x, r.y + (r.h - bs) / 2, bs, bs};
        fill(box, on ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 70 : 44), 6 * s);
        if (!on) fill(box.x + 1, box.y + 1, bs - 2, bs - 2, Color::rgb(0x07080a), 5.5f * s);
        else {
            const int ip = std::max(12, static_cast<int>(std::round(bs * 0.85f)));
            Icons::glyph(dl_, font_, box.x + (bs - static_cast<float>(ip)) / 2, box.y + (bs - static_cast<float>(ip)) / 2, ip, 0xF012C, Color::rgb(0xffffff));
        }
        uiLabel(box.right() + 12 * s, r.y, r.h, label, theme::text);
    }

    void drawNewProject(const Rect& r) {
        const float s = S(), sideW = std::round(290 * s);
        dlgSide(r, sideW);
        const float lx = r.x + 32 * s;
        dl_.titleText(font_, lx, r.y + 30 * s, "New Project", theme::text);
        const auto& ts = projectTemplates();
        float y = r.y + 30 * s + font_.titleLineHeight() + 26 * s;
        const float rowH = std::round(48 * s);
        static constexpr std::array<char32_t, 5> icons = {0xF0672, 0xF0672, 0xF03D6, 0xF0671, 0xF0224};
        for (std::size_t i = 0; i < ts.size(); ++i) {
            const Rect row{r.x + 14 * s, y, sideW - 28 * s, rowH - 4 * s};
            dlgHit(row, kDlgTmpl + static_cast<int>(i));
            const bool sel = static_cast<int>(i) == dlg_.tmpl, hot = dlgHot(kDlgTmpl + static_cast<int>(i));
            if (sel) fill(row, theme::sideSel, 8 * s);
            else if (hot) fill(row, theme::sideHover, 8 * s);
            const int ip = static_cast<int>(std::round(iconSize() * 1.25f));
            Icons::glyph(dl_, font_, row.x + 12 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, i < icons.size() ? icons[i] : 0xF0224, sel ? theme::accent.mix(Color::rgb(0xffffff), 0.3f) : theme::dim);
            uiLabel(row.x + 12 * s + static_cast<float>(ip) + 12 * s, row.y, row.h, ts[i].name, sel ? theme::text : Color::rgb(0xb9bdc6), row.right() - 8 * s);
            y += rowH;
        }

        const float fx = r.x + sideW + 40 * s, fw = r.right() - fx - 40 * s;
        const auto& t = ts[static_cast<std::size_t>(dlg_.tmpl)];
        float fy = r.y + 36 * s;
        uiLabel(fx, fy, uiH() + 8 * s, t.name, theme::text);
        fy += uiH() + 10 * s;
        uiLabel(fx, fy, uiH() + 6 * s, t.description, theme::dim, fx + fw);
        fy += uiH() + 34 * s;

        const float fh = std::round(38 * s);
        uiLabel(fx, fy, uiH() + 4 * s, "Name", theme::dim);
        fy += uiH() + 8 * s;
        dlgField({fx, fy, fw, fh}, dlg_.name, dlg_.focus == 0, kDlgName, "project-name");
        fy += fh + 22 * s;
        uiLabel(fx, fy, uiH() + 4 * s, "Location", theme::dim);
        fy += uiH() + 8 * s;
        const float browseW = std::round(110 * s);
        dlgField({fx, fy, fw - browseW - 10 * s, fh}, dlg_.location, dlg_.focus == 1, kDlgLoc, "/path/to/projects");
        dlgButton({fx + fw - browseW, fy, browseW, fh}, "Browse...", kDlgBrowseBtn, false);
        fy += fh + 22 * s;
        uiLabel(fx, fy, uiH() + 4 * s, "Language standard", theme::dim);
        fy += uiH() + 8 * s;
        const std::vector<std::string> opts = t.isC ? std::vector<std::string>{"C11", "C17"}
                                                    : (t.needsModules ? std::vector<std::string>{"C++20", "C++23"} : std::vector<std::string>{"C++17", "C++20", "C++23"});
        // Modules need at least C++20: indices shift for that template.
        const int shift = (!t.isC && t.needsModules) ? 1 : 0;
        float sx = fx;
        for (std::size_t i = 0; i < opts.size(); ++i) {
            const float w = uiW(opts[i]) + 34 * s;
            const int id = kDlgStd + static_cast<int>(i) + shift;
            const Rect b{sx, fy, w, std::round(34 * s)};
            dlgHit(b, id);
            const bool on = dlg_.std_ == static_cast<int>(i) + shift;
            fill(b, on ? theme::accent : Color::rgb(0xffffff, dlgHot(id) ? 56 : 34), 8 * s);
            if (!on) fill(b.x + 1, b.y + 1, b.w - 2, b.h - 2, Color::rgb(0x111317), 7.5f * s);
            uiLabelC(b.x + b.w / 2, b.y, b.h, opts[i], on ? Color::rgb(0xffffff) : theme::text);
            sx += w + 8 * s;
        }
        fy += std::round(34 * s) + 26 * s;
        dlgCheckbox({fx, fy, fw, std::round(28 * s)}, dlg_.git, "Create a git repository", kDlgGit);
        fy += std::round(28 * s) + 20 * s;
        const std::string where = (fs::path(expandPath(dlg_.location.text)) / dlg_.name.text).string();
        uiLabel(fx, fy, uiH() + 6 * s, "Will be created in  " + where, theme::lineNum, fx + fw);
        const std::string verr = validateProjectName(dlg_.name.text);
        const std::string& err = dlg_.error.empty() ? verr : dlg_.error;
        if (!err.empty() && (!dlg_.error.empty() || !dlg_.name.text.empty()))
            uiLabel(fx, fy + uiH() + 10 * s, uiH() + 6 * s, err, Color::rgb(0xf2777a), fx + fw);

        const float bh = std::round(40 * s), bw = std::round(120 * s);
        dlgButton({r.right() - 40 * s - bw, r.bottom() - 30 * s - bh, bw, bh}, "Create", kDlgCreate, true);
        dlgButton({r.right() - 40 * s - 2 * bw - 12 * s, r.bottom() - 30 * s - bh, bw, bh}, "Back", kDlgCancel, false);
    }

    void drawBrowse(const Rect& r) {
        const float s = S(), sideW = std::round(210 * s);
        dlgSide(r, sideW);
        const float lx = r.x + 28 * s;
        dl_.titleText(font_, lx, r.y + 30 * s, dlg_.forLocation ? "Choose Location" : "Open Project", theme::text);
        // Places.
        dlgPlaces_.clear();
        std::vector<std::pair<std::string, std::string>> places;
        const fs::path home = homeDir();
        std::error_code ec;
        places.emplace_back("Home", home.string());
        for (const char* n : {"Projects", "CLionProjects", "Documents", "Desktop", "Downloads"})
            if (fs::is_directory(home / n, ec)) places.emplace_back(n, (home / n).string());
        places.emplace_back("Computer", "/");
        float y = r.y + 30 * s + font_.titleLineHeight() + 24 * s;
        for (std::size_t i = 0; i < places.size(); ++i) {
            const Rect row{r.x + 12 * s, y, sideW - 24 * s, std::round(34 * s)};
            const int id = kDlgBm + static_cast<int>(i);
            dlgHit(row, id);
            dlgPlaces_.push_back(places[i].second);
            const bool here = dlg_.dir == places[i].second;
            if (here) fill(row, theme::accent.alpha(60), 8 * s);
            else if (dlgHot(id)) fill(row, Color::rgb(0xffffff, 16), 8 * s);
            const int ip = std::max(14, static_cast<int>(std::round(iconSize() * 1.05f)));
            Icons::glyph(dl_, font_, row.x + 10 * s, row.y + (row.h - static_cast<float>(ip)) / 2, ip, i == 0 ? 0xF02DC : 0xF0256, theme::dim);
            uiLabel(row.x + 10 * s + static_cast<float>(ip) + 10 * s, row.y, row.h, places[i].first, theme::text, row.right() - 6 * s);
            y += row.h + 2 * s;
        }

        const float cx = r.x + sideW + 28 * s, cw_ = r.right() - cx - 32 * s;
        const float fh = std::round(38 * s);
        float fy = r.y + 30 * s;
        const float upW = std::round(44 * s), hidW = std::round(150 * s);
        dlgButton({cx, fy, upW, fh}, "", kDlgUp, false);
        {
            const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.15f)));
            Icons::glyph(dl_, font_, cx + (upW - static_cast<float>(ip)) / 2, fy + (fh - static_cast<float>(ip)) / 2, ip, 0xF005D, theme::text);
        }
        dlgField({cx + upW + 10 * s, fy, cw_ - upW - hidW - 20 * s, fh}, dlg_.path, dlg_.pathFocus, kDlgPath, "/path/to/folder");
        dlgCheckbox({r.right() - 32 * s - hidW + 6 * s, fy, hidW, fh}, dlg_.showHidden, "Hidden", kDlgHidden);
        fy += fh + 18 * s;
        const float listTop = fy, listBottom = r.bottom() - 30 * s - std::round(40 * s) - 20 * s;
        browseListH_ = listBottom - listTop;
        fill(cx, listTop, cw_, browseListH_, Color::rgb(0x07080a), 10 * s);
        const float rowH = std::round(34 * s);
        dlg_.scroll = std::clamp(dlg_.scroll, 0.0f, std::max(0.0f, static_cast<float>(dlg_.entries.size()) * rowH - browseListH_));
        if (dlg_.entries.empty()) uiLabel(cx + 16 * s, listTop + 10 * s, rowH, "No sub-folders here", theme::lineNum);
        for (std::size_t i = 0; i < dlg_.entries.size() && i < 399; ++i) {
            const float ry = listTop + 4 * s + static_cast<float>(i) * rowH - dlg_.scroll;
            if (ry + rowH < listTop || ry > listTop + browseListH_) continue;
            const bool full = ry >= listTop && ry + rowH <= listTop + browseListH_;
            const Rect row{cx + 6 * s, ry, cw_ - 12 * s, rowH - 2 * s};
            const int id = kDlgEntry + static_cast<int>(i);
            if (full) dlgHit(row, id);
            const bool sel = dlg_.sel == static_cast<int>(i);
            if (sel) fill(row, theme::accent.alpha(80), 7 * s);
            else if (dlgHot(id)) fill(row, Color::rgb(0xffffff, 16), 7 * s);
            const BrowseEntry& e = dlg_.entries[i];
            drawFolderIcon(row.x + 10 * s, row.y + (row.h - iconSize()) / 2, e.name, false);
            const float nx = uiLabel(row.x + 10 * s + iconSize() + 10 * s, row.y, row.h, e.name, theme::text, row.right() - 90 * s);
            if (e.cmake) {
                const float w = uiW("CMake") + 16 * s;
                const Rect badge{std::min(nx + 12 * s, row.right() - w - 8 * s), row.y + 5 * s, w, row.h - 10 * s};
                fill(badge, theme::accent.alpha(50), 6 * s);
                uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, "CMake", theme::text);
            }
        }
        const float bh = std::round(40 * s), bw = std::round(120 * s);
        const std::string target = dlg_.sel >= 0 && dlg_.sel < static_cast<int>(dlg_.entries.size())
                                       ? (fs::path(dlg_.dir) / dlg_.entries[static_cast<std::size_t>(dlg_.sel)].name).string() : dlg_.dir;
        uiLabel(cx, r.bottom() - 30 * s - bh, bh, target, theme::dim, r.right() - 40 * s - 2 * bw - 40 * s);
        dlgButton({r.right() - 32 * s - bw, r.bottom() - 30 * s - bh, bw, bh}, dlg_.forLocation ? "Select" : "Open", kDlgOk, true);
        dlgButton({r.right() - 32 * s - 2 * bw - 12 * s, r.bottom() - 30 * s - bh, bw, bh}, "Cancel", kDlgBrowseCancel, false);
    }

    Rect dlgPanel_;
    std::vector<std::string> dlgRecentShown_, dlgPlaces_;
    float dlgRecentMax_ = 0, browseListH_ = 200;

    // ============================================================ drawing

    // Floating overlay (menu, popup, tooltip): soft shadow, hairline rim, and a frosted-glass body that blurs the
    // scene behind it (see shaders/glass.frag).
    void glassPanel(const Rect& r, float radius) {
        const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
        fill(r.x - 4 * s, r.y + 3 * s, r.w + 8 * s, r.h + 10 * s, Color{0, 0, 0, 26}, radius + 8 * s);
        fill(r.x - 1.5f * s, r.y + 1.5f * s, r.w + 3 * s, r.h + 5 * s, Color{0, 0, 0, 40}, radius + 3 * s);
        fill(r, Color::rgb(0xffffff, 38), radius);
        dl_.glass(true);
        fill(r.x + hair, r.y + hair, r.w - 2 * hair, r.h - 2 * hair, Color::rgb(0x0b0c0f, 205), radius - hair);
        dl_.glass(false);
    }

    // Translucent chrome colour (opaque when the compositor path has no alpha), and carving it out of the panels.
    Color frameC() { Color c = theme::frame; if (!ren_.translucent()) c.a = 255; return c; }
    void carve(const Rect& r, float radius = 0) {
        Color f = frameC();
        const float a = static_cast<float>(f.a) / 255.0f;
        Color pre{static_cast<std::uint8_t>(f.r * a + 0.5f), static_cast<std::uint8_t>(f.g * a + 0.5f), static_cast<std::uint8_t>(f.b * a + 0.5f), 255};
        dl_.erase(true);
        dl_.rect(font_, r.x, r.y, r.w, r.h, pre, radius);
        dl_.erase(false);
    }
    void fill(float x, float y, float w, float h, Color c, float radius = 0) { dl_.rect(font_, x, y, w, h, c, radius); }
    void fill(const Rect& r, Color c, float radius = 0) { dl_.rect(font_, r.x, r.y, r.w, r.h, c, radius); }
    float label(float x, float y, std::string_view t, Color c, float maxX = std::numeric_limits<float>::infinity()) {
        return dl_.text(font_, x, y, t, c, maxX);
    }

    void draw() {
        layout();
        dl_.clear();
        const float W = L_.status.w, H = L_.status.bottom();
        const Rect& isl = L_.island;
        blameRect_ = {};
        for (Node* n : leaves()) {
            Pane& p = *n->pane;
            fill(p.rect, theme::editorBg);
            if (!p.cur) continue;
            const PaneCtx sv = enterPane(p);
            drawActive_ = &p == activePane_;
            drawEditor(*p.cur);
            leavePane(p, sv);
        }
        drawActive_ = true;
        // Content may overflow the panes (partial lines, horizontal scroll): repaint the frame around them.
        carve({0, isl.bottom(), W, H - isl.bottom()});
        carve({0, isl.y, isl.x, isl.h});
        carve({isl.right(), isl.y, W - isl.right(), isl.h});
        for (Node* n : splits()) carve(n->gap);
        if (L_.side.w > 1) drawSidebar();
        for (Node* n : leaves()) {
            Pane& p = *n->pane;
            const PaneCtx sv = enterPane(p);
            drawTabs(p);
            if (p.cur) drawCrumbs();
            leavePane(p, sv);
        }
        drawPanel();
        if (prompt_.kind != PromptKind::None) { if (sidePrompt()) drawSidePrompt(); else drawPrompt(); }
        if (L_.panel.h > 0.5f) {
            const Rect& pn = L_.panel;
            carve({pn.x, pn.bottom(), pn.w, H - pn.bottom()});  // rows that overflow the bottom edge
            carve(pn, -L_.radius);
        }
        for (Node* n : leaves()) carve(n->pane->rect, -L_.radius);
        if (L_.side.w > 1) {
            if (sideAnim_ > 0.002f && sideAnim_ < 0.998f) carve({L_.activity.x, L_.side.y, W - L_.activity.x, L_.side.h});  // clip content sliding past the edge
            carve(L_.side, -L_.radius);
        }
        drawActivityBar();   // after the clip above, which would otherwise erase the buttons while the explorer slides  // inverted rounded rect = clips the four corners
        // Thin outline around every island so it reads as a separate surface against the window background.
        {
            const float t = std::max(1.0f, std::round(S() * 0.8f));
            const Color oc = Color::rgb(0xffffff, 34);
            auto outline = [&](const Rect& r) { if (r.w > 8 && r.h > 8) dl_.ring(font_, r.x, r.y, r.w, r.h, oc, L_.radius, t); };
            for (Node* n : leaves()) outline(n->pane->rect);
            if (L_.side.w > 1) outline(L_.side);
            if (L_.panel.h > 0.5f) outline(L_.panel);
        }
        if (L_.csd) drawTitleBar();
        drawToolbar();
        drawStatus();
        drawCompletion();
        drawHover();
        drawMenu();
        drawDebugBar();
        drawTabDrag();
        drawFileDrag();
        drawToolTip();
        drawDialog();
        if (L_.csd && !win_.maximized() && !win_.fullscreen()) {  // hairline window outline
            const float s = std::max(1.0f, std::round(S() * 0.5f));
            fill(0, 0, W, s, theme::border); fill(0, H - s, W, s, theme::border);
            fill(0, 0, s, H, theme::border); fill(W - s, 0, s, H, theme::border);
        }
    }

    void drawTitleBar() {
        const Rect& t = L_.title;
        const float s = S();
        carve(t);
        const int ls = std::max(14, static_cast<int>(std::round(t.h * 0.5f)));
        Icons::logo(dl_, font_, std::round(14 * s), std::round(t.y + (t.h - static_cast<float>(ls)) / 2), ls, theme::accent);
        std::string title = docs_.empty() ? "Daemon" : doc().name + (doc().modified() ? " \xE2\x80\xA2" : "");
        if (explorer_.isOpen()) title += "  \xE2\x80\x94  " + explorer_.rootName();
        const float bw = std::round(t.h * 1.25f);
        const float maxX = t.right() - 3 * bw - 12 * s;
        const float tw = std::min(uiW(title), maxX - 60 * s);
        if (!cmakeOpen_) uiLabel(std::max(std::round(48 * s), (t.w - tw) / 2), t.y, t.h, title, theme::dim, maxX);

        const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.95f)));
        for (int b = 1; b <= 3; ++b) {
            Rect r{t.right() - bw * static_cast<float>(4 - b), t.y, bw, t.h};
            const bool hot = hoverBtn_ == b;
            if (hot) fill(r.x + 3 * s, r.y + 4 * s, r.w - 6 * s, r.h - 8 * s, b == 3 ? theme::danger : theme::tabHover.mix(Color::rgb(0xffffff), 0.05f), 7 * s);
            const ToolIcon ic = b == 1 ? ToolIcon::Minimize : (b == 2 ? ToolIcon::Maximize : ToolIcon::Close);
            Icons::tool(dl_, font_, std::round(r.x + (r.w - static_cast<float>(ip)) / 2), std::round(r.y + (r.h - static_cast<float>(ip)) / 2), ip, ic,
                        hot && b == 3 ? Color::rgb(0xffffff) : theme::text);
        }
    }

    int iconPx() const { return std::max(12, static_cast<int>(std::round(font_.uiLineHeight() * 1.02f))); }
    float iconSize() const { return static_cast<float>(iconPx()); }

    void drawFileIcon(float x, float y, std::string_view name, float alpha = 1.0f) {
        Icons::file(dl_, font_, x, y, iconPx(), name, alpha);
    }
    void drawFolderIcon(float x, float y, std::string_view name, bool open, float alpha = 1.0f) {
        Icons::folder(dl_, font_, x, y, iconPx(), name, open, alpha);
    }

    void drawSidebar() {
        const Rect& sd = L_.side;
        const float s = S(), rh = L_.sideRow;
        fill(sd, theme::sideBg);
        const float headH = L_.tabs.h;

        if (sideDebug_) {
            drawDebugBody();
            fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
            uiLabel(sd.x + 16 * s, sd.y, headH, "RUN AND DEBUG", theme::dim, sd.right() - 12 * s);
            return;
        }
        if (sideSearch_) {
            drawSearchBody();
            fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
            uiLabel(sd.x + 16 * s, sd.y, headH, "SEARCH", theme::dim, sd.right() - 12 * s);
            if (explorer_.isOpen()) uiLabelR(sd.right() - 16 * s, sd.y, headH, explorer_.rootName(), theme::lineNum);
            return;
        }
        if (sideGit_) {   // Source Control instead of the file tree
            drawGitBody(L_.sideList);
            fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
            uiLabel(sd.x + 16 * s, sd.y, headH, "SOURCE CONTROL", theme::dim, sd.right() - 12 * s);
            if (!gitBranch_.empty()) {
                const std::string b = gitBranch_;
                uiLabelR(sd.right() - 16 * s, sd.y, headH, b, theme::lineNum);
            }
            return;
        }

        if (!explorer_.isOpen()) {
            const float y = sd.y + headH + 22 * s, lhh = std::round(uiH() + 8 * s);
            uiLabel(sd.x + 20 * s, y, lhh, "No folder open", theme::dim, sd.right() - 8 * s);
            uiLabel(sd.x + 20 * s, y + lhh, lhh, "Open one with Ctrl+Shift+O", theme::accent, sd.right() - 8 * s);
        } else {
            const auto& rows = explorer_.rows();
            const float sy = std::round(sideScroll_), indent = std::round(16 * s);
            const int first = std::max(0, static_cast<int>(std::floor(sy / rh)));
            const int last = std::min(static_cast<int>(rows.size()) - 1, static_cast<int>(std::floor((sy + L_.sideList.h) / rh)));
            const std::string activePath = docs_.empty() ? std::string() : doc().path;
            for (int i = first; i <= last; ++i) {
                const auto& row = rows[static_cast<std::size_t>(i)];
                const FileNode& nd = *row.node;
                const float y = L_.sideList.y + static_cast<float>(i) * rh - sy;
                const Rect rr{sd.x + 6 * s, y + 1, sd.w - 12 * s, rh - 2};
                const bool selected = nd.path.string() == explorer_.selected();
                const bool active = !activePath.empty() && nd.path.string() == activePath;
                if (selected && sideFocus_) fill(rr, theme::selection, 6 * s);
                else if (selected) fill(rr, theme::sideSel, 6 * s);
                else if (active) fill(rr, theme::accent.alpha(40), 6 * s);
                else if (i == hoverRow_ && !fdrag_.active) fill(rr, theme::sideHover, 6 * s);
                if (fdrag_.active && !fdrag_.target.empty() && nd.dir && nd.path == fdrag_.target) {
                    fill(rr, theme::accent.alpha(46), 6 * s);
                    const float t = std::max(1.0f, std::round(1.5f * s));
                    fill(rr.x, rr.y, rr.w, t, theme::accent.alpha(170)); fill(rr.x, rr.bottom() - t, rr.w, t, theme::accent.alpha(170));
                    fill(rr.x, rr.y, t, rr.h, theme::accent.alpha(170)); fill(rr.right() - t, rr.y, t, rr.h, theme::accent.alpha(170));
                }
                const float x = sd.x + 12 * s + static_cast<float>(row.depth) * indent;
                for (int lvl = 0; lvl < row.depth; ++lvl)
                    fill(std::round(sd.x + 12 * s + static_cast<float>(lvl) * indent + indent * 0.5f), y, std::max(1.0f, std::round(s * 0.5f)), rh, theme::guide);
                Color nameColor = nd.name.starts_with('.') ? theme::dim : (nd.dir ? theme::text : Color::rgb(0xb9bdc6));
                if (active) nameColor = Color::rgb(0xffffff).mix(theme::accent, 0.25f);
                float nameMax = sd.right() - 10 * s;
                float rightEdge = sd.right() - 22 * s;
                const std::string pathStr = nd.path.lexically_normal().string();
                Color gitColor{};
                bool hasGit = false;
                if (auto gf = gitFiles_.find(pathStr); gf != gitFiles_.end()) {   // git status letter (files, untracked folders)
                    const git::Entry& ge = gf->second;
                    const char ch = ge.x == '?' ? 'U' : (ge.conflicted() ? '!' : (ge.y != ' ' ? ge.y : ge.x));
                    gitColor = (ch == 'U' || ch == 'A') ? Color::rgb(0x73c990) : ch == 'D' || ch == '!' ? Color::rgb(0xe06c75) : ch == 'R' ? Color::rgb(0x61afef) : Color::rgb(0xe5c07b);
                    const std::string letter(1, ch);
                    const float lw = uiW(letter);
                    uiLabel(rightEdge - lw, y, rh, letter, gitColor);
                    rightEdge -= lw + 10 * s;
                    hasGit = true;
                } else if (nd.dir && gitDirs_.contains(pathStr)) {   // folder containing changes: a dot
                    const float dot = std::round(6 * s);
                    fill(rightEdge - dot, y + (rh - dot) / 2, dot, dot, Color::rgb(0xe5c07b), dot / 2);
                    rightEdge -= dot + 10 * s;
                }
                if (hasGit && !active) nameColor = gitColor;
                nameMax = std::min(nameMax, rightEdge - 6 * s);
                if (auto pc = problemCounts_.find(nd.path.string()); pc != problemCounts_.end()) {   // problems in this file / folder
                    const auto [errs, warns] = pc->second;
                    const Color pcol = errs > 0 ? Color::rgb(0xf2777a) : Color::rgb(0xe5c07b);
                    if (!nd.dir) nameColor = pcol;
                    const int cnt = errs + warns;
                    const std::string txt = cnt > 99 ? "99+" : std::to_string(cnt);
                    const float bw = std::max(uiW(txt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                    const Rect badge{rightEdge - bw, y + (rh - bh) / 2, bw, bh};
                    fill(badge, pcol.alpha(46), bh / 2);
                    uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, txt, pcol);
                    nameMax = badge.x - 6 * s;
                }
                const bool cutPending = fileClip_.cut && std::ranges::find(fileClip_.paths, nd.path) != fileClip_.paths.end();
                if (cutPending) nameColor = nameColor.alpha(110);
                const float fade = cutPending ? 0.45f : (nd.name.starts_with('.') ? 0.7f : 1.0f);
                if (nd.dir) {
                    const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                    Icons::chevron(dl_, font_, x + (indent - static_cast<float>(cs)) / 2, y + (rh - static_cast<float>(cs)) / 2, cs, nd.expanded,
                                   selected && sideFocus_ ? Color::rgb(0xffffff) : theme::dim);
                }
                const float ix = x + indent, iy = y + (rh - iconSize()) / 2;
                if (nd.dir) drawFolderIcon(ix, iy, nd.name, nd.expanded, fade);
                else drawFileIcon(ix, iy, nd.name, fade);
                uiLabel(ix + iconSize() + 8 * s, y, rh, nd.name, nameColor, nameMax);
            }
            const float contentH = static_cast<float>(rows.size()) * rh, listH = L_.sideList.h;
            if (contentH > listH) {
                const float maxS = std::max(1.0f, contentH - listH * 0.5f);
                const float thumbH = std::max(24 * s, listH * listH / (contentH + listH * 0.5f));
                const float thumbY = L_.sideList.y + (listH - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
                fill(sd.right() - 9 * s, thumbY, 5 * s, thumbH, theme::scrollbar, 2.5f * s);
            }
        }

        // Header (opaque, so scrolled rows disappear beneath it).
        fill(sd.x, sd.y, sd.w, headH, theme::sideBg);
        std::string title = explorer_.isOpen() ? explorer_.rootName() : "Explorer";
        for (auto& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const float hx = sd.x + 16 * s;
        uiLabel(hx, sd.y, headH, title, theme::dim, sd.right() - 12 * s);
    }

    void drawTabs(Pane& p) {
        const float s = S();
        fill(p.tabsR, theme::stripBg);
        const bool focusedPane = &p == activePane_;
        const int xs = std::max(8, static_cast<int>(std::round(uiH() * 0.62f)));
        auto fade = [](Color c, float a) { return c.alpha(static_cast<std::uint8_t>(static_cast<float>(c.a) * a)); };
        for (const auto& e : p.draw) {
            const Rect& t = e.tab;
            if (t.w < 2) continue;
            const bool live = e.index >= 0;
            const bool dragged = tabDrag_.active && e.doc == tabDrag_.doc;
            const float al = e.alpha * (dragged ? 0.35f : 1.0f);
            const bool active = live && e.doc == p.cur;
            const bool hover = live && hoverPane_ == &p && e.index == hoverTab_;
            if (active) {
                fill(t, fade(theme::editorBg, al), 9 * s);
                fill(t.x, t.y + t.h - 12 * s, t.w, 12 * s, fade(theme::editorBg, al));  // square off the bottom: merges with the editor
                fill(t.x + 12 * s, t.y, std::max(0.0f, t.w - 24 * s), std::round(2 * s), fade(focusedPane ? theme::accent : theme::accent.alpha(90), al), std::round(s));
            } else if (hover) {
                fill(t.x + 1 * s, t.y + 3 * s, t.w - 2 * s, t.h - 3 * s, fade(Color::rgb(0x15171b), al), 8 * s);
            }
            const float ix = t.x + 14 * s;
            drawFileIcon(ix, t.y + (t.h - iconSize()) / 2 + s, e.name, (active ? 1.0f : 0.7f) * al);
            const Color tc = fade(active && focusedPane ? theme::text : (active ? Color::rgb(0xb4b8c1) : theme::dim), al);
            uiLabel(ix + iconSize() + 8 * s, t.y + s, t.h, e.name, tc, e.close.x - 2 * s);
            if (!live) continue;
            const bool overClose = hoverPane_ == &p && hoverClose_ == e.index;
            if (e.doc->modified() && !overClose) {
                const float dot = std::round(7 * s);
                fill(e.close.x + (e.close.w - dot) / 2, t.y + (t.h - dot) / 2 + s, dot, dot, fade(theme::accent, al), dot / 2);
            } else if (active || hover) {
                const float bs = std::round(18 * s);
                if (overClose) fill(e.close.x + (e.close.w - bs) / 2, t.y + (t.h - bs) / 2 + s, bs, bs, Color::rgb(0xffffff, 28), 5 * s);
                Icons::tool(dl_, font_, std::round(e.close.x + (e.close.w - static_cast<float>(xs)) / 2), std::round(t.y + (t.h - static_cast<float>(xs)) / 2 + s), xs,
                            ToolIcon::Close, overClose ? Color::rgb(0xffffff) : theme::dim);
            }
        }
    }

    // Breadcrumbs: project-relative path of the active file.
    // Path segments shown in the breadcrumb row (project-relative when a folder is open).
    std::vector<std::string> crumbParts(const Document& d) const {
        fs::path rel = d.path;
        if (explorer_.isOpen()) {
            auto r = fs::path(d.path).lexically_relative(explorer_.rootPath());
            if (!r.empty() && *r.begin() != "..") rel = r;
        }
        std::vector<std::string> parts;
        for (const auto& p : rel) if (p != "/" && !p.empty()) parts.push_back(p.string());
        if (parts.size() > 4) parts.erase(parts.begin(), parts.end() - 4);
        return parts;
    }

    void drawCrumbs() {
        if (L_.crumbs.h <= 0 || docs_.empty()) return;
        const Document& d = doc();
        const Rect& c = L_.crumbs;
        const float s = S();
        fill(c, theme::editorBg);
        const std::vector<std::string> parts = crumbParts(d);
        float x = c.x + 16 * s;
        const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            if (last) { drawFileIcon(x, c.y + (c.h - iconSize()) / 2, parts[i], 1.0f); x += iconSize() + 6 * s; }
            x = uiLabel(x, c.y, c.h, parts[i], last ? theme::text : theme::dim, c.right() - 12 * s);
            if (!last) {
                Icons::chevron(dl_, font_, x + 5 * s, c.y + (c.h - static_cast<float>(cs)) / 2, cs, false, theme::lineNum);
                x += static_cast<float>(cs) + 10 * s;
            }
        }
    }

    void drawEditor(Document& d) {
        const Rect& e = L_.editor;
        if (e.h <= 0) return;
        const float line = lh(), cell = cw(), s = S();
        const float sy = std::round(d.scrollY), sx = std::round(d.scrollX);
        const float baseX = L_.textX - sx;
        const int total = d.buf.lineCount();
        const int first = std::max(0, static_cast<int>(std::floor(sy / line)));
        const int last = std::min(total - 1, static_cast<int>(std::floor((sy + e.h) / line)));
        const Pos selA = d.selBegin(), selB = d.selEnd();
        const bool hasSel = d.hasSelection();
        const bool showMatches = inFindMode() && !findText_.empty();
        std::optional<std::pair<Pos, Pos>> bracket;
        if (!hasSel) bracket = d.matchBracket();

        // Background layers: current line, selection, search matches.
        for (int ln = first; ln <= last; ++ln) {
            const float y = e.y + static_cast<float>(ln) * line - sy;
            const std::string& str = d.buf.line(ln);
            if (!hasSel && ln == d.cursor.line) fill(e.x, y, e.w, line, theme::lineHl);
            if (dbgLine_.line > 0 && dbgLine_.line - 1 == ln && dbgLine_.file == d.path) fill(e.x, y, e.w, line, Color::rgb(0xe5c07b, 38));
            if (showMatches)
                for (int c : findAll(str, findText_, matchCase_)) {
                    float x0 = baseX + static_cast<float>(displayCol(str, c, d.tabWidth)) * cell;
                    float x1 = baseX + static_cast<float>(displayCol(str, c + static_cast<int>(findText_.size()), d.tabWidth)) * cell;
                    fill(x0, y + 1, x1 - x0, line - 2, theme::match, 3 * s);
                }
            if (hasSel && ln >= selA.line && ln <= selB.line) {
                int c0 = ln == selA.line ? selA.col : 0;
                int c1 = ln == selB.line ? selB.col : static_cast<int>(str.size());
                float x0 = baseX + static_cast<float>(displayCol(str, c0, d.tabWidth)) * cell;
                float x1 = baseX + static_cast<float>(displayCol(str, c1, d.tabWidth)) * cell;
                if (ln < selB.line) x1 += cell * 0.6f;
                fill(x0, y, x1 - x0, line, focused_ && drawActive_ ? theme::selection : theme::selectionIdle);
            }
        }
        if (bracket) {
            for (Pos p : {bracket->first, bracket->second}) {
                if (p.line < first || p.line > last) continue;
                const std::string& str = d.buf.line(p.line);
                float x = baseX + static_cast<float>(displayCol(str, p.col, d.tabWidth)) * cell;
                float y = e.y + static_cast<float>(p.line) * line - sy, t = std::max(1.0f, std::round(s));
                fill(x, y, cell, t, theme::bracket); fill(x, y + line - t, cell, t, theme::bracket);
                fill(x, y, t, line, theme::bracket); fill(x + cell - t, y, t, line, theme::bracket);
            }
        }

        // Text.
        for (int ln = first; ln <= last; ++ln) {
            const float y = e.y + static_cast<float>(ln) * line - sy;
            const std::string& str = d.buf.line(ln);
            d.hl.highlight(d.buf, ln, toks_);
            const float baseline = y + font_.baseline();
            float x = baseX;
            int dcol = 0;
            const float right = e.right();
            // Indent guides for leading whitespace.
            if (!str.empty()) {
                std::size_t ws = 0;
                int cols = 0;
                while (ws < str.size() && (str[ws] == ' ' || str[ws] == '\t')) { cols += str[ws] == '\t' ? d.tabWidth : 1; ++ws; }
                if (ws < str.size())
                    for (int g = 0; g < cols / d.tabWidth; ++g)
                        fill(baseX + static_cast<float>(g * d.tabWidth) * cell, y, std::max(1.0f, std::round(s * 0.5f)), line, theme::guide);
            }
            for (std::size_t i = 0; i < str.size() && x < right;) {
                const std::size_t start = i;
                char32_t cp = decodeUtf8(str, i);
                if (cp == '\t') {
                    int adv = d.tabWidth - dcol % d.tabWidth;
                    dcol += adv;
                    x += static_cast<float>(adv) * cell;
                    continue;
                }
                if (cp != ' ' && x + cell > L_.textX) {
                    if (cp < 0x20 || cp == 0x7f) cp = 0xFFFD;
                    dl_.glyph(font_.glyph(cp), x, baseline, theme::syntax(toks_[start]));
                }
                x += cell;
                ++dcol;
            }
        }

        // Diagnostics: squiggles under the offending text; the worst severity per line marks the gutter.
        std::unordered_map<int, int> worstSeverity;
        if (auto dit = diags_.find(d.path); dit != diags_.end()) {
            for (const LspDiag& dg : dit->second) {
                const Color c = diagColor(dg.severity);
                for (int ln = std::max(dg.line, first); ln <= std::min(dg.endLine, last); ++ln) {
                    const std::string& str = d.buf.line(ln);
                    int sc = ln == dg.line ? byteColFromUtf16(str, dg.col) : static_cast<int>(std::min(str.find_first_not_of(" \t"), str.size()));
                    int ec = ln == dg.endLine ? byteColFromUtf16(str, dg.endCol) : static_cast<int>(str.size());
                    if (ec <= sc) ec = static_cast<int>(utf8Next(str, static_cast<std::size_t>(sc)));
                    float x0 = baseX + static_cast<float>(displayCol(str, sc, d.tabWidth)) * cell;
                    float x1 = std::max(x0 + cell, baseX + static_cast<float>(displayCol(str, ec, d.tabWidth)) * cell);
                    const float y = e.y + static_cast<float>(ln + 1) * line - sy - std::round(3.5f * s);
                    const float step = std::max(2.0f, std::round(2.0f * s)), th = std::max(1.0f, std::round(1.2f * s));
                    int k = 0;
                    for (float x = x0; x < x1; x += step, ++k) fill(x, y + ((k & 1) ? std::round(1.5f * s) : 0.0f), std::min(step, x1 - x), th, c);
                    auto it = worstSeverity.find(ln);
                    if (it == worstSeverity.end() || dg.severity < it->second) worstSeverity[ln] = dg.severity;
                }
            }
        }

        // Caret: the animated, stretchy bar (gutter and tab strip are painted over any overflow).
        if ((focused_ || !opt_.screenshot.empty()) && caretOn_ && caret_.init && caret_.doc == &d) {
            const float cwid = caret_.xr - caret_.xl, chei = caret_.yb - caret_.yt;
            fill(baseX + caret_.xl, e.y + caret_.yt - sy, cwid, chei, theme::caret, std::min(cwid, chei) / 2);
        }

        // Inline blame for the cursor line: who changed it, when, and the commit summary.
        if (drawActive_) {
            if (const git::Commit* bc = blameOf(d, d.cursor.line)) {
                const std::string& cl = d.buf.line(d.cursor.line);
                const float by = e.y + static_cast<float>(d.cursor.line) * line - sy;
                const float limit = (L_.minimap.w > 0 ? L_.minimap.x : e.right()) - 14 * s;
                const float bx = baseX + static_cast<float>(displayCol(cl, static_cast<int>(cl.size()), d.tabWidth) + 3) * cell;
                if (by > e.y - line && by < e.bottom() && bx < limit - 14 * cell) {
                    std::string t = blameText(*bc);
                    const std::size_t maxCells = static_cast<std::size_t>((limit - bx) / cell);
                    if (utf8Length(t) > maxCells) {
                        std::size_t i = 0, cells = 0;
                        while (i < t.size() && cells + 1 < maxCells) { i = utf8Next(t, i); ++cells; }
                        t = t.substr(0, i) + "\xE2\x80\xA6";
                    }
                    label(bx, by, t, Color::rgb(0x646a78), limit);
                    blameRect_ = {bx - cell, by, static_cast<float>(utf8Length(t) + 2) * cell, line};
                    blameHash_ = bc->hash;
                    blameUncommitted_ = bc->uncommitted;
                }
            }
        }

        // Gutter (opaque, so scrolled-away text disappears beneath it).
        fill(e.x, e.y, L_.gutterW, e.h, theme::editorBg);
        const GitDoc* gd = nullptr;
        if (auto git_it = gitDocs_.find(&d); git_it != gitDocs_.end() && git_it->second.hunksVersion == d.buf.version()) gd = &git_it->second;
        for (int ln = first; ln <= last; ++ln) {
            const float y = e.y + static_cast<float>(ln) * line - sy;
            std::string num = std::to_string(ln + 1);
            bool active = ln == d.cursor.line;
            if (active && !hasSel) fill(e.x, y, L_.gutterW, line, theme::lineHl);
            label(e.x + L_.gutterW - 1.5f * cell - static_cast<float>(num.size()) * cell, y, num,
                  active ? theme::lineNumActive : theme::lineNum);
            if (auto wit = worstSeverity.find(ln); wit != worstSeverity.end())
                fill(e.x + std::round(3 * s), y + std::round(3 * s), std::max(2.0f, std::round(3 * s)), line - std::round(6 * s), diagColor(wit->second), 1.5f * s);
            drawDebugGutter(d, ln, y, line);
            if (gd && ln < static_cast<int>(gd->kinds.size()) && gd->kinds[static_cast<std::size_t>(ln)])   // change bar vs HEAD
                fill(e.x + L_.gutterW - std::round(5 * s), y + 1, std::max(2.0f, std::round(3 * s)), line - 2,
                     gd->kinds[static_cast<std::size_t>(ln)] == 1 ? Color::rgb(0x4fb36d) : Color::rgb(0x5b9bff), 1.5f * s);
        }
        if (gd)
            for (int dl : gd->deletedAt) {   // removed lines: a small red wedge at the boundary
                if (dl < first || dl > last + 1) continue;
                const float y = e.y + static_cast<float>(dl) * line - sy;
                fill(e.x + L_.gutterW - std::round(6 * s), y - std::round(s), std::round(6 * s), std::max(2.0f, std::round(2 * s)), Color::rgb(0xe5484d), s);
            }

        drawMinimap(d);

        // Scrollbar.
        float maxS = maxScroll(d);
        if (maxS > 0) {
            float contentH = maxS + e.h;
            float thumbH = std::max(24 * s, e.h * e.h / contentH);
            float thumbY = e.y + (e.h - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
            float w = (scrollbarDrag_ ? 10.0f : 7.0f) * s;
            fill(e.right() - w - 4 * s, thumbY, w, thumbH, theme::scrollbar, w / 2);
        }

        // Welcome hint on an empty untitled buffer.
        if (docs_.size() == 1 && d.path.empty() && total == 1 && d.buf.line(0).empty() && (prompt_.kind == PromptKind::None || sidePrompt())) {
            static constexpr std::array<std::string_view, 8> hints = {
                "Ctrl+O   open file", "Ctrl+Shift+O   open folder", "Ctrl+B   toggle sidebar", "Ctrl+N   new file", "Ctrl+S   save", "Ctrl+F   find / Ctrl+H replace",
                "Ctrl+G   go to line", "Ctrl+Q   quit"};
            const float rowH = std::round(uiH() + 10 * s);
            float y = e.y + e.h * 0.30f;
            uiLabel(L_.textX + 4 * cell, y - 2 * rowH, rowH, "Daemon", theme::text);
            uiLabel(L_.textX + 4 * cell, y - rowH, rowH, "Wayland + Vulkan code editor", theme::dim);
            const float kx = L_.textX + 4 * cell, vx = kx + std::round(180 * s);
            for (auto h : hints) {
                auto gap = h.find("   ");
                const std::string_view key = h.substr(0, gap), what = gap == std::string_view::npos ? std::string_view() : h.substr(gap + 3);
                uiLabel(kx, y, rowH, key, theme::accent.mix(theme::text, 0.35f));
                uiLabel(vx, y, rowH, what, theme::dim);
                y += rowH;
            }
        }
    }

    void drawPrompt() {
        const Rect& p = L_.prompt;
        const float s = S(), hair = std::max(1.0f, std::round(s * 0.5f));
        fill(p, theme::promptBg);
        fill(p.x, p.y, p.w, hair, theme::border);
        for (std::size_t i = 0; i < prompt_.fields.size(); ++i) {
            const Field& f = prompt_.fields[i];
            const float rowY = p.y + static_cast<float>(i) * L_.rowH;
            const bool active = static_cast<int>(i) == prompt_.active;
            float x = p.x + 16 * s;
            const std::string lab = f.label + ":";
            uiLabel(x, rowY, L_.rowH, lab, active ? theme::accent : theme::dim);
            x += uiW(lab) + 10 * s;

            float rightPad = 12 * s;
            std::string info;
            if (inFindMode() && i == 0) {
                info = matchSummary();
                rightPad = uiW(info) + uiW("Aa") + 50 * s;
            }
            const float maxX = p.right() - rightPad;
            // Scroll the field so the caret stays visible.
            std::size_t startByte = 0;
            while (startByte < f.cur && uiW(std::string_view(f.text).substr(startByte, f.cur - startByte)) > maxX - x - 8 * s)
                startByte = utf8Next(f.text, startByte);
            const std::string_view shown = std::string_view(f.text).substr(startByte);
            if (f.fresh && active)
                fill(x - 3 * s, rowY + 5 * s, std::min(uiW(shown), maxX - x) + 6 * s, L_.rowH - 10 * s, theme::selection, 4 * s);
            uiLabel(x, rowY, L_.rowH, shown, theme::text, maxX);
            if (active && caretOn_ && focused_) {
                const float cx = x + uiW(std::string_view(f.text).substr(startByte, f.cur - startByte));
                fill(std::round(cx), rowY + 7 * s, std::max(2.0f, std::round(1.4f * s)), L_.rowH - 14 * s, theme::caret, s);
            }
            if (inFindMode() && i == 0) {
                const float chipW = uiW("Aa") + 16 * s;
                const Rect chip{p.right() - 14 * s - chipW, rowY + 5 * s, chipW, L_.rowH - 10 * s};
                if (matchCase_) fill(chip, theme::accent.alpha(60), 5 * s);
                uiLabel(chip.x + 8 * s, chip.y, chip.h, "Aa", matchCase_ ? theme::accent : theme::lineNum);
                uiLabelR(chip.x - 12 * s, rowY, L_.rowH, info, theme::dim);
            }
        }
    }

    std::string matchSummary() {
        Document& d = doc();
        if (findText_.empty()) return "";
        auto key = std::tuple{&d, d.buf.version(), findText_, matchCase_};
        if (key != countKey_) {
            countKey_ = key;
            matchCount_ = 0;
            for (int l = 0; l < d.buf.lineCount(); ++l) matchCount_ += static_cast<int>(findAll(d.buf.line(l), findText_, matchCase_).size());
        }
        return matchCount_ == 0 ? "no results" : std::format("{} found", matchCount_);
    }

    void drawStatus() {
        const Rect& r = L_.status;
        const float s = S();
        carve(r);
        if (docs_.empty()) return;
        Document& d = doc();

        float x = r.w - std::round(16 * s);
        const float gap = std::round(18 * s);
        auto seg = [&](const std::string& t, Color c = theme::dim) { x = uiLabelR(x, r.y, r.h, t, c) - gap; };
        seg(d.lang ? d.lang->name : "Plain Text", theme::text.mix(theme::dim, 0.3f));
        seg(d.buf.crlf() ? "CRLF" : "LF");
        seg("UTF-8");
        seg(d.useTabs ? "Tabs" : std::format("Spaces: {}", d.tabWidth));
        std::string pos = std::format("Ln {}, Col {}", d.cursor.line + 1, displayCol(d.buf.line(d.cursor.line), d.cursor.col, d.tabWidth) + 1);
        if (d.hasSelection()) pos += std::format(" ({} selected)", utf8Length(d.selectedText()));
        seg(pos);
        if (!gitBranch_.empty()) {   // current branch, ahead / behind and whether the work tree has changes
            std::string t = gitBranch_;
            if (gitAhead_) t += std::format(" \xE2\x86\x91{}", gitAhead_);
            if (gitBehind_) t += std::format(" \xE2\x86\x93{}", gitBehind_);
            if (!gitEntries_.empty()) t += std::format("  \xE2\x80\xA2 {}", gitEntries_.size());
            x = uiLabelR(x, r.y, r.h, t, gitEntries_.empty() ? theme::dim : Color::rgb(0xe5c07b));
            const int ip = std::max(12, static_cast<int>(std::round(uiH() * 0.95f)));
            Icons::glyph(dl_, font_, x - static_cast<float>(ip) - 4 * s, r.y + (r.h - static_cast<float>(ip)) / 2, ip, 0xF062C, theme::dim);
            x -= static_cast<float>(ip) + 4 * s + gap;
        }

        // Language server and diagnostics.
        if (isCppDoc(d) || !lspStatus_.empty()) {
            const float dot = std::round(7 * s);
            if (!lspStatus_.empty()) {
                const bool ok = lsp_ && lsp_->ready();
                const bool starting = lsp_ && lsp_->state() == LspClient::State::Starting;
                const Color dc = ok ? Color::rgb(0x6aab73) : (starting ? diagColor(2) : theme::lineNum);
                x = uiLabelR(x, r.y, r.h, lspStatus_, theme::dim);
                x -= dot + 6 * s;
                fill(x, r.y + (r.h - dot) / 2, dot, dot, dc, dot / 2);
                x -= gap;
            }
            if (auto it = diags_.find(d.path); it != diags_.end()) {
                int errs = 0, warns = 0;
                for (const auto& dg : it->second) { if (dg.severity == 1) ++errs; else if (dg.severity == 2) ++warns; }
                auto badge = [&](int n, Color c) {
                    if (n == 0) return;
                    x = uiLabelR(x, r.y, r.h, std::to_string(n), c);
                    x -= dot + 5 * s;
                    fill(x, r.y + (r.h - dot) / 2, dot, dot, c, dot / 2);
                    x -= 12 * s;
                };
                badge(warns, diagColor(2));
                badge(errs, diagColor(1));
            }
        }

        const float lx = L_.island.x + 8 * s, maxX = x - 12 * s;
        if (!message_.empty()) {
            uiLabel(lx, r.y, r.h, message_, messageError_ ? Color::rgb(0xe6a23c) : theme::text, maxX);
        } else if (const LspDiag* dg = diagOnLine(d, d.cursor.line)) {
            std::string m = dg->message;
            if (auto nl = m.find('\n'); nl != std::string::npos) m.resize(nl);
            uiLabel(lx, r.y, r.h, m, diagColor(dg->severity), maxX);
        } else {
            uiLabel(lx, r.y, r.h, d.path.empty() ? std::string("untitled") : d.path, theme::dim, maxX);
        }
    }

    // ============================================================ members

    Options opt_;
    Window win_;     // declared before the renderer: the Vulkan surface must die first
    Renderer ren_;
    FontAtlas font_;
    DrawList dl_;
    Layout L_;
    std::vector<TabHit> tabHits_;
    std::vector<Tok> toks_;
    // ---- CMake project / tasks / output panel
    bool cmakeOpen_ = false, haveNinja_ = false;
    fs::path cmakeRoot_;
    std::vector<CMakeProfile> profiles_;
    int profile_ = 0;
    std::vector<CMakeTarget> targets_;
    int target_ = -1;
    std::string runArgs_;
    std::unique_ptr<Process> buildProc_, runProc_;
    TaskKind buildKind_ = TaskKind::None;
    bool runFocus_ = false, scriptSleeping_ = false;
    Clock::time_point scriptSleepUntil_{};
    bool sideSearch_ = false;   // the sidebar shows project search
    bool sideGit_ = false;   // the right-hand island shows Git instead of the file explorer
    bool gitView() const { return sidebar_ && sideGit_; }
    bool debugRun_ = false, toolTipShown_ = false;
    Clock::time_point toolHoverAt_{};
    bool buildEof_ = false, runEof_ = false, runAfterBuild_ = false, pendingBuild_ = false;
    std::string buildPartial_, runPartial_, buildStatus_;
    int buildCur_ = 0, buildTotal_ = 0;
    std::vector<OutLine> buildOut_, runOut_;
    std::vector<Problem> problems_;        // from the last build / configure
    std::unordered_map<std::string, std::pair<int, int>> problemCounts_;   // path -> (errors, warnings), files and their folders
    std::vector<Problem> shownProblems_;   // build problems + live clangd diagnostics (the Problems tab)
    bool panelOpen_ = false, panelDrag_ = false;
    int panelTab_ = 0;
    // ---- animation state (tabs, side bar, bottom panel)
    struct TabAnim {
        const Document* doc = nullptr;
        const Document* anchor = nullptr;   // ghosts: the tab they sat behind
        std::string name;
        float x = -1, tx = 0, w = 0, target = 0;
        bool ghost = false;   // closed tab still shrinking away
        bool used = false;
    };
    struct TabDraw { Rect tab, close; int index = -1; std::string name; float alpha = 1; const Document* doc = nullptr; };

    // ---- editor panes: a split tree whose leaves each own a tab strip and show one document at a time
    struct Pane {
        std::vector<Document*> tabs;
        Document* cur = nullptr;
        std::vector<TabAnim> anim;
        std::vector<TabDraw> draw;
        std::vector<TabHit> hits;
        MiniCache mini;
        Rect rect, tabsR, crumbs, editor, minimap;
        float gutterW = 0, textX = 0, textW = 0;
    };
    struct Node {
        std::unique_ptr<Pane> pane;          // leaf
        bool sideBySide = true;              // internal node: children left | right (true) or top / bottom (false)
        float ratio = 0.5f;
        std::unique_ptr<Node> a, b;
        Rect rect, gap;
        bool leaf() const { return pane != nullptr; }
    };
    enum class Zone { None, Tabs, Center, Left, Right, Top, Bottom };
    struct TabDrag {
        Document* doc = nullptr;
        Pane* from = nullptr;
        float sx = 0, sy = 0, grabX = 0;
        bool active = false;
        Zone zone = Zone::None;
        Pane* target = nullptr;
        int insertAt = 0;
    };
    std::unique_ptr<Node> root_;
    Pane* activePane_ = nullptr;
    TabDrag tabDrag_;
    Node* splitDragNode_ = nullptr;
    Pane* hoverPane_ = nullptr;
    bool drawActive_ = true;
    float panelAnim_ = 0, sideAnim_ = 0;
    std::deque<std::string> bgQueue_;
    std::unordered_set<std::string> bgOpen_, bgSources_;
    Clock::time_point bgNext_{};
    bool bgScanned_ = false;
    bool lspCrashed_ = false;
    std::deque<Clock::time_point> lspCrashTimes_;
    int compReq_ = -1;
    fs::file_time_type dbStamp_{};
    Clock::time_point nextDbCheck_{};
    CMakeKnowledge cmakeKb_;
    std::vector<ModuleDecl> modules_;
    bool clangdModules_ = false;
    float panelH_ = 230;   // logical px
    std::array<float, 5> panelScroll_{};
    std::array<bool, 5> panelFollow_{true, true, true, false, true};
    int hoverPanelRow_ = -1, hoverTool_ = 0;
    std::array<Rect, 5> panelTabRects_{};
    Rect panelClear_, panelClose_;
    ToolRects tool_;
    std::vector<std::function<void()>> menuActions_;
    int hoverAct_ = 0;
    bool miniDrag_ = false;
    MiniCache mini_;

    // ---- language server (clangd)
    std::unique_ptr<LspClient> lsp_;
    int lspFd_ = -1;
    bool lspNoticeShown_ = false;
    std::string lspStatus_;
    std::unordered_map<std::string, std::vector<LspDiag>> diags_;
    std::optional<Clock::time_point> lspFlushAt_;
    Completion comp_;
    std::uint64_t compSeq_ = 0, hoverSeq_ = 0;
    Hover hover_;
    std::vector<NavEntry> navBack_;

    Explorer explorer_;
    Menu menu_;
    FileClip fileClip_;
    fs::path fileOpPath_;
    bool deleteForever_ = false;
    bool sidebar_ = false, sideFocus_ = false, sideReveal_ = false, splitDrag_ = false;
    float sidebarWidth_ = 380;   // logical px
    float sideScroll_ = 0, sideTarget_ = 0;
    int hoverRow_ = -1, dragRow_ = -1;
    struct CaretAnim {
        float xl = 0, xr = 0, yt = 0, yb = 0;
        bool init = false;
        const Document* doc = nullptr;
        float cell = 0, line = 0;
    } caret_;

    std::vector<std::unique_ptr<Document>> docs_;
    int cur_ = 0;
    Prompt prompt_;
    std::string findText_, replaceText_;
    bool matchCase_ = false;
    std::tuple<const Document*, std::uint64_t, std::string, bool> countKey_{};
    int matchCount_ = 0;
    std::string overwriteArmed_;

    int scale_ = 1, wLog_ = 1280, hLog_ = 800;
    float fontSize_ = 14;
    bool rendererReady_ = false, running_ = true, dirty_ = true, skipped_ = false, animating_ = false;
    bool focused_ = true, caretOn_ = true, scrollToCursor_ = false;
    bool quitArmed_ = false;
    int closeArmed_ = -1;
    Clock::time_point nextBlink_ = Clock::now();
    std::string message_;
    bool messageError_ = false;
    Clock::time_point messageUntil_ = Clock::now();
    std::string lastTitle_;
    int frames_ = 0;
    Clock::time_point settleUntil_{};
    std::string script_;      // scripted input (--keys)
    std::size_t scriptPos_ = 0;
    bool shotRequested_ = false;

    float mx_ = 0, my_ = 0, lastClickX_ = 0, lastClickY_ = 0;
    int hoverBtn_ = 0, hoverTab_ = -1, hoverClose_ = -1;
    bool dragging_ = false, scrollbarDrag_ = false;
    int clickCount_ = 0;
    Clock::time_point lastClick_{}, lastTitleClick_{};
};

}  // namespace ide
