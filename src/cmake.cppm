// ide.cmake — CMake project support in the style of CLion: build profiles, configure / build
// command lines, target discovery through the CMake File API, and compiler diagnostics parsing.
export module ide.cmake;
import std;
import ide.json;
import ide.process;
import ide.util;

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
inline std::vector<std::string> configureArgv(const fs::path& root, const CMakeProfile& p, bool haveNinja) {
    std::vector<std::string> a = {"cmake", "-S", root.string(), "-B", p.dir.string()};
    if (!isConfigured(p) && haveNinja) { a.push_back("-G"); a.push_back("Ninja"); }
    a.push_back("-DCMAKE_BUILD_TYPE=" + p.buildType);
    a.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    return a;
}

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
inline std::vector<CMakeTarget> loadTargets(const fs::path& buildDir) {
    std::vector<CMakeTarget> out;
    const fs::path reply = buildDir / ".cmake/api/v1/reply";
    std::error_code ec;
    fs::path newest;
    for (fs::directory_iterator it(reply, ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().string();
        if (n.starts_with("index-") && n.ends_with(".json") && (newest.empty() || n > newest.filename().string())) newest = it->path();
    }
    if (newest.empty()) return out;
    auto index = readJson(newest);
    if (!index) return out;

    std::string codemodel;
    if (auto* objs = (*index)["objects"].array())
        for (const auto& obj : *objs)
            if (obj["kind"].str() == "codemodel") codemodel = obj["jsonFile"].str();
    if (codemodel.empty()) return out;
    auto model = readJson(reply / codemodel);
    if (!model) return out;

    const auto& configs = (*model)["configurations"];
    if (!configs.array() || configs.array()->empty()) return out;
    const auto* targetList = configs[0]["targets"].array();
    if (!targetList) return out;
    for (const auto& t : *targetList) {
        CMakeTarget target;
        target.name = t["name"].str();
        auto tj = readJson(reply / t["jsonFile"].str());
        if (tj) {
            target.type = (*tj)["type"].str();
            const auto& arts = (*tj)["artifacts"];
            if (arts.array() && !arts.array()->empty()) target.artifact = buildDir / arts[0]["path"].str();
        }
        out.push_back(std::move(target));
    }
    std::ranges::stable_sort(out, [](const CMakeTarget& a, const CMakeTarget& b) {
        return a.executable() > b.executable();
    });
    return out;
}

// Removes ANSI escape sequences from tool output.
inline std::string stripAnsi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

// Parses one line of compiler / CMake output into a located problem, e.g.
//   ../src/main.cpp:12:5: error: expected ';'      CMake Error at CMakeLists.txt:7 (add_executable):
inline std::optional<Problem> parseProblem(std::string_view line, const fs::path& baseDir, const fs::path& root) {
    auto resolve = [&](std::string file) {
        fs::path p(file);
        std::error_code ec;
        if (p.is_relative()) {
            fs::path a = (baseDir / p).lexically_normal();
            fs::path b = (root / p).lexically_normal();
            // Ninja prints paths relative to the build directory; fall back to the project root.
            p = (!fs::exists(a, ec) && fs::exists(b, ec)) ? b : a;
        }
        return p.lexically_normal();
    };

    for (auto [prefix, sev] : {std::pair<std::string_view, Severity>{"CMake Error at ", Severity::Error},
                               std::pair<std::string_view, Severity>{"CMake Warning at ", Severity::Warning}}) {
        if (line.starts_with(prefix)) {
            auto rest = line.substr(prefix.size());
            auto colon = rest.find(':');
            if (colon == std::string_view::npos) return std::nullopt;
            Problem p;
            p.severity = sev;
            fs::path f(rest.substr(0, colon));  // CMake reports paths relative to the source root
            p.file = (f.is_relative() ? root / f : f).lexically_normal();
            int ln = 1;
            std::from_chars(rest.data() + colon + 1, rest.data() + rest.size(), ln);
            p.line = ln;
            p.message = std::string(rest.substr(colon + 1));
            return p;
        }
    }

    // gcc / clang:  file:line[:col]: severity: message
    for (auto [marker, sev] : {std::pair<std::string_view, Severity>{": fatal error: ", Severity::Error},
                               std::pair<std::string_view, Severity>{": error: ", Severity::Error},
                               std::pair<std::string_view, Severity>{": warning: ", Severity::Warning},
                               std::pair<std::string_view, Severity>{": note: ", Severity::Note}}) {
        auto at = line.find(marker);
        if (at == std::string_view::npos) continue;
        std::string_view loc = line.substr(0, at);
        // loc = file:line[:col]; the file name itself may contain colons only on exotic systems.
        auto c2 = loc.rfind(':');
        if (c2 == std::string_view::npos) continue;
        int a = 0, b = 0;
        auto num = [](std::string_view t, int& v) {
            if (t.empty()) return false;
            auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
            return ec == std::errc() && p == t.data() + t.size();
        };
        Problem p;
        p.severity = sev;
        std::string_view file;
        if (num(loc.substr(c2 + 1), b)) {
            auto c1 = loc.substr(0, c2).rfind(':');
            if (c1 != std::string_view::npos && num(loc.substr(c1 + 1, c2 - c1 - 1), a)) {
                p.line = a; p.col = b; file = loc.substr(0, c1);
            } else {
                p.line = b; p.col = 1; file = loc.substr(0, c2);
            }
        } else {
            continue;
        }
        if (file.empty()) continue;
        p.file = resolve(std::string(file));
        p.message = std::string(line.substr(at + marker.size()));
        return p;
    }
    return std::nullopt;
}

// "[12/34] Building ..." -> {12, 34}
inline std::optional<std::pair<int, int>> parseProgress(std::string_view line) {
    if (line.size() < 5 || line[0] != '[') return std::nullopt;
    auto slash = line.find('/');
    auto close = line.find(']');
    if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) return std::nullopt;
    int a = 0, b = 0;
    auto r1 = std::from_chars(line.data() + 1, line.data() + slash, a);
    auto r2 = std::from_chars(line.data() + slash + 1, line.data() + close, b);
    if (r1.ec != std::errc() || r2.ec != std::errc() || b <= 0) return std::nullopt;
    return std::pair{a, b};
}

