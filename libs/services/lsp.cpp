// SPDX-License-Identifier: GPL-3.0-or-later
// lsp - implementation of module daemon.services.lsp.
module;
#include <unistd.h>
module daemon.services.lsp;
import std;
import daemon.base.json;
import daemon.base.process;
import daemon.base.util;
namespace fs = std::filesystem;

namespace ide {

int utf16Col(std::string_view line, int byteCol) {
    int units = 0;
    std::size_t i = 0;
    const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(std::max(0, byteCol)), line.size());
    while (i < end) {
        char32_t c = decodeUtf8(line, i);
        units += c >= 0x10000 ? 2 : 1;
    }
    return units;
}

int byteColFromUtf16(std::string_view line, int u16) {
    int units = 0;
    std::size_t i = 0;
    while (i < line.size() && units < u16) {
        char32_t c = decodeUtf8(line, i);
        units += c >= 0x10000 ? 2 : 1;
    }
    return static_cast<int>(i);
}

std::string pathToUri(const std::string& path) {
    std::string out = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}

std::string uriToPath(std::string_view uri) {
    if (uri.starts_with("file://")) uri.remove_prefix(7);
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size() + 0 && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
            unsigned v = 0;
            std::from_chars(uri.data() + i + 1, uri.data() + i + 3, v, 16);
            out += static_cast<char>(v);
            i += 2;
        } else {
            out += uri[i];
        }
    }
    return out;
}

std::expected<void, std::string> LspClient::start(std::vector<std::string> argv, const std::string& rootPath) {
    stop();
    ProcessOptions o;
    o.argv = std::move(argv);
    o.cwd = rootPath;
    o.pipeStdin = true;
    o.mergeStderr = false;
    proc_ = std::make_unique<Process>();
    if (auto r = proc_->start(o); !r) { proc_.reset(); setState(State::Failed); return std::unexpected(r.error()); }
    root_ = rootPath;
    setState(State::Starting);

    auto params = json::object();
    params.set("processId", static_cast<long>(::getpid()));
    params.set("clientInfo", json::object().set("name", "ide"));
    params.set("rootUri", pathToUri(rootPath));
    params.set("rootPath", rootPath);
    auto completion = json::object();
    completion.set("contextSupport", true);
    completion.set("completionItem", json::object()
                                         .set("snippetSupport", false)
                                         .set("documentationFormat", json::array().push("plaintext")));
    auto textDoc = json::object();
    textDoc.set("synchronization", json::object().set("didSave", true).set("dynamicRegistration", false));
    textDoc.set("completion", completion);
    textDoc.set("hover", json::object().set("contentFormat", json::array().push("plaintext")));
    textDoc.set("publishDiagnostics", json::object());
    textDoc.set("definition", json::object().set("linkSupport", false));
    params.set("capabilities", json::object().set("textDocument", textDoc).set("workspace", json::object()));
    request("initialize", std::move(params), [this](const json::Value&, bool error) {
        if (error) { setState(State::Failed); return; }
        notify("initialized", json::object(), true);
        ready_ = true;
        setState(State::Ready);
        for (auto& m : queued_) send(m);
        queued_.clear();
    }, true);
    return {};
}

void LspClient::stop() {
    if (proc_) {
        if (ready_) {
            // Polite shutdown; the process is killed regardless when the object goes away.
            send(frame(json::dump(makeRequest("shutdown", json::Value(), nextId_++))));
            send(frame(json::dump(makeNotification("exit", json::Value()))));
        }
        proc_->closeStdin();
        proc_->terminate();
        proc_.reset();
    }
    ready_ = false;
    buffer_.clear();
    queued_.clear();
    pending_.clear();
    if (state_ != State::Stopped && state_ != State::Failed) setState(State::Stopped);
}

void LspClient::pump() {
    if (!proc_) return;
    long n = proc_->read(buffer_);
    parseBuffer();
    if (n == 0) {  // server closed its output
        proc_->tryWait();
        if (state_ != State::Stopped) setState(State::Failed);
        proc_->closeOutput();
    }
}

