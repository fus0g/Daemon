// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / glsl_view - GLSL shaders: completion, parameter hints, hover, go to definition, outline and error checking, all
// from libs/services/glsl (no language server). The results feed the same popups the C++ intelligence uses.
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.theme;
import daemon.text.document;
import daemon.services.lsp;
import daemon.services.glsl;
import daemon.services.settings;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

// What the GLSL intelligence looks at: a whole shader file, or the contents of a raw string inside a C++ file.
std::optional<App::GlslView> App::glslViewAt(Document& d, Pos p) {
    GlslView v;
    if (isGlslDoc(d)) {
        v.text = d.buf.text();
        v.line = p.line; v.col = p.col;
        v.stage = glsl::stageOf(d.path, v.text);
        return v;
    }
    if (!d.lang || !d.lang->rawStrings) return std::nullopt;
    auto r = d.hl.rawRegionAt(d.buf, p.line, p.col);
    if (!r || !r->glsl) return std::nullopt;
    v.text = d.buf.slice(r->begin, r->end);
    v.origin = r->begin;
    v.line = p.line - r->begin.line;
    v.col = v.line == 0 ? p.col - r->begin.col : p.col;
    v.stage = glsl::guessStage(v.text, r->hint);
    return v;
}

void App::glslComplete() {
    if (docs_.empty()) return;
    Document& d = doc();
    auto v = glslViewAt(d, d.cursor);
    if (!v) { closeCompletion(); return; }
    auto items = glsl::complete(v->text, v->line, v->col, v->stage);
    if (items.empty()) { closeCompletion(); return; }
    showCompletion(std::move(items), d.cursor, -1, false);
}

void App::glslSignature() {
    if (docs_.empty()) return;
    Document& d = doc();
    auto v = glslViewAt(d, d.cursor);
    auto help = v ? glsl::signature(v->text, v->line, v->col) : std::nullopt;
    if (!help) { if (sig_.open) { sig_ = {}; dirty_ = true; } return; }
    sig_ = {};
    sig_.open = true;
    sig_.help = std::move(*help);
    sig_.at = d.cursor;
    sig_.owner = &d;
    dirty_ = true;
}

// Called after every key in a GLSL document or a GLSL raw string (the completion list has already been refiltered).
void App::glslAfterKey(const KeyEvent& k, std::uint64_t verBefore) {
    Document& d = doc();
    if (k.key == Key::Escape && sig_.open) { closeSignature(); return; }
    const bool typed = !k.ctrl && !k.alt && !k.text.empty() && d.buf.version() != verBefore;
    if (typed) {
        const char c = k.text.back();
        const std::string& ln = d.buf.line(d.cursor.line);
        const std::size_t first = ln.find_first_not_of(" \t");
        if (identChar(c)) { if (!comp_.open) glslComplete(); }
        else if (c == '.' || (c == '#' && first != std::string::npos && ln[first] == '#') || c == '(') glslComplete();
        else closeCompletion();
        if (c == '(' || c == ',' || sig_.open || c == ')') glslSignature();
    } else if (sig_.open) glslSignature();   // the cursor moved: follow it
    if (k.ctrl && !k.alt && k.key == Key::Char && k.ch == ' ') glslComplete();
}

bool App::glslHover(Document& d, Pos at, std::string& out) {
    auto v = glslViewAt(d, at);
    if (!v) return false;
    out = glsl::hover(v->text, v->line, v->col, v->stage);
    return !out.empty();
}

bool App::glslGoto() {
    Document& d = doc();
    auto v = glslViewAt(d, d.cursor);
    if (!v) return false;
    auto def = glsl::definition(v->text, v->line, v->col);
    if (!def) { setMessage("No definition found", true, 2.5f); return true; }
    navBack_.push_back({d.path, d.cursor});
    if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
    d.setCursor({def->first + v->origin.line, def->second + (def->first == 0 ? v->origin.col : 0)});
    scrollToCursor_ = true;
    dirty_ = true;
    return true;
}

// Keeps the GLSL diagnostics of raw strings next to the ones clangd publishes for the same file.
void App::mergeEmbeddedDiags(const std::string& path) {
    auto& v = diags_[path];
    std::erase_if(v, [](const LspDiag& g) { return g.source == "glslc" || g.source == "glslang"; });
    if (auto it = embDiags_.find(path); it != embDiags_.end()) v.insert(v.end(), it->second.begin(), it->second.end());
    if (v.empty()) diags_.erase(path);
}

