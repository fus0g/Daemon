// daemon.app / git_view - part of the App implementation (see app.cppm for the class).
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

void App::pollJobs() {
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

void App::gitDetect() {
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

void App::gitReset() {   // a different project was opened
    gitRoot_.reset();
    gitDocs_.clear();
    gitEntries_.clear(); gitFiles_.clear(); gitDirs_.clear();
    gitBranch_.clear(); gitHead_.clear(); gitUpstream_.clear();
    gitAhead_ = gitBehind_ = 0;
    cardCache_.clear();
    gitDetect();
}

std::string App::gitRel(const std::string& absPath) const {
    if (!gitRoot_) return {};
    auto r = fs::path(absPath).lexically_relative(*gitRoot_);
    if (r.empty() || *r.begin() == "..") return {};
    return r.string();
}

void App::gitRefresh() {
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

void App::gitUpdateDoc(Document& d, Clock::time_point now) {
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

void App::gitTick(Clock::time_point now) {
    if (jobs_.size() > 0) pollJobs();
    if (!gitRoot_) return;
    if (focused_ && now >= gitNextPoll_) { gitNextPoll_ = now + std::chrono::seconds(3); gitRefresh(); }
    for (Node* n : leaves()) if (n->pane->cur) gitUpdateDoc(*n->pane->cur, now);
}

std::string App::blameText(const git::Commit& c) {
    if (c.uncommitted) return "Uncommitted changes";
    return c.author + ", " + git::relativeTime(c.time, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()) +
           "  \xE2\x80\xA2  " + c.summary;
}

const git::Commit* App::blameOf(const Document& d, int ln) const {
    auto it = gitDocs_.find(&d);
    if (it == gitDocs_.end()) return nullptr;
    const GitDoc& g = it->second;
    if (!g.blameValid || g.blameVersion != d.buf.version() || ln < 0 || ln >= static_cast<int>(g.blame.lineCommit.size())) return nullptr;
    const int ci = g.blame.lineCommit[static_cast<std::size_t>(ln)];
    return ci >= 0 && ci < static_cast<int>(g.blame.commits.size()) ? &g.blame.commits[static_cast<std::size_t>(ci)] : nullptr;
}

void App::showBlameCard(const std::string& hash, bool uncommitted) {
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

void App::gitUpdateHover() {
    GitHit h;
    for (auto it = gitHits_.rbegin(); it != gitHits_.rend(); ++it)
        if (it->r.contains(mx_, my_)) { h = *it; break; }
    if (!gitHoverSame(h, gitHover_)) { gitHover_ = h; dirty_ = true; }
}

void App::gitAction(std::function<git::Result(const fs::path&)> fn, std::string okMsg) {
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

void App::gitCommit() {
    if (!gitRoot_) return;
    const std::string msg = git::trim(gitMsg_.text);
    const bool anyStaged = std::ranges::any_of(gitEntries_, [](const git::Entry& e) { return e.staged(); });
    if (msg.empty()) { setMessage("Enter a commit message first", true, 2.5f); return; }
    if (!anyStaged) { setMessage("Nothing staged - stage some files first", true, 2.5f); return; }
    gitMsg_ = {};
    const std::string summary = msg.substr(0, msg.find('\n'));
    gitAction([msg](const fs::path& root) { return git::commit(root, msg); }, "Committed: " + summary);
}

bool App::gitMsgKey(const KeyEvent& k) {
    if (k.key == Key::Escape) { gitMsgFocus_ = false; return true; }
    if (k.key == Key::Enter) { gitCommit(); return true; }
    if (k.ctrl && k.key != Key::Char) return false;
    if (k.key >= Key::F1) return false;
    return editText(gitMsg_, k);
}

void App::gitOpenEntry(const git::Entry& e) {
    if (!gitRoot_) return;
    const fs::path abs = *gitRoot_ / e.path;
    std::error_code ec;
    if (fs::is_regular_file(abs, ec)) openFile(abs.string());
    else setMessage("File no longer exists: " + e.path, true, 2.5f);
}

void App::gitPanelClick() {
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

void App::drawGitChanges(const Rect& body) {
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

void App::buildGraph() {
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

void App::gitEnsureLoaded() {
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

void App::gitToggleCommit(const std::string& hash) {
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

void App::gLine(float x0, float y0, float x1, float y1, Color c, float w) {
    if (x0 == x1) { fill(x0 - w / 2, std::min(y0, y1), w, std::abs(y1 - y0) + 0.5f, c); return; }
    const float len = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    const int n = std::max(2, static_cast<int>(len / (w * 0.6f)));
    for (int i = 0; i <= n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n);
        fill(x0 + (x1 - x0) * t - w / 2, y0 + (y1 - y0) * t - w / 2, w, w, c, w / 2);
    }
}

void App::drawChip(float& x, float y, float h, const std::string& text, Color c) {
    const float s = S();
    const float w = uiW(text) + 14 * s;
    fill(x, y + 2 * s, w, h - 4 * s, c.alpha(60), (h - 4 * s) / 2);
    uiLabelC(x + w / 2, y, h, text, c.mix(Color::rgb(0xffffff), 0.35f));
    x += w + 6 * s;
}

void App::drawGitHeader(const Rect& body, float headH) {
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

void App::drawGitHistory(const Rect& body) {
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

void App::drawGitBranches(const Rect& body) {
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

void App::drawGitBody(const Rect& body) {
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

}  // namespace ide