void LspClient::didChange(const std::string& path, int version, const std::string& text) {
    auto td = json::object();
    td.set("uri", pathToUri(path)).set("version", version);
    auto change = json::object();
    change.set("text", text);  // full-document sync
    notify("textDocument/didChange", json::object().set("textDocument", td).set("contentChanges", json::array().push(change)));
}

int LspClient::completion(const std::string& path, int line, int col, int triggerKind,
               std::function<void(std::vector<LspCompletion>)> cb) {
    return completion(path, line, col, triggerKind, [cb = std::move(cb)](std::vector<LspCompletion> items, bool) { cb(std::move(items)); });
}

int LspClient::completion(const std::string& path, int line, int col, int triggerKind,
               std::function<void(std::vector<LspCompletion>, bool)> cb) {
    auto params = positionParams(path, line, col);
    params.set("context", json::object().set("triggerKind", triggerKind));
    return request("textDocument/completion", std::move(params), [cb = std::move(cb)](const json::Value& r, bool err) {
        std::vector<LspCompletion> out;
        bool incomplete = false;
        if (!err) {
            const json::Value* items = &r;
            if (r.isObject()) { items = &r["items"]; incomplete = r["isIncomplete"].boolean(); }  // CompletionList
            if (auto* a = items->array()) {
                for (const auto& it : *a) {
                    LspCompletion c;
                    c.label = it["label"].str();
                    c.kind = it["kind"].integer();
                    c.detail = it["detail"].str();
                    c.sortText = it["sortText"].isString() ? it["sortText"].str() : c.label;
                    c.filterText = it["filterText"].isString() ? it["filterText"].str() : c.label;
                    c.insertText = it["insertText"].str();
                    if (c.insertText.empty()) c.insertText = it["textEdit"]["newText"].str();
                    if (c.insertText.empty()) c.insertText = c.label;
                    out.push_back(std::move(c));
                }
            }
        }
        cb(std::move(out), incomplete);
    });
}

void LspClient::definition(const std::string& path, int line, int col, std::function<void(std::vector<LspLocation>)> cb) {
    request("textDocument/definition", positionParams(path, line, col), [cb = std::move(cb)](const json::Value& r, bool err) {
        std::vector<LspLocation> out;
        auto add = [&](const json::Value& loc) {
            std::string uri = loc["uri"].isString() ? loc["uri"].str() : loc["targetUri"].str();
            const auto& range = loc["range"].isObject() ? loc["range"] : loc["targetSelectionRange"];
            if (uri.empty()) return;
            out.push_back({uriToPath(uri), range["start"]["line"].integer(), range["start"]["character"].integer()});
        };
        if (!err) {
            if (r.isArray()) { for (const auto& l : *r.array()) add(l); }
            else if (r.isObject()) add(r);
        }
        cb(std::move(out));
    });
}

void LspClient::hover(const std::string& path, int line, int col, std::function<void(std::string)> cb) {
    request("textDocument/hover", positionParams(path, line, col), [cb = std::move(cb)](const json::Value& r, bool err) {
        std::string text;
        if (!err && r.isObject()) {
            const auto& c = r["contents"];
            if (c.isString()) text = c.str();
            else if (c.isObject()) text = c["value"].str();
            else if (auto* a = c.array()) {
                for (const auto& part : *a) {
                    if (!text.empty()) text += "\n";
                    text += part.isString() ? part.str() : part["value"].str();
                }
            }
        }
        cb(std::move(text));
    });
}