// Shell-like splitting of a run-configuration argument string (quotes and backslashes).
inline std::vector<std::string> splitArgs(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool inArg = false;
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            inArg = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            inArg = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inArg) { out.push_back(std::move(cur)); cur.clear(); inArg = false; }
        } else {
            cur += c;
            inArg = true;
        }
    }
    if (inArg) out.push_back(std::move(cur));
    return out;
}

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
    void load() {
        if (loaded_) return;
        loaded_ = true;
        auto run = [](const char* flag) {
            std::vector<std::string> out;
            ProcessOptions o;
            o.argv = {"cmake", flag};
            auto r = Process::runCapture(o, 10000);
            std::size_t i = 0;
            while (i < r.output.size()) {
                auto e = r.output.find('\n', i);
                if (e == std::string::npos) e = r.output.size();
                std::string l = r.output.substr(i, e - i);
                while (!l.empty() && std::isspace(static_cast<unsigned char>(l.back()))) l.pop_back();
                if (!l.empty()) out.push_back(std::move(l));
                i = e + 1;
            }
            return out;
        };
        // Templated names such as CMAKE_<LANG>_COMPILER are expanded for the common languages / configs.
        auto expand = [](const std::vector<std::string>& in) {
            std::vector<std::string> out;
            for (const auto& name : in) {
                auto lt = name.find('<');
                if (lt == std::string::npos) { out.push_back(name); continue; }
                const auto gt = name.find('>', lt);
                if (gt == std::string::npos) continue;
                const std::string ph = name.substr(lt, gt - lt + 1);
                std::vector<std::string> subs;
                if (ph == "<LANG>") subs = {"C", "CXX", "CUDA", "OBJC", "OBJCXX", "Fortran", "ASM", "HIP"};
                else if (ph == "<CONFIG>") subs = {"DEBUG", "RELEASE", "RELWITHDEBINFO", "MINSIZEREL"};
                else continue;
                for (const auto& sub : subs) {
                    std::string e = name;
                    e.replace(lt, ph.size(), sub);
                    if (e.find('<') == std::string::npos) out.push_back(std::move(e));
                    else { auto again = e; (void)again; }
                }
            }
            return out;
        };
        commands = run("--help-command-list");
        variables = expand(run("--help-variable-list"));
        properties = expand(run("--help-property-list"));
        modules = run("--help-module-list");
        policies = run("--help-policy-list");
    }
    bool ready() const { return loaded_ && !commands.empty(); }

    std::vector<std::string> commands, variables, properties, modules, policies;

