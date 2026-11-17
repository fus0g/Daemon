// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.cmake — CMake project support in the style of CLion: build profiles, configure / build
// command lines, target discovery through the CMake File API, and compiler diagnostics parsing.
export module daemon.services.cmake;
import std;
import daemon.base.json;
import daemon.base.process;
import daemon.base.util;

namespace fs = std::filesystem;

export namespace ide {

struct CMakeProfile {
    std::string name;       // "Debug"
    std::string buildType;  // CMAKE_BUILD_TYPE
    fs::path dir;           // <root>/cmake-build-debug
};

struct CMakeTarget {
    std::string name;
    std::string type;       // EXECUTABLE, STATIC_LIBRARY, SHARED_LIBRARY, OBJECT_LIBRARY, UTILITY, ...
    fs::path artifact;      // absolute path of the primary output, if any
    bool executable() const { return type == "EXECUTABLE"; }
    bool buildable() const { return type != "INTERFACE_LIBRARY"; }
};

enum class Severity { Error, Warning, Note };

struct Problem {
    Severity severity = Severity::Error;
    fs::path file;
    int line = 1, col = 1;
    std::string message;
};

inline bool hasCMakeLists(const fs::path& root) {
    std::error_code ec;
    return fs::is_regular_file(root / "CMakeLists.txt", ec);
}

inline std::vector<CMakeProfile> defaultProfiles(const fs::path& root) {
    return {{"Debug", "Debug", root / "cmake-build-debug"},
            {"Release", "Release", root / "cmake-build-release"},
            {"RelWithDebInfo", "RelWithDebInfo", root / "cmake-build-relwithdebinfo"}};
}

inline bool isConfigured(const CMakeProfile& p) {
    std::error_code ec;
    return fs::is_regular_file(p.dir / "CMakeCache.txt", ec);
}

inline fs::path compileCommands(const CMakeProfile& p) { return p.dir / "compile_commands.json"; }

// Command line for (re)configuring a profile.
std::vector<std::string> configureArgv(const fs::path& root, const CMakeProfile& p, bool haveNinja);

inline std::vector<std::string> buildArgv(const CMakeProfile& p, const std::string& target) {
    std::vector<std::string> a = {"cmake", "--build", p.dir.string()};
    if (!target.empty()) { a.push_back("--target"); a.push_back(target); }
    a.push_back("--parallel");
    return a;
}

// Registers a File API query so the next configure produces the codemodel reply we read below.
inline void prepareFileApi(const fs::path& buildDir) {
    std::error_code ec;
    fs::create_directories(buildDir / ".cmake/api/v1/query", ec);
    std::ofstream(buildDir / ".cmake/api/v1/query/codemodel-v2").put('\n');
}

inline std::optional<json::Value> readJson(const fs::path& p) {
    auto text = readFile(p);
    if (!text) return std::nullopt;
    return json::parse(*text);
}

// Reads targets from the newest File API reply. Executables sort first (they are what you run).
std::vector<CMakeTarget> loadTargets(const fs::path& buildDir);

// Removes ANSI escape sequences from tool output.
std::string stripAnsi(std::string_view s);

// Parses one line of compiler / CMake output into a located problem, e.g.
//   ../src/main.cpp:12:5: error: expected ';'      CMake Error at CMakeLists.txt:7 (add_executable):
std::optional<Problem> parseProblem(std::string_view line, const fs::path& baseDir, const fs::path& root);

// "[12/34] Building ..." -> {12, 34}
std::optional<std::pair<int, int>> parseProgress(std::string_view line);

// Shell-like splitting of a run-configuration argument string (quotes and backslashes).
std::vector<std::string> splitArgs(std::string_view s);

// ---------------------------------------------------------------------------------------------
// CMake language intelligence: completion for commands, variables, properties, modules, policies,
// generator expressions, targets and per-command keywords. Name lists come from the installed
// `cmake --help-*-list`, so they always match the CMake version in use.

struct CMakeItem {
    std::string label, detail, insert;
    int kind = 0;           // LSP CompletionItemKind
    std::string sort;
};

class CMakeKnowledge {
public:
    // Loads the name lists once (a handful of quick `cmake --help-*-list` runs).
    void load();
    bool ready() const { return loaded_ && !commands.empty(); }

