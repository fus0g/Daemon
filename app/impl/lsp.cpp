// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / lsp - part of the App implementation (see app.cppm for the class).
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

Color App::diagColor(int sev) {
    switch (sev) {
        case 1: return Color::rgb(0xf2777a);
        case 2: return Color::rgb(0xe5c07b);
        case 3: return Color::rgb(0x7fb7ff);
        default: return Color::rgb(0x8a8e98);
    }
}

const LspDiag* App::diagOnLine(const Document& d, int line) const {
    auto it = diags_.find(d.path);
    if (it == diags_.end()) return nullptr;
    const LspDiag* best = nullptr;
    for (const auto& dg : it->second)
        if (dg.line <= line && line <= dg.endLine && (!best || dg.severity < best->severity)) best = &dg;
    return best;
}

std::string App::clangdName() const {
    if (const char* env = std::getenv("IDE_CLANGD"); env && *env) return env;
    if (auto s = cfgS("lsp.clangd"); !s.empty()) return s;
    return "clangd";
}

void App::startLsp() {
    if (lsp_ && lsp_->running()) return;
    if (!cfgB("lsp.enabled")) { lspStatus_ = "clangd off (Settings)"; return; }
    auto exe = findExecutable(clangdName());
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
                                     "--completion-parse=always",  // real AST completions, never the text-based fallback
                                     // Foreground work (completion, diagnostics) runs at normal priority; only the background indexing is
                                     // idle priority. The preamble stays in memory: reading it back from disk costs on every keystroke.
                                     "-j=2", "--limit-results=1000", "--background-index-priority=low", "--pch-storage=memory", "--malloc-trim"};
    if (!cfgB("lsp.clangTidy")) argv.push_back("--clang-tidy=false");   // tidy checks run after every parse and make completion wait behind them
    if (const char* o = std::getenv("IDE_BGINDEX"); (o && *o == '0') || (!o && !cfgB("lsp.backgroundIndex"))) std::erase(argv, std::string("--background-index"));
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
        if (bgClosed_.contains(path)) return;   // clangd's empty publish after we closed a background file
        if (bgPending_.erase(path)) bgSettled_.push_back(path);
        if (d.empty()) diags_.erase(path); else diags_[path] = std::move(d);
        mergeEmbeddedDiags(path);   // keep the shader diagnostics of raw strings
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

void App::refreshModules() {
    modules_.clear();
    if (!cmakeOpen_) return;
    modules_ = scanModules(compileCommands(profile()));
}