private:
    bool loaded_ = false;
};

namespace cmake_detail {

inline bool wordCh(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

inline const std::unordered_map<std::string, std::vector<std::string>>& keywordTable() {
    static const std::unordered_map<std::string, std::vector<std::string>> t = {
        {"add_executable", {"WIN32", "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL", "IMPORTED", "ALIAS"}},
        {"add_library", {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "IMPORTED", "ALIAS", "EXCLUDE_FROM_ALL"}},
        {"target_link_libraries", {"PUBLIC", "PRIVATE", "INTERFACE", "debug", "optimized", "general"}},
        {"target_include_directories", {"PUBLIC", "PRIVATE", "INTERFACE", "SYSTEM", "BEFORE", "AFTER"}},
        {"target_compile_definitions", {"PUBLIC", "PRIVATE", "INTERFACE"}},
        {"target_compile_options", {"PUBLIC", "PRIVATE", "INTERFACE", "BEFORE"}},
        {"target_compile_features", {"PUBLIC", "PRIVATE", "INTERFACE", "cxx_std_17", "cxx_std_20", "cxx_std_23"}},
        {"target_link_options", {"PUBLIC", "PRIVATE", "INTERFACE", "BEFORE"}},
        {"target_link_directories", {"PUBLIC", "PRIVATE", "INTERFACE", "BEFORE"}},
        {"target_sources", {"PUBLIC", "PRIVATE", "INTERFACE", "FILE_SET", "TYPE", "BASE_DIRS", "FILES", "HEADERS", "CXX_MODULES"}},
        {"set", {"CACHE", "PARENT_SCOPE", "FORCE", "STRING", "BOOL", "PATH", "FILEPATH", "INTERNAL"}},
        {"option", {"ON", "OFF"}},
        {"project", {"VERSION", "DESCRIPTION", "HOMEPAGE_URL", "LANGUAGES", "CXX", "C", "CUDA", "NONE"}},
        {"find_package", {"REQUIRED", "QUIET", "EXACT", "CONFIG", "MODULE", "COMPONENTS", "OPTIONAL_COMPONENTS", "NO_MODULE", "PATHS", "HINTS"}},
        {"find_library", {"NAMES", "PATHS", "HINTS", "REQUIRED", "NO_DEFAULT_PATH", "PATH_SUFFIXES"}},
        {"find_path", {"NAMES", "PATHS", "HINTS", "REQUIRED", "NO_DEFAULT_PATH", "PATH_SUFFIXES"}},
        {"find_program", {"NAMES", "PATHS", "HINTS", "REQUIRED", "NO_DEFAULT_PATH"}},
        {"if", {"NOT", "AND", "OR", "DEFINED", "EXISTS", "TARGET", "COMMAND", "STREQUAL", "MATCHES", "VERSION_LESS", "VERSION_GREATER",
                "VERSION_EQUAL", "IN_LIST", "IS_DIRECTORY", "POLICY", "TEST", "ON", "OFF", "TRUE", "FALSE"}},
        {"elseif", {"NOT", "AND", "OR", "DEFINED", "EXISTS", "TARGET", "COMMAND", "STREQUAL", "MATCHES", "VERSION_LESS", "VERSION_GREATER"}},
        {"while", {"NOT", "AND", "OR", "DEFINED", "EXISTS"}},
        {"foreach", {"IN", "LISTS", "ITEMS", "RANGE", "ZIP_LISTS"}},
        {"list", {"APPEND", "PREPEND", "INSERT", "REMOVE_ITEM", "REMOVE_AT", "REMOVE_DUPLICATES", "LENGTH", "GET", "FIND", "JOIN", "SORT",
                  "REVERSE", "TRANSFORM", "FILTER", "SUBLIST", "POP_BACK", "POP_FRONT", "INCLUDE", "EXCLUDE", "REGEX"}},
        {"string", {"APPEND", "PREPEND", "FIND", "REPLACE", "REGEX", "MATCH", "MATCHALL", "SUBSTRING", "LENGTH", "TOLOWER", "TOUPPER", "STRIP",
                    "CONFIGURE", "JOIN", "TIMESTAMP", "MD5", "SHA256", "RANDOM", "COMPARE", "REVERSE"}},
        {"file", {"READ", "WRITE", "APPEND", "GLOB", "GLOB_RECURSE", "MAKE_DIRECTORY", "COPY", "RENAME", "REMOVE", "REMOVE_RECURSE", "DOWNLOAD",
                  "STRINGS", "SIZE", "TOUCH", "RELATIVE_PATH", "TO_CMAKE_PATH", "CONFIGURE", "GENERATE", "INSTALL", "CONFIGURE_DEPENDS",
                  "FOLLOW_SYMLINKS", "RELATIVE", "LIST_DIRECTORIES"}},
        {"install", {"TARGETS", "FILES", "DIRECTORY", "PROGRAMS", "EXPORT", "DESTINATION", "RUNTIME", "LIBRARY", "ARCHIVE", "PUBLIC_HEADER",
                     "COMPONENT", "PERMISSIONS", "CONFIGURATIONS", "OPTIONAL", "FILE_SET", "INCLUDES", "NAMESPACE"}},
        {"add_custom_command", {"OUTPUT", "COMMAND", "ARGS", "DEPENDS", "MAIN_DEPENDENCY", "WORKING_DIRECTORY", "COMMENT", "VERBATIM", "TARGET",
                                "PRE_BUILD", "PRE_LINK", "POST_BUILD", "BYPRODUCTS", "USES_TERMINAL", "DEPFILE", "COMMAND_EXPAND_LISTS"}},
        {"add_custom_target", {"ALL", "COMMAND", "DEPENDS", "WORKING_DIRECTORY", "COMMENT", "VERBATIM", "BYPRODUCTS", "SOURCES", "USES_TERMINAL"}},
        {"add_subdirectory", {"EXCLUDE_FROM_ALL", "SYSTEM"}},
        {"add_test", {"NAME", "COMMAND", "WORKING_DIRECTORY", "CONFIGURATIONS"}},
        {"set_target_properties", {"PROPERTIES"}},
        {"set_property", {"TARGET", "SOURCE", "DIRECTORY", "GLOBAL", "TEST", "CACHE", "PROPERTY", "APPEND", "APPEND_STRING"}},
        {"get_property", {"TARGET", "SOURCE", "DIRECTORY", "GLOBAL", "TEST", "CACHE", "PROPERTY", "SET", "DEFINED", "BRIEF_DOCS", "FULL_DOCS"}},
        {"include", {"OPTIONAL", "RESULT_VARIABLE", "NO_POLICY_SCOPE"}},
        {"include_directories", {"SYSTEM", "BEFORE", "AFTER"}},
        {"configure_file", {"@ONLY", "COPYONLY", "ESCAPE_QUOTES", "NEWLINE_STYLE"}},
        {"message", {"STATUS", "WARNING", "AUTHOR_WARNING", "SEND_ERROR", "FATAL_ERROR", "DEPRECATION", "NOTICE", "VERBOSE", "DEBUG", "TRACE", "CHECK_START", "CHECK_PASS", "CHECK_FAIL"}},
        {"cmake_minimum_required", {"VERSION", "FATAL_ERROR"}},
        {"cmake_policy", {"SET", "GET", "VERSION", "PUSH", "POP", "NEW", "OLD"}},
        {"function", {}}, {"macro", {}},
        {"execute_process", {"COMMAND", "WORKING_DIRECTORY", "OUTPUT_VARIABLE", "ERROR_VARIABLE", "RESULT_VARIABLE", "OUTPUT_STRIP_TRAILING_WHITESPACE", "TIMEOUT", "COMMAND_ERROR_IS_FATAL"}},
        {"FetchContent_Declare", {"GIT_REPOSITORY", "GIT_TAG", "URL", "URL_HASH", "SOURCE_DIR", "SYSTEM", "FIND_PACKAGE_ARGS"}},
        {"export", {"TARGETS", "EXPORT", "FILE", "NAMESPACE", "PACKAGE"}},
        {"try_compile", {"SOURCES", "LINK_LIBRARIES", "CMAKE_FLAGS", "COMPILE_DEFINITIONS", "OUTPUT_VARIABLE"}},
        {"block", {"SCOPE_FOR", "VARIABLES", "POLICIES", "PROPAGATE"}},
    };
    return t;
}

inline bool takesTargets(const std::string& cmd) {
    static const std::unordered_set<std::string> s = {
        "target_link_libraries", "target_include_directories", "target_compile_definitions", "target_compile_options",
        "target_compile_features", "target_sources", "target_link_options", "target_link_directories", "target_precompile_headers",
        "add_dependencies", "set_target_properties", "get_target_property", "set_property", "get_property", "install", "export",
        "add_custom_command", "if", "elseif", "while", "add_test", "target_link_libraries", "set_tests_properties", "cmake_print_properties"};
    return s.contains(cmd);
}

inline const std::vector<std::string>& genexNames() {
    static const std::vector<std::string> g = {
        "TARGET_FILE", "TARGET_FILE_DIR", "TARGET_FILE_NAME", "TARGET_LINKER_FILE", "TARGET_PROPERTY", "TARGET_OBJECTS", "TARGET_EXISTS",
        "CONFIG", "BOOL", "IF", "AND", "OR", "NOT", "STREQUAL", "EQUAL", "IN_LIST", "BUILD_INTERFACE", "INSTALL_INTERFACE", "COMPILE_LANGUAGE",
        "CXX_COMPILER_ID", "C_COMPILER_ID", "PLATFORM_ID", "JOIN", "LOWER_CASE", "UPPER_CASE", "COMPILE_FEATURES", "LINK_LANGUAGE",
        "CONFIG:Debug", "CONFIG:Release", "INSTALL_PREFIX", "SHELL_PATH", "REMOVE_DUPLICATES", "FILTER", "GENEX_EVAL"};
    return g;
}

}  // namespace cmake_detail

// Everything `text` defines that completion should know about: variables, targets, functions / macros.
struct CMakeDefs {
    std::vector<std::string> variables, targets, functions;
};

inline CMakeDefs scanCMakeDefs(std::string_view text) {
    CMakeDefs d;
    std::unordered_set<std::string> seen;
    static const std::regex re(R"((?:^|\n)[ \t]*([A-Za-z_]+)[ \t]*\([ \t]*([A-Za-z_][A-Za-z0-9_.+\-]*))", std::regex::icase);
    const std::string t(text);
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re), end = std::sregex_iterator(); it != end; ++it) {
        std::string cmd = (*it)[1], name = (*it)[2];
        std::ranges::transform(cmd, cmd.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!seen.insert(cmd + ":" + name).second) continue;
        if (cmd == "set" || cmd == "option" || cmd == "list" || cmd == "string" || cmd == "file" || cmd == "find_package" ||
            cmd == "find_library" || cmd == "find_path" || cmd == "find_program" || cmd == "foreach") {
            if (cmd == "set" || cmd == "option") d.variables.push_back(name);
        } else if (cmd == "add_executable" || cmd == "add_library" || cmd == "add_custom_target") {
            d.targets.push_back(name);
        } else if (cmd == "function" || cmd == "macro") {
            d.functions.push_back(name);
        } else if (cmd == "project") {
            for (const char* suf : {"_SOURCE_DIR", "_BINARY_DIR", "_VERSION"}) d.variables.push_back(name + suf);
        }
    }
    return d;
}

