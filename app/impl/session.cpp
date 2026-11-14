// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / session - remembers the open files, split layout and cursor / scroll positions of each project.
module daemon.app;
import std;
import daemon.base.util;
import daemon.base.json;
import daemon.text.buffer;
import daemon.services.project;

namespace ide {

namespace {
constexpr int kSessionVersion = 1;
}

void App::saveSession(bool force) {
    if (!sessionOn_ || !root_) return;
    // The project the tabs belong to; files opened without a folder are not remembered.
    if (!explorer_.isOpen()) return;
    const std::string rootPath = explorer_.rootPath().string();

    std::function<json::Value(Node&)> dump = [&](Node& n) -> json::Value {
        auto o = json::object();
        if (n.leaf()) {
            auto tabs = json::array();
            int cur = -1;
            for (Document* d : n.pane->tabs) {
                if (auto pv = previews_.find(d); pv != previews_.end()) {   // a Markdown preview is remembered by its source file
                    if (!pv->second.src || pv->second.src->path.empty()) continue;
                    if (d == n.pane->cur) cur = static_cast<int>(tabs.array()->size());
                    auto t = json::object();
                    t.set("preview", pv->second.src->path);
                    tabs.push(std::move(t));
                    continue;
                }
                if (d->path.empty()) continue;   // untitled buffers are not restored
                if (d == n.pane->cur) cur = static_cast<int>(tabs.array()->size());
                auto t = json::object();
                t.set("path", d->path);
                t.set("line", d->cursor.line);
                t.set("col", d->cursor.col);
                t.set("aline", d->anchor.line);
                t.set("acol", d->anchor.col);
                t.set("scroll", static_cast<double>(d->targetY));
                tabs.push(std::move(t));
            }
            o.set("tabs", std::move(tabs));
            o.set("cur", cur);
        } else {
            o.set("sideBySide", n.sideBySide);
            o.set("ratio", static_cast<double>(n.ratio));
            o.set("a", dump(*n.a));
            o.set("b", dump(*n.b));
        }
        return o;
    };

    auto j = json::object();
    j.set("version", kSessionVersion);
    j.set("root", rootPath);
    if (!docs_.empty() && cur_ >= 0 && cur_ < static_cast<int>(docs_.size())) j.set("active", docs_[static_cast<std::size_t>(cur_)]->path);
    j.set("tree", dump(*root_));
    const std::string text = json::dump(j);
    if (text == lastSession_ && !force) return;
    lastSession_ = text;

    const fs::path f = sessionFile(rootPath);
    if (f.empty()) return;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    const fs::path tmp = f.string() + ".tmp";   // write + rename: a crash never leaves a half-written session
    if (writeFile(tmp, text)) fs::rename(tmp, f, ec);
    if (state_.lastProject != rootPath) { state_.lastProject = rootPath; saveState(state_); }
}

// Debounced autosave, so a crash or kill loses at most a couple of seconds of layout changes.
void App::sessionTick(Clock::time_point now) {
    if (!sessionOn_ || now < nextSessionCheck_) return;
    nextSessionCheck_ = now + std::chrono::seconds(3);
    syncPanes();
    saveSession(false);
}

bool App::restoreSession() {
    if (!sessionOn_ || !explorer_.isOpen()) return false;
    const std::string rootPath = explorer_.rootPath().string();
    // Never throw away edits: only replace tabs that are all saved (or the untitled placeholder).
    if (std::ranges::any_of(docs_, [](const auto& d) { return d->modified(); })) return false;
    const fs::path f = sessionFile(rootPath);
    if (f.empty()) return false;
    auto text = readFile(f);
    if (!text) return false;
    auto j = json::parse(*text);
    if (!j || (*j)["root"].str() != rootPath || (*j)["version"].integer() != kSessionVersion) return false;

    std::vector<std::unique_ptr<Document>> loaded;
    struct PendingPreview { Pane* pane; std::size_t at; std::string path; bool cur; };
    std::vector<PendingPreview> pendingPreviews;   // resolved once every source document is loaded
    std::function<std::unique_ptr<Node>(const json::Value&)> build = [&](const json::Value& v) -> std::unique_ptr<Node> {
        auto node = std::make_unique<Node>();
        if (auto* tabs = v["tabs"].array()) {
            auto pane = std::make_unique<Pane>();
            const int curIdx = v["cur"].integer(-1);
            int i = 0;
            for (const auto& t : *tabs) {
                if (const std::string pv = t["preview"].str(); !pv.empty()) {
                    pendingPreviews.push_back({pane.get(), pane->tabs.size(), pv, i == curIdx});
                    ++i;
                    continue;
                }
                const std::string path = t["path"].str();
                std::error_code ec;
                std::unique_ptr<Document> d;
                if (nb::isNotebookPath(path)) { std::string err; if (fs::is_regular_file(path, ec)) d = createNotebookDoc(path, err); if (!d) { ++i; continue; } }
                else {
                    d = std::make_unique<Document>();
                    if (path.empty() || !fs::is_regular_file(path, ec) || !d->load(path)) { ++i; continue; }
                }
                d->cursor = d->buf.clamp({t["line"].integer(), t["col"].integer()});
                d->anchor = d->buf.clamp({t["aline"].integer(), t["acol"].integer()});
                d->scrollY = d->targetY = static_cast<float>(t["scroll"].num());
                pane->tabs.push_back(d.get());
                if (i == curIdx) pane->cur = d.get();
                loaded.push_back(std::move(d));
                ++i;
            }
            if (pane->tabs.empty() && std::ranges::none_of(pendingPreviews, [&](const auto& p) { return p.pane == pane.get(); })) return nullptr;
            node->pane = std::move(pane);
            return node;
        }
        auto a = build(v["a"]), b = build(v["b"]);
        if (!a || !b) return a ? std::move(a) : std::move(b);
        node->sideBySide = v["sideBySide"].boolean(true);
        node->ratio = std::clamp(static_cast<float>(v["ratio"].num(0.5)), 0.1f, 0.9f);
        node->a = std::move(a);
        node->b = std::move(b);
        return node;
    };
    auto tree = build((*j)["tree"]);
    if (!tree || loaded.empty()) return false;

    for (auto& d : docs_) lspDetach(*d);
    closeCompletion();
    previews_.clear();
    for (auto& od : docs_) notebooks_.erase(od.get());
    docs_ = std::move(loaded);
    root_ = std::move(tree);
    for (const auto& pp : pendingPreviews) {
        auto src = std::ranges::find(docs_, pp.path, [](const auto& d) { return d->path; });
        if (src == docs_.end()) continue;
        Document* pd = makePreviewDoc(**src);
        pp.pane->tabs.insert(pp.pane->tabs.begin() + static_cast<std::ptrdiff_t>(std::min(pp.at, pp.pane->tabs.size())), pd);
        if (pp.cur) pp.pane->cur = pd;
    }
    cur_ = 0;
    activePane_ = nullptr;
    const std::string active = (*j)["active"].str();
    for (std::size_t i = 0; i < docs_.size(); ++i)
        if (docs_[i]->path == active) cur_ = static_cast<int>(i);
    syncPanes();
    for (auto& d : docs_) ensureLsp(*d);
    revealCurrent();
    resetCaret();
    lastSession_.clear();
    dirty_ = true;
    return true;
}

}  // namespace ide