bool App::completeImport(Document& d) {
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

bool App::gotoModule(Document& d) {
    auto name = importNameAt(d.buf.line(d.cursor.line), static_cast<std::size_t>(d.cursor.col));
    if (!name) return false;
    std::string full = *name;
    if (full.starts_with(':')) {  // partition of the current module: ide.app + :part
        for (const auto& m : modules_)
            if (m.path == fs::path(d.path)) { full = m.name.substr(0, m.name.find(':')) + full; break; }
    }
    const ModuleDecl* m = findModule(full);
    if (!m) return false;
    navPush({d.path, d.cursor});
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

void App::pumpLsp() {
    if (!lsp_) return;
    lsp_->pump();
    dirty_ = true;
    if (lsp_->fd() < 0) { win_.unwatch(lspFd_); lspFd_ = -1; }
}

void App::stopLsp() {
    if (lspFd_ >= 0) win_.unwatch(lspFd_);
    lspFd_ = -1;
    lsp_.reset();
    compReq_ = -1;
    diags_.clear();
    bgQueue_.clear(); bgOpen_.clear(); bgSources_.clear(); bgScanned_ = false;
    healTries_ = 0; nextHeal_ = {};
    closeCompletion();
    for (auto& d : docs_) d->lspOpen = false;
}

void App::handleLspCrash(Clock::time_point now) {
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

void App::restartLsp() {
    const bool had = lsp_ != nullptr;
    stopLsp();
    lspStatus_.clear();
    if (had || std::ranges::any_of(docs_, [](const auto& d) { return isCppDoc(*d); })) {
        if (std::ranges::any_of(docs_, [](const auto& d) { return isCppDoc(*d); })) startLsp();
    }
}

std::vector<std::string> App::projectSources() {
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

bool App::transientDiag(const LspDiag& d) {
    if (d.severity != 1) return false;
    std::string m = d.message;
    for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return m.find("not found") != std::string::npos || m.find("could not build module") != std::string::npos ||
           m.find("failed to build module") != std::string::npos || m.find("unknown type name") != std::string::npos;
}

void App::refreshStaleDiagnostics(bool onlyTransient) {
    if (!lsp_ || !lsp_->ready()) return;
    std::vector<std::string> paths;
    for (const auto& [path, ds] : diags_)
        if (std::ranges::any_of(ds, [&](const LspDiag& d) { return onlyTransient ? transientDiag(d) : d.severity == 1; })) paths.push_back(path);
    for (const std::string& path : paths) {
        Document* open = nullptr;
        for (auto& d : docs_) if (d->path == path && d->lspOpen) { open = d.get(); break; }
        diags_.erase(path);
        if (open) {   // an open file is simply parsed again
            lsp_->didClose(path);
            lsp_->didOpen(path, languageId(*open), ++open->lspVersion, open->buf.text());
            open->lspSent = open->buf.version();
        } else {      // a background file goes back to the front of the queue
            if (bgOpen_.erase(path)) lsp_->didClose(path);
            bgClosed_.erase(path);
            bgPending_.erase(path);
            std::erase(bgSettled_, path);
            if (!std::ranges::contains(bgQueue_, path)) bgQueue_.push_front(path);
            bgNext_ = Clock::now();
        }
    }
    if (!paths.empty()) dirty_ = true;
}

// clangd builds the module cache and the project's generated headers appear while we are already collecting diagnostics:
// look again a few times after start-up.
void App::healDiagnostics(Clock::time_point now) {
    if (!lsp_ || !lsp_->ready()) return;
    if (nextHeal_ == Clock::time_point{}) { nextHeal_ = now + std::chrono::seconds(25); return; }
    if (healTries_ >= 3 || now < nextHeal_) return;
    ++healTries_;
    nextHeal_ = now + std::chrono::seconds(45);
    refreshStaleDiagnostics(true);
}

void App::queueProjectDiagnostics() {
    bgQueue_.clear();
    bgOpen_.clear();
    bgSources_.clear();
    bgPending_.clear(); bgClosed_.clear(); bgSettled_.clear();
    for (auto& p : projectSources()) { bgQueue_.push_back(p); bgSources_.insert(p); }
    bgNext_ = Clock::now();
}

void App::pumpProjectDiagnostics(Clock::time_point now) {
    if (!lsp_ || !lsp_->ready() || bgQueue_.empty() || now < bgNext_) return;
    // close the oldest finished files so memory stays flat, but keep their diagnostics
    constexpr std::size_t kWindow = 8;
    while (bgSettled_.size() > kWindow) {
        const std::string old = bgSettled_.front();
        bgSettled_.pop_front();
        if (bgOpen_.erase(old)) { bgClosed_.insert(old); lsp_->didClose(old); }
    }
    if (bgPending_.size() >= 2) { bgNext_ = now + std::chrono::milliseconds(300); return; }   // let clangd catch up
    for (int n = 0; n < 1 && !bgQueue_.empty(); ++n) {   // trickle: a burst of opens pegs every core
        const std::string path = bgQueue_.front();
        bgQueue_.pop_front();
        if (openInEditor(path) || bgOpen_.contains(path)) continue;
        auto text = readFile(path);
        if (!text) continue;
        lsp_->didOpen(path, path.ends_with(".c") ? "c" : "cpp", 1, *text);
        bgOpen_.insert(path);
        bgClosed_.erase(path);
        bgPending_.insert(path);
    }
    bgNext_ = now + std::chrono::milliseconds(500);
}

void App::attachLsp(Document& d) {
    if (!lsp_ || !isCppDoc(d) || d.lspOpen) return;
    bgPending_.erase(d.path); bgClosed_.erase(d.path);
    std::erase(bgSettled_, d.path);
    if (bgOpen_.erase(d.path)) lsp_->didClose(d.path);   // the editor takes over from the background copy
    lsp_->didOpen(d.path, languageId(d), ++d.lspVersion, d.buf.text());
    d.lspOpen = true;
    d.lspSent = d.buf.version();
}

void App::lspDetach(Document& d) {
    if (lsp_ && d.lspOpen) lsp_->didClose(d.path);
    d.lspOpen = false;
    diags_.erase(d.path);
    if (lsp_ && bgSources_.contains(d.path)) bgQueue_.push_back(d.path);   // keep watching it in the background
}

void App::flushDoc(Document& d) {
    if (lsp_ && d.lspOpen && d.buf.version() != d.lspSent) {
        lsp_->didChange(d.path, ++d.lspVersion, d.buf.text());
        d.lspSent = d.buf.version();
    }
}

void App::lspSync(Clock::time_point now) {
    if (!lsp_) { lspFlushAt_.reset(); return; }
    const bool pending = std::ranges::any_of(docs_, [](const auto& d) { return d->lspOpen && d->buf.version() != d->lspSent; });
    if (!pending) { lspFlushAt_.reset(); lspDirtySince_ = {}; return; }
    // Wait for a pause in typing (each didChange makes the server parse the file again, and a parse in progress delays the next
    // completion), but never longer than a second.
    std::uint64_t sig = 0;
    for (const auto& d : docs_) sig += d->buf.version();
    if (lspDirtySince_ == Clock::time_point{}) lspDirtySince_ = now;
    if (sig != lspSig_) { lspSig_ = sig; lspLastEdit_ = now; }
    const auto due = std::min(lspLastEdit_ + std::chrono::milliseconds(180), lspDirtySince_ + std::chrono::milliseconds(1000));
    lspFlushAt_ = due;
    if (now >= due) {
        for (auto& d : docs_) flushDoc(*d);
        lspFlushAt_.reset();
        lspDirtySince_ = {};
    }
}

// How well `prefix` matches `text`: 0 same-case prefix, 1 prefix ignoring case, 2 the capitals / word starts (gw -> GLFWwindow,
// vi -> vector_iterator), 3 any subsequence (two characters or more), -1 no match.
int App::matchScore(std::string_view prefix, std::string_view text) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (prefix.empty()) return 0;
    if (text.starts_with(prefix)) return 0;
    auto lower = [](char c) { return std::tolower(static_cast<unsigned char>(c)); };
    bool prefixCI = text.size() >= prefix.size();
    for (std::size_t i = 0; prefixCI && i < prefix.size(); ++i) prefixCI = lower(text[i]) == lower(prefix[i]);
    if (prefixCI) return 1;
    if (prefix.size() < 2) return -1;
    auto wordStart = [&](std::size_t j) {
        if (j == 0) return true;
        const unsigned char c = static_cast<unsigned char>(text[j]), p = static_cast<unsigned char>(text[j - 1]);
        if (p == '_' || p == ':' || p == ' ') return std::isalnum(c) != 0;
        if (std::isupper(c) && !std::isupper(p)) return true;                                          // fooBar
        return std::isupper(c) && std::isupper(p) && j + 1 < text.size() && std::islower(static_cast<unsigned char>(text[j + 1]));   // HTTPServer
    };
    std::size_t pi = 0;
    bool prevMatched = false;
    for (std::size_t j = 0; j < text.size() && pi < prefix.size(); ++j) {
        if (lower(text[j]) == lower(prefix[pi]) && (pi == 0 || prevMatched || wordStart(j))) { ++pi; prevMatched = true; }
        else prevMatched = false;
    }
    if (pi == prefix.size()) return 2;
    pi = 0;
    for (char c : text)
        if (pi < prefix.size() && lower(c) == lower(prefix[pi])) ++pi;
    return pi == prefix.size() ? 3 : -1;
}

// Types and objects before functions before values before macros (LSP CompletionItemKind numbers).
int App::kindTier(int kind) {
    switch (kind) {
        case 7: case 8: case 13: case 22: case 25: return 0;   // class, interface, enum, struct, type parameter
        case 9: case 14: return 1;                              // module / namespace, keyword
        case 2: case 3: case 4: case 15: return 2;              // method, function, constructor, snippet
        case 1: return 4;                                       // clangd reports macros as plain text
        default: return 3;                                      // variables, fields, constants, enum members, ...
    }
}

void App::requestCompletion(int triggerKind) {
    if (docs_.empty()) return;
    Document& d = doc();
    if (isGlslDoc(d) || glslViewAt(d, d.cursor)) { glslComplete(); return; }
    if (isCppDoc(d) && completeImport(d)) return;
    const auto now = Clock::now();
    if (compInFlight_ && now - compSentAt_ < std::chrono::milliseconds(2500)) { compPending_ = std::max(compPending_, triggerKind); return; }   // see compInFlight_ (no text is sent either)
    auto target = lspTarget(d, d.cursor);   // an ordinary C++ file, or a cell of a notebook (mapped into its virtual file)
    if (!target) return;
    if (compInFlight_) target->c->cancel(compReq_);   // clangd has not answered for seconds: ask afresh
    NbView* nbv = nbOfCell(d);
    const Pos at = d.cursor;
    const Document* dp = &d;
    const std::uint64_t epoch = compEpoch_;
    compInFlight_ = true;
    compPending_ = 0;
    compSentAt_ = now;
    compLocalDue_ = nbv ? Clock::time_point{} : now + std::chrono::milliseconds(35);
    std::string reqPrefix;
    bool reqMember = false;
    const std::string reqKey = nbv ? std::string() : compScopeKey(d, at, reqPrefix, reqMember);
    if (!nbv && !reqMember && !comp_.open) {   // the last answer for this scope, at once; clangd's replaces it
        if (auto it = compCache_.find(reqKey); it != compCache_.end() && !it->second.items.empty() &&
            std::ranges::equal(it->second.prefix, reqPrefix.substr(0, it->second.prefix.size()), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); }) &&
            reqPrefix.size() >= it->second.prefix.size()) {
            showCompletion(it->second.items, at, -1, false);
            if (comp_.open) comp_.local = true;
        }
    }
    compReq_ = target->c->completion(target->path, target->line, target->col, triggerKind, [this, epoch, at, dp, nbv, now, reqKey, reqPrefix, reqMember](std::vector<LspCompletion> items, bool incomplete) {
        compInFlight_ = false;
        compLocalDue_ = {};
        if (std::getenv("IDE_LSP_TRACE")) std::println(std::cerr, "completion: {} items in {} ms", items.size(), std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - now).count());
        if (docs_.empty()) return;
        if (!nbv && !reqMember && !items.empty()) {   // remembered for the next time this scope is typed
            if (compCache_.size() > 32) compCache_.clear();
            compCache_[reqKey] = {reqPrefix, items};
        }
        const int pending = std::exchange(compPending_, 0);
        if (nbv && items.empty() && !nbv->lsp.warm && nbv->lsp.retries < 40) {   // clangd answers nothing while it still parses: ask again shortly
            ++nbv->lsp.retries;
            nbv->lsp.retryAt = Clock::now() + std::chrono::milliseconds(400);
            nbv->lsp.retryDoc = const_cast<Document*>(dp);
            return;
        }
        struct Reset { Document*& o; ~Reset() { o = nullptr; } } reset{docOverride_};
        if (nbv) {   // the cell must still be the one being edited
            auto nit = notebooks_.find(&doc());
            if (nit == notebooks_.end() || nit->second.get() != nbv || !nbv->editing || nbv->sel < 0 || nbv->sel >= static_cast<int>(nbv->cells.size()) ||
                nbv->cells[static_cast<std::size_t>(nbv->sel)].doc.get() != dp) return;
            docOverride_ = const_cast<Document*>(dp);
        } else if (&doc() != dp) return;
        Document& d = doc();
        const std::string& ln = d.buf.line(at.line);
        int a = at.col;
        while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
        const bool sameWord = d.cursor.line == at.line && d.cursor.col >= a && epoch == compEpoch_;
        bool again = pending != 0;   // something was typed while waiting: is this answer still what is needed?
        if (sameWord) {
            // typed past the point of the request inside the same identifier: the list is filtered with the longer prefix
            const bool typedAhead = d.cursor.col > at.col;
            showCompletion(std::move(items), at, -1, incomplete);
            if (comp_.open && !(incomplete && typedAhead) && pending != 2) again = false;
        } else if (!pending) return;   // the cursor moved away and nothing new was asked for
        if (again) requestCompletion(pending ? pending : 3);
    });
}