// Completion items for the cursor position at the end of `before` (all text up to the cursor).
// `full` is the whole document (to find user-defined names); `knownTargets` come from the CMake File API.
inline std::vector<CMakeItem> cmakeCompletions(const CMakeKnowledge& kb, std::string_view before, std::string_view full,
                                               const std::vector<std::string>& knownTargets) {
    using namespace cmake_detail;
    std::vector<CMakeItem> out;
    auto add = [&](std::string label, std::string detail, int kind, std::string sort, std::string insert = {}) {
        CMakeItem it;
        it.label = std::move(label);
        it.detail = std::move(detail);
        it.kind = kind;
        it.sort = std::move(sort);
        it.insert = std::move(insert);
        out.push_back(std::move(it));
    };

    // The current line, to rule out comments.
    const auto nl = before.rfind('\n');
    const std::string_view line = nl == std::string_view::npos ? before : before.substr(nl + 1);
    bool inQuote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\') { ++i; continue; }
        if (line[i] == '"') inQuote = !inQuote;
        else if (line[i] == '#' && !inQuote) return out;  // comment
    }

    std::size_t ws = before.size();
    while (ws > 0 && wordCh(before[ws - 1])) --ws;
    const std::string_view head = before.substr(0, ws);
    const CMakeDefs defs = scanCMakeDefs(full);

    auto addVariables = [&] {
        for (const auto& v : kb.variables) add(v, "variable", 6, "1" + v);
        for (const auto& v : defs.variables) add(v, "variable (this project)", 6, "0" + v);
    };

    if (head.ends_with("$ENV{")) return out;
    if (head.ends_with("${")) { addVariables(); return out; }
    if (head.ends_with("$<")) {
        for (const auto& g : genexNames()) add(g, "generator expression", 3, "0" + g);
        return out;
    }
    if (inQuote) return out;  // plain text inside a string

    // Forward scan: which command's argument list are we in?
    int depth = 0;
    std::string cmd, lastUpper;
    bool str = false, comment = false;
    for (std::size_t i = 0; i < head.size(); ++i) {
        const char c = head[i];
        if (comment) { if (c == '\n') comment = false; continue; }
        if (str) { if (c == '\\') ++i; else if (c == '"') str = false; continue; }
        if (c == '"') { str = true; continue; }
        if (c == '#') { comment = true; continue; }
        if (c == '(') {
            if (depth == 0) {
                std::size_t e = i;
                while (e > 0 && std::isspace(static_cast<unsigned char>(head[e - 1]))) --e;
                std::size_t b = e;
                while (b > 0 && wordCh(head[b - 1])) --b;
                cmd = std::string(head.substr(b, e - b));
                std::ranges::transform(cmd, cmd.begin(), [](unsigned char ch) { return std::tolower(ch); });
                lastUpper.clear();
            }
            ++depth;
        } else if (c == ')') {
            if (depth > 0 && --depth == 0) { cmd.clear(); lastUpper.clear(); }
        } else if (depth > 0 && std::isupper(static_cast<unsigned char>(c)) && (i == 0 || !wordCh(head[i - 1]))) {
            std::size_t e = i;
            while (e < head.size() && (std::isupper(static_cast<unsigned char>(head[e])) || head[e] == '_')) ++e;
            if (e - i >= 2 && (e >= head.size() || !std::islower(static_cast<unsigned char>(head[e])))) lastUpper = std::string(head.substr(i, e - i));
            i = e - 1;
        }
    }
    if (str) return out;

    if (depth == 0) {  // start of a statement: commands
        for (const auto& c : kb.commands) add(c, "command", 3, "1" + c, c + "()");
        for (const auto& f : defs.functions) add(f, "function (this project)", 3, "0" + f, f + "()");
        return out;
    }

    // Inside a command's arguments.
    if (cmd == "include") for (const auto& m : kb.modules) if (!m.starts_with("Find")) add(m, "module", 9, "1" + m);
    if (cmd == "find_package") for (const auto& m : kb.modules) if (m.starts_with("Find")) add(m.substr(4), "find module", 9, "1" + m);
    if (cmd == "cmake_policy" || (cmd == "if" && lastUpper == "POLICY")) for (const auto& p : kb.policies) add(p, "policy", 21, "1" + p);
    if (lastUpper == "PROPERTIES" || lastUpper == "PROPERTY") for (const auto& p : kb.properties) add(p, "property", 10, "1" + p);
    if (auto it = keywordTable().find(cmd); it != keywordTable().end())
        for (const auto& k : it->second) add(k, "keyword", 14, "0" + k);
    if (takesTargets(cmd)) {
        std::unordered_set<std::string> seen;
        for (const auto& t : defs.targets) if (seen.insert(t).second) add(t, "target", 7, "0" + t);
        for (const auto& t : knownTargets) if (seen.insert(t).second) add(t, "target", 7, "0" + t);
    }
    if (cmd == "set" || cmd == "if" || cmd == "elseif" || cmd == "while" || cmd == "option" || cmd == "unset" || cmd == "list" || cmd == "string" || cmd == "message") {
        // Bare variable names (no ${}) in commands that take them.
        for (const auto& v : defs.variables) add(v, "variable (this project)", 6, "0" + v);
        for (const auto& v : kb.variables) if (v.starts_with("CMAKE_") || v.starts_with("PROJECT_")) add(v, "variable", 6, "1" + v);
    }
    // Deduplicate by label (first wins).
    std::unordered_set<std::string> seen;
    std::erase_if(out, [&](const CMakeItem& i) { return !seen.insert(i.label).second; });
    return out;
}

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

