// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app — the application: tabs, editor view, prompts, input routing and the frame loop.
export module daemon.app;
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
import daemon.services.toolchain;
import daemon.services.git;
import daemon.services.search;
import daemon.services.gdb;
import daemon.services.terminal;
import daemon.services.lsp;

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

export namespace ide {

class App {
public:
    explicit App(Options opts) : opt_(std::move(opts)) {}
    ~App() { if (dbg_) dbg_->kill(); lsp_.reset(); }  // while the members its callbacks touch are still alive

    std::expected<void, std::string> init();

    // Replays a '|'-separated list of key presses: "ctrl+shift+f", "enter", "left", or "t:literal text".
    void feedKeys(std::string_view spec);

    // "wait" pauses the script until builds are done and the language server (if any) is ready.
    bool scriptReady() const {
        if (tasksRunning() || pendingBuild_) return false;
        return !lsp_ || lsp_->state() != LspClient::State::Starting;
    }

    void stepScript();

    int run();

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

    void newDoc();

    static std::string expandPath(std::string p);

    void openFile(const std::string& raw);

    // ---- folder / sidebar

    // ---- changes made outside the IDE (inotify): the explorer, open documents and git state follow along

    DirWatcher fsw_;
    bool fsPending_ = false;
    Clock::time_point fsEventAt_{}, fsSyncAt_{};

    void syncFsWatches();

    void handleFsChanges();

    void checkDocsOnDisk();

    void openFolder(const std::string& raw);

    // Highlights the active document in the tree (expanding its parents).
    void revealCurrent() {
        if (!sidebar_ || !explorer_.isOpen() || docs_.empty() || doc().path.empty()) return;
        if (explorer_.reveal(doc().path)) sideReveal_ = true;
    }

    void toggleSidebar();

    void focusSidebar();

    void switchTab(int i);

    void closeTab(int i, bool force = false);

    void saveCurrent();

    void requestQuit();

    // ---- sessions: open files, split layout, cursor and scroll positions are kept per project and restored on open
    void saveSession(bool force = false);
    bool restoreSession();
    void sessionTick(Clock::time_point now);

    // ============================================================ messages / timing

    void setMessage(std::string m, bool error = false, float seconds = 4);

    void resetCaret();

    std::optional<Clock::time_point> nextWake() const;

    void update(Clock::time_point now, float dt);

    // Glides one axis of the caret toward its target with exponential easing. Both edges use the
    // same rate, so the caret keeps its exact size while moving. Returns true while still moving.
    static bool chaseEdges(float& lo, float& hi, float tlo, float thi, float dt, float snapDist);

    // Advances the caret animation toward the document's cursor. Coordinates are in content space
    // (independent of scrolling, which animates separately).
    bool stepCaret(const Document& d, float dt);

    void clampSide();

    // Scrolls the file list so the selected row is visible.
    void revealInSidebar();

    void updateTitle();
    void installDesktopEntry();

    void screenshotStep();

    // ============================================================ resize / zoom

    void rescaleViews(float ratio) {
        for (auto& d : docs_) { d->scrollY *= ratio; d->targetY *= ratio; d->scrollX *= ratio; }
    }

    void onResize(int w, int h, int s);

    void zoom(float delta, bool reset = false);

    // ============================================================ activity bar (right edge)

    // Two vertical pills: the right-hand island's views (Explorer, Git), then the bottom panel's (Output, Problems).
    float activityCell() const { return std::round(L_.activity.w - 20 * S()); }
    float activityPadV() const { return std::round(7 * S()); }
    Rect activityPill(int group) const;
    // Buttons by id (0 Explorer, 1 Git, 2 Output, 3 Terminal, 4 Problems, 5 Search, 6 Debug). Pills: Explorer / Search / Git,
    // Output / Terminal / Problems, and Debug on its own.
    static int activityPos(int id) { static constexpr int pos[7] = {0, 2, 3, 4, 5, 1, 6}; return pos[id]; }
    Rect activityRect(int i) const;
    int activityAt(float x, float y) const;
    bool activityActive(int i) const;
    void activityClick(int i);

    void drawActivityBar();

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

    void rebuildMini(Document& d);

    MiniMetrics miniMetrics(const Document& d) const;

    bool inMinimap(float x, float y) const {
        return L_.minimap.w > 0 && L_.minimap.contains(x, y) && x < L_.minimap.right() - 12 * S();
    }