// The scope the identifier at `at` is written in: "path|std::" for `std::vec`, "path|" for a plain name. `member` is set after . and ->
// (those lists depend on the object and are not cached).
std::string App::compScopeKey(const Document& d, Pos at, std::string& prefix, bool& member) const {
    const std::string& ln = d.buf.line(at.line);
    int a = std::min<int>(at.col, static_cast<int>(ln.size()));
    int e = a;
    while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
    prefix = ln.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(e - a));
    member = a > 0 && (ln[static_cast<std::size_t>(a - 1)] == '.' || (ln[static_cast<std::size_t>(a - 1)] == '>' && a >= 2 && ln[static_cast<std::size_t>(a - 2)] == '-'));
    int q = a;   // the qualifier: ident(::ident)*:: in front
    while (q >= 2 && ln[static_cast<std::size_t>(q - 1)] == ':' && ln[static_cast<std::size_t>(q - 2)] == ':') {
        q -= 2;
        while (q > 0 && identChar(ln[static_cast<std::size_t>(q - 1)])) --q;
    }
    return d.path + "|" + ln.substr(static_cast<std::size_t>(q), static_cast<std::size_t>(a - q));
}

// Words of the file, as an instant placeholder while clangd is still working (replaced by its answer).
void App::showLocalCompletion() {
    if (docs_.empty() || comp_.open) return;
    Document& d = doc();
    if (!isCppDoc(d) || nbOfCell(d)) return;
    const std::string& ln = d.buf.line(d.cursor.line);
    int a = d.cursor.col;
    while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
    const std::string prefix = ln.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(d.cursor.col - a));
    if (prefix.size() < 2) return;
    if (a > 0 && (ln[static_cast<std::size_t>(a - 1)] == '.' || ln[static_cast<std::size_t>(a - 1)] == ':' || (ln[static_cast<std::size_t>(a - 1)] == '>' && a >= 2 && ln[static_cast<std::size_t>(a - 2)] == '-'))) return;   // members: only clangd knows
    std::unordered_set<std::string> seen;
    std::vector<LspCompletion> items;
    auto consider = [&](const std::string& w, int kind, const char* detail) {
        if (w == prefix || w.size() < 3 || matchScore(prefix, w) < 0 || !seen.insert(w).second) return;
        LspCompletion c;
        c.label = c.insertText = c.filterText = w;
        c.sortText = w;
        c.kind = kind;
        c.detail = detail;
        items.push_back(std::move(c));
    };
    if (d.lang) {
        for (const auto& w : d.lang->keywords) consider(w, 14, "keyword");
        for (const auto& w : d.lang->types) consider(w, 7, "type");
    }
    const int n = std::min(d.buf.lineCount(), 30000);
    for (int i = 0; i < n && items.size() < 400; ++i) {
        const std::string& l = d.buf.line(i);
        for (std::size_t p = 0; p < l.size();) {
            if (!identChar(l[p]) || std::isdigit(static_cast<unsigned char>(l[p]))) { ++p; continue; }
            std::size_t e = p;
            while (e < l.size() && identChar(l[e])) ++e;
            if (!(i == d.cursor.line && static_cast<int>(p) == a)) consider(l.substr(p, e - p), 1, "word");
            p = e;
        }
    }
    if (items.empty()) return;
    showCompletion(std::move(items), d.cursor);
    if (comp_.open) comp_.local = true;
}

