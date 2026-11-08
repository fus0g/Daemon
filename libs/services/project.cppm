// daemon.project — project templates (the "New Project" flow) and the recent-projects list.
export module daemon.services.project;
import std;
import daemon.base.json;
import daemon.base.process;
import daemon.base.util;

namespace fs = std::filesystem;

export namespace ide {

struct ProjectTemplate {
    std::string id, name, description;
    bool isC = false;        // C instead of C++
    bool needsModules = false;
};

inline const std::vector<ProjectTemplate>& projectTemplates() {
    static const std::vector<ProjectTemplate> t = {
        {"cpp-exe", "C++ Executable", "A console application with a single main.cpp.", false, false},
        {"cpp-modules", "C++ Executable (modules)", "C++20 modules: a module interface unit imported by main.cpp.", false, true},
        {"cpp-lib", "C++ Static Library", "A library with a public header, plus an example application that uses it.", false, false},
        {"c-exe", "C Executable", "A console application in C with a single main.c.", true, false},
        {"empty", "Empty CMake Project", "Just a CMakeLists.txt; add your own targets.", false, false},
    };
    return t;
}

struct NewProjectSpec {
    std::string templateId = "cpp-exe";
    std::string name;
    fs::path parent;
    std::string standard = "20";   // C++: 17 / 20 / 23, C: 11 / 17
    bool initGit = true;
};

struct CreatedProject {
    fs::path dir;
    fs::path mainFile;   // the file to open first
    std::string note;    // non-fatal remark (e.g. git not found)
};

// Empty string when valid, otherwise a short human-readable problem.
inline std::string validateProjectName(std::string_view name) {
    if (name.empty()) return "Enter a project name";
    if (name.size() > 64) return "The name is too long";
    if (name.front() == '.' || name.front() == '-') return "The name cannot start with '.' or '-'";
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return "Use letters, digits, '_', '-' and '.' only";
    return {};
}

namespace project_detail {

inline std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

inline std::string identOf(std::string_view name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

inline bool write(const fs::path& p, const std::string& text, std::string& err) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) { err = "Cannot write " + p.string(); return false; }
    o << text;
    return true;
}

}  // namespace project_detail