inline std::vector<std::string> entryArgs(const json::Value& e) {
    std::vector<std::string> a;
    if (auto* arr = e["arguments"].array()) {
        for (const auto& x : *arr) a.push_back(x.str());
    } else {
        a = splitArgs(e["command"].str());
    }
    return a;
}

inline bool usesGccModules(const std::vector<std::string>& args) {
    return std::ranges::any_of(args, [](const std::string& a) {
        return a == "-fmodules-ts" || a.starts_with("-fmodule-mapper=") || a.starts_with("-fdeps-format=");
    });
}

}  // namespace cmake_detail

struct ClangdDb {
    fs::path dir;          // value for --compile-commands-dir
    bool derived = false;  // true when a Clang-flavoured copy was generated
    int sources = 0;
};

// Returns the compile-database directory clangd should use for `buildDir`, generating the derived
// module-aware database under <buildDir>/.daemon/clangd when the CMake one targets GCC modules.
inline std::optional<ClangdDb> prepareClangdDb(const fs::path& buildDir) {
    using namespace cmake_detail;
    const fs::path src = buildDir / "compile_commands.json";
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return std::nullopt;
    const fs::path outDir = buildDir / ".daemon" / "clangd";
    const fs::path out = outDir / "compile_commands.json";
    auto parsed = readJson(src);
    if (!parsed || !parsed->array()) return ClangdDb{buildDir, false, 0};

    bool gccModules = false;
    for (const auto& e : *parsed->array())
        if (usesGccModules(entryArgs(e))) { gccModules = true; break; }
    if (!gccModules) return ClangdDb{buildDir, false, static_cast<int>(parsed->array()->size())};

    // Up to date?
    if (fs::is_regular_file(out, ec) && fs::last_write_time(out, ec) >= fs::last_write_time(src, ec))
        return ClangdDb{outDir, true, 0};

    auto clang = findExecutable("clang++");
    if (!clang) return ClangdDb{buildDir, false, 0};

    std::vector<std::string> order;
    std::unordered_map<std::string, json::Value> entries;
    std::unordered_map<std::string, bool> viaBmiOnly;
    for (const auto& e : *parsed->array()) {
        const std::string file = e["file"].str();
        if (file.empty()) continue;
        const auto args = entryArgs(e);
        if (args.empty()) continue;
        bool bmiOnly = false;
        std::vector<std::string> out_{*clang};
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "-o" || a == "-MT" || a == "-MF" || a == "-MQ") { ++i; continue; }
            if (a == "-c" || a == "-MD" || a == "-MMD" || a == "-fmodules-ts") continue;
            if (a == "-fmodule-only") { bmiOnly = true; continue; }
            if (a.starts_with("-fmodule-mapper=") || a.starts_with("-fdeps-") || a.starts_with("-fdiagnostics-color") ||
                a.starts_with("-fcolor-diagnostics") || a == "-fconcepts-diagnostics-depth")
                continue;
            if (a == file) continue;
            out_.push_back(a);
        }
        const std::string name = fs::path(file).filename().string();
        const bool moduleSource = isModuleInterfaceExt(file) || name == "std.cc" || name == "std.compat.cc";
        if (moduleSource) {
            bool replaced = false;
            for (std::size_t i = 0; i + 1 < out_.size(); ++i)
                if (out_[i] == "-x" && out_[i + 1] == "c++") { out_[i + 1] = "c++-module"; replaced = true; }
            if (!replaced) { out_.push_back("-x"); out_.push_back("c++-module"); }
        }
        out_.push_back("-stdlib=libstdc++");
        out_.push_back(file);

        json::Value entry = json::object();
        entry.set("directory", e["directory"].str());
        json::Value arr = json::array();
        for (auto& x : out_) arr.push(x);
        entry.set("arguments", std::move(arr));
        entry.set("file", file);

        auto it = entries.find(file);
        if (it == entries.end()) { order.push_back(file); entries.emplace(file, std::move(entry)); viaBmiOnly[file] = bmiOnly; }
        else if (viaBmiOnly[file] && !bmiOnly) { it->second = std::move(entry); viaBmiOnly[file] = false; }
    }

    json::Value db = json::array();
    for (const auto& f : order) db.push(entries[f]);
    fs::create_directories(outDir, ec);
    {
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (!o) return ClangdDb{buildDir, false, 0};
        o << json::dump(db);
    }
    return ClangdDb{outDir, true, static_cast<int>(order.size())};
}