    void miniJump(Document& d);

    void drawMinimap(Document& d);

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
    float uiLabelR(float xRight, float rowY, float rowH, std::string_view t, Color c);
    float uiLabelC(float cx, float rowY, float rowH, std::string_view t, Color c);

    // ============================================================ layout

    // ---- animations

    static float easeInOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

    float tabNaturalW(const std::string& name) {
        const float s = S();
        return std::clamp(14 * s + iconSize() + 8 * s + uiW(name) + 12 * s + 22 * s + 8 * s, 96 * s, 260 * s);
    }

    // Keeps one animated entry per tab of a pane, plus ghosts for tabs that were just closed or moved away.
    void syncTabs(Pane& p);

    // Advances tab / side bar / panel animations. Returns true while anything is still moving.
    bool stepAnimations(float dt);

    void layout();

    // ---- panes

    std::vector<Node*> leaves();
    std::vector<Node*> splits();
    Pane* paneOfDoc(const Document* d);
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
    Node* gapAt(float x, float y);

    static bool pruneNode(std::unique_ptr<Node>& n);

    // Reconciles panes with the document list: drops closed tabs, puts new documents into the active pane,
    // removes empty panes and makes the pane of the current document the active one.
    void syncPanes();

    void layoutTree(Node& n, Rect r, float gap);

    void layoutPane(Pane& p, bool active, float promptH);

    // Draws with a pane's geometry installed in L_ (and its document current).
    struct PaneCtx { Rect tabs, crumbs, editor, minimap; float g, tx, tw; int cur; };
    PaneCtx enterPane(Pane& p);
    void leavePane(Pane& p, const PaneCtx& sv);

    float maxScroll(const Document& d) const {
        return std::max(0.0f, (static_cast<float>(d.buf.lineCount()) - 1.0f) * lh());
    }

    void clampScroll(Document& d);

    void ensureCursorVisible(Document& d);

    // Maps window coordinates to a position in the document.
    Pos posAt(Document& d, float mx, float my) const;

    // ============================================================ input: keyboard

    // Keystrokes for the running program (it sits on a pseudo-terminal, so the kernel echoes and line-edits).
    // Returns false for keys that should keep their normal (global) meaning.
    bool runKey(const KeyEvent& k);

    void onKey(const KeyEvent& k);

    // ============================================================ language server

    static bool isCMakeDoc(const Document& d) { return d.lang && d.lang->name == "CMake"; }
    static bool isCppDoc(const Document& d) { return d.lang && (d.lang->name == "C++" || d.lang->name == "C") && !d.path.empty(); }
    static bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
    static Color diagColor(int sev);

    const LspDiag* diagOnLine(const Document& d, int line) const;

    static std::string languageId(const Document& d) { return d.lang && d.lang->name == "C" && d.path.ends_with(".c") ? "c" : "cpp"; }

    void startLsp();

    // ---- C++20 modules: project module list (for `import` completion and navigation)

    void refreshModules();

    const ModuleDecl* findModule(const std::string& name) const {
        for (const auto& m : modules_) if (m.interface && m.name == name) return &m;
        return nullptr;
    }

    // Completion inside `import <name>;`: module names from the project (and std).
    bool completeImport(Document& d);

    bool gotoModule(Document& d);

    void pumpLsp();

    void stopLsp();

    // clangd died on its own (crash / killed). Its persistent module cache can be what it chokes on (a stale BMI left
    // over from an older version of a module crashes clangd's module reader), so clear it and start again; give up
    // when it keeps happening.
    void handleLspCrash(Clock::time_point now);

    void restartLsp();

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

    std::vector<std::string> projectSources();

    bool openInEditor(const std::string& path) const {
        return std::ranges::any_of(docs_, [&](const auto& d) { return d->path == path; });
    }

    void queueProjectDiagnostics();

    void pumpProjectDiagnostics(Clock::time_point now);

    void attachLsp(Document& d);

    void lspDetach(Document& d);

    void flushDoc(Document& d);

    // Sends edits to the server shortly after typing stops.
    void lspSync(Clock::time_point now);

    static std::pair<int, int> lspPos(const Document& d, Pos p) { return {p.line, utf16Col(d.buf.line(p.line), p.col)}; }

    // ---- completion

    static int matchScore(std::string_view prefix, std::string_view text);