// Creates the project directory with its files (and optionally a git repository).
inline std::expected<CreatedProject, std::string> createProject(const NewProjectSpec& spec) {
    using namespace project_detail;
    if (auto e = validateProjectName(spec.name); !e.empty()) return std::unexpected(e);
    if (spec.parent.empty()) return std::unexpected("Choose a location");
    const fs::path dir = spec.parent / spec.name;
    std::error_code ec;
    if (fs::exists(dir, ec) && !fs::is_empty(dir, ec)) return std::unexpected("'" + dir.string() + "' already exists and is not empty");
    fs::create_directories(dir, ec);
    if (ec) return std::unexpected("Cannot create " + dir.string() + ": " + ec.message());

    const std::string name = spec.name, ident = identOf(name);
    const ProjectTemplate* tmpl = nullptr;
    for (const auto& t : projectTemplates()) if (t.id == spec.templateId) tmpl = &t;
    if (!tmpl) return std::unexpected("Unknown template");
    std::string std_ = spec.standard.empty() ? (tmpl->isC ? "17" : "20") : spec.standard;
    if (tmpl->needsModules && (std_ == "11" || std_ == "14" || std_ == "17")) std_ = "20";

    auto sub = [&](std::string text) {
        text = replaceAll(std::move(text), "@NAME@", name);
        text = replaceAll(std::move(text), "@IDENT@", ident);
        text = replaceAll(std::move(text), "@STD@", std_);
        return text;
    };
    std::string err;
    CreatedProject out;
    out.dir = dir;

    const std::string cxxCommon =
        "set(CMAKE_CXX_STANDARD @STD@)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n";

    if (tmpl->id == "cpp-exe") {
        if (!write(dir / "CMakeLists.txt", sub("cmake_minimum_required(VERSION 3.20)\nproject(@NAME@ LANGUAGES CXX)\n\n" + cxxCommon +
                                               "\nadd_executable(@NAME@ main.cpp)\n"), err)) return std::unexpected(err);
        if (!write(dir / "main.cpp", sub("#include <iostream>\n\nint main() {\n    std::cout << \"Hello, @NAME@!\\n\";\n    return 0;\n}\n"), err)) return std::unexpected(err);
        out.mainFile = dir / "main.cpp";
    } else if (tmpl->id == "cpp-modules") {
        if (!write(dir / "CMakeLists.txt", sub("cmake_minimum_required(VERSION 3.28)\nproject(@NAME@ LANGUAGES CXX)\n\n" + cxxCommon +
                                               "\nadd_executable(@NAME@ main.cpp)\n"
                                               "target_sources(@NAME@ PRIVATE\n    FILE_SET CXX_MODULES FILES\n        greeter.cppm)\n"), err)) return std::unexpected(err);
        if (!write(dir / "greeter.cppm", sub("module;\n\n#include <iostream>\n#include <string_view>\n\nexport module greeter;\n\n"
                                             "export void greet(std::string_view who) {\n    std::cout << \"Hello, \" << who << \"!\\n\";\n}\n"), err)) return std::unexpected(err);
        if (!write(dir / "main.cpp", sub("import greeter;\n\nint main() {\n    greet(\"@NAME@\");\n    return 0;\n}\n"), err)) return std::unexpected(err);
        out.mainFile = dir / "main.cpp";
    } else if (tmpl->id == "cpp-lib") {
        if (!write(dir / "CMakeLists.txt", sub("cmake_minimum_required(VERSION 3.20)\nproject(@NAME@ LANGUAGES CXX)\n\n" + cxxCommon +
                                               "\nadd_library(@NAME@ STATIC src/@NAME@.cpp)\n"
                                               "target_include_directories(@NAME@ PUBLIC include)\n\n"
                                               "add_executable(@NAME@_app app/main.cpp)\n"
                                               "target_link_libraries(@NAME@_app PRIVATE @NAME@)\n"), err)) return std::unexpected(err);
        if (!write(dir / "include" / (name + ".hpp"), sub("#pragma once\n\n#include <string>\n\nnamespace @IDENT@ {\n\nstd::string greeting();\n\n}  // namespace @IDENT@\n"), err)) return std::unexpected(err);
        if (!write(dir / "src" / (name + ".cpp"), sub("#include \"@NAME@.hpp\"\n\nnamespace @IDENT@ {\n\nstd::string greeting() { return \"Hello from @NAME@!\"; }\n\n}  // namespace @IDENT@\n"), err)) return std::unexpected(err);
        if (!write(dir / "app" / "main.cpp", sub("#include <iostream>\n\n#include \"@NAME@.hpp\"\n\nint main() {\n    std::cout << @IDENT@::greeting() << \"\\n\";\n    return 0;\n}\n"), err)) return std::unexpected(err);
        out.mainFile = dir / "include" / (name + ".hpp");
    } else if (tmpl->id == "c-exe") {
        if (!write(dir / "CMakeLists.txt", sub("cmake_minimum_required(VERSION 3.20)\nproject(@NAME@ LANGUAGES C)\n\n"
                                               "set(CMAKE_C_STANDARD @STD@)\nset(CMAKE_C_STANDARD_REQUIRED ON)\nset(CMAKE_C_EXTENSIONS OFF)\n"
                                               "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n\nadd_executable(@NAME@ main.c)\n"), err)) return std::unexpected(err);
        if (!write(dir / "main.c", sub("#include <stdio.h>\n\nint main(void) {\n    printf(\"Hello, @NAME@!\\n\");\n    return 0;\n}\n"), err)) return std::unexpected(err);
        out.mainFile = dir / "main.c";
    } else {
        if (!write(dir / "CMakeLists.txt", sub("cmake_minimum_required(VERSION 3.20)\nproject(@NAME@ LANGUAGES CXX)\n\n" + cxxCommon +
                                               "\n# add_executable(@NAME@ main.cpp)\n"), err)) return std::unexpected(err);
        out.mainFile = dir / "CMakeLists.txt";
    }

    if (spec.initGit) {
        if (!findExecutable("git")) {
            out.note = "git was not found, so no repository was created";
        } else {
            write(dir / ".gitignore", "cmake-build-*/\nbuild/\n.cache/\n", err);
            ProcessOptions o;
            o.argv = {"git", "init", "-q"};
            o.cwd = dir.string();
            auto r = Process::runCapture(o, 15000);
            if (r.exitCode != 0) out.note = "git init failed";
        }
    }
    return out;
}

// ---- persistent state: recent projects and the last "new project" location

struct DaemonState {
    std::vector<std::string> recent;
    std::string lastLocation;
};

inline fs::path stateFile() {
    fs::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = fs::path(h) / ".config";
    else return {};
    return base / "daemon" / "state.json";
}

inline DaemonState loadState() {
    DaemonState st;
    const fs::path f = stateFile();
    if (f.empty()) return st;
    auto text = readFile(f);
    if (!text) return st;
    auto j = json::parse(*text);
    if (!j) return st;
    if (auto* a = (*j)["recent"].array())
        for (const auto& v : *a) if (v.isString()) st.recent.push_back(v.str());
    st.lastLocation = (*j)["lastLocation"].str();
    return st;
}

inline void saveState(const DaemonState& st) {
    const fs::path f = stateFile();
    if (f.empty()) return;
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    auto j = json::object();
    auto arr = json::array();
    for (const auto& r : st.recent) arr.push(r);
    j.set("recent", std::move(arr));
    j.set("lastLocation", st.lastLocation);
    std::ofstream(f, std::ios::binary | std::ios::trunc) << json::dump(j);
}

// Moves `path` to the front of the recent list (newest first, de-duplicated, at most 12 entries).
inline void addRecent(DaemonState& st, const std::string& path) {
    std::erase(st.recent, path);
    st.recent.insert(st.recent.begin(), path);
    if (st.recent.size() > 12) st.recent.resize(12);
}

}  // namespace ide