int LspClient::signatureHelp(const std::string& path, int line, int col, std::function<void(LspSignatureHelp)> cb) {
    auto params = positionParams(path, line, col);
    params.set("context", json::object().set("triggerKind", 1).set("isRetrigger", false));
    return request("textDocument/signatureHelp", std::move(params), [cb = std::move(cb)](const json::Value& r, bool err) {
        LspSignatureHelp out;
        if (!err && r.isObject()) {
            auto docText = [](const json::Value& d) { return d.isString() ? d.str() : d["value"].str(); };
            out.activeSig = r["activeSignature"].integer();
            const int activeParam = r["activeParameter"].integer(-1);
            if (auto* sigs = r["signatures"].array()) {
                for (const auto& s : *sigs) {
                    LspSignature sg;
                    sg.label = s["label"].str();
                    sg.doc = docText(s["documentation"]);
                    sg.active = s["activeParameter"].isNumber() ? s["activeParameter"].integer() : activeParam;
                    if (auto* ps = s["parameters"].array()) {
                        for (const auto& p : *ps) {
                            const auto& l = p["label"];
                            std::pair<int, int> range{-1, -1};
                            if (auto* a = l.array(); a && a->size() == 2) range = {(*a)[0].integer(), (*a)[1].integer()};
                            else if (l.isString()) { const auto at = sg.label.find(l.str()); if (at != std::string::npos) range = {static_cast<int>(at), static_cast<int>(at + l.str().size())}; }
                            sg.params.push_back(range);
                            sg.paramDocs.push_back(docText(p["documentation"]));
                        }
                    }
                    out.sigs.push_back(std::move(sg));
                }
            }
            if (out.activeSig < 0 || out.activeSig >= static_cast<int>(out.sigs.size())) out.activeSig = 0;
        }
        cb(std::move(out));
    });
}

int LspClient::request(const std::string& method, json::Value params, std::function<void(const json::Value&, bool)> cb,
            bool immediate) {
    if (!proc_) { cb(json::Value(), true); return -1; }
    int id = nextId_++;
    pending_[id] = std::move(cb);
    post(frame(json::dump(makeRequest(method, std::move(params), id))), immediate);
    return id;
}

void LspClient::parseBuffer() {
    while (true) {
        auto hdrEnd = buffer_.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return;
        std::size_t len = 0;
        bool haveLen = false;
        std::string_view headers(buffer_.data(), hdrEnd);
        while (!headers.empty()) {
            auto nl = headers.find("\r\n");
            std::string_view line = headers.substr(0, nl);
            if (line.size() > 15 && std::ranges::equal(line.substr(0, 15), std::string_view("Content-Length:"), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                })) {
                auto v = line.substr(15);
                while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
                std::from_chars(v.data(), v.data() + v.size(), len);
                haveLen = true;
            }
            if (nl == std::string_view::npos) break;
            headers.remove_prefix(nl + 2);
        }
        if (!haveLen) { buffer_.erase(0, hdrEnd + 4); continue; }
        if (buffer_.size() < hdrEnd + 4 + len) return;
        std::string body = buffer_.substr(hdrEnd + 4, len);
        buffer_.erase(0, hdrEnd + 4 + len);
        if (auto msg = json::parse(body)) handle(*msg);
    }
}

void LspClient::handle(const json::Value& msg) {
    if (trace()) std::println(std::cerr, "<< {}", json::dump(msg).substr(0, 240));
    const bool hasMethod = msg["method"].isString();
    const bool hasId = !msg["id"].isNull();
    if (hasMethod && hasId) {  // server -> client request: acknowledge so the server never stalls
        auto reply = json::object();
        reply.set("jsonrpc", "2.0").set("id", msg["id"]).set("result", json::Value());
        send(frame(json::dump(reply)));
        return;
    }
    if (hasMethod) {
        const std::string method = msg["method"].str();
        if (method == "textDocument/publishDiagnostics" && onDiagnostics) {
            std::vector<LspDiag> diags;
            if (auto* a = msg["params"]["diagnostics"].array()) {
                for (const auto& d : *a) {
                    LspDiag x;
                    x.line = d["range"]["start"]["line"].integer();
                    x.col = d["range"]["start"]["character"].integer();
                    x.endLine = d["range"]["end"]["line"].integer();
                    x.endCol = d["range"]["end"]["character"].integer();
                    x.severity = d["severity"].isNumber() ? d["severity"].integer() : 1;
                    x.message = d["message"].str();
                    x.source = d["source"].str();
                    x.code = d["code"].isString() ? d["code"].str() : (d["code"].isNumber() ? std::to_string(d["code"].integer()) : "");
                    diags.push_back(std::move(x));
                }
            }
            onDiagnostics(uriToPath(msg["params"]["uri"].str()), std::move(diags));
        }
        return;
    }
    if (hasId) {  // response
        int id = msg["id"].integer(-1);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        auto cb = std::move(it->second);
        pending_.erase(it);
        const bool error = !msg["error"].isNull();
        cb(msg["result"], error);
    }
}

}  // namespace ide