    void closeCompletion() { if (comp_.open) { comp_ = {}; dirty_ = true; } }

    void requestCompletion(int triggerKind);

    void showCompletion(std::vector<LspCompletion> items, Pos at, int anchorCol = -1);

    // CMake files: completion is computed locally from the installed cmake's own name lists.
    void requestCMakeCompletion();

    void refilterCompletion();

    bool completionKey(const KeyEvent& k);

    void acceptCompletion();

    // Runs after every editor key: keeps the popup in sync and opens it when typing identifiers.
    void afterEditorKey(const KeyEvent& k, std::uint64_t verBefore);

    struct KindIcon { char32_t glyph; Color color; };
    // LSP CompletionItemKind -> Remix icon + VS Code-like colour.
    static KindIcon completionKindIcon(int kind);

    static constexpr int kCompletionRows = 10;

    void drawCompletion();

    // ---- hover (diagnostics + clangd documentation)

    void hideHover() {
        if (hover_.pending || hover_.show) { hover_.pending = hover_.show = false; ++hoverSeq_; hsel_ = {}; dirty_ = true; }
    }

    void hoverMoved();

    static std::string cleanMarkdown(const std::string& md);

    void addHoverText(const std::string& text, int sev);

    void hoverTick(Clock::time_point now);

    // ---- tooltip text selection

    Rect hoverRect_, hoverCopyRect_;

    std::pair<int, int> hoverPosAt(float x, float y);
    void hoverSelAnchor();
    void hoverSelUpdate();

    void drawHover();

    // ---- navigation

    void jumpTo(const std::string& path, int line, int u16col);

    void gotoDefinition();

    void navBack();

    void switchHeaderSource();

    // ============================================================ CMake: configure / build / run

    CMakeProfile& profile() { return profiles_[static_cast<std::size_t>(profile_)]; }

    // Called when a folder is opened: enables the toolbar and loads (or creates) the build configuration.
    void detectCMake();

    void loadTargetsNow();

    void selectProfile(int i);

    void saveAllForBuild() {
        for (auto& d : docs_)
            if (d->modified() && !d->path.empty()) { flushDoc(*d); (void)d->save(); if (lsp_ && d->lspOpen) lsp_->didSave(d->path); }
    }

    void startBuildProc(TaskKind kind, std::vector<std::string> argv, const std::string& title);

    void cmakeReload();

    void cmakeBuild(bool thenRun, bool debug = false);

    void cmakeStop();

    bool tasksRunning() const { return buildProc_ || runProc_; }

    // Terminal text -> plain text: strips ANSI sequences and applies backspaces (the echo of a deleted character
    // arrives as "\b \b"); other control characters are dropped.
    static std::string cleanTerm(std::string_view raw);

    static std::vector<std::string> takeLines(std::string& partial, std::string_view chunk);

    void addBuildLine(const std::string& raw);

    void pumpBuild();

    void finishBuild(int code);

    void startRun(bool debug = false);

    void pumpRun();

    void finishRun(int code);

    // Detects finished child processes (their output has reached EOF).
    void pollTasks() {
        if (buildProc_ && buildEof_) { if (auto c = buildProc_->tryWait()) finishBuild(*c); }
        if (runProc_ && runEof_) { if (auto c = runProc_->tryWait()) finishRun(*c); }
    }

    // Problems tab content: compiler / CMake problems from the last build plus clangd's live diagnostics
    // (errors, warnings and notes; hints are left out), ordered by file and line.
    void refreshProblems();

    void gotoProblem(const Problem& p);

    // ---- toolbar

    void togglePanel() {
        panelOpen_ = !panelOpen_;
        dirty_ = true;
    }

    void toolbarClick();

    int toolAt(float x, float y) const;

    void drawToolbar();

    // Tooltip for the toolbar buttons (name and shortcut), shown after the pointer rests on one.
    void drawToolTip();

    // ---- output panel

    const std::vector<OutLine>& panelLines() const { return panelTab_ == 1 ? runOut_ : buildOut_; }

    static Color outColor(int sev);

    std::size_t panelRowCount() const;
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

    std::string panelRowText(int i) const;

    // Row and byte column under a pointer position inside the panel body.
    std::pair<int, int> panelPosAt(float x, float y) const;

    static bool selOrdered(const TextSel& t) { return t.ar < t.cr || (t.ar == t.cr && t.ac <= t.cc); }
    static std::string selectedOf(const TextSel& t, const std::function<std::string(int)>& rowText, int rows);

