// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.lsp — a Language Server Protocol client (JSON-RPC over stdio), built for clangd.
//
// It is single-threaded and event-loop driven: the app watches `fd()` and calls `pump()` when it is
// readable. Requests take callbacks that run from `pump()`.
module;
#include <unistd.h>
export module daemon.services.lsp;
import std;
import daemon.base.json;
import daemon.base.process;
import daemon.base.util;

namespace fs = std::filesystem;

export namespace ide {

struct LspDiag {
    int line = 0, col = 0, endLine = 0, endCol = 0;  // LSP coordinates (0-based, UTF-16 columns)
    int severity = 1;                                // 1 error, 2 warning, 3 info, 4 hint
    std::string message, source, code;
};

struct LspCompletion {
    std::string label, detail, insertText, filterText, sortText;
    int kind = 0;
};

struct LspLocation {
    std::string path;
    int line = 0, col = 0;  // LSP coordinates
};

// ---- position helpers (LSP columns are UTF-16 code units; our buffer columns are UTF-8 bytes)

int utf16Col(std::string_view line, int byteCol);

int byteColFromUtf16(std::string_view line, int u16);

// ---- URIs

std::string pathToUri(const std::string& path);

std::string uriToPath(std::string_view uri);

class LspClient {
public:
    enum class State { Stopped, Starting, Ready, Failed };

    std::function<void(const std::string& path, std::vector<LspDiag>)> onDiagnostics;
    std::function<void(State)> onState;

    // Callbacks usually capture their owner, which may already be partly destroyed when we are: never fire them from here.
    ~LspClient() {
        onState = nullptr;
        onDiagnostics = nullptr;
        stop();
    }

    // `argv[0]` is the server executable, e.g. {"clangd", "--compile-commands-dir=build"}.
    std::expected<void, std::string> start(std::vector<std::string> argv, const std::string& rootPath);

    void stop();

    State state() const { return state_; }
    bool ready() const { return ready_ && state_ == State::Ready; }
    int fd() const { return proc_ ? proc_->outFd() : -1; }
    bool running() const { return proc_ != nullptr; }

    // Call when `fd()` is readable (or periodically).
    void pump();

    // ---- document sync

    void didOpen(const std::string& path, const std::string& languageId, int version, const std::string& text) {
        auto td = json::object();
        td.set("uri", pathToUri(path)).set("languageId", languageId).set("version", version).set("text", text);
        notify("textDocument/didOpen", json::object().set("textDocument", td));
    }
    void didChange(const std::string& path, int version, const std::string& text);
    void didSave(const std::string& path) {
        notify("textDocument/didSave", json::object().set("textDocument", json::object().set("uri", pathToUri(path))));
    }
    void didClose(const std::string& path) {
        notify("textDocument/didClose", json::object().set("textDocument", json::object().set("uri", pathToUri(path))));
    }
    void didChangeWatchedFile(const std::string& path, int type = 2) {
        auto ev = json::object().set("uri", pathToUri(path)).set("type", type);
        notify("workspace/didChangeWatchedFiles", json::object().set("changes", json::array().push(ev)));
    }

    // ---- requests

    // Tells the server a request is no longer wanted (its late answer is dropped).
    void cancel(int id) {
        if (id < 0 || !pending_.erase(id)) return;
        notify("$/cancelRequest", json::object().set("id", id));
    }

    // Returns the request id (for cancel()).
    int completion(const std::string& path, int line, int col, int triggerKind,
                   std::function<void(std::vector<LspCompletion>)> cb);
    // Also tells whether the list is incomplete: the server cut it off (or ranked it for the prefix typed so far), so a longer
    // prefix needs a new request instead of filtering this list.
    int completion(const std::string& path, int line, int col, int triggerKind,
                   std::function<void(std::vector<LspCompletion>, bool incomplete)> cb);

    void definition(const std::string& path, int line, int col, std::function<void(std::vector<LspLocation>)> cb);

    void hover(const std::string& path, int line, int col, std::function<void(std::string)> cb);

    // clangd extension: the matching header for a source file and vice versa.
    void switchSourceHeader(const std::string& path, std::function<void(std::string)> cb) {
        request("textDocument/switchSourceHeader", json::object().set("uri", pathToUri(path)),
                [cb = std::move(cb)](const json::Value& r, bool err) { cb(!err && r.isString() ? uriToPath(r.str()) : std::string()); });
    }

private:
    static json::Value positionParams(const std::string& path, int line, int col) {
        return json::object()
            .set("textDocument", json::object().set("uri", pathToUri(path)))
            .set("position", json::object().set("line", line).set("character", col));
    }

    static json::Value makeRequest(const std::string& method, json::Value params, int id) {
        auto m = json::object();
        m.set("jsonrpc", "2.0").set("id", id).set("method", method);
        if (!params.isNull()) m.set("params", std::move(params));
        return m;
    }
    static json::Value makeNotification(const std::string& method, json::Value params) {
        auto m = json::object();
        m.set("jsonrpc", "2.0").set("method", method);
        if (!params.isNull()) m.set("params", std::move(params));
        return m;
    }
    static std::string frame(const std::string& body) { return std::format("Content-Length: {}\r\n\r\n{}", body.size(), body); }

    static bool trace() { static const bool on = std::getenv("IDE_LSP_TRACE") != nullptr; return on; }

    void send(const std::string& framed) {
        if (trace()) std::println(std::cerr, ">> {}", framed.substr(framed.find("\r\n\r\n") + 4, 240));
        if (proc_ && !proc_->write(framed)) setState(State::Failed);
    }

    // Messages other than initialize wait until the server answered it.
    void post(const std::string& framed, bool immediate) {
        if (immediate || ready_) send(framed);
        else queued_.push_back(framed);
    }

    void notify(const std::string& method, json::Value params, bool immediate = false) {
        if (!proc_) return;
        post(frame(json::dump(makeNotification(method, std::move(params)))), immediate);
    }

    int request(const std::string& method, json::Value params, std::function<void(const json::Value&, bool)> cb,
                bool immediate = false);

    void parseBuffer();

    void handle(const json::Value& msg);

    void setState(State s) {
        if (state_ == s) return;
        state_ = s;
        if (onState) onState(s);
    }

    std::unique_ptr<Process> proc_;
    std::string root_, buffer_;
    std::vector<std::string> queued_;
    std::unordered_map<int, std::function<void(const json::Value&, bool)>> pending_;
    int nextId_ = 1;
    bool ready_ = false;
    State state_ = State::Stopped;
};

}  // namespace ide
