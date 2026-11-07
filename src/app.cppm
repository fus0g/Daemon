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
inline constexpr Color frame = Color::rgb(0x0e1017, 192);    // translucent chrome between islands (blurred by the compositor)
inline constexpr Color editorBg = Color::rgb(0x0a0b0d);     // code editor: near-black
inline constexpr Color bg = Color::rgb(0x1a1b1f);           // island (side panels) surface
inline constexpr Color lineHl = Color::rgb(0x15171b);
inline constexpr Color selection = Color::rgb(0x2a4d80);
inline constexpr Color selectionIdle = Color::rgb(0x34363d);
inline constexpr Color text = Color::rgb(0xd1d4da);
inline constexpr Color lineNum = Color::rgb(0x565962);
inline constexpr Color lineNumActive = Color::rgb(0xaeb2bb);
inline constexpr Color guide = Color::rgb(0x1f2126);
inline constexpr Color caret = Color::rgb(0x7aaeff);
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
inline constexpr Color accent = Color::rgb(0x5b9bff);
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
    ~App() { lsp_.reset(); }  // while the members its callbacks touch are still alive

    std::expected<void, std::string> init() {
        fontSize_ = opt_.fontSize;
        if (auto r = font_.load(); !r) return r;

        win_.onResize = [this](int w, int h, int s) { onResize(w, h, s); };
        win_.onKey = [this](const KeyEvent& k) { onKey(k); };
        win_.onPointerMove = [this](float x, float y) { onPointerMove(x, y); };
        win_.onPointerButton = [this](Button b, bool p, float x, float y) { onPointerButton(b, p, x, y); };
        win_.onScroll = [this](float dx, float dy, int sx, int sy) { onScroll(dx, dy, sx, sy); };
        win_.onFocus = [this](bool f) { focused_ = f; resetCaret(); dirty_ = true; };
        win_.onClose = [this] { requestQuit(); };
        if (auto r = win_.create("Daemon", 1280, 800); !r) return r;

        scale_ = win_.scale();
        wLog_ = win_.width();
        hLog_ = win_.height();
        focused_ = win_.focused();
        applyFonts();
        if (auto r = ren_.init(win_.display(), win_.surface()); !r) return r;
        ren_.setExtent(static_cast<std::uint32_t>(wLog_ * scale_), static_cast<std::uint32_t>(hLog_ * scale_));
        rendererReady_ = true;
        std::println("Vulkan device: {}   font: {}   scale: {}x", ren_.deviceName(), font_.fontPath(), scale_);

        for (const auto& f : opt_.files) openFile(f);
        if (docs_.empty()) newDoc();
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
            std::string_view tok = spec.substr(pos, bar == std::string_view::npos ? bar : bar - pos);
            const std::size_t next = bar == std::string_view::npos ? spec.size() : bar + 1;
            if (tok == "wait") {
                if (!scriptReady()) return;  // retried from update()
                pos = next;
                continue;
            }
            pos = next;
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

    enum class PromptKind { None, Find, Replace, Goto, Open, OpenFolder, SaveAs, NewFile, NewFolder, Rename, Delete, RunArgs };
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
        Rect profile, target, run, build, stop, reload, panel;
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

    void openFolder(const std::string& raw) {
        std::string err;
        if (!explorer_.open(expandPath(raw), err)) { setMessage(err, true); return; }
        sidebar_ = true;
        sideScroll_ = sideTarget_ = 0;
        sideFocus_ = false;
        revealCurrent();
        detectCMake();
        setMessage("Opened folder " + explorer_.rootPath().string());
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
        if (!explorer_.isOpen()) { openPrompt(PromptKind::OpenFolder); return; }
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
        if (lsp_ && cmakeOpen_) consider(nextDbCheck_);
        if (hover_.pending) consider(hover_.due);
        return t;
    }

    void update(Clock::time_point now, float dt) {
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
        if (sidebar_ && explorer_.isOpen()) {
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
        if (docs_.empty()) animating_ = false;
        if (stepAnimations(dt)) { animating_ = true; dirty_ = true; }
        pollTasks();
        if (scriptPos_ < script_.size()) stepScript();
        lspSync(now);
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

    Rect activityRect(int i) const {
        const Rect& a = L_.activity;
        return {a.x, a.y + std::round(6 * S()) + static_cast<float>(i) * a.w, a.w, a.w};
    }
    int activityAt(float x, float y) const {
        if (!L_.activity.contains(x, y)) return 0;
        for (int i = 0; i < 3; ++i)
            if (activityRect(i).contains(x, y)) return i + 1;
        return 0;
    }
    bool activityActive(int i) const {
        switch (i) {
            case 0: return sidebar_;
            case 1: return panelOpen_ && panelTab_ < 2;
            default: return panelOpen_ && panelTab_ == 2;
        }
    }
    void activityClick(int i) {
        switch (i) {
            case 0: toggleSidebar(); break;
            case 1:
                if (activityActive(1)) panelOpen_ = false;
                else { panelOpen_ = true; panelTab_ = runProc_ ? 1 : 0; }
                break;
            default:
                if (activityActive(2)) panelOpen_ = false;
                else { panelOpen_ = true; panelTab_ = 2; }
                break;
        }
        dirty_ = true;
    }

    void drawActivityBar() {
        const Rect& a = L_.activity;
        if (a.w <= 0) return;
        const float s = S();
        const int ip = std::max(14, static_cast<int>(std::round(uiH() * 1.35f)));
        static constexpr std::array<ToolIcon, 3> icons = {ToolIcon::Files, ToolIcon::Terminal, ToolIcon::Warning};
        const bool hasErrors = !problems_.empty() ||
                               (!docs_.empty() && diags_.contains(doc().path) &&
                                std::ranges::any_of(diags_.at(doc().path), [](const LspDiag& d) { return d.severity == 1; }));
        for (int i = 0; i < 3; ++i) {
            const Rect r = activityRect(i);
            const bool active = activityActive(i), hot = hoverAct_ == i + 1;
            if (hot && !active) fill(r.x + 5 * s, r.y + 5 * s, r.w - 10 * s, r.h - 10 * s, theme::tabHover, 9 * s);
            if (active) {
                fill(r.x + 5 * s, r.y + 5 * s, r.w - 10 * s, r.h - 10 * s, theme::tabActive, 9 * s);
                fill(a.right() - std::round(2.5f * s), r.y + 10 * s, std::round(2.5f * s), r.h - 20 * s, theme::accent, 1.2f * s);
            }
            const float ix = std::round(r.x + (r.w - static_cast<float>(ip)) / 2), iy = std::round(r.y + (r.h - static_cast<float>(ip)) / 2);
            Icons::tool(dl_, font_, ix, iy, ip, icons[static_cast<std::size_t>(i)], active || hot ? theme::text : theme::dim);
            if (i == 2 && hasErrors) {
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
        const float act = std::round(44 * s);  // activity bar width
        L_.rowH = std::round(uh + 14 * s);
        L_.radius = std::round(12 * s);
        const float promptH = prompt_.kind == PromptKind::None ? 0 : L_.rowH * static_cast<float>(prompt_.fields.size());
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
        L_.sideList = sideShown ? Rect{L_.side.x, L_.side.y + tabH, L_.side.w, std::max(0.0f, L_.side.h - tabH)} : Rect{};
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
            for (Rect* r : {&tool_.run, &tool_.build, &tool_.stop, &tool_.reload}) {
                *r = {x, cy, ch, ch};
                x += ch + std::round(2 * s);
            }
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
        p.gutterW = (static_cast<float>(digits) + 2.5f) * cw();
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

    void onKey(const KeyEvent& k) {
        resetCaret();
        if (!(k.ctrl && k.key == Key::Char && k.ch == 'q')) quitArmed_ = false;
        if (!(k.ctrl && k.key == Key::Char && k.ch == 'w')) closeArmed_ = -1;
        if (tabDrag_.active) {  // a tab is being dragged: Esc cancels, other keys are ignored
            if (k.key == Key::Escape) { tabDrag_ = {}; dirty_ = true; }
            return;
        }
        if (menu_.open) { menuKey(k); dirty_ = true; return; }
        if (prompt_.kind != PromptKind::None) { promptKey(k); return; }
        if (sidebar_ && sideFocus_ && explorerKey(k)) { dirty_ = true; return; }
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
        closeCompletion();
        for (auto& d : docs_) d->lspOpen = false;
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

    void attachLsp(Document& d) {
        if (!lsp_ || !isCppDoc(d) || d.lspOpen) return;
        lsp_->didOpen(d.path, languageId(d), ++d.lspVersion, d.buf.text());
        d.lspOpen = true;
        d.lspSent = d.buf.version();
    }

    void lspDetach(Document& d) {
        if (lsp_ && d.lspOpen) lsp_->didClose(d.path);
        d.lspOpen = false;
        diags_.erase(d.path);
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
        if (hover_.pending || hover_.show) { hover_.pending = hover_.show = false; ++hoverSeq_; dirty_ = true; }
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
        hover_.show = !hover_.lines.empty();
        dirty_ = true;
    }

    void drawHover() {
        if (!hover_.show || hover_.lines.empty() || menu_.open) return;
        const float s = S(), line = std::round(uiH() + 3 * s);
        float tw = 0;
        for (const auto& [t, sev] : hover_.lines) tw = std::max(tw, uiW(t));
        const float w = tw + 26 * s, h = static_cast<float>(hover_.lines.size()) * line + 16 * s;
        const float W = L_.status.w, H = L_.status.bottom();
        float x = std::min(hover_.px + 12 * s, W - w - 6 * s);
        float y = hover_.py + 22 * s;
        if (y + h > H - 6 * s) y = hover_.py - h - 10 * s;
        x = std::max(6 * s, x);
        glassPanel({x, y, w, h}, 10 * s);
        float ty = y + 8 * s;
        for (const auto& [t, sev] : hover_.lines) {
            uiLabel(x + 13 * s, ty, line, t, sev == 0 ? theme::text : diagColor(sev));
            ty += line;
        }
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

    void cmakeBuild(bool thenRun) {
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
        pendingBuild_ = false;
        runAfterBuild_ = false;
        if (buildProc_) buildProc_->terminate();
        if (runProc_) runProc_->terminate();
    }

    bool tasksRunning() const { return buildProc_ || runProc_; }

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
                if (pendingBuild_) { pendingBuild_ = false; cmakeBuild(runAfterBuild_); return; }
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
                if (runAfterBuild_) { runAfterBuild_ = false; startRun(); return; }
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

    void startRun() {
        if (target_ < 0) return;
        const CMakeTarget& t = targets_[static_cast<std::size_t>(target_)];
        std::error_code ec;
        if (!fs::exists(t.artifact, ec)) { setMessage("Executable not found: " + t.artifact.string(), true); return; }
        if (runProc_) { runProc_->terminate(); runProc_.reset(); }
        runOut_.clear();
        runPartial_.clear();
        ProcessOptions o;
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
        buildStatus_ = "Running " + t.name;
        win_.watch(runProc_->outFd(), [this] { pumpRun(); });
        dirty_ = true;
    }

    void pumpRun() {
        if (!runProc_) return;
        std::string chunk;
        long n = runProc_->read(chunk);
        if (!chunk.empty()) {
            for (auto& l : takeLines(runPartial_, chunk)) runOut_.push_back({stripAnsi(l), 0, -1});
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
        if (!runPartial_.empty()) { runOut_.push_back({stripAnsi(runPartial_), 0, -1}); runPartial_.clear(); }
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
        else if (tool_.build.contains(x, y)) cmakeBuild(false);
        else if (tool_.stop.contains(x, y)) cmakeStop();
        else if (tool_.reload.contains(x, y)) cmakeReload();
        else if (tool_.panel.contains(x, y)) togglePanel();
    }

    int toolAt(float x, float y) const {
        if (!cmakeOpen_) return 0;
        const Rect* rs[] = {&tool_.profile, &tool_.target, &tool_.run, &tool_.build, &tool_.stop, &tool_.reload, &tool_.panel};
        for (int i = 0; i < 7; ++i)
            if (rs[i]->contains(x, y)) return i + 1;
        return 0;
    }

    void drawToolbar() {
        if (!cmakeOpen_) return;
        const float s = S(), m = std::round(8 * s);
        const int ip = iconPx();
        auto chip = [&](const Rect& r, const std::string& text, bool hot) {
            fill(r, hot ? theme::tabHover : theme::tabBg, 8 * s);
            uiLabel(r.x + std::round(14 * s), r.y, r.h, text, theme::text, r.right() - std::round(26 * s));
            const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.6f)));
            Icons::chevron(dl_, font_, r.right() - static_cast<float>(cs) - std::round(10 * s), r.y + (r.h - static_cast<float>(cs)) / 2, cs, true, theme::dim);
        };
        chip(tool_.profile, profiles_.empty() ? "-" : profiles_[static_cast<std::size_t>(profile_)].name, hoverTool_ == 1);
        chip(tool_.target, target_ >= 0 ? targets_[static_cast<std::size_t>(target_)].name : "No target", hoverTool_ == 2);

        const bool busy = buildProc_ != nullptr;
        const bool anyRunning = busy || runProc_ != nullptr;
        auto btn = [&](const Rect& r, ToolIcon ic, Color c, bool enabled, int id) {
            if (hoverTool_ == id && enabled) fill(r, theme::tabHover, 8 * s);
            Icons::tool(dl_, font_, std::round(r.x + (r.w - static_cast<float>(ip)) / 2), std::round(r.y + (r.h - static_cast<float>(ip)) / 2), ip, ic,
                        enabled ? c : c.alpha(70));
        };
        btn(tool_.run, ToolIcon::Play, Color::rgb(0x5fb760), !busy && (target_ >= 0), 3);
        btn(tool_.build, ToolIcon::Build, theme::text, !busy, 4);
        btn(tool_.stop, ToolIcon::Stop, theme::danger, anyRunning, 5);
        btn(tool_.reload, ToolIcon::Reload, theme::text, !busy, 6);

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

    std::size_t panelRowCount() const { return panelTab_ == 2 ? problems_.size() : panelLines().size(); }
    float panelHeadH() const { return std::round(font_.uiLineHeight() + 14 * S()); }
    Rect panelBody() const {
        const Rect& p = L_.panel;
        return {p.x, p.y + panelHeadH(), p.w, std::max(0.0f, p.h - panelHeadH())};
    }
    float panelMaxScroll() const { return std::max(0.0f, static_cast<float>(panelRowCount()) * lh() - panelBody().h); }

    void panelClick(Button b) {
        const Rect& p = L_.panel;
        if (b != Button::Left) return;
        if (my_ < p.y + 6 * S()) { panelDrag_ = true; return; }  // splitter at the top edge
        for (std::size_t i = 0; i < panelTabRects_.size(); ++i)
            if (panelTabRects_[i].contains(mx_, my_)) { panelTab_ = static_cast<int>(i); dirty_ = true; return; }
        if (panelClose_.contains(mx_, my_)) { panelOpen_ = false; dirty_ = true; return; }
        if (panelClear_.contains(mx_, my_)) {
            if (panelTab_ == 1) runOut_.clear(); else if (panelTab_ == 0) { buildOut_.clear(); problems_.clear(); }
            else problems_.clear();
            dirty_ = true;
            return;
        }
        Rect body = panelBody();
        if (body.contains(mx_, my_)) {
            int row = static_cast<int>(std::floor((my_ - body.y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
            if (panelTab_ == 2) {
                if (row >= 0 && row < static_cast<int>(problems_.size())) gotoProblem(problems_[static_cast<std::size_t>(row)]);
            } else {
                const auto& lines = panelLines();
                if (row >= 0 && row < static_cast<int>(lines.size()) && lines[static_cast<std::size_t>(row)].problem >= 0)
                    gotoProblem(problems_[static_cast<std::size_t>(lines[static_cast<std::size_t>(row)].problem)]);
            }
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
                const Problem& pr = problems_[static_cast<std::size_t>(i)];
                std::string rel = pr.file.lexically_relative(cmakeRoot_).string();
                if (rel.empty() || rel.starts_with("..")) rel = pr.file.string();
                text = std::format("{}:{}:{}  {}", rel, pr.line, pr.col, pr.message);
                sev = pr.severity == Severity::Error ? 2 : pr.severity == Severity::Warning ? 1 : 3;
                clickable = true;
            } else {
                const OutLine& ol = panelLines()[static_cast<std::size_t>(i)];
                text = ol.text;
                sev = ol.sev;
                clickable = ol.problem >= 0;
            }
            if (clickable && i == hoverPanelRow_) fill(body.x + 4 * s, y, body.w - 8 * s, lh(), theme::tabHover, 4 * s);
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

        // Header: darker band, text tabs with an accent underline (no pills, no borders).
        fill(p.x, p.y, p.w, hh, theme::panelHead);
        static constexpr std::array<std::string_view, 3> names = {"Build", "Run", "Problems"};
        float x = p.x + 8 * s;
        for (std::size_t i = 0; i < 3; ++i) {
            std::string t = std::string(names[i]);
            if (i == 2 && !problems_.empty()) t += std::format("  {}", problems_.size());
            const float w = uiW(t) + 28 * s;
            panelTabRects_[i] = {x, p.y + 4 * s, w, hh - 8 * s};
            const bool active = static_cast<int>(i) == panelTab_;
            const bool hot = panelTabRects_[i].contains(mx_, my_);
            Color c = active ? theme::text : (hot ? Color::rgb(0xb4b8c1) : theme::dim);
            if (i == 2 && !problems_.empty()) c = active ? Color::rgb(0xf2777a) : Color::rgb(0xc86a6c);
            uiLabel(x + 14 * s, p.y, hh, t, c);
            if (active) fill(x + 14 * s, p.y + hh - std::round(2 * s) - s, uiW(t), std::round(2 * s), i == 2 && !problems_.empty() ? Color::rgb(0xf2777a) : theme::accent, std::round(s));
            x += w + 2 * s;
        }
        const std::string clear = "Clear";
        const float cwid = uiW(clear) + 24 * s, xs = std::round(hh * 0.72f);
        panelClose_ = {p.right() - 8 * s - xs, p.y + (hh - xs) / 2, xs, xs};
        panelClear_ = {panelClose_.x - 6 * s - cwid, p.y + 4 * s, cwid, hh - 8 * s};
        uiLabel(panelClear_.x + 12 * s, p.y, hh, clear, theme::dim);
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
        if (k.key == Key::F9 && ctrl) { cmakeBuild(false); scrollToCursor_ = false; return; }
        if (k.key == Key::F10 && shift) { cmakeBuild(true); scrollToCursor_ = false; return; }
        if (k.key == Key::F2 && ctrl) { cmakeStop(); scrollToCursor_ = false; return; }

        if (ctrl && !alt && k.key == Key::Char) {
            switch (k.ch) {
                case 'r': if (shift) cmakeReload(); scrollToCursor_ = false; break;
                case '`': togglePanel(); scrollToCursor_ = false; break;
                case 's': if (shift) openPrompt(PromptKind::SaveAs); else saveCurrent(); break;
                case 'o': openPrompt(shift ? PromptKind::OpenFolder : PromptKind::Open); break;
                case 'b': toggleSidebar(); scrollToCursor_ = false; break;
                case 'e': if (shift) focusSidebar(); scrollToCursor_ = false; break;
                case 'n': newDoc(); break;
                case 'w': closeTab(cur_); break;
                case 'q': requestQuit(); break;
                case 'f': openPrompt(PromptKind::Find); break;
                case 'h': openPrompt(PromptKind::Replace); break;
                case 'g': openPrompt(PromptKind::Goto); break;
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
        if (sidebar_ && L_.sideList.contains(mx_, my_)) {
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
        const int hover = toolAt(mx_, my_);
        int hpr = -1;
        if (panelOpen_ && panelBody().contains(mx_, my_))
            hpr = static_cast<int>(std::floor((my_ - panelBody().y + panelScroll_[static_cast<std::size_t>(panelTab_)]) / lh()));
        if (hb != hoverBtn_ || ht != hoverTab_ || hc != hoverClose_ || hover != hoverTool_ || hpr != hoverPanelRow_) {
            hoverBtn_ = hb; hoverTab_ = ht; hoverClose_ = hc; hoverTool_ = hover; hoverPanelRow_ = hpr;
            dirty_ = true;
        }
        if (!opt_.screenshot.empty()) {}  // scripted runs drive hover themselves
        else if (!docs_.empty() && !menu_.open && !dragging_ && L_.editor.contains(mx_, my_) && mx_ >= L_.textX && mx_ < L_.minimap.x) hoverMoved();
        else hideHover();
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
        if (menu_.open) {  // the menu captures the click; clicking outside just dismisses it
            if (pressed) {
                int i = menuItemAt(mx_, my_);
                if (i >= 0) activateMenuItem(i); else if (!menu_.rect.contains(mx_, my_)) closeMenu();
            }
            return;
        }
        if (!pressed) {
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
            if (!explorer_.isOpen()) { if (b == Button::Left) openPrompt(PromptKind::OpenFolder); return; }
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
                if (row >= 0) activateRow(*rows[static_cast<std::size_t>(row)].node);
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
        if (b != Button::Left) return;
        if (inScrollbar(d, mx_, my_)) { scrollbarDrag_ = true; dragStep(d); return; }
        if (inMinimap(mx_, my_)) { miniDrag_ = true; miniJump(d); return; }
        if (mx_ < L_.textX) {  // gutter: select the whole line
            int line = posAt(d, mx_, my_).line;
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
        if (menu_.open) closeMenu();
        hideHover();
        if (panelOpen_ && L_.panel.contains(mx_, my_)) {
            auto t = static_cast<std::size_t>(panelTab_);
            float dy = stepsY != 0 ? static_cast<float>(stepsY) * 3 * lh() : dyPx * S();
            panelScroll_[t] = std::clamp(panelScroll_[t] + dy, 0.0f, panelMaxScroll());
            panelFollow_[t] = panelScroll_[t] >= panelMaxScroll() - 1;
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
        carve({0, 0, W, H});   // frosted base for the whole window; islands are painted over it
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
        drawActivityBar();
        for (Node* n : leaves()) {
            Pane& p = *n->pane;
            const PaneCtx sv = enterPane(p);
            drawTabs(p);
            if (p.cur) drawCrumbs();
            leavePane(p, sv);
        }
        drawPanel();
        if (prompt_.kind != PromptKind::None) drawPrompt();
        if (L_.panel.h > 0.5f) {
            const Rect& pn = L_.panel;
            carve({pn.x, pn.bottom(), pn.w, H - pn.bottom()});  // rows that overflow the bottom edge
            carve(pn, -L_.radius);
        }
        // Glass edge: a faint light line along the inner border of every island, trimmed by the rounded-corner masks.
        auto rim = [&](const Rect& r) {
            if (r.w < 4 || r.h < 4) return;
            const float t = std::max(1.0f, std::round(S() * 0.6f));
            const Color c = Color::rgb(0xffffff, 20);
            fill(r.x, r.y, r.w, t, c); fill(r.x, r.bottom() - t, r.w, t, c);
            fill(r.x, r.y, t, r.h, c); fill(r.right() - t, r.y, t, r.h, c);
        };
        for (Node* n : leaves()) rim(n->pane->rect);
        if (L_.side.w > 1) rim(L_.side);
        if (L_.panel.h > 0.5f) rim(L_.panel);
        for (Node* n : leaves()) carve(n->pane->rect, -L_.radius);
        if (L_.side.w > 1) {
            if (sideAnim_ > 0.002f && sideAnim_ < 0.998f) carve({L_.activity.x, L_.side.y, W - L_.activity.x, L_.side.h});  // clip content sliding past the edge
            carve(L_.side, -L_.radius);
        }  // inverted rounded rect = clips the four corners
        if (L_.csd) drawTitleBar();
        drawToolbar();
        drawStatus();
        drawCompletion();
        drawHover();
        drawMenu();
        drawTabDrag();
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
                else if (i == hoverRow_) fill(rr, theme::sideHover, 6 * s);
                const float x = sd.x + 12 * s + static_cast<float>(row.depth) * indent;
                for (int lvl = 0; lvl < row.depth; ++lvl)
                    fill(std::round(sd.x + 12 * s + static_cast<float>(lvl) * indent + indent * 0.5f), y, std::max(1.0f, std::round(s * 0.5f)), rh, theme::guide);
                Color nameColor = nd.name.starts_with('.') ? theme::dim : (nd.dir ? theme::text : Color::rgb(0xb9bdc6));
                if (active) nameColor = Color::rgb(0xffffff).mix(theme::accent, 0.25f);
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
                uiLabel(ix + iconSize() + 8 * s, y, rh, nd.name, nameColor, sd.right() - 10 * s);
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

        // Gutter (opaque, so scrolled-away text disappears beneath it).
        fill(e.x, e.y, L_.gutterW, e.h, theme::editorBg);
        for (int ln = first; ln <= last; ++ln) {
            const float y = e.y + static_cast<float>(ln) * line - sy;
            std::string num = std::to_string(ln + 1);
            bool active = ln == d.cursor.line;
            if (active && !hasSel) fill(e.x, y, L_.gutterW, line, theme::lineHl);
            label(e.x + L_.gutterW - 1.5f * cell - static_cast<float>(num.size()) * cell, y, num,
                  active ? theme::lineNumActive : theme::lineNum);
            if (auto wit = worstSeverity.find(ln); wit != worstSeverity.end())
                fill(e.x + std::round(3 * s), y + std::round(3 * s), std::max(2.0f, std::round(3 * s)), line - std::round(6 * s), diagColor(wit->second), 1.5f * s);
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
        if (docs_.size() == 1 && d.path.empty() && total == 1 && d.buf.line(0).empty() && prompt_.kind == PromptKind::None) {
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
    bool buildEof_ = false, runEof_ = false, runAfterBuild_ = false, pendingBuild_ = false;
    std::string buildPartial_, runPartial_, buildStatus_;
    int buildCur_ = 0, buildTotal_ = 0;
    std::vector<OutLine> buildOut_, runOut_;
    std::vector<Problem> problems_;
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
    int compReq_ = -1;
    fs::file_time_type dbStamp_{};
    Clock::time_point nextDbCheck_{};
    CMakeKnowledge cmakeKb_;
    std::vector<ModuleDecl> modules_;
    bool clangdModules_ = false;
    float panelH_ = 230;   // logical px
    std::array<float, 3> panelScroll_{};
    std::array<bool, 3> panelFollow_{true, true, true};
    int hoverPanelRow_ = -1, hoverTool_ = 0;
    std::array<Rect, 3> panelTabRects_{};
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
    float sidebarWidth_ = 280;   // logical px
    float sideScroll_ = 0, sideTarget_ = 0;
    int hoverRow_ = -1;
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
