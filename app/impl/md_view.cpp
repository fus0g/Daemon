// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / md_view - the Markdown preview: a read-only tab that renders another document, live.
module;
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.ui.icons;
import daemon.app.theme;
import daemon.app.mdview;
import daemon.text.markdown;


namespace ide {

bool App::isMarkdownPath(std::string_view path) {
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".md" || ext == ".markdown" || ext == ".mdown" || ext == ".mkd";
}

Document* App::makePreviewDoc(Document& src) {
    auto d = std::make_unique<Document>();
    d->name = "Preview " + src.name;
    d->buf.readOnly = true;
    Document* raw = d.get();
    docs_.push_back(std::move(d));
    previews_[raw].src = &src;
    return raw;
}

Document* App::previewOf(const Document& src) {
    for (auto& [pd, v] : previews_) if (v.src == &src) return const_cast<Document*>(pd);
    return nullptr;
}

// Opens the preview to the right of `src` (the editing focus stays where it is).
void App::openPreview(Document& src) {
    if (Document* existing = previewOf(src)) { if (int k = docIndex(existing); k >= 0) switchTab(k); return; }
    syncPanes();
    Pane* from = paneOfDoc(&src);
    if (!from) from = activePane_;
    Document* pd = makePreviewDoc(src);
    splitPane(from, Zone::Right, pd);
    dirty_ = true;
}

void App::togglePreview() {
    if (docs_.empty()) return;
    Document& d = doc();
    if (isPreview(d)) {   // back to the source
        Document* src = previews_.at(&d).src;
        closeTab(cur_, true);
        if (src) if (int k = docIndex(src); k >= 0) switchTab(k);
        return;
    }
    if (!isMarkdownPath(d.path)) { setMessage("The preview works on Markdown files (.md)", false, 3); return; }
    if (Document* existing = previewOf(d)) { if (int k = docIndex(existing); k >= 0) closeTab(k, true); return; }
    openPreview(d);
}

// Parses and lays the source out again when its text, the pane width or the fonts changed.
void App::refreshPreview(Document& d) {
    auto it = previews_.find(&d);
    if (it == previews_.end()) return;
    MdView& v = it->second;
    if (!v.src) return;
    const float width = L_.editor.w;
    if (width < 40) return;
    const float fontPx = font_.uiPixelSize() + font_.pixelSize() * 0.001f;
    const bool textChanged = !v.parsed || v.ver != v.src->buf.version();
    if (!textChanged && v.width == width && v.scale == S() && v.fontPx == fontPx) return;
    if (textChanged) {
        v.blocks = md::parse(v.src->buf.text());
        v.ver = v.src->buf.version();
        v.parsed = true;
    }
    v.width = width; v.scale = S(); v.fontPx = fontPx;
    v.lay = mdLayout(v.blocks, font_, width, S(), [this, &v](const std::string& src) { return pictureSize(v, src); });
    d.targetY = std::clamp(d.targetY, 0.0f, std::max(0.0f, v.lay.height - L_.editor.h));
    dirty_ = true;
}

void App::drawPreview(Document& d) {
    refreshPreview(d);
    auto it = previews_.find(&d);
    if (it == previews_.end()) return;
    const MdView& v = it->second;
    const Rect& e = L_.editor;
    const float s = S(), sy = std::round(d.scrollY);
    const float ox = e.x, oy = e.y - sy;
    const float boldOff = std::max(1.0f, std::round(0.5f * s));
    for (const MdItem& m : v.lay.items) {
        const float y = oy + m.y;
        if (y > e.bottom() || y + std::max(m.h, 1.0f) < e.y) continue;
        const bool hot = m.link >= 0 && !v.hoverUrl.empty() && v.lay.links[static_cast<std::size_t>(m.link)].url == v.hoverUrl;
        Color c = m.c;
        if (hot) c = m.t == MdItem::T::Rect ? c.alpha(255) : c.mix(Color::rgb(0xffffff), 0.35f);
        switch (m.t) {
            case MdItem::T::Rect: fill(ox + m.x, y, m.w, m.h, c, m.radius); break;
            case MdItem::T::Ring: dl_.ring(font_, ox + m.x, y, m.w, m.h, c, m.radius, std::max(1.0f, std::round(s * 0.8f))); break;
            case MdItem::T::Text: {
                const float cx = m.w > 0 ? ox + m.w : std::numeric_limits<float>::infinity();
                dl_.uiText(font_, ox + m.x, y, m.s, c, cx);
                if (m.bold) dl_.uiText(font_, ox + m.x + boldOff, y, m.s, c, cx);
                break;
            }
            case MdItem::T::Title:
                dl_.titleText(font_, ox + m.x, y, m.s, c);
                if (m.bold) dl_.titleText(font_, ox + m.x + boldOff, y, m.s, c);
                break;
            case MdItem::T::Image: {
                const std::string key = pictureKey(const_cast<MdView&>(v), m.s);
                auto pit = picCache_.find(key);
                if (pit == picCache_.end() || pit->second.state != Pic::State::Ready || pit->second.epoch != pics_.epoch() || pit->second.levels.empty()) break;
                const auto& lv = pit->second.levels;
                std::size_t pick = 0;   // the smallest level that is still at least as large as the box: at most a 2x downscale
                for (std::size_t i = 0; i < lv.size(); ++i) if (static_cast<float>(lv[i].w) >= m.w) pick = i;
                const ImageAtlas::Region& r = lv[pick];
                dl_.image(ox + m.x, y, m.w, m.h, r.u0, r.v0, r.u1, r.v1, 255, m.radius);
                break;
            }
            case MdItem::T::Check: Icons::glyph(dl_, font_, ox + m.x, y, static_cast<int>(m.w), 0xF012C, c); break;   // mdi-check
            case MdItem::T::Mono: dl_.text(font_, ox + m.x, y, m.s, c, m.w > 0 ? ox + m.w : std::numeric_limits<float>::infinity()); break;
        }
    }
    const float maxS = maxScroll(d);
    if (maxS > 0) {   // same scrollbar as the editor
        const float contentH = maxS + e.h;
        const float thumbH = std::max(24 * s, e.h * e.h / contentH);
        const float thumbY = e.y + (e.h - thumbH) * std::clamp(sy / maxS, 0.0f, 1.0f);
        const float w = (scrollbarDrag_ ? 10.0f : 7.0f) * s;
        fill(e.right() - w - 4 * s, thumbY, w, thumbH, theme::scrollbar, w / 2);
    }
}

bool App::previewKey(const KeyEvent& k) {
    if (k.ctrl || k.alt) return false;
    Document& d = doc();
    const float page = std::max(40.0f, L_.editor.h * 0.9f), line = 56 * S();
    switch (k.key) {
        case Key::Up: d.targetY -= line; break;
        case Key::Down: d.targetY += line; break;
        case Key::PageUp: d.targetY -= page; break;
        case Key::PageDown: d.targetY += page; break;
        case Key::Home: d.targetY = 0; break;
        case Key::End: d.targetY = maxScroll(d); break;
        case Key::Char: if (k.ch == U' ') { d.targetY += k.shift ? -page : page; break; } return true;   // typing does nothing here
        case Key::Enter: case Key::Backspace: case Key::Delete: case Key::Tab: return true;
        default: return false;
    }
    clampScroll(d);
    dirty_ = true;
    return true;
}

// Is the pointer over a link of the preview shown in pane `p`? Keeps the hover highlight in sync.
bool App::previewHoverAt(Pane& p) {
    auto it = previews_.find(p.cur);
    if (it == previews_.end()) return false;
    MdView& v = it->second;
    const float cx = mx_ - p.editor.x, cy = my_ - p.editor.y + std::round(p.cur->scrollY);
    std::string url;
    for (const MdLink& l : v.lay.links)
        if (l.r.contains(cx, cy)) { url = l.url; break; }
    if (url != v.hoverUrl) { v.hoverUrl = url; dirty_ = true; }
    return !url.empty();
}

void App::previewClick() {
    Pane* p = paneAt(mx_, my_);
    if (!p || !p->cur) return;
    auto it = previews_.find(p->cur);
    if (it == previews_.end()) return;
    MdView& v = it->second;
    const float cx = mx_ - p->editor.x, cy = my_ - p->editor.y + std::round(p->cur->scrollY);
    for (const MdLink& l : v.lay.links)
        if (l.r.contains(cx, cy)) { openLink(v, l.url); return; }
}

void App::openLink(MdView& v, const std::string& url) {
    if (url.empty()) return;
    if (url[0] == '#') {   // anchor inside this document
        std::string want = url.substr(1);
        for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& [slug, y] : v.lay.anchors)
            if (slug == want) {
                if (Document* pd = previewOf(*v.src)) {
                    pd->targetY = std::clamp(y - std::round(14 * S()), 0.0f, std::max(0.0f, v.lay.height - L_.editor.h));
                    dirty_ = true;
                }
                return;
            }
        setMessage("No heading " + url, true, 3);
        return;
    }
    const bool web = url.starts_with("http://") || url.starts_with("https://") || url.starts_with("mailto:");
    if (web) {
        if (std::ranges::any_of(url, [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return;
        char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>("exec xdg-open \"$1\" >/dev/null 2>&1 &"),
                        const_cast<char*>("sh"), const_cast<char*>(url.c_str()), nullptr};   // the URL is an argument, never part of the command
        pid_t pid = 0;
        if (::posix_spawnp(&pid, "sh", nullptr, nullptr, argv, environ) == 0) ::waitpid(pid, nullptr, 0);
        setMessage("Opening " + url, false, 2);
        return;
    }
    if (url.find("://") != std::string::npos) return;   // unknown scheme
    std::string rel = url.substr(0, url.find_first_of("#?"));
    if (rel.empty()) return;
    fs::path target = rel[0] == '/' && explorer_.isOpen() ? explorer_.rootPath() / rel.substr(1) : fs::path(v.src->path).parent_path() / rel;
    target = target.lexically_normal();
    std::error_code ec;
    if (fs::is_regular_file(target, ec)) openFile(target.string());
    else setMessage("Not found: " + target.string(), true, 3);
}

// ---------------------------------------------------------------------------- pictures

namespace {

std::string cacheFileFor(const std::string& url) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (unsigned char c : url) { h ^= c; h *= 1099511628211ull; }
    fs::path base;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) base = x;
    else if (const char* home = std::getenv("HOME"); home && *home) base = fs::path(home) / ".cache";
    else return {};
    return (base / "daemon" / "images" / std::format("{:016x}.img", h)).string();
}

}  // namespace

