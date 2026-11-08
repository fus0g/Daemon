// daemon.gdbmi — a client for GDB's machine interface (MI3): record parser and an asynchronous command session.
export module daemon.services.gdb;
import std;
import daemon.base.json;
import daemon.base.process;

export namespace ide::gdb {

// One line of MI output.
struct Record {
    char kind = 0;          // '^' result, '*' exec async, '=' notify async, '+' status, '~' console, '@' target, '&' log
    int token = -1;
    std::string cls;        // done / running / error / stopped / thread-created ...
    json::Value data;       // the results as an object (tuples -> objects, lists -> arrays)
    std::string text;       // stream records: the decoded string
    bool ok() const { return cls != "error"; }
    std::string msg() const { return data["msg"].str(); }
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}
    bool atEnd() const { return i_ >= s_.size(); }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool eat(char c) { if (peek() == c) { ++i_; return true; } return false; }

    std::string cstring();

    std::string ident() {
        const std::size_t st = i_;
        while (i_ < s_.size() && s_[i_] != '=' && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']') ++i_;
        return std::string(s_.substr(st, i_ - st));
    }

    json::Value value();

    json::Value results();

    std::string_view rest() const { return s_.substr(i_); }
    std::string classOf() {
        const std::size_t st = i_;
        while (i_ < s_.size() && s_[i_] != ',') ++i_;
        return std::string(s_.substr(st, i_ - st));
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;
};

}  // namespace detail

std::optional<Record> parseLine(std::string_view line);

// MI string argument: "…" with quotes and backslashes escaped.
std::string quote(std::string_view s);

class Session {
public:
    using Reply = std::function<void(const Record&)>;

    std::expected<void, std::string> start(const std::vector<std::string>& argv, const std::string& cwd);

    int fd() const { return proc_.outFd(); }
    bool started() const { return proc_.started(); }

    // Queues a command (without token); `cb` gets its result record.
    void send(const std::string& cmd, Reply cb = {}) {
        const int tok = next_++;
        if (cb) pending_[tok] = std::move(cb);
        proc_.write(std::to_string(tok) + cmd + "\n");
    }

    // Reads what gdb has printed and dispatches it. Returns false when gdb has closed its output.
    bool pump();

    // Fails every pending command (used when gdb dies).
    void abortAll();

    void closeOutput() { proc_.closeOutput(); }
    void kill() { proc_.terminate(true); }
    std::optional<int> tryWait() { return proc_.tryWait(); }
    int pid() const { return proc_.pid(); }

    std::function<void(const Record&)> onAsync;                  // '*' and '=' records
    std::function<void(char, const std::string&)> onStream;      // '~' console, '&' log, '@' target

private:
    void dispatch(const Record& r);

    Process proc_;
    std::string buf_;
    int next_ = 1;
    std::unordered_map<int, Reply> pending_;
};

}  // namespace ide::gdb