// Error checking: a moment after the last edit, the text goes to glslc / glslangValidator on a worker thread.
void App::glslTick(Clock::time_point now) {
    if (docs_.empty()) return;
    Document& d = doc();
    if (d.path.empty()) return;
    if (!isGlslDoc(d)) { if (d.lang && d.lang->rawStrings) glslEmbeddedTick(now, d); return; }
    GlslState& st = glsl_[&d];
    const std::uint64_t ver = d.buf.version();
    if (!cfgB("glsl.diagnostics")) { if (diags_.erase(d.path)) dirty_ = true; st.askedVersion = ver; return; }
    if (st.askedVersion == ver || st.busy) return;
    if (st.dueVersion != ver) { st.dueVersion = ver; st.due = now + std::chrono::milliseconds(st.askedVersion == ~0ull ? 150 : 700); }
    if (now < st.due) return;
    const std::string text = d.buf.text();
    const std::string stage = glsl::stageOf(d.path, text);
    st.askedVersion = ver;
    if (stage.empty()) { if (diags_.erase(d.path)) dirty_ = true; return; }   // an include file: nothing to compile on its own
    const std::string tgt = cfgS("glsl.target");
    const glsl::Target target = tgt == "vulkan" ? glsl::Target::Vulkan : tgt == "opengl" ? glsl::Target::OpenGL : glsl::Target::Auto;
    const std::string dir = fs::path(d.path).parent_path().string(), path = d.path;
    const Document* dp = &d;
    st.busy = true;
    spawn([this, text, stage, target, dir, path, dp]() -> std::function<void()> {
        std::string error;
        auto diags = glsl::diagnose(text, stage, target, dir, error);
        return [this, dp, path, diags = std::move(diags), error = std::move(error)]() mutable {
            if (!docAlive(dp)) return;
            glsl_[dp].busy = false;
            if (!error.empty()) {
                if (!glslNoticeShown_) { glslNoticeShown_ = true; setMessage("Shader error checking is off: " + error + " (see Settings > Tools)", true, 7); }
                return;
            }
            if (diags.empty()) diags_.erase(path); else diags_[path] = std::move(diags);
            dirty_ = true;
        };
    });
}

// Shaders inside raw strings of a C++ file are compiled too; the messages land on the right lines of the C++ file.
void App::glslEmbeddedTick(Clock::time_point now, Document& d) {
    GlslState& st = glsl_[&d];
    const std::uint64_t ver = d.buf.version();
    if (st.askedVersion == ver || st.busy) return;
    if (st.dueVersion != ver) { st.dueVersion = ver; st.due = now + std::chrono::milliseconds(st.askedVersion == ~0ull ? 400 : 900); }
    if (now < st.due) return;
    st.askedVersion = ver;
    const auto regions = d.hl.glslRegions(d.buf);
    if (regions.empty() || !cfgB("glsl.diagnostics")) {
        if (embDiags_.erase(d.path)) { mergeEmbeddedDiags(d.path); dirty_ = true; }
        return;
    }
    struct Job { std::string text, stage; Pos origin; };
    std::vector<Job> jobs;
    for (const auto& r : regions) {
        Job j;
        j.text = d.buf.slice(r.begin, r.end);
        j.stage = glsl::guessStage(j.text, r.hint);
        j.origin = r.begin;
        if (!j.stage.empty()) jobs.push_back(std::move(j));
    }
    const std::string tgt = cfgS("glsl.target");
    const glsl::Target target = tgt == "vulkan" ? glsl::Target::Vulkan : tgt == "opengl" ? glsl::Target::OpenGL : glsl::Target::Auto;
    const std::string dir = fs::path(d.path).parent_path().string(), path = d.path;
    const Document* dp = &d;
    st.busy = true;
    spawn([this, jobs = std::move(jobs), target, dir, path, dp]() -> std::function<void()> {
        std::vector<LspDiag> all;
        std::string error;
        for (const auto& j : jobs) {
            std::string e;
            auto diags = glsl::diagnose(j.text, j.stage, target, dir, e);
            if (!e.empty()) { error = e; break; }
            for (auto& g : diags) {   // lines count from the start of the string: move them into the file
                const bool firstLine = g.line == 0, endFirst = g.endLine == 0;
                g.line += j.origin.line; g.endLine += j.origin.line;
                if (firstLine) g.col += j.origin.col;
                if (endFirst) g.endCol += j.origin.col;
                all.push_back(std::move(g));
            }
        }
        return [this, dp, path, all = std::move(all), error = std::move(error)]() mutable {
            if (!docAlive(dp)) return;
            glsl_[dp].busy = false;
            if (!error.empty()) return;   // (no compiler: said once for .glsl files; embedded shaders stay quiet)
            if (all.empty()) embDiags_.erase(path); else embDiags_[path] = std::move(all);
            mergeEmbeddedDiags(path);
            dirty_ = true;
        };
    });
}

std::optional<Clock::time_point> App::glslWake() const {
    if (docs_.empty() || !cfgB("glsl.diagnostics")) return std::nullopt;
    const Document& d = *docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))];
    if (d.path.empty() || (!isGlslDoc(d) && !(d.lang && d.lang->rawStrings))) return std::nullopt;
    auto it = glsl_.find(&d);
    if (it == glsl_.end()) return Clock::now() + std::chrono::milliseconds(50);
    if (it->second.busy || it->second.askedVersion == d.buf.version()) return std::nullopt;
    return it->second.dueVersion == d.buf.version() ? it->second.due : Clock::now() + std::chrono::milliseconds(50);
}

}  // namespace ide
