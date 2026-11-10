// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / prompts - part of the App implementation (see app.cppm for the class).
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

void App::openPrompt(PromptKind kind) {
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

void App::promptChanged() {
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

void App::promptKey(const KeyEvent& k) {
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

void App::submitPrompt(const KeyEvent& k) {
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

void App::completePath(Field& f) {
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

}  // namespace ide
