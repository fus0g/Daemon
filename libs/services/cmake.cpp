// cmake - implementation of module daemon.services.cmake.
module daemon.services.cmake;
import std;
import daemon.base.json;
import daemon.base.process;
import daemon.base.util;
namespace fs = std::filesystem;

namespace ide {

std::vector<std::string> configureArgv(const fs::path& root, const CMakeProfile& p, bool haveNinja) {
    std::vector<std::string> a = {"cmake", "-S", root.string(), "-B", p.dir.string()};
    if (!isConfigured(p) && haveNinja) { a.push_back("-G"); a.push_back("Ninja"); }
    a.push_back("-DCMAKE_BUILD_TYPE=" + p.buildType);
    a.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    return a;
}

std::vector<CMakeTarget> loadTargets(const fs::path& buildDir) {
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

std::string stripAnsi(std::string_view s) {
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

std::optional<Problem> parseProblem(std::string_view line, const fs::path& baseDir, const fs::path& root) {
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

std::optional<std::pair<int, int>> parseProgress(std::string_view line) {
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

std::vector<std::string> splitArgs(std::string_view s) {
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

void CMakeKnowledge::load() {
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

}  // namespace ide

namespace ide::cmake_detail {

const std::unordered_map<std::string, std::vector<std::string>>& keywordTable() {
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

bool takesTargets(const std::string& cmd) {
    static const std::unordered_set<std::string> s = {
        "target_link_libraries", "target_include_directories", "target_compile_definitions", "target_compile_options",
        "target_compile_features", "target_sources", "target_link_options", "target_link_directories", "target_precompile_headers",
        "add_dependencies", "set_target_properties", "get_target_property", "set_property", "get_property", "install", "export",
        "add_custom_command", "if", "elseif", "while", "add_test", "target_link_libraries", "set_tests_properties", "cmake_print_properties"};
    return s.contains(cmd);
}

const std::vector<std::string>& genexNames() {
    static const std::vector<std::string> g = {
        "TARGET_FILE", "TARGET_FILE_DIR", "TARGET_FILE_NAME", "TARGET_LINKER_FILE", "TARGET_PROPERTY", "TARGET_OBJECTS", "TARGET_EXISTS",
        "CONFIG", "BOOL", "IF", "AND", "OR", "NOT", "STREQUAL", "EQUAL", "IN_LIST", "BUILD_INTERFACE", "INSTALL_INTERFACE", "COMPILE_LANGUAGE",
        "CXX_COMPILER_ID", "C_COMPILER_ID", "PLATFORM_ID", "JOIN", "LOWER_CASE", "UPPER_CASE", "COMPILE_FEATURES", "LINK_LANGUAGE",
        "CONFIG:Debug", "CONFIG:Release", "INSTALL_PREFIX", "SHELL_PATH", "REMOVE_DUPLICATES", "FILTER", "GENEX_EVAL"};
    return g;
}

}  // namespace ide::cmake_detail

namespace ide {

CMakeDefs scanCMakeDefs(std::string_view text) {
    // Hand-written line scanner (no std::regex: its templates in a module interface crash clangd 23's module reader).
    CMakeDefs d;
    std::unordered_set<std::string> seen;
    auto isName = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '+' || c == '-'; };
    for (std::size_t i = 0; i < text.size();) {
        auto e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(i, e - i);
        i = e + 1;
        std::size_t k = 0;
        auto skipWs = [&] { while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k; };
        skipWs();
        const std::size_t cs = k;
        while (k < line.size() && (std::isalpha(static_cast<unsigned char>(line[k])) || line[k] == '_')) ++k;
        if (k == cs) continue;
        std::string cmd(line.substr(cs, k - cs));
        skipWs();
        if (k >= line.size() || line[k] != '(') continue;
        ++k;
        skipWs();
        const std::size_t ns = k;
        if (k >= line.size() || !(std::isalpha(static_cast<unsigned char>(line[k])) || line[k] == '_')) continue;
        while (k < line.size() && isName(line[k])) ++k;
        std::string name(line.substr(ns, k - ns));
        std::ranges::transform(cmd, cmd.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!seen.insert(cmd + ":" + name).second) continue;
        if (cmd == "set" || cmd == "option") d.variables.push_back(name);
        else if (cmd == "add_executable" || cmd == "add_library" || cmd == "add_custom_target") d.targets.push_back(name);
        else if (cmd == "function" || cmd == "macro") d.functions.push_back(name);
        else if (cmd == "project") for (const char* suf : {"_SOURCE_DIR", "_BINARY_DIR", "_VERSION"}) d.variables.push_back(name + suf);
    }
    return d;
}

std::vector<CMakeItem> cmakeCompletions(const CMakeKnowledge& kb, std::string_view before, std::string_view full,
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

}  // namespace ide

namespace ide::cmake_detail {

std::vector<std::string> entryArgs(const json::Value& e) {
    std::vector<std::string> a;
    if (auto* arr = e["arguments"].array()) {
        for (const auto& x : *arr) a.push_back(x.str());
    } else {
        a = splitArgs(e["command"].str());
    }
    return a;
}

std::string compilerDefaultStd(const std::string& compiler) {
    static std::unordered_map<std::string, std::string> cache;
    if (auto it = cache.find(compiler); it != cache.end()) return it->second;
    std::string result = "gnu++17";
    ProcessOptions o;
    o.argv = {compiler, "-dM", "-E", "-x", "c++", "/dev/null"};
    auto r = Process::runCapture(o, 10000);
    const auto at = r.output.find("#define __cplusplus ");
    if (at != std::string::npos) {
        const long v = std::atol(r.output.c_str() + at + 20);
        if (v >= 202400) result = "gnu++26";
        else if (v >= 202302) result = "gnu++23";
        else if (v >= 202002) result = "gnu++20";
    }
    return cache[compiler] = result;
}

}  // namespace ide::cmake_detail

namespace ide {

std::optional<ClangdDb> prepareClangdDb(const fs::path& buildDir) {
    using namespace cmake_detail;
    const fs::path src = buildDir / "compile_commands.json";
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return std::nullopt;
    const fs::path outDir = buildDir / ".daemon" / "clangd-v2";
    const fs::path out = outDir / "compile_commands.json";
    auto parsed = readJson(src);
    if (!parsed || !parsed->array()) return ClangdDb{buildDir, false, 0};

    bool gccModules = false;
    for (const auto& e : *parsed->array())
        {
            const auto a = entryArgs(e);
            if (usesGccModules(a) || !hasStdFlag(a) || isModuleInterfaceExt(e["file"].str())) { gccModules = true; break; }
        }
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
        const bool needStd = !hasStdFlag(args);
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
        if (needStd) {
            // Modules need at least C++20 even if the compiler's default is older.
            std::string std_ = compilerDefaultStd(args[0]);
            if (std_ == "gnu++17" || std_ == "gnu++14" || std_ == "gnu++11") std_ = "gnu++20";
            out_.push_back("-std=" + std_);
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

    // Module interface units that are not (yet) in CMake's database, e.g. a new file or a configure that failed
    // before listing it, would leave `import foo;` unresolved. Add them, borrowing the flags of a sibling entry.
    if (!order.empty()) {
        fs::path home;
        if (auto cache = readFile(buildDir / "CMakeCache.txt")) {
            const auto at = cache->find("CMAKE_HOME_DIRECTORY:INTERNAL=");
            if (at != std::string::npos) {
                const auto e = cache->find('\n', at);
                home = cache->substr(at + 30, e == std::string::npos ? std::string::npos : e - at - 30);
            }
        }
        const json::Value* proto = nullptr;
        for (const auto& f : order) {
            const auto name = fs::path(f).filename().string();
            if (name != "std.cc" && name != "std.compat.cc" && !isModuleInterfaceExt(f)) { proto = &entries[f]; break; }
        }
        if (!home.empty() && proto && fs::is_directory(home, ec)) {
            for (fs::recursive_directory_iterator it(home, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().string();
                if (it->is_directory(ec) && (name.starts_with('.') || name.starts_with("cmake-build") || name == "build" || name == "node_modules")) {
                    it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(ec) || !isModuleInterfaceExt(it->path())) continue;
                const std::string file = it->path().string();
                if (entries.contains(file)) continue;
                std::vector<std::string> args;
                for (const auto& a : *(*proto)["arguments"].array()) args.push_back(a.str());
                if (!args.empty()) args.pop_back();  // the prototype's source file
                for (std::size_t i = 0; i + 1 < args.size(); ++i)
                    if (args[i] == "-x") args[i + 1] = "c++-module";
                args.push_back(file);
                json::Value entry = json::object();
                entry.set("directory", (*proto)["directory"].str());
                json::Value arr = json::array();
                for (auto& x : args) arr.push(x);
                entry.set("arguments", std::move(arr));
                entry.set("file", file);
                order.push_back(file);
                entries.emplace(file, std::move(entry));
            }
        }
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

std::vector<ModuleDecl> scanModules(const fs::path& compileCommandsJson) {
    std::vector<ModuleDecl> out;
    auto db = readJson(compileCommandsJson);
    if (!db || !db->array()) return out;
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
        // Only the leading part of a file can hold the module declaration (after the global fragment).
        const std::string_view head(text->data(), std::min<std::size_t>(text->size(), 64 * 1024));
        for (std::size_t i = 0; i < head.size();) {
            auto le = head.find('\n', i);
            if (le == std::string_view::npos) le = head.size();
            std::string_view line = head.substr(i, le - i);
            i = le + 1;
            std::size_t k = 0;
            auto skipWs = [&] { while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) ++k; };
            auto word = [&](std::string_view w) {
                if (line.substr(k, w.size()) == w && k + w.size() < line.size() && (line[k + w.size()] == ' ' || line[k + w.size()] == '\t' || line[k + w.size()] == ';')) { k += w.size(); return true; }
                return false;
            };
            skipWs();
            const bool isExport = word("export");
            if (isExport) skipWs();
            if (!word("module")) continue;
            skipWs();
            const std::size_t ns = k;
            while (k < line.size() && (std::isalnum(static_cast<unsigned char>(line[k])) || line[k] == '_' || line[k] == '.' || line[k] == ':')) ++k;
            if (k == ns) continue;   // "module;" starts the global fragment
            const std::string name(line.substr(ns, k - ns));
            skipWs();
            while (k + 1 < line.size() && line[k] == '[' && line[k + 1] == '[') { auto close = line.find("]]", k); if (close == std::string_view::npos) break; k = close + 2; skipWs(); }
            if (k < line.size() && line[k] == ';') { out.push_back({name, file, isExport}); break; }
        }
    }
    return out;
}

std::optional<std::size_t> importNameStart(std::string_view line, std::size_t col) {
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

std::optional<std::string> importNameAt(std::string_view line, std::size_t col) {
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