    // Ctrl+C: output selection first, then the hover popup. Returns true when it handled the key.
    bool copyOverlay();

    void panelRowClick(int row);

    void panelClick(Button b);

    void drawPanel();

    // ============================================================ file operations (sidebar)

    // Directory that new items / pastes go into: the selected folder, or the selected file's parent.
    fs::path targetDir() const;

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

    void drawSidePrompt();

    // ---- dragging files inside the explorer to move them
    struct FileDrag {
        fs::path path;
        bool pending = false, active = false;
        float sx = 0, sy = 0;
        fs::path target;   // folder under the pointer (empty: not a valid drop)
    };
    FileDrag fdrag_;

    fs::path fileDropTarget();

    void finishFileDrag();

    void drawFileDrag();

    void openFileOp(PromptKind kind, const fs::path& target, std::string label, std::string text = {}, bool fresh = false);

    void beginNewFile(const fs::path& dir) { openFileOp(PromptKind::NewFile, dir, "New file in " + relToRoot(dir) + "/"); }
    void beginNewFolder(const fs::path& dir) { openFileOp(PromptKind::NewFolder, dir, "New folder in " + relToRoot(dir) + "/"); }
    void beginRename(const fs::path& p) {
        if (isItem(p)) openFileOp(PromptKind::Rename, p, "Rename", p.filename().string(), true);
    }
    void beginDelete(const fs::path& p, bool forever);

    void stageClipboard(const fs::path& p, bool cut);

    // Refreshes the tree and (optionally) selects `focus`.
    void afterFsChange(const fs::path& focus);

    // Keeps open documents attached to files that were renamed / moved.
    void retargetDocs(const fs::path& from, const fs::path& to);

    void pasteFiles(const fs::path& dir);

    void duplicateItem(const fs::path& p);

    void finishFileOp(const std::string& text);

    // ---- context menu

    void closeMenu() { menu_.open = false; dirty_ = true; }

    void openMenu(float x, float y, const std::string* path);

    void layoutMenu(float x, float y);

    // A pop-up list of arbitrary actions (profile / target selectors).
    void openCustomMenu(float x, float y, const std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>>& entries);

    int menuItemAt(float px, float py) const;

    void runAct(Act a, const std::string& target);

    void activateMenuItem(int i);

    void menuKey(const KeyEvent& k);

    void drawMenu();

    // Keyboard navigation while the sidebar has focus. Returns false for keys it does not own
    // (Ctrl/Alt shortcuts), which then fall through to the normal handlers.
    bool explorerKey(const KeyEvent& k);

    void activateRow(FileNode& node);

    void editorKey(const KeyEvent& k);

    void editorKeyImpl(const KeyEvent& k);

    void pageMove(Document& d, int dir, bool extend);

    void copy(bool cut);

    void paste() {
        std::string t = win_.clipboard();
        if (!t.empty()) doc().insertRaw(t);
    }

    // ============================================================ prompts

    void openPrompt(PromptKind kind);

    void closePrompt() {
        prompt_ = {};
        dirty_ = true;
    }

    bool inFindMode() const { return prompt_.kind == PromptKind::Find || prompt_.kind == PromptKind::Replace; }

    void promptChanged();

    void promptKey(const KeyEvent& k);

    void submitPrompt(const KeyEvent& k);

    // Tab completion for file prompts: extends to the longest common prefix.
    void completePath(Field& f);

    // ============================================================ input: pointer

    Edge edgeAt(float x, float y) const;

    static Cursor cursorForEdge(Edge e);

    // 1 = minimise, 2 = maximise, 3 = close
    int titleButtonAt(float x, float y) const;

    bool inScrollbar(const Document& d, float x, float y) const {
        return L_.editor.contains(x, y) && x >= L_.editor.right() - 14 * S() && maxScroll(d) > 0;
    }

    void onPointerMove(float lx, float ly);

    // Applies an in-progress mouse drag (selection or scrollbar) at the current pointer position.
    void dragStep(Document& d);

    // ---- tab drag & drop (reorder, move between panes, split)

    void updateDropTarget();

    void moveTab(Document* doc, Pane* to, int at);

    // Moves `doc` into a new pane created next to `target` (on the side given by `z`).
    void splitPane(Pane* target, Zone z, Document* doc);

    void performDrop();

