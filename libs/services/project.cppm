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

const std::vector<ProjectTemplate>& projectTemplates();

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
std::string validateProjectName(std::string_view name);

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

bool write(const fs::path& p, const std::string& text, std::string& err);

}  // namespace project_detail

// Creates the project directory with its files (and optionally a git repository).
std::expected<CreatedProject, std::string> createProject(const NewProjectSpec& spec);

// ---- persistent state: recent projects and the last "new project" location

struct DaemonState {
    std::vector<std::string> recent;
    std::string lastLocation;
};

fs::path stateFile();

DaemonState loadState();

void saveState(const DaemonState& st);

// Moves `path` to the front of the recent list (newest first, de-duplicated, at most 12 entries).
inline void addRecent(DaemonState& st, const std::string& path) {
    std::erase(st.recent, path);
    st.recent.insert(st.recent.begin(), path);
    if (st.recent.size() > 12) st.recent.resize(12);
}

}  // namespace ide