// Maps an image source of a document to its cache key: an absolute file path, or an https URL. Anything else is refused
// (plain http, data: URIs, other schemes): the alt text is shown instead.
std::string App::pictureKey(MdView& v, const std::string& src) {
    if (auto it = v.keys.find(src); it != v.keys.end()) return it->second;
    std::string key;
    if (src.starts_with("https://")) {
        if (!std::getenv("DAEMON_NO_REMOTE_IMAGES")) key = src;
    } else if (src.find("://") == std::string::npos && !src.starts_with("data:") && !src.empty() && v.src) {
        std::string rel = src.substr(0, src.find_first_of("#?"));
        fs::path p = rel.starts_with("/") && explorer_.isOpen() && fs::exists(explorer_.rootPath() / rel.substr(1))
                         ? explorer_.rootPath() / rel.substr(1)
                         : (rel.starts_with("/") ? fs::path(rel) : fs::path(v.src->path).parent_path() / rel);
        key = p.lexically_normal().string();
    }
    v.keys.emplace(src, key);
    return key;
}

std::pair<int, int> App::pictureSize(MdView& v, const std::string& src) {
    const std::string key = pictureKey(v, src);
    if (key.empty()) return {0, 0};
    auto it = picCache_.find(key);
    if (it != picCache_.end()) {
        Pic& p = it->second;
        if (p.state == Pic::State::Failed) return {0, 0};
        if (p.state == Pic::State::Ready) {
            if (p.epoch == pics_.epoch()) return {p.natW, p.natH};
            const int loads = p.loads + 1;   // the atlas was emptied meanwhile: load it again (but never ping-pong forever)
            p = Pic{};
            p.loads = loads;
            if (loads > 2) { p.state = Pic::State::Failed; return {0, 0}; }
            picQueue_.push_back(key);
            pumpPictures();
        }
        return {-1, -1};
    }
    picCache_[key] = Pic{};
    picQueue_.push_back(key);
    pumpPictures();
    return {-1, -1};
}

