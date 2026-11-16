// SPDX-License-Identifier: GPL-3.0-or-later
module daemon.services.settings;
import std;
import daemon.base.json;
import daemon.base.util;

namespace fs = std::filesystem;

namespace ide::settings {

namespace {

Def boolean(const char* key, const char* section, const char* label, const char* help, bool def, bool restart = false) {
    Def d; d.key = key; d.section = section; d.label = label; d.help = help; d.type = Type::Bool; d.def = def; d.needsRestart = restart;
    return d;
}
Def integer(const char* key, const char* section, const char* label, const char* help, int def, int lo, int hi, const char* unit = "", int step = 1) {
    Def d; d.key = key; d.section = section; d.label = label; d.help = help; d.type = Type::Int; d.def = def; d.min = lo; d.max = hi; d.unit = unit; d.step = step;
    return d;
}
Def text(const char* key, const char* section, const char* label, const char* help, const char* def = "") {
    Def d; d.key = key; d.section = section; d.label = label; d.help = help; d.type = Type::Text; d.def = std::string(def);
    return d;
}

}  // namespace

const std::vector<Def>& defs() {
    static const std::vector<Def> t = [] {
        std::vector<Def> v;
        v.push_back(integer("editor.fontSize", "Editor", "Font size", "Size of the code font (Ctrl+= / Ctrl+- change it too).", 14, 8, 48, "px"));
        v.push_back(integer("editor.tabWidth", "Editor", "Tab width", "Columns a tab stands for, and spaces per indent step (when not detected).", 4, 1, 8));
        v.push_back(boolean("editor.insertSpaces", "Editor", "Indent with spaces", "Tab and the indent commands insert spaces instead of tab characters.", true));
        v.push_back(boolean("editor.detectIndent", "Editor", "Detect indentation", "Use the tabs or spaces a file already uses; these settings apply to new and unclear files.", true));
        v.push_back(boolean("editor.autoPair", "Editor", "Close brackets and quotes", "Typing ( [ { \" ' also types the closing character.", true));
        v.push_back(boolean("editor.minimap", "Editor", "Minimap", "Show the code overview at the right edge of the editor.", true));
        v.push_back(boolean("editor.breadcrumbs", "Editor", "Breadcrumbs", "Show the path of the file above the code.", true));
        v.push_back(boolean("editor.cursorBlink", "Editor", "Blinking cursor", "The caret blinks while you are idle.", true));
        v.push_back(boolean("editor.autoReload", "Editor", "Reload changed files", "Reload a file that changed on disk when it has no unsaved edits.", true));

        v.push_back(boolean("outline.show", "Appearance", "Structure view", "List the classes, functions and headings of the current file under the file explorer.", true));
        v.push_back(integer("outline.heightPercent", "Appearance", "Structure view height", "Share of the sidebar the structure view takes (drag the divider to change it).", 38, 15, 70, "%", 5));
        v.push_back(integer("appearance.uiFontSize", "Appearance", "Interface font size", "Size of menus, tabs, the sidebar and the status bar.", 13, 10, 20, "px"));
        v.push_back(boolean("appearance.animations", "Appearance", "Animations", "Slide the panels, tabs and the menu bar instead of switching at once.", true));
        v.push_back(boolean("appearance.startMaximized", "Appearance", "Start maximized", "Open the window maximized.", true, true));

        v.push_back(boolean("general.restoreLastProject", "General", "Reopen the last project", "A plain launch continues with the project you closed last.", true));
        v.push_back(boolean("general.restoreSession", "General", "Restore open tabs", "Reopen the files, splits and cursor positions of a project.", true));

        v.push_back(boolean("git.gutter", "Git", "Change markers", "Mark added, changed and removed lines next to the line numbers.", true));
        v.push_back(boolean("git.blame", "Git", "Inline blame", "Show who last changed the current line, at the end of the line.", true));

        v.push_back(boolean("lsp.enabled", "C/C++ intelligence", "Use clangd", "Completion, diagnostics, hover and go to definition.", true));
        v.push_back(text("lsp.clangd", "C/C++ intelligence", "clangd program", "Name or path of clangd. Empty: look it up in PATH (the IDE_CLANGD variable wins)."));
        v.push_back(boolean("lsp.backgroundIndex", "C/C++ intelligence", "Index the project in the background", "Faster navigation and references across files, at the cost of some CPU.", true));

        v.push_back(boolean("glsl.diagnostics", "Shaders", "Check shaders for errors", "Compile the shader with glslc (or glslangValidator) while you type and mark the errors. Needs one of them installed.", true));
        {
            Def d; d.key = "glsl.target"; d.section = "Shaders"; d.label = "Target API"; d.help = "Auto: Vulkan for shaders that use sets, push constants or gl_VertexIndex, OpenGL otherwise.";
            d.type = Type::Choice; d.def = std::string("auto"); d.choices = {"auto", "vulkan", "opengl"};
            v.push_back(std::move(d));
        }
        v.push_back(text("terminal.shell", "Terminal", "Shell", "Program started in the terminal. Empty: your login shell ($SHELL)."));

        v.push_back(text("notebook.compiler", "Notebooks", "Compiler", "g++ 15 or newer that builds the cells. Empty: g++ (the DAEMON_NB_CXX variable wins)."));
        v.push_back(text("notebook.flags", "Notebooks", "Extra compiler flags", "Added to every cell, e.g. -O1 -Wall. %flags in a cell adds to them."));

        v.push_back(boolean("markdown.remoteImages", "Markdown", "Load images from the web", "Preview pictures that are linked with https (cached on disk).", true));
        v.push_back(boolean("general.toolReminders", "Tools", "Remind me about missing tools", "Open this page at start-up when a tool Daemon needs is missing.", true));
        return v;
    }();
    return t;
}

const Def* find(std::string_view key) {
    for (const auto& d : defs()) if (d.key == key) return &d;
    return nullptr;
}

std::vector<std::string> sections() {
    std::vector<std::string> out;
    for (const auto& d : defs())
        if (std::ranges::find(out, d.section) == out.end()) out.push_back(d.section);
    return out;
}

Value Settings::get(std::string_view key) const {
    if (auto it = v_.find(key); it != v_.end()) return it->second;
    if (const Def* d = find(key)) return d->def;
    return false;
}
bool Settings::b(std::string_view key) const { const Value v = get(key); return std::holds_alternative<bool>(v) && std::get<bool>(v); }
int Settings::i(std::string_view key) const { const Value v = get(key); return std::holds_alternative<int>(v) ? std::get<int>(v) : 0; }
std::string Settings::s(std::string_view key) const { const Value v = get(key); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string(); }

bool Settings::set(std::string_view key, const Value& val) {
    const Def* d = find(key);
    if (!d) return false;
    Value v = val;
    switch (d->type) {
        case Type::Bool: if (!std::holds_alternative<bool>(v)) return false; break;
        case Type::Int:
            if (!std::holds_alternative<int>(v)) return false;
            v = std::clamp(std::get<int>(v), d->min, d->max);
            break;
        case Type::Choice:
            if (!std::holds_alternative<std::string>(v) || std::ranges::find(d->choices, std::get<std::string>(v)) == d->choices.end()) return false;
            break;
        case Type::Text:
            if (!std::holds_alternative<std::string>(v)) return false;
            {   // one line, no stray blanks
                std::string t = std::get<std::string>(v);
                std::erase_if(t, [](char c) { return c == '\n' || c == '\r'; });
                const auto a = t.find_first_not_of(" \t"), z = t.find_last_not_of(" \t");
                v = a == std::string::npos ? std::string() : t.substr(a, z - a + 1);
            }
            break;
    }
    if (v == d->def) v_.erase(std::string(key)); else v_[std::string(key)] = v;
    return true;
}

void Settings::reset(std::string_view key) { if (auto it = v_.find(key); it != v_.end()) v_.erase(it); }
bool Settings::isDefault(std::string_view key) const { return !v_.contains(key); }

std::string Settings::toText() const {
    std::string out = "{";
    bool first = true;
    auto line = [&](const std::string& key, const json::Value& val) {
        out += first ? "\n  " : ",\n  ";
        first = false;
        json::detail::dumpString(out, key);
        out += ": " + json::dump(val);
    };
    for (const auto& d : defs()) {
        auto it = v_.find(d.key);
        if (it == v_.end()) continue;
        if (auto* b = std::get_if<bool>(&it->second)) line(d.key, json::Value(*b));
        else if (auto* n = std::get_if<int>(&it->second)) line(d.key, json::Value(*n));
        else line(d.key, json::Value(std::get<std::string>(it->second)));
    }
    for (const auto& [k, val] : extra_) line(k, val);
    out += first ? "}\n" : "\n}\n";
    return out;
}

Settings Settings::fromText(std::string_view text, std::vector<std::string>* problems) {
    Settings s;
    auto j = json::parse(text);
    if (!j || !j->isObject()) {
        if (problems && !text.empty() && text.find_first_not_of(" \t\r\n") != std::string_view::npos) problems->push_back("settings.json is not valid JSON: all settings are at their defaults");
        return s;
    }
    for (const auto& [key, val] : *j->object()) {
        const Def* d = find(key);
        if (!d) { s.extra_.emplace_back(key, val); continue; }
        bool ok = false;
        switch (d->type) {
            case Type::Bool: if (auto* b = std::get_if<bool>(&val.v)) ok = s.set(key, *b); break;
            case Type::Int: if (val.isNumber()) ok = s.set(key, val.integer()); break;
            case Type::Choice: case Type::Text: if (val.isString()) ok = s.set(key, val.str()); break;
        }
        if (!ok && problems) problems->push_back(std::format("{}: unusable value, the default is used", key));
    }
    return s;
}

fs::path settingsFile() {
    fs::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = fs::path(h) / ".config";
    else return {};
    return base / "daemon" / "settings.json";
}

Settings load(std::vector<std::string>* problems) {
    const fs::path f = settingsFile();
    if (f.empty()) return {};
    auto text = readFile(f);
    if (!text) return {};
    return Settings::fromText(*text, problems);
}

bool save(const Settings& s) {
    const fs::path f = settingsFile();
    if (f.empty()) return false;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    const fs::path tmp = f.string() + ".tmp";
    if (!writeFile(tmp, s.toText())) return false;
    fs::rename(tmp, f, ec);
    return !ec;
}

Settings& current() {
    static Settings s;
    return s;
}

}  // namespace ide::settings