    // Floating tab under the pointer plus the drop indicator of the target pane.
    void drawTabDrag();

    void onPointerButton(Button b, bool pressed, float lx, float ly);

    void onScroll(float dxPx, float dyPx, int stepsX, int stepsY);

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
    void pollJobs();
    bool docAlive(const Document* d) const {
        return std::ranges::any_of(docs_, [&](const auto& x) { return x.get() == d; });
    }

    void gitDetect();

    void gitReset();

    std::string gitRel(const std::string& absPath) const;

    void gitRefresh();

    // Per visible document: HEAD content, change markers and blame (all computed off the UI thread or cheaply here).
    void gitUpdateDoc(Document& d, Clock::time_point now);
    Clock::time_point nextBlameWake_{};

    void gitTick(Clock::time_point now);

    static std::string blameText(const git::Commit& c);

    // The commit that last changed line `ln` of `d` (null when blame is not available for the current text).
    const git::Commit* blameOf(const Document& d, int ln) const;

    // Hover card for the commit behind the blame annotation (fetched once, then cached).
    void showBlameCard(const std::string& hash, bool uncommitted);

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

    void gitUpdateHover();

    // Runs a git action on a worker thread, then refreshes the status and shows the outcome.
    void gitAction(std::function<git::Result(const fs::path&)> fn, std::string okMsg = {});

    void gitCommit();

    bool gitMsgKey(const KeyEvent& k);

    void gitOpenEntry(const git::Entry& e);

    void gitPanelClick();

    static Color gitLetterColor(char ch) {
        return (ch == 'U' || ch == 'A') ? Color::rgb(0x73c990) : (ch == 'D' || ch == '!') ? Color::rgb(0xe06c75) : ch == 'R' ? Color::rgb(0x61afef) : Color::rgb(0xe5c07b);
    }

    void drawGitChanges(const Rect& body);

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
    void buildGraph();

    // Loads the lists the current sub-tab needs (off the UI thread).
    void gitEnsureLoaded();

    void gitToggleCommit(const std::string& hash);

    static Color laneColor(int lane) {
        static constexpr std::array<std::uint32_t, 7> pal = {0xe5484d, 0x5b9bff, 0x73c990, 0xe5c07b, 0xc586c0, 0x4ec9b0, 0xf0a04b};
        return Color::rgb(pal[static_cast<std::size_t>(lane) % pal.size()]);
    }

    // A straight or slanted line drawn from small squares (the renderer only has rects / rounded rects).
    void gLine(float x0, float y0, float x1, float y1, Color c, float w);

    void drawChip(float& x, float y, float h, const std::string& text, Color c);

    void drawGitHeader(const Rect& body, float headH);

    void drawGitHistory(const Rect& body);

    void drawGitBranches(const Rect& body);

    void drawGitBody(const Rect& body);

    // ============================================================ integrated terminal (bottom panel, "Terminal" tab)

    Terminal term_{80, 24};
    std::unique_ptr<Process> termProc_;
    bool termFocus_ = false, termExited_ = false;
    int termScroll_ = 0;           // lines scrolled back from the live screen
    TextSel tsel_;                 // selection in absolute terminal rows (see Terminal::absRow)
    int termDrawCols_ = 0, termDrawRows_ = 0;

    void startTerminal();

    void pumpTerminal();

    void openTerminal();

    static std::string termMods(const KeyEvent& k) {
        const int m = 1 + (k.shift ? 1 : 0) + (k.alt ? 2 : 0) + (k.ctrl ? 4 : 0);
        return std::to_string(m);
    }

    // Translates a key press into what a terminal sends. Returns false for keys that keep their IDE meaning.
    bool termKey(const KeyEvent& k);

    std::pair<int, int> termPosAt(float x, float y, const Rect& body) const;

    std::string termSelectedText() const;

    void termCopy();

    void termClick(const Rect& body);

    static Color termColor(std::uint32_t c, Color def) { return c ? Color::rgb(c & 0xFFFFFF) : def; }

    void drawTerminalBody(const Rect& body);

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

    search::Options searchOptions() const;

    void openSearch(bool replace);

    void scheduleSearch() { sPending_ = true; sRunAt_ = Clock::now() + std::chrono::milliseconds(280); dirty_ = true; }

    void rebuildSearchRows();

    void recountSearch();

    void startSearch();

    void searchOpenMatch(int fi, int mi);