struct ModuleDecl {
    std::string name;       // "ide.json", or "ide.app:part" for partitions
    fs::path path;
    bool interface = true;  // `export module` (vs. an implementation unit `module x;`)
};

// Finds module declarations in the sources named by a compile database.
inline std::vector<ModuleDecl> scanModules(const fs::path& compileCommandsJson) {
    std::vector<ModuleDecl> out;
    auto db = readJson(compileCommandsJson);
    if (!db || !db->array()) return out;
    static const std::regex re(R"(^[ \t]*(export[ \t]+)?module[ \t]+([A-Za-z_][A-Za-z0-9_.]*(?::[A-Za-z_][A-Za-z0-9_.]*)?)[ \t]*(?:\[\[[^\n]*\]\])?[ \t]*;)",
                               std::regex::multiline);
    std::unordered_set<std::string> seen;
    for (const auto& e : *db->array()) {
        const fs::path file = e["file"].str();
        if (file.empty() || !seen.insert(file.string()).second) continue;
        const std::string n = file.filename().string();
        if (n == "std.cc" || n == "std.compat.cc") {
            out.push_back({n == "std.cc" ? "std" : "std.compat", file, true});
            continue;
        }
        auto text = readFile(file);
        if (!text) continue;
        std::smatch m;
        // Only the leading part of a file can hold the module declaration (after the global fragment).
        const std::string head = text->substr(0, std::min<std::size_t>(text->size(), 64 * 1024));
        if (std::regex_search(head, m, re)) out.push_back({m[2].str(), file, m[1].matched});
    }
    return out;
}