void App::requestSignature() {
    if (docs_.empty()) return;
    Document& d = doc();
    if (isGlslDoc(d) || glslViewAt(d, d.cursor)) { glslSignature(); return; }
    auto target = lspTarget(d, d.cursor);
    if (!target) { closeSignature(); return; }
    const Pos at = d.cursor;
    const Document* dp = &d;
    const std::uint64_t seq = ++sigSeq_;
    target->c->cancel(sigReq_);
    sigReq_ = target->c->signatureHelp(target->path, target->line, target->col, [this, seq, at, dp](LspSignatureHelp h) {
        if (seq != sigSeq_) return;
        if (h.sigs.empty()) { if (sig_.open) { sig_ = {}; dirty_ = true; } return; }
        sig_ = {};
        sig_.open = true;
        sig_.help = std::move(h);
        sig_.at = at;
        sig_.owner = dp;
        dirty_ = true;
    });
}

void App::showCompletion(std::vector<LspCompletion> items, Pos at, int anchorCol, bool incomplete, bool pathMode) {
    Document& d = doc();
    const std::string& ln = d.buf.line(at.line);
    int a = at.col;
    if (anchorCol >= 0) a = anchorCol;
    else while (a > 0 && identChar(ln[static_cast<std::size_t>(a - 1)])) --a;
    std::ranges::stable_sort(items, [](const LspCompletion& x, const LspCompletion& y) { return x.sortText < y.sortText; });
    comp_ = {};
    comp_.owner = &d;
    comp_.all = std::move(items);
    comp_.anchor = {at.line, a};
    comp_.dotted = anchorCol >= 0;
    comp_.incomplete = incomplete;
    comp_.path = pathMode;
    compStale_ = incomplete;
    comp_.member = a > 0 && (ln[static_cast<std::size_t>(a - 1)] == '.' || (ln[static_cast<std::size_t>(a - 1)] == '>' && a >= 2 && ln[static_cast<std::size_t>(a - 2)] == '-'));
    comp_.open = true;
    refilterCompletion();
    dirty_ = true;
}