    // fi/mi: -1/-1 everything, fi/-1 one file, fi/mi a single occurrence.
    void searchReplace(int fi, int mi);

    void searchDismiss(int fi, int mi);

    bool searchKey(const KeyEvent& k);

    void searchClick(Button b);

    void searchScroll(float dy) {
        sScroll_ = std::clamp(sScroll_ + dy, 0.0f, sMax_);
        dirty_ = true;
    }

    void searchField(const Rect& r, TextBox& t, bool focused, int kind, std::string_view placeholder, float rightPad);

    // Small toggle ("Aa", "ab", ".*") docked inside the right edge of a field; returns its left edge.
    float searchChip(float right, const Rect& field, std::string_view label, bool on, int kind);

    void drawSearchBody();

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

    void dbgConsole(std::string text, int kind = 0);
    void dbgConsoleStream(const std::string& t);

    // ---- breakpoints

    fs::path dbgFile() const { return explorer_.isOpen() ? explorer_.rootPath() / ".daemon" / "debug.json" : fs::path(); }

    void dbgSave();

    void dbgLoad();

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

    std::string dbgBpTitle(const Breakpoint& b) const;

    // gdb refuses breakpoint changes while the program runs: interrupt, apply, continue.
    void dbgWhenPaused(std::function<void()> fn);

    static std::string dbgBpCommand(const Breakpoint& b);

    void dbgInsert(int uid);

    void dbgAddBp(Breakpoint b);

    void dbgRemoveBp(int uid);

    void dbgToggleBpAt(const std::string& file, int line);

    void dbgSetEnabled(int uid, bool on);

    // Edit text: "expr" = condition, "#N" = ignore the first N hits, "log: text {expr}" = logpoint, empty = plain breakpoint.
    static std::string dbgBpEditText(const Breakpoint& b);
    void dbgApplyBpEdit(int uid, std::string t);

    // ---- starting and ending a session

    void debugLaunch(const DbgLaunch& L);

    void pumpDebug();

    void dbgScheduleEnd(std::string why, int code) { dbgEndPending_ = true; dbgEndWhy_ = std::move(why); dbgEndCode_ = code; }

    void dbgEnd(const std::string& why, int code, bool quiet);

    void debugStop() { if (dbg_) dbgEnd("Debugging stopped", 0, false); }

    // ---- events from gdb

    void dbgAsync(const gdb::Record& r);

    void dbgOnStopped(const json::Value& d);

    void dbgRunLogpoint(const std::string& msg);

    // Shows the source line of a frame in the editor and marks it as the execution line.
    void dbgShowFrame(const DbgFrame& f);

    void dbgRefreshStack();

    void dbgSelectFrame(int level);

    void dbgSelectThread(int id);

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
    void dbgRestart();
    void dbgRunToCursor();
    void dbgSetNextStatement();

    // ---- variables and watches

    void dbgCollect(const std::vector<VarP>& v, std::map<std::string, std::string>& out) {
        for (const auto& n : v) { out[n->path] = n->value; dbgCollect(n->kids, out); }
    }

    void dbgRefreshVars();

    void dbgCreateVar(const VarP& v, int gen);

    // Children of a varobj, with C++ access-specifier pseudo nodes (public / private / protected) spliced into the list.
    void dbgFetchKids(const std::string& vo, const std::string& parentPath, int gen, std::function<void(std::vector<VarP>, bool)> done);

    void dbgExpand(const VarP& v, int gen);

    void dbgToggleVar(const VarP& v);

    void dbgAddWatch(const std::string& e);

    void dbgSetValue(const VarP& v, const std::string& value);

    // ---- console input

    std::deque<std::string> dbgEvalQ_;
    bool dbgEvalBusy_ = false;

    void dbgEval(const std::string& text);

    void dbgEvalNext();

    bool dbgConKey(const KeyEvent& k);

    void drawDebugConsole(const Rect& body);
    Rect dbgConIn_;

    // ---- the Run and Debug sidebar view

    struct DRow { int kind = 0, idx = 0, depth = 0; VarP var; };   // 1 section, 2 var, 3 hint, 4 thread, 5 frame, 6 breakpoint, 7 status/start
    std::vector<DRow> dbgRows_;
    struct DBtn { Rect r; int id; };

    void dbgBuildRows();

    // Hover buttons of a row (right-aligned), by action id.
    std::vector<std::pair<int, char32_t>> dbgRowActions(const DRow& r) const;