// If `line` is an import / module declaration, returns the column where the module name starts.
inline std::optional<std::size_t> importNameStart(std::string_view line, std::size_t col) {
    std::size_t i = 0;
    auto skipWs = [&] { while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i; };
    auto word = [&](std::string_view w) {
        if (line.substr(i, w.size()) == w && (i + w.size() >= line.size() || !(std::isalnum(static_cast<unsigned char>(line[i + w.size()])) || line[i + w.size()] == '_'))) {
            i += w.size();
            return true;
        }
        return false;
    };
    skipWs();
    if (word("export")) skipWs();
    if (!word("import")) return std::nullopt;
    if (i >= line.size() || (line[i] != ' ' && line[i] != '\t')) return std::nullopt;
    skipWs();
    if (i > col) return std::nullopt;
    for (std::size_t k = i; k < col && k < line.size(); ++k) {
        const char c = line[k];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':')) return std::nullopt;
    }
    return i;
}

// The module name (possibly with ':partition') under or around `col` on an import line.
inline std::optional<std::string> importNameAt(std::string_view line, std::size_t col) {
    auto start = importNameStart(line, std::min(col, line.size()));
    if (!start) {
        // cursor might be past the name: retry at the end of the name
        std::size_t i = line.find("import");
        if (i == std::string_view::npos) return std::nullopt;
        start = importNameStart(line, line.size());
        if (!start) return std::nullopt;
    }
    std::size_t e = *start;
    while (e < line.size() && (std::isalnum(static_cast<unsigned char>(line[e])) || line[e] == '_' || line[e] == '.' || line[e] == ':')) ++e;
    if (e == *start) return std::nullopt;
    return std::string(line.substr(*start, e - *start));
}

}  // namespace ide