void App::requestCMakeCompletion(bool pathOnly) {
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
    // Folder and file names inside an argument (src/ma, ../, "${CMAKE_CURRENT_SOURCE_DIR}/inc).
    std::vector<LspCompletion> pathItems;
    if (!d.path.empty()) {
        const fs::path fileDir = fs::path(d.path).parent_path();
        const fs::path srcRoot = cmakeOpen_ && !cmakeRoot_.empty() ? cmakeRoot_ : (explorer_.isOpen() ? explorer_.rootPath() : fileDir);
        const fs::path buildDir = cmakeOpen_ ? profile().dir : srcRoot / "build";
        if (auto pr = cmakePathCompletions(before, fileDir, srcRoot, buildDir)) {
            for (auto& c : pr->items) {
                LspCompletion it;
                it.label = std::move(c.label); it.detail = std::move(c.detail); it.insertText = std::move(c.insert); it.sortText = std::move(c.sort); it.kind = c.kind;
                it.filterText = it.insertText;
                pathItems.push_back(std::move(it));
            }
            if (pr->exclusive) {   // clearly a path: nothing but its entries
                if (pathItems.empty()) { closeCompletion(); return; }
                showCompletion(std::move(pathItems), d.cursor, d.cursor.col - static_cast<int>(pr->typed), false, true);
                return;
            }
        }
    }
    if (pathOnly) { closeCompletion(); return; }
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
    for (auto& p : pathItems) items.push_back(std::move(p));   // commands that take files: their folder's entries next to the usual names
    if (items.empty()) { closeCompletion(); return; }
    showCompletion(std::move(items), d.cursor);
}

void App::refilterCompletion() {
    if (!comp_.open || docs_.empty()) return;
    Document& d = doc();
    if (d.cursor.line != comp_.anchor.line || d.cursor.col < comp_.anchor.col) { closeCompletion(); return; }
    const std::string& ln = d.buf.line(comp_.anchor.line);
    const std::string prefix = ln.substr(static_cast<std::size_t>(comp_.anchor.col), static_cast<std::size_t>(d.cursor.col - comp_.anchor.col));
    const bool dotted = comp_.dotted, pathMode = comp_.path;
    if (!std::ranges::all_of(prefix, [dotted, pathMode](char c) { return identChar(c) || (dotted && (c == '.' || c == ':')) || (pathMode && (c == '.' || c == '-' || c == '+' || c == '~' || c == '@')); })) { closeCompletion(); return; }
    struct Ranked { int match, kind, used; int idx; };
    std::vector<Ranked> scored;
    for (int i = 0; i < static_cast<int>(comp_.all.size()); ++i) {
        const auto& it = comp_.all[static_cast<std::size_t>(i)];
        const int sc = matchScore(prefix, it.filterText.empty() ? it.label : it.filterText);
        if (sc < 0) continue;
        const auto used = compUsed_.find(it.label);
        scored.push_back({sc, comp_.member ? 0 : kindTier(it.kind), used == compUsed_.end() ? 0 : -std::min(used->second, 9), i});
    }
    // Better match first, then types before functions before values before macros, then what you picked before, then the
    // server's own order (its sort text), then shorter names.
    std::ranges::stable_sort(scored, [&](const Ranked& a, const Ranked& b) {
        if (a.match != b.match) return a.match < b.match;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.used != b.used) return a.used < b.used;
        const auto& x = comp_.all[static_cast<std::size_t>(a.idx)];
        const auto& y = comp_.all[static_cast<std::size_t>(b.idx)];
        if (x.sortText != y.sortText) return x.sortText < y.sortText;
        return x.label.size() < y.label.size();
    });
    comp_.shown.clear();
    std::unordered_set<std::string> typeNames;   // `typedef struct X X;` gives two entries for one name: show one
    for (const auto& r : scored) {
        if (comp_.shown.size() >= 200) break;
        if (r.kind == 0 && !comp_.member && !typeNames.insert(comp_.all[static_cast<std::size_t>(r.idx)].label).second) continue;
        comp_.shown.push_back(r.idx);
    }
    comp_.sel = 0;
    comp_.scroll = 0;
    if (comp_.shown.empty()) closeCompletion();
    dirty_ = true;
}