    std::vector<float> dbgY_;   // top of every row (relative to the list), filled by dbgLayoutRows

    float dbgRowH(const DRow& r) const;
    void dbgLayoutRows();
    int dbgRowAt(float yRel) const {
        for (std::size_t i = 0; i + 1 < dbgY_.size(); ++i) if (yRel >= dbgY_[i] && yRel < dbgY_[i + 1] - (i + 2 == dbgY_.size() ? 12 * S() : 0)) return static_cast<int>(i);
        return -1;
    }

    void drawDebugBody();

    void dbgPromptFor(PromptKind k, const std::string& label, const std::string& text = {}) {
        openFileOp(k, {}, label, text, !text.empty());
    }

    void debugClick(Button b);

    // The full C expression of a variable (members of members, array elements...), asked from gdb for children.
    void dbgVarExpr(const VarP& v, std::function<void(const std::string&)> done);

    void dbgAction(int id, const DRow& r);

    void dbgFinishPrompt(PromptKind k, const std::string& t);

    void debugScroll(float dy) {
        dbgScroll_ = std::clamp(dbgScroll_ + dy, 0.0f, dbgMax_);
        dirty_ = true;
    }

    // ---- floating control bar over the editor

    struct DbgBarBtn { Rect r; int cmd; };
    std::vector<DbgBarBtn> dbgBar_;

    void drawDebugBar();
    std::string dbgBarTip_;
    Rect dbgBarTipRect_;

    void dbgCommand(int cmd);

    bool dbgBarClick();

    // Debugger keys: F5 continue / start, Shift+F5 stop, Ctrl+Shift+F5 restart, F6 pause, F9 breakpoint, F10 over, F11 into,
    // Shift+F11 out, Ctrl+F10 run to cursor, Ctrl+Shift+F10 set next statement.
    bool debugKey(const KeyEvent& k);

    // Editor context menu (right click): breakpoints, debugger actions and the usual clipboard entries.
    void openEditorMenu(Pos p);

    // Breakpoint dots and the execution marker in the gutter of `d`, plus the execution line highlight in the text area.
    void drawDebugGutter(const Document& d, int ln, float y, float lineH);

    // ============================================================ project dialogs (welcome / new project / folder browser)

    enum DlgId : int {
        kDlgNew = 1, kDlgOpen = 2, kDlgRecent = 100, kDlgRecentDel = 200, kDlgTmpl = 300,
        kDlgName = 400, kDlgLoc = 401, kDlgBrowseBtn = 402, kDlgStd = 410, kDlgGit = 420, kDlgCreate = 430, kDlgCancel = 431,
        kDlgBm = 500, kDlgPath = 550, kDlgUp = 551, kDlgHidden = 552, kDlgOk = 553, kDlgBrowseCancel = 554, kDlgEntry = 600,
        kDlgSetup = 3, kDlgSetupCopy = 700, kDlgSetupRun = 701, kDlgSetupRecheck = 702, kDlgSetupClose = 703, kDlgSetupMute = 704, kDlgSetupDl = 710,
    };
    enum class DlgKind { None, Welcome, NewProject, Browse, Setup };
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
    // Toolchain check (app/impl/setup.cpp): what is missing, and how to install it on this distribution.
    std::vector<toolchain::Status> tools_;
    toolchain::Distro distro_;
    std::string toolsCmd_;
    bool toolsChecking_ = false;
    struct ToolDownload { std::string id; std::shared_ptr<toolchain::Progress> progress; };
    std::optional<ToolDownload> toolDl_;     // one managed download at a time
    std::string toolDlError_;
    void toolsDownload(const std::string& id);
    void toolsCheck(bool openDialog);
    void setupOpen() { dlg_ = {}; dlg_.kind = DlgKind::Setup; dirty_ = true; toolsCheck(false); }
    void drawSetup(const Rect& r);
    void setupClick(int id);
    void welcomeOpen() { dlg_ = {}; dlg_.kind = DlgKind::Welcome; dirty_ = true; }

    std::string uniqueProjectName(const std::string& parent, const std::string& base);

    void newProjectOpen();

    void browseRefresh();

    // Folder choosers: the desktop's own dialog (zenity talks to GTK / the portal, so it is GNOME's file manager dialog);
    // the built-in browser is only the fallback when no system dialog is available.
    bool pickerBusy_ = false;
    void browseOpen(bool forLocation);