    std::vector<std::string> commands, variables, properties, modules, policies;

private:
    bool loaded_ = false;
};

namespace cmake_detail {

inline bool wordCh(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

const std::unordered_map<std::string, std::vector<std::string>>& keywordTable();

bool takesTargets(const std::string& cmd);

const std::vector<std::string>& genexNames();

}  // namespace cmake_detail

// Everything `text` defines that completion should know about: variables, targets, functions / macros.
struct CMakeDefs {
    std::vector<std::string> variables, targets, functions;
};

CMakeDefs scanCMakeDefs(std::string_view text);

// Completion items for the cursor position at the end of `before` (all text up to the cursor).
// `full` is the whole document (to find user-defined names); `knownTargets` come from the CMake File API.
std::vector<CMakeItem> cmakeCompletions(const CMakeKnowledge& kb, std::string_view before, std::string_view full,
                                               const std::vector<std::string>& knownTargets);

// Folder and file names for the path being typed in an argument: `src/ma`, `../`, `"${CMAKE_CURRENT_SOURCE_DIR}/include/`.
struct CMakePathResult {
    std::vector<CMakeItem> items;   // folders (kind 19, inserted with a trailing /) and files (kind 17)
    std::size_t typed = 0;          // characters of the last path segment already typed (the completion replaces them)
    bool exclusive = false;         // the text is clearly a path (has a /): offer nothing but these
};
// `before` is all text up to the cursor. Relative paths start at `fileDir`; `${CMAKE_SOURCE_DIR}`-like variables are expanded
// with `sourceRoot` / `buildDir`. Empty when the cursor is not in an argument of a command, or the folder cannot be resolved.
std::optional<CMakePathResult> cmakePathCompletions(std::string_view before, const std::filesystem::path& fileDir,
                                                    const std::filesystem::path& sourceRoot, const std::filesystem::path& buildDir);

// ---------------------------------------------------------------------------------------------
// C++20 modules support for clangd.
//
// CMake + GCC writes a compile_commands.json full of GCC-only module flags (-fmodules-ts,
// -fmodule-mapper, p1689 dependency output, duplicate "synthesized BMI" entries), which clangd can
// not interpret: `import foo;` and `import std;` stay unresolved. clangd (with
// --experimental-modules-support) can however build its own Clang BMIs from the module sources, as
// long as the compile database describes them in Clang's terms. So we derive such a database:
// same sources, same defines / include paths / standard, driven by clang++ with libstdc++. The
// `std` module is included because CMake lists libstdc++'s bits/std.cc as a module source.

inline bool isModuleInterfaceExt(const fs::path& p) {
    const std::string e = p.extension().string();
    return e == ".cppm" || e == ".ixx" || e == ".mpp" || e == ".ccm" || e == ".cxxm" || e == ".c++m";
}

namespace cmake_detail {

std::vector<std::string> entryArgs(const json::Value& e);

inline bool usesGccModules(const std::vector<std::string>& args) {
    return std::ranges::any_of(args, [](const std::string& a) {
        return a == "-fmodules-ts" || a.starts_with("-fmodule-mapper=") || a.starts_with("-fdeps-format=");
    });
}

}  // namespace cmake_detail

namespace cmake_detail {

// The language standard a compiler uses when no -std flag is given (CMake omits -std when it matches this,
// but clang's own default differs, and without C++20 `module` / `import` are not even keywords).
std::string compilerDefaultStd(const std::string& compiler);

inline bool hasStdFlag(const std::vector<std::string>& args) {
    return std::ranges::any_of(args, [](const std::string& a) { return a.starts_with("-std=") || a.starts_with("--std=") || a.starts_with("-std:"); });
}

}  // namespace cmake_detail

struct ClangdDb {
    fs::path dir;          // value for --compile-commands-dir
    bool derived = false;  // true when a Clang-flavoured copy was generated
    int sources = 0;
};

// Returns the compile-database directory clangd should use for `buildDir`, generating the derived
// module-aware database under <buildDir>/.daemon/clangd when the CMake one targets GCC modules.
std::optional<ClangdDb> prepareClangdDb(const fs::path& buildDir);

struct ModuleDecl {
    std::string name;       // "ide.json", or "ide.app:part" for partitions
    fs::path path;
    bool interface = true;  // `export module` (vs. an implementation unit `module x;`)
};

// Finds module declarations in the sources named by a compile database.
std::vector<ModuleDecl> scanModules(const fs::path& compileCommandsJson);

// If `line` is an import / module declaration, returns the column where the module name starts.
std::optional<std::size_t> importNameStart(std::string_view line, std::size_t col);

// The module name (possibly with ':partition') under or around `col` on an import line.
std::optional<std::string> importNameAt(std::string_view line, std::size_t col);

}  // namespace ide