void App::pumpPictures() {
    while (picJobs_ < 4 && !picQueue_.empty()) {
        const std::string key = picQueue_.front();
        picQueue_.pop_front();
        if (picCache_.contains(key)) startPicture(key);
    }
}

void App::startPicture(const std::string& key) {
    ++picJobs_;
    spawn([this, key] {
        std::optional<ImageLevels> out;
        std::string error;
        std::string bytes;
        if (key.starts_with("https://")) {
            const std::string file = cacheFileFor(key);
            if (file.empty()) error = "no cache directory";
            else {
                std::error_code ec;
                if (!fs::exists(file, ec)) {   // not cached yet: fetch (https only, size and time limited, generic client name)
                    fs::create_directories(fs::path(file).parent_path(), ec);
                    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
                    for (const auto& e : fs::directory_iterator(fs::path(file).parent_path(), ec))   // forget pictures not used for a month
                        if (e.path().extension() == ".img" && fs::last_write_time(e.path(), ec) < cutoff) fs::remove(e.path(), ec);
                    ProcessOptions po;
                    po.argv = {"curl", "-fsSL", "--max-time", "20", "--max-filesize", "12000000", "--proto", "=https", "--proto-redir", "=https",
                               "--max-redirs", "5", "-A", "Daemon-IDE", "-o", file + ".part", "--url", key};
                    auto r = Process::runCapture(po, 30000);
                    if (r.exitCode != 0) { error = "download failed"; fs::remove(file + ".part", ec); }
                    else fs::rename(file + ".part", file, ec);
                }
                if (error.empty()) {
                    if (auto sz = fs::file_size(file, ec); !ec && sz <= 32ull * 1024 * 1024) { if (auto d = readFile(file)) bytes = std::move(*d); }
                    else error = "file too large";
                }
            }
        } else {
            std::error_code ec;
            if (!fs::is_regular_file(key, ec)) error = "not found";
            else if (auto sz = fs::file_size(key, ec); ec || sz > 32ull * 1024 * 1024) error = "file too large";
            else if (auto d = readFile(key)) bytes = std::move(*d);
            else error = "unreadable";
        }
        if (error.empty())
            out = decodeImage({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, 1400, error);
        return [this, key, out = std::move(out), error]() mutable { finishPicture(key, std::move(out), error); };
    });
}

void App::finishPicture(const std::string& key, std::optional<ImageLevels> img, const std::string& error) {
    --picJobs_;
    auto it = picCache_.find(key);
    if (it != picCache_.end()) {
        Pic& p = it->second;
        if (!img) { p.state = Pic::State::Failed; if (!error.empty()) logLine(std::format("preview image {}: {}", key, error)); }
        else {
            auto place = [&] {
                p.levels.clear();
                for (const Image& lv : img->levels) {
                    auto r = pics_.add(lv);
                    if (!r) return false;
                    p.levels.push_back(*r);
                }
                return true;
            };
            bool ok = place();
            if (!ok) {   // atlas full: start over (other pictures reload when the layout asks for them again)
                pics_.clear();
                ok = place();
            }
            if (ok) { p.state = Pic::State::Ready; p.natW = img->naturalW; p.natH = img->naturalH; p.epoch = pics_.epoch(); }
            else p.state = Pic::State::Failed;
        }
    }
    for (auto& [d, v] : previews_) v.width = 0;   // lay the documents out again with the picture's real size
    dirty_ = true;
    pumpPictures();
}

void App::releasePictures() {
    picCache_.clear();
    picQueue_.clear();
    pics_.clear();
}

}  // namespace ide
