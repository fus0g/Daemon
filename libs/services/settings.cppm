// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.services.settings - the user settings: a table of everything that can be configured (key, section, label, type,
// default, limits), the values, and the file they live in ($XDG_CONFIG_HOME/daemon/settings.json, flat "section.name" keys).
// Only values that differ from the default are written; keys the table does not know are kept untouched.
export module daemon.services.settings;
import std;
import daemon.base.json;

export namespace ide::settings {

using Value = std::variant<bool, int, std::string>;
enum class Type { Bool, Int, Choice, Text };

struct Def {
    std::string key, section, label, help;
    Type type = Type::Bool;
    Value def;
    int min = 0, max = 0, step = 1;          // Int
    std::vector<std::string> choices;        // Choice: the allowed texts
    std::string unit;                        // shown after an Int ("px")
    bool needsRestart = false;               // applied the next time Daemon starts
};

const std::vector<Def>& defs();
const Def* find(std::string_view key);
std::vector<std::string> sections();         // in display order

class Settings {
public:
    bool b(std::string_view key) const;
    int i(std::string_view key) const;
    std::string s(std::string_view key) const;

    // Validates (clamps numbers, rejects unknown choices); returns false when the key is unknown or the value unusable.
    bool set(std::string_view key, const Value& v);
    void reset(std::string_view key);
    bool isDefault(std::string_view key) const;
    Value get(std::string_view key) const;

    bool operator==(const Settings& o) const { return v_ == o.v_; }   // the values; unknown keys do not count

    // The file text: only non-default values (plus unknown keys), one per line.
    std::string toText() const;
    // Parses a settings file; bad values fall back to the default and are listed in `problems`.
    static Settings fromText(std::string_view text, std::vector<std::string>* problems = nullptr);

private:
    std::map<std::string, Value, std::less<>> v_;
    std::vector<std::pair<std::string, json::Value>> extra_;
};

std::filesystem::path settingsFile();
Settings load(std::vector<std::string>* problems = nullptr);
bool save(const Settings& s);

// The settings of this process: the App owns the changes, library code (notebook kernel, ...) reads them.
Settings& current();

}  // namespace ide::settings
