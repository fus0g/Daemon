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

void App::glslComplete() {
    if (docs_.empty()) return;
    Document& d = doc();
    const std::string text = d.buf.text();
    const std::string stage = glsl::stageOf(d.path, text);
    auto items = glsl::complete(text, d.cursor.line, d.cursor.col, stage);
    if (items.empty()) { closeCompletion(); return; }
    showCompletion(std::move(items), d.cursor, -1, false);
}

void App::glslSignature() {
    if (docs_.empty()) return;
    Document& d = doc();
    auto help = glsl::signature(d.buf.text(), d.cursor.line, d.cursor.col);
    if (!help) { if (sig_.open) { sig_ = {}; dirty_ = true; } return; }
    sig_ = {};
    sig_.open = true;
    sig_.help = std::move(*help);
    sig_.at = d.cursor;
    sig_.owner = &d;
    dirty_ = true;
}

// Called after every key in a GLSL document (the completion list has already been refiltered).
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

bool App::glslHover(const Document& d, Pos at, std::string& out) {
    const std::string text = d.buf.text();
    out = glsl::hover(text, at.line, at.col, glsl::stageOf(d.path, text));
    return !out.empty();
}

bool App::glslGoto() {
    Document& d = doc();
    auto def = glsl::definition(d.buf.text(), d.cursor.line, d.cursor.col);
    if (!def) { setMessage("No definition found", true, 2.5f); return true; }
    navBack_.push_back({d.path, d.cursor});
    if (navBack_.size() > 100) navBack_.erase(navBack_.begin());
    d.setCursor({def->first, def->second});
    scrollToCursor_ = true;
    dirty_ = true;
    return true;
}

// Error checking: a moment after the last edit, the text goes to glslc / glslangValidator on a worker thread.
void App::glslTick(Clock::time_point now) {
    if (docs_.empty()) return;
    Document& d = doc();
    if (!isGlslDoc(d) || d.path.empty()) return;
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

std::optional<Clock::time_point> App::glslWake() const {
    if (docs_.empty() || !cfgB("glsl.diagnostics")) return std::nullopt;
    const Document& d = *docs_[static_cast<std::size_t>(std::clamp(cur_, 0, static_cast<int>(docs_.size()) - 1))];
    if (!isGlslDoc(d) || d.path.empty()) return std::nullopt;
    auto it = glsl_.find(&d);
    if (it == glsl_.end()) return Clock::now() + std::chrono::milliseconds(50);
    if (it->second.busy || it->second.askedVersion == d.buf.version()) return std::nullopt;
    return it->second.dueVersion == d.buf.version() ? it->second.due : Clock::now() + std::chrono::milliseconds(50);
}

}  // namespace ide
