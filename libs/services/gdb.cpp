// gdb - implementation of module daemon.services.gdb.
module daemon.services.gdb;
import std;
import daemon.base.json;
import daemon.base.process;

namespace ide::gdb::detail {

std::string Parser::cstring() {   // positioned at the opening quote
    std::string out;
    if (!eat('"')) return out;
    while (i_ < s_.size() && s_[i_] != '"') {
        char c = s_[i_++];
        if (c != '\\' || i_ >= s_.size()) { out += c; continue; }
        char e = s_[i_++];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'v': out += '\v'; break;
            case 'e': out += '\x1b'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default:
                if (e >= '0' && e <= '7') {   // octal byte (UTF-8 sequences come this way)
                    int v = e - '0';
                    for (int k = 0; k < 2 && i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '7'; ++k) v = v * 8 + (s_[i_++] - '0');
                    out += static_cast<char>(v);
                } else out += e;
        }
    }
    eat('"');
    return out;
}

json::Value Parser::value() {
    if (peek() == '"') return json::Value(cstring());
    if (eat('{')) {
        json::Object o;
        while (!atEnd() && peek() != '}') {
            std::string k = ident();
            eat('=');
            o.emplace_back(std::move(k), value());
            if (!eat(',')) break;
        }
        eat('}');
        return json::Value(std::move(o));
    }
    if (eat('[')) {
        json::Array a;
        while (!atEnd() && peek() != ']') {
            if (peek() == '"' || peek() == '{' || peek() == '[') a.push_back(value());
            else { ident(); eat('='); a.push_back(value()); }   // [name=value, ...] lists: keep the values
            if (!eat(',')) break;
        }
        eat(']');
        return json::Value(std::move(a));
    }
    return json::Value();
}

json::Value Parser::results() {   // ",k=v,k=v..." after the class
    json::Object o;
    while (eat(',')) {
        std::string k = ident();
        eat('=');
        o.emplace_back(std::move(k), value());
    }
    return json::Value(std::move(o));
}

}  // namespace ide::gdb::detail

namespace ide::gdb {

std::optional<Record> parseLine(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.empty() || line.starts_with("(gdb)")) return std::nullopt;
    Record r;
    std::size_t i = 0;
    int tok = 0;
    bool haveTok = false;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) { tok = tok * 10 + (line[i] - '0'); ++i; haveTok = true; }
    if (haveTok) r.token = tok;
    if (i >= line.size()) return std::nullopt;
    r.kind = line[i++];
    detail::Parser p(line.substr(i));
    switch (r.kind) {
        case '~': case '@': case '&': r.text = p.cstring(); return r;
        case '^': case '*': case '=': case '+':
            r.cls = p.classOf();
            r.data = p.results();
            return r;
        default: return std::nullopt;   // stray output of the debuggee / gdb itself
    }
}

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

std::expected<void, std::string> Session::start(const std::vector<std::string>& argv, const std::string& cwd) {
    ProcessOptions o;
    o.argv = argv;
    o.cwd = cwd;
    o.pipeStdin = true;
    o.mergeStderr = false;
    return proc_.start(o);
}

bool Session::pump() {
    std::string chunk;
    const long n = proc_.read(chunk);
    buf_ += chunk;
    for (std::size_t nl; (nl = buf_.find('\n')) != std::string::npos;) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        auto rec = parseLine(line);
        if (!rec) continue;
        dispatch(*rec);
    }
    return n != 0;
}

void Session::abortAll() {
    Record e; e.kind = '^'; e.cls = "error"; e.data = json::object();
    auto d = e.data;
    auto pend = std::move(pending_);
    pending_.clear();
    for (auto& [t, cb] : pend) { Record r = e; r.token = t; if (cb) cb(r); }
}

void Session::dispatch(const Record& r) {
    switch (r.kind) {
        case '^': {
            auto it = pending_.find(r.token);
            if (it == pending_.end()) return;
            auto cb = std::move(it->second);
            pending_.erase(it);
            if (cb) cb(r);
            return;
        }
        case '*': case '=': if (onAsync) onAsync(r); return;
        case '~': case '@': case '&': if (onStream) onStream(r.kind, r.text); return;
        default: return;
    }
}

}  // namespace ide::gdb