    void browseOpenBuiltin(bool forLocation);

    void browseGo(const std::string& dir);

    void browseConfirm();

    void openProject(const std::string& path);

    void createProjectFromDialog();

    // ---- text boxes

    bool editText(TextBox& t, const KeyEvent& k);

    std::size_t caretFromX(const std::string& t, float relX);

    // ---- input

    void dlgKey(const KeyEvent& k);

    void dlgScrollToSel();

    void dlgClick(Button b, float x, float y);

    void dlgScroll(float dy);

    // ---- drawing helpers

    void dlgHit(const Rect& r, int id) { dlgHits_.push_back({r, id}); }
    bool dlgHot(int id) const { return dlg_.hover == id; }

    void dlgButton(const Rect& r, std::string_view label, int id, bool primary, char32_t icon = 0);

    void dlgField(const Rect& r, const TextBox& t, bool focused, int id, std::string_view placeholder);

    void drawDialog();

    // The dialogs are built from islands like the main window: a narrow one on the left, the main one on the right.
    void dlgSide(const Rect& r, float w);

    // One clickable row of the welcome screen's action list: icon tile, label, shortcut.
    void welcomeAction(const Rect& row, std::string_view label, std::string_view keys, int id, char32_t icon, bool primary);

    void drawWelcome(const Rect& r);

    void dlgCheckbox(const Rect& r, bool on, std::string_view label, int id);

    void drawNewProject(const Rect& r);

    void drawBrowse(const Rect& r);

    Rect dlgPanel_;
    std::vector<std::string> dlgRecentShown_, dlgPlaces_;
    float dlgRecentMax_ = 0, browseListH_ = 200;

    // ============================================================ drawing

    // Floating overlay (menu, popup, tooltip): soft shadow, hairline rim, and a frosted-glass body that blurs the
    // scene behind it (see shaders/glass.frag).
    void glassPanel(const Rect& r, float radius);

    // Translucent chrome colour (opaque when the compositor path has no alpha), and carving it out of the panels.
    Color frameC() { Color c = theme::frame; if (!ren_.translucent()) c.a = 255; return c; }
    void carve(const Rect& r, float radius = 0);
    void fill(float x, float y, float w, float h, Color c, float radius = 0) { dl_.rect(font_, x, y, w, h, c, radius); }
    void fill(const Rect& r, Color c, float radius = 0) { dl_.rect(font_, r.x, r.y, r.w, r.h, c, radius); }
    float label(float x, float y, std::string_view t, Color c, float maxX = std::numeric_limits<float>::infinity()) {
        return dl_.text(font_, x, y, t, c, maxX);
    }

    void draw();

    void drawTitleBar();

    int iconPx() const { return std::max(12, static_cast<int>(std::round(font_.uiLineHeight() * 1.02f))); }
    float iconSize() const { return static_cast<float>(iconPx()); }

    void drawFileIcon(float x, float y, std::string_view name, float alpha = 1.0f) {
        Icons::file(dl_, font_, x, y, iconPx(), name, alpha);
    }
    void drawFolderIcon(float x, float y, std::string_view name, bool open, float alpha = 1.0f) {
        Icons::folder(dl_, font_, x, y, iconPx(), name, open, alpha);
    }

    void drawSidebar();

    void drawTabs(Pane& p);

    // Breadcrumbs: project-relative path of the active file.
    // Path segments shown in the breadcrumb row (project-relative when a folder is open).
    std::vector<std::string> crumbParts(const Document& d) const;

    void drawCrumbs();

    void drawEditor(Document& d);

    void drawPrompt();

    std::string matchSummary();

    void drawStatus();

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
    // Background diagnostics keep a small rolling window of files open in clangd (each open file costs an AST/preamble):
    // bgPending_ = opened, no diagnostics yet; bgSettled_ = in the window, finished; bgClosed_ = closed, diagnostics kept.
    std::unordered_set<std::string> bgPending_, bgClosed_;
    std::deque<std::string> bgSettled_;
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
    bool sessionOn_ = false;                 // off for --screenshot runs and when files were named on the command line
    std::string lastSession_;                // last written session text (to skip identical writes)
    Clock::time_point nextSessionCheck_{};
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
    Clock::time_point caretReset_ = Clock::now();
    bool blinking(Clock::time_point now) const;
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