bool App::completionKey(const KeyEvent& k) {
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

void App::acceptCompletion() {
    if (!comp_.open || comp_.shown.empty() || docs_.empty()) { closeCompletion(); return; }
    Document& d = doc();
    const LspCompletion item = comp_.all[static_cast<std::size_t>(comp_.shown[static_cast<std::size_t>(comp_.sel)])];
    std::string text = item.insertText.empty() ? item.label : item.insertText;
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    ++compUsed_[item.label];
    const bool pathItem = (comp_.path || isCMakeDoc(d)) && item.kind == 19 && item.insertText.ends_with("/");   // a folder: keep going inside it
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
    if (pathItem) { requestCMakeCompletion(); return; }
    if (isCall) requestSignature();   // the cursor sits inside the new parentheses: show the parameters right away
    scrollToCursor_ = true;
    dirty_ = true;
}

void App::afterEditorKey(const KeyEvent& k, std::uint64_t verBefore) {
    Document& d = doc();
    if (comp_.open) refilterCompletion();
    const bool wasStale = compStale_ && !k.ctrl && !k.alt && !k.text.empty() && d.buf.version() != verBefore && identChar(k.text.back());
    if (isGlslDoc(d) || (d.lang && d.lang->rawStrings && glslViewAt(d, d.cursor))) { glslAfterKey(k, verBefore); return; }
    if (isCMakeDoc(d)) {
        if (k.ctrl && !k.alt && k.key == Key::Char && k.ch == ' ') { requestCMakeCompletion(); return; }
        if (k.ctrl || k.alt || k.text.empty() || d.buf.version() == verBefore) return;
        const char c = k.text.back();
        const std::string& ln = d.buf.line(d.cursor.line);
        const int col = d.cursor.col;
        if (identChar(c)) { if (!comp_.open) requestCMakeCompletion(); }
        else if (c == '/' || c == '.' || c == '~') requestCMakeCompletion(true);   // path segments
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
    if (!lspAvailable(d)) return;
    if (k.key == Key::Escape && sig_.open) closeSignature();
    else if (sig_.open || (!k.ctrl && !k.alt && !k.text.empty() && d.buf.version() != verBefore && (k.text.back() == '(' || k.text.back() == ','))) requestSignature();
    if (k.ctrl && !k.alt && k.key == Key::Char && k.ch == ' ') { requestCompletion(1); return; }
    if (k.ctrl || k.alt || k.text.empty() || d.buf.version() == verBefore) return;
    const char c = k.text.back();
    const std::string& ln = d.buf.line(d.cursor.line);
    const int col = d.cursor.col;
    if (identChar(c)) {
        if (!comp_.open) requestCompletion(1);
        else if (wasStale) requestCompletion(3);   // the server's list was cut off: ask again for the longer prefix (the old list stays until the answer)
    } else if (c == '.' || (c == '>' && col >= 2 && ln[static_cast<std::size_t>(col - 2)] == '-') ||
               (c == ':' && col >= 2 && ln[static_cast<std::size_t>(col - 2)] == ':')) {
        requestCompletion(2);
    } else {
        closeCompletion();
    }
}

auto App::completionKindIcon(int kind) -> KindIcon {
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

bool App::anchorPoint(const Document* owner, Pos p, float& x, float& top) {
    if (docs_.empty() || !owner) return false;
    Document& active = doc();
    const float s = S(), cell = cw(), line = lh();
    if (isNotebook(active) && owner != &active) {   // a cell of the notebook on screen
        const NbView* nbv = notebooks_.at(&active).get();
        for (const auto& c : nbv->cells) {
            if (c.doc.get() != owner) continue;
            const float bpad = std::round(8 * s);
            top = nbv->viewTop + c.boxY - std::round(active.scrollY) + bpad + static_cast<float>(p.line) * line;
            x = c.boxX + bpad + static_cast<float>(displayCol(owner->buf.line(p.line), p.col, owner->tabWidth)) * cell;
            return true;
        }
        return false;
    }
    if (owner != &active) return false;
    x = L_.textX - std::round(active.scrollX) + colX(active, p.line, p.col);
    top = L_.editor.y + static_cast<float>(p.line) * line - std::round(active.scrollY);
    return true;
}

void App::drawSignature() {
    if (!sig_.open || docs_.empty() || sig_.help.sigs.empty()) return;
    float ax = 0, top = 0;
    if (!anchorPoint(sig_.owner, sig_.at, ax, top)) return;
    const LspSignature& sg = sig_.help.sigs[static_cast<std::size_t>(sig_.help.activeSig)];
    const float s = S(), cell = cw(), line = lh(), rowH = std::round(line + 4 * s);
    // Text: "1/2 " when there are overloads, then the signature with the parameter being typed picked out.
    const std::string count = sig_.help.sigs.size() > 1 ? std::format("{}/{}  ", sig_.help.activeSig + 1, sig_.help.sigs.size()) : std::string();
    std::string text = sg.label;
    int a0 = -1, a1 = -1;
    if (sg.active >= 0 && sg.active < static_cast<int>(sg.params.size()) && sg.params[static_cast<std::size_t>(sg.active)].first >= 0) {
        a0 = sg.params[static_cast<std::size_t>(sg.active)].first;
        a1 = std::min<int>(sg.params[static_cast<std::size_t>(sg.active)].second, static_cast<int>(text.size()));
    }
    const std::size_t maxChars = std::max<std::size_t>(20, static_cast<std::size_t>((L_.editor.w * 0.9f - 28 * s) / cell)) - count.size();
    std::string head;   // a very long signature is cut at the front so the active parameter stays visible
    if (utf8Length(text) > maxChars && a0 > 0) {
        const int cut = std::max(0, a0 - static_cast<int>(maxChars) / 3);
        if (cut > 0) { text = text.substr(static_cast<std::size_t>(cut)); a0 -= cut; a1 -= cut; head = "..."; }
    }
    std::string doc = sg.active >= 0 && sg.active < static_cast<int>(sg.paramDocs.size()) ? sg.paramDocs[static_cast<std::size_t>(sg.active)] : std::string();
    if (doc.empty()) doc = sg.doc;
    if (const auto nl = doc.find('\n'); nl != std::string::npos) doc.resize(nl);
    const std::size_t shownChars = std::min(utf8Length(count) + utf8Length(head) + utf8Length(text), maxChars + count.size());
    const std::size_t docChars = std::min(utf8Length(doc), maxChars);
    const float w = std::round((static_cast<float>(std::max(shownChars, docChars)) + 3) * cell + 20 * s);
    const int rows = doc.empty() ? 1 : 2;
    const float h = static_cast<float>(rows) * rowH + 8 * s;
    float x = std::clamp(ax - 10 * s, L_.editor.x + 4 * s, std::max(L_.editor.x + 4 * s, L_.editor.right() - w - 4 * s));
    float y = top - h - 4 * s;   // above the line, so it does not cover the completion list below it
    if (y < L_.editor.y) y = top + line + 4 * s;
    const Rect r{x, y, w, h};
    glassPanel(r, 10 * s);
    const float ty = r.y + 4 * s + (rowH - line) / 2, maxX = r.right() - 8 * s;
    float tx = r.x + 10 * s;
    auto put = [&](const std::string& t, Color c) {
        if (t.empty()) return;
        label(tx, ty, t, c, maxX);
        tx += static_cast<float>(utf8Length(t)) * cell;
    };
    put(count, theme::lineNum);
    put(head, theme::dim);
    if (a0 >= 0 && a1 > a0 && a1 <= static_cast<int>(text.size())) {
        put(text.substr(0, static_cast<std::size_t>(a0)), theme::dim);
        const float ux = tx;
        put(text.substr(static_cast<std::size_t>(a0), static_cast<std::size_t>(a1 - a0)), theme::text);
        fill(ux, ty + line - std::round(2 * s), std::min(tx, maxX) - ux, std::max(1.0f, std::round(1.5f * s)), theme::accent, s);
        put(text.substr(static_cast<std::size_t>(a1)), theme::dim);
    } else {
        put(text, theme::dim);
    }
    if (!doc.empty()) label(r.x + 10 * s, ty + rowH, doc, theme::lineNum, maxX);
}

void App::drawCompletion() {
    if (!comp_.open || comp_.shown.empty() || docs_.empty()) return;
    Document& active = doc();
    const Document& d = comp_.owner ? *comp_.owner : active;
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
    // A footer for the selected entry with everything the list had to cut: return type, name and all parameters with their types.
    std::string footDetail, footLabel;
    if (comp_.sel >= 0 && comp_.sel < n) {
        const auto& si = comp_.all[static_cast<std::size_t>(comp_.shown[static_cast<std::size_t>(comp_.sel)])];
        footLabel = si.label;
        while (!footLabel.empty() && footLabel.front() == ' ') footLabel.erase(0, 1);
        footDetail = si.detail;
        const bool fn = si.kind == 2 || si.kind == 3 || si.kind == 4;
        if (!fn && utf8Length(footLabel) <= 60 && utf8Length(footDetail) <= maxDetail) footLabel.clear();   // nothing was cut
    }
    const float footH = footLabel.empty() ? 0.0f : rowH + 6 * s;
    const float footW = (static_cast<float>(utf8Length(footDetail) + utf8Length(footLabel) + (footDetail.empty() ? 0 : 2)) + 2) * cell + 24 * s;
    w = std::min(std::max(w, footW), L_.editor.w * 0.9f);
    const float listH = static_cast<float>(rows) * rowH + 8 * s;
    const float h = listH + footH;
    const float sx = std::round(d.scrollX), sy = std::round(d.scrollY);
    float x, y;
    const NbView* nbv = isNotebook(active) && comp_.owner != &active ? notebooks_.at(&active).get() : nullptr;
    if (nbv) {   // a cell of a notebook: place the popup under the text of that cell
        const NbCell* cellp = nullptr;
        for (const auto& c : nbv->cells) if (c.doc.get() == comp_.owner) { cellp = &c; break; }
        if (!cellp) return;
        const float bpad = std::round(8 * s), top = nbv->viewTop + cellp->boxY - std::round(active.scrollY) + bpad + static_cast<float>(comp_.anchor.line) * line;
        x = cellp->boxX + bpad + static_cast<float>(displayCol(d.buf.line(comp_.anchor.line), comp_.anchor.col, d.tabWidth)) * cell - iconW;
        y = top + line + 2 * s;
        if (y + h > L_.editor.bottom() && top - h - 2 * s >= nbv->viewTop) y = top - h - 2 * s;   // above the line, unless that would cover the toolbar
    } else {
        x = L_.textX - sx + colX(d, comp_.anchor.line, comp_.anchor.col) - iconW;
        y = L_.editor.y + static_cast<float>(comp_.anchor.line + 1) * line - sy + 2 * s;
        if (y + h > L_.editor.bottom()) y = L_.editor.y + static_cast<float>(comp_.anchor.line) * line - sy - h - 2 * s;  // flip above
    }
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
        const float tx = r.right() - 5 * s, th = (listH - 8 * s) * static_cast<float>(rows) / static_cast<float>(n);
        const float ty = r.y + 4 * s + (listH - 8 * s - th) * static_cast<float>(comp_.scroll) / static_cast<float>(n - rows);
        fill(tx, ty, 2.5f * s, th, theme::scrollbar, 1.2f * s);
    }
    if (footH > 0) {
        const float fy = r.y + listH;
        fill(r.x + 8 * s, fy - 1, r.w - 16 * s, std::max(1.0f, std::round(0.8f * s)), theme::border);
        const float fty = fy + (footH - line) / 2 + 1;
        float fx = r.x + 12 * s;
        const float fmax = r.right() - 10 * s;
        if (!footDetail.empty()) { label(fx, fty, footDetail, theme::dim, fmax); fx += static_cast<float>(utf8Length(footDetail) + 2) * cell; }
        label(fx, fty, footLabel, theme::text, fmax);
    }
}

void App::hoverMoved() {
    hoverCell_ = nullptr;
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

std::string App::cleanMarkdown(const std::string& md) {
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

void App::addHoverText(const std::string& text, int sev) {
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

void App::hoverTick(Clock::time_point now) {
    if (hoverCell_) { nbHoverTick(now); return; }
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
    if (isGlslDoc(d) || (d.lang && d.lang->rawStrings)) {
        std::string text;
        if (glslHover(d, at, text)) {
            if (!hover_.lines.empty()) hover_.lines.emplace_back("", 0);
            addHoverText(text, 0);
        }
    }
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

std::pair<int, int> App::hoverPosAt(float x, float y) {
    const float s = S(), line = std::round(uiH() + 3 * s);
    const int n = static_cast<int>(hover_.lines.size());
    const int row = std::clamp(static_cast<int>(std::floor((y - (hoverRect_.y + 8 * s)) / line)), 0, std::max(0, n - 1));
    return {row, static_cast<int>(caretFromX(hover_.lines[static_cast<std::size_t>(row)].first, x - (hoverRect_.x + 13 * s)))};
}

void App::hoverSelAnchor() {
    const auto [r, c] = hoverPosAt(mx_, my_);
    hsel_.ar = hsel_.cr = r;
    hsel_.ac = hsel_.cc = c;
    hsel_.sx = mx_; hsel_.sy = my_;
}

void App::hoverSelUpdate() {
    const auto [r, c] = hoverPosAt(mx_, my_);
    hsel_.cr = r; hsel_.cc = c;
    if (std::hypot(mx_ - hsel_.sx, my_ - hsel_.sy) > 3 * S()) hsel_.moved = true;
}

void App::drawHover() {
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

void App::jumpTo(const std::string& path, int line, int u16col) {
    openFile(path);
    if (docs_.empty()) return;
    Document& d = doc();
    const int l = std::clamp(line, 0, d.buf.lineCount() - 1);
    d.setCursor({l, byteColFromUtf16(d.buf.line(l), u16col)});
    scrollToCursor_ = true;
    sideFocus_ = false;
    dirty_ = true;
}

void App::gotoDefinition() {
    if (docs_.empty()) return;
    Document& d = doc();
    if ((isGlslDoc(d) || (d.lang && d.lang->rawStrings)) && glslGoto()) return;
    if (isCppDoc(d) && gotoModule(d)) return;  // `import ide.json;` jumps to the module interface
    if (!lsp_ || !lsp_->ready() || !isCppDoc(d) || !d.lspOpen) { setMessage("Language server not ready", true, 2.5f); return; }
    flushDoc(d);
    auto [l, c] = lspPos(d, d.cursor);
    const NavEntry from{d.path, d.cursor};
    lsp_->definition(d.path, l, c, [this, from](std::vector<LspLocation> locs) {
        if (locs.empty()) { setMessage("No definition found", true, 2.5f); return; }
        navPush(from);
        jumpTo(locs[0].path, locs[0].line, locs[0].col);
    });
}

void App::navBack() {
    if (navBack_.empty()) { setMessage("Nothing to go back to", false, 1.5f); return; }
    if (!docs_.empty()) navFwd_.push_back({doc().path, doc().cursor});
    NavEntry e = navBack_.back();
    navBack_.pop_back();
    openFile(e.path);
    if (!docs_.empty()) { doc().setCursor(e.pos); scrollToCursor_ = true; }
}

void App::navForward() {
    if (navFwd_.empty()) { setMessage("Nothing to go forward to", false, 1.5f); return; }
    NavEntry e = navFwd_.back();
    navFwd_.pop_back();
    if (!docs_.empty()) navBack_.push_back({doc().path, doc().cursor});
    openFile(e.path);
    if (!docs_.empty()) { doc().setCursor(e.pos); scrollToCursor_ = true; }
}

void App::switchHeaderSource() {
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

}  // namespace ide
