// SPDX-License-Identifier: GPL-3.0-or-later
// daemon.app / debugger - part of the App implementation (see app.cppm for the class).
module daemon.app;
import std;
import daemon.base.util;
import daemon.ui.input;
import daemon.text.buffer;
import daemon.text.syntax;
import daemon.ui.font;
import daemon.ui.draw;
import daemon.ui.geometry;
import daemon.app.options;
import daemon.app.theme;
import daemon.ui.vulkan;
import daemon.ui.wayland;
import daemon.text.document;
import daemon.services.explorer;
import daemon.ui.icons;
import daemon.base.json;
import daemon.base.process;
import daemon.services.cmake;
import daemon.services.project;
import daemon.services.git;
import daemon.services.search;
import daemon.services.gdb;
import daemon.services.terminal;
import daemon.services.lsp;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ide {

void App::dbgConsole(std::string text, int kind) {
    dbgCon_.push_back({std::move(text), kind});
    if (dbgCon_.size() > 5000) dbgCon_.erase(dbgCon_.begin(), dbgCon_.begin() + 1000);
    panelFollow_[4] = true;
    dirty_ = true;
}

void App::dbgConsoleStream(const std::string& t) {
    dbgConPartial_ += t;
    for (std::size_t nl; (nl = dbgConPartial_.find('\n')) != std::string::npos;) {
        std::string line = dbgConPartial_.substr(0, nl);
        dbgConPartial_.erase(0, nl + 1);
        dbgConsole(std::move(line), 0);
    }
}

void App::dbgSave() {
    const fs::path f = dbgFile();
    if (f.empty()) return;
    auto j = json::object();
    auto arr = json::array();
    for (const auto& b : bps_) {
        auto o = json::object();
        o.set("file", b.file.empty() ? std::string() : fs::path(b.file).lexically_relative(explorer_.rootPath()).string());
        o.set("line", b.line); o.set("func", b.func); o.set("watch", b.watch); o.set("exceptions", b.exceptions);
        o.set("cond", b.cond); o.set("log", b.log); o.set("ignore", b.ignore); o.set("enabled", b.enabled);
        arr.push(std::move(o));
    }
    j.set("breakpoints", std::move(arr));
    auto w = json::array();
    for (const auto& e : watches_) w.push(e);
    j.set("watches", std::move(w));
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    std::ofstream(f, std::ios::binary | std::ios::trunc) << json::dump(j);
}

void App::dbgLoad() {
    bps_.clear(); watches_.clear();
    const fs::path f = dbgFile();
    if (f.empty()) return;
    auto text = readFile(f);
    if (!text) return;
    auto j = json::parse(*text);
    if (!j) return;
    if (auto* a = (*j)["breakpoints"].array())
        for (const auto& v : *a) {
            Breakpoint b;
            b.uid = dbgUid_++;
            const std::string rel = v["file"].str();
            b.file = rel.empty() ? std::string() : (explorer_.rootPath() / rel).lexically_normal().string();
            b.line = v["line"].integer(); b.func = v["func"].str(); b.watch = v["watch"].str();
            b.exceptions = v["exceptions"].boolean(); b.cond = v["cond"].str(); b.log = v["log"].str();
            b.ignore = v["ignore"].integer(); b.enabled = v["enabled"].boolean(true);
            bps_.push_back(std::move(b));
        }
    if (auto* a = (*j)["watches"].array()) for (const auto& v : *a) if (v.isString()) watches_.push_back(v.str());
}

std::string App::dbgBpTitle(const Breakpoint& b) const {
    if (b.exceptions) return "C++ exceptions (throw)";
    if (!b.watch.empty()) return "watch " + b.watch;
    if (!b.func.empty()) return b.func + "()";
    return fs::path(b.file).filename().string() + ":" + std::to_string(b.line);
}

void App::dbgWhenPaused(std::function<void()> fn) {
    if (!dbg_) return;
    if (dbgState_ != DbgState::Running) { fn(); return; }
    dbgOps_.push_back(std::move(fn));
    if (!dbgInterrupting_) { dbgInterrupting_ = true; dbg_->send("-exec-interrupt --all"); }
}

std::string App::dbgBpCommand(const Breakpoint& b) {
    if (b.exceptions) return "-catch-throw";
    if (!b.watch.empty()) return "-break-watch " + gdb::quote(b.watch);
    std::string c = "-break-insert -f";
    if (!b.cond.empty()) c += " -c " + gdb::quote(b.cond);
    if (b.ignore > 0) c += " -i " + std::to_string(b.ignore);
    if (!b.func.empty()) return c + " " + gdb::quote(b.func);
    return c + " --source " + gdb::quote(b.file) + " --line " + std::to_string(b.line);
}

void App::dbgInsert(int uid) {
    Breakpoint* b = dbgBpByUid(uid);
    if (!dbg_ || !b) return;
    const bool enabled = b->enabled, isLine = b->func.empty() && b->watch.empty() && !b->exceptions;
    dbg_->send(dbgBpCommand(*b), [this, uid, enabled, isLine](const gdb::Record& r) {
        Breakpoint* p = dbgBpByUid(uid);
        if (!r.ok()) { if (p) p->verified = false; dbgConsole("Breakpoint: " + r.msg(), 1); return; }
        const json::Value& bk = r.data["bkpt"].isNull() ? r.data["wpt"] : r.data["bkpt"];
        const int id = std::atoi(bk["number"].str().c_str());
        if (!p) { if (dbg_) dbg_->send("-break-delete " + std::to_string(id)); return; }   // removed while it was being inserted
        p->id = id;
        p->verified = bk["addr"].str() != "<PENDING>";
        if (isLine) {
            const int l = std::atoi(bk["line"].str().c_str());
            if (l > 0 && l != p->line && p->verified) { p->line = l; dbgSave(); }   // gdb moved it to the next line with code
        }
        if (!enabled && dbg_) dbg_->send("-break-disable " + std::to_string(id));
        dirty_ = true;
    });
}

void App::dbgAddBp(Breakpoint b) {
    b.uid = dbgUid_++;
    bps_.push_back(std::move(b));
    const int uid = bps_.back().uid;
    dbgSave();
    dbgWhenPaused([this, uid] { dbgInsert(uid); });
    dirty_ = true;
}

void App::dbgRemoveBp(int uid) {
    auto it = std::ranges::find_if(bps_, [&](const Breakpoint& b) { return b.uid == uid; });
    if (it == bps_.end()) return;
    const int id = it->id;
    bps_.erase(it);
    if (id >= 0) dbgWhenPaused([this, id] { if (dbg_) dbg_->send("-break-delete " + std::to_string(id)); });
    dbgSave();
    dirty_ = true;
}

void App::dbgToggleBpAt(const std::string& file, int line) {
    if (file.empty()) { setMessage("Save the file first to set breakpoints", true, 2.5f); return; }
    if (Breakpoint* b = dbgBpAt(file, line)) { dbgRemoveBp(b->uid); return; }
    Breakpoint nb;
    nb.file = file; nb.line = line;
    dbgAddBp(std::move(nb));
}

void App::dbgSetEnabled(int uid, bool on) {
    Breakpoint* b = dbgBpByUid(uid);
    if (!b) return;
    b->enabled = on;
    const int id = b->id;
    if (id >= 0) dbgWhenPaused([this, id, on] { if (dbg_) dbg_->send((on ? "-break-enable " : "-break-disable ") + std::to_string(id)); });
    dbgSave();
    dirty_ = true;
}

std::string App::dbgBpEditText(const Breakpoint& b) {
    if (!b.log.empty()) return "log: " + b.log;
    if (b.ignore > 0 && b.cond.empty()) return "#" + std::to_string(b.ignore);
    return b.cond;
}

void App::dbgApplyBpEdit(int uid, std::string t) {
    Breakpoint* b = dbgBpByUid(uid);
    if (!b) return;
    while (!t.empty() && t.front() == ' ') t.erase(0, 1);
    b->cond.clear(); b->log.clear(); b->ignore = 0;
    if (t.starts_with("log:")) { b->log = t.substr(4); while (!b->log.empty() && b->log.front() == ' ') b->log.erase(0, 1); }
    else if (t.starts_with("#")) b->ignore = std::max(0, std::atoi(t.c_str() + 1));
    else b->cond = t;
    const int id = b->id, ign = b->ignore;
    const std::string cond = b->cond;
    if (id >= 0)
        dbgWhenPaused([this, id, cond, ign] {
            if (!dbg_) return;
            dbg_->send("-break-condition " + std::to_string(id) + (cond.empty() ? "" : " " + cond));
            dbg_->send("-break-after " + std::to_string(id) + " " + std::to_string(ign));
        });
    dbgSave();
    dirty_ = true;
}

void App::debugLaunch(const DbgLaunch& L) {
    if (!findExecutable("gdb")) { setMessage("gdb was not found - install it to debug", true, 4); return; }
    if (dbg_) { dbgEnd("Restarting", 0, true); }
    dbgLast_ = L;
    if (runProc_) { win_.unwatch(runProc_->outFd()); runProc_->terminate(); runProc_.reset(); }
    runOut_.clear(); runPartial_.clear();
    runProc_ = std::make_unique<Process>();
    auto tty = runProc_->openPtyOnly();
    if (!tty) { setMessage("Cannot open a terminal for the program: " + tty.error(), true, 4); runProc_.reset(); return; }
    dbg_ = std::make_unique<gdb::Session>();
    if (auto st = dbg_->start({"gdb", "--interpreter=mi3", "-q", "-nx"}, L.cwd); !st) {
        setMessage("Cannot start gdb: " + st.error(), true, 4);
        dbg_.reset(); runProc_.reset();
        return;
    }
    runOut_.push_back({"$ " + L.exe + (L.args.empty() ? "" : " " + L.args), 4, -1});
    runEof_ = false;
    runFocus_ = true;
    win_.watch(runProc_->outFd(), [this] { pumpRun(); });
    win_.watch(dbg_->fd(), [this] { pumpDebug(); });
    dbg_->onAsync = [this](const gdb::Record& r) { dbgAsync(r); };
    dbg_->onStream = [this](char k, const std::string& t) { if (k == '~') dbgConsoleStream(t); };
    dbgState_ = DbgState::Starting;
    ++dbgGen_;
    dbgFrames_.clear(); dbgThreads_.clear(); dbgLocals_.clear(); dbgWatchVars_.clear(); dbgLine_ = {}; dbgSel_ = 0;
    dbgOps_.clear(); dbgInterrupting_ = false; dbgEndPending_ = false;
    for (auto& b : bps_) { b.id = -1; b.verified = false; b.hits = 0; }
    dbgConsole("--- Debugging " + L.name + " ---", 4);
    panelOpen_ = true; panelTab_ = 1; panelFollow_[1] = true;
    sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; sideFocus_ = false;
    buildStatus_ = "Debugging " + L.name;

    auto& g = *dbg_;
    g.send("-gdb-set confirm off");
    g.send("-gdb-set pagination off");
    g.send("-gdb-set breakpoint pending on");
    g.send("-gdb-set debuginfod enabled off");
    g.send("-gdb-set print elements 200");
    g.send("-gdb-set inferior-tty " + *tty);
    g.send("-gdb-set cwd " + L.cwd);
    if (!L.args.empty()) g.send("-gdb-set args " + L.args);
    g.send("-enable-pretty-printing");
    g.send("-file-exec-and-symbols " + gdb::quote(L.exe), [this](const gdb::Record& r) { if (!r.ok()) dbgConsole(r.msg(), 1); });
    bool anyStop = false;
    for (const auto& b : bps_) if (b.enabled && b.watch.empty()) anyStop = true;
    for (const auto& b : bps_) dbgInsert(b.uid);
    const bool stopAtMain = dbgForceMain_ || !anyStop;
    dbgForceMain_ = false;
    g.send(stopAtMain ? "-exec-run --start" : "-exec-run", [this](const gdb::Record& r) {
        if (!r.ok()) { dbgConsole("Cannot run: " + r.msg(), 1); dbgScheduleEnd("Failed to start the program", -1); }
    });
    dirty_ = true;
}

void App::pumpDebug() {
    if (!dbg_) return;
    const bool alive = dbg_->pump();
    dirty_ = true;
    if (!alive && dbg_ && !dbgEndPending_) dbgScheduleEnd("gdb exited", -1);
    if (dbgEndPending_) dbgEnd(dbgEndWhy_, dbgEndCode_, false);
}

void App::dbgEnd(const std::string& why, int code, bool quiet) {
    dbgEndPending_ = false;
    if (!dbg_) return;
    win_.unwatch(dbg_->fd());
    dbg_->kill();
    dbg_.reset();   // (the destructor reaps gdb; the program dies with it)
    dbgState_ = DbgState::Off;
    ++dbgGen_;
    dbgFrames_.clear(); dbgThreads_.clear(); dbgLocals_.clear(); dbgWatchVars_.clear(); dbgLine_ = {}; dbgOps_.clear(); dbgInterrupting_ = false;
    for (auto& b : bps_) { b.id = -1; b.verified = false; }
    if (!dbgConPartial_.empty()) { dbgConsole(dbgConPartial_, 0); dbgConPartial_.clear(); }
    dbgConsole("--- " + why + " ---", 4);
    if (runProc_) {
        win_.unwatch(runProc_->outFd());
        std::string rest;
        runProc_->read(rest);
        if (!rest.empty()) for (auto& l : takeLines(runPartial_, rest)) runOut_.push_back({cleanTerm(l), 0, -1});
        if (!runPartial_.empty()) { runOut_.push_back({cleanTerm(runPartial_), 0, -1}); runPartial_.clear(); }
        runOut_.push_back({"", 0, -1});
        runOut_.push_back({why, code == 0 ? 5 : 2, -1});
        runProc_.reset();
        runEof_ = false;
    }
    runFocus_ = false;
    if (!quiet) { buildStatus_ = why; setMessage(why, code != 0, 3.5f); }
    dirty_ = true;
}

void App::dbgAsync(const gdb::Record& r) {
    if (r.kind == '*' && r.cls == "running") {
        dbgState_ = DbgState::Running;
        ++dbgGen_; ++dbgVarGen_;
        dbgLine_ = {};
        dbgFrames_.clear();
        dbgThreads_.clear();
        dirty_ = true;
    } else if (r.kind == '*' && r.cls == "stopped") {
        dbgOnStopped(r.data);
    } else if (r.kind == '=' && (r.cls == "breakpoint-modified" || r.cls == "breakpoint-created")) {
        const auto& bk = r.data["bkpt"];
        if (Breakpoint* b = dbgBpById(std::atoi(bk["number"].str().c_str()))) {
            b->hits = std::atoi(bk["times"].str().c_str());
            if (r.cls == "breakpoint-modified") b->verified = bk["addr"].str() != "<PENDING>";
            dirty_ = true;
        }
    }
}

void App::dbgOnStopped(const json::Value& d) {
    const std::string reason = d["reason"].str();
    if (reason.starts_with("exited")) {
        if (reason == "exited-signalled") dbgScheduleEnd("Program terminated by signal " + d["signal-name"].str() + " (" + d["signal-meaning"].str() + ")", 128);
        else {
            const int code = std::strtol(d["exit-code"].str("0").c_str(), nullptr, 8);   // gdb prints the code in octal
            dbgScheduleEnd(reason == "exited-normally" ? "Program exited normally" : std::format("Program exited with code {}", code), code);
        }
        return;
    }
    if (dbgInterrupting_ && reason == "signal-received" && d["signal-name"].str() == "SIGINT") {   // we paused it to edit breakpoints
        dbgInterrupting_ = false;
        dbgState_ = DbgState::Stopped;
        auto ops = std::move(dbgOps_);
        dbgOps_.clear();
        for (auto& op : ops) op();
        if (dbg_) dbg_->send("-exec-continue");
        return;
    }
    dbgState_ = DbgState::Stopped;
    ++dbgGen_;
    if (reason == "breakpoint-hit") {
        if (Breakpoint* b = dbgBpById(std::atoi(d["bkptno"].str().c_str()))) {
            b->hits = std::max(b->hits, 0) + 0;
            if (!b->log.empty()) { dbgRunLogpoint(b->log); return; }
        }
    }
    dbgThread_ = std::atoi(d["thread-id"].str().c_str());
    dbgSel_ = 0;
    std::string text;
    if (reason == "breakpoint-hit") text = "Paused on breakpoint";
    else if (reason == "end-stepping-range" || reason == "function-finished" || reason == "location-reached") text = "Paused";
    else if (reason == "signal-received") {
        text = "Received signal " + d["signal-name"].str() + " (" + d["signal-meaning"].str() + ")";
        setMessage(text, true, 5);
        dbgConsole(text, 1);
    } else if (reason.find("watchpoint") != std::string::npos) {
        const auto& w = d["wpt"].isNull() ? d["hw-wpt"] : d["wpt"];
        text = "Watchpoint " + w["exp"].str() + ": " + d["value"]["old"].str() + " -> " + d["value"]["new"].str();
        if (d["value"]["old"].isNull() && !d["value"]["value"].isNull()) text = "Watchpoint " + w["exp"].str() + " = " + d["value"]["value"].str();
        dbgConsole(text, 4);
    } else text = "Paused (" + reason + ")";
    dbgStopText_ = text;
    const auto& fr = d["frame"];
    DbgFrame f;
    f.func = fr["func"].str(); f.file = fr["file"].str(); f.full = fr["fullname"].str(); f.line = std::atoi(fr["line"].str().c_str()); f.addr = fr["addr"].str();
    dbgShowFrame(f);
    dbgRefreshStack();
    dirty_ = true;
}

void App::dbgRunLogpoint(const std::string& msg) {
    std::vector<std::string> parts, exprs;
    std::string cur;
    for (std::size_t i = 0; i < msg.size(); ++i) {
        if (msg[i] == '{') {
            const auto e = msg.find('}', i);
            if (e == std::string::npos) { cur += msg.substr(i); break; }
            parts.push_back(cur); cur.clear();
            exprs.push_back(msg.substr(i + 1, e - i - 1));
            i = e;
        } else cur += msg[i];
    }
    parts.push_back(cur);
    auto vals = std::make_shared<std::vector<std::string>>(exprs.size());
    auto left = std::make_shared<int>(static_cast<int>(exprs.size()));
    auto finish = [this, parts, vals] {
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i) { out += parts[i]; if (i < vals->size()) out += (*vals)[i]; }
        dbgConsole(out, 3);
        if (dbg_) dbg_->send("-exec-continue");
    };
    if (exprs.empty()) { finish(); return; }
    for (std::size_t i = 0; i < exprs.size(); ++i)
        dbg_->send("-data-evaluate-expression " + gdb::quote(exprs[i]), [vals, left, i, finish](const gdb::Record& r) {
            (*vals)[i] = r.ok() ? r.data["value"].str() : "<" + r.msg() + ">";
            if (--*left == 0) finish();
        });
}

void App::dbgShowFrame(const DbgFrame& f) {
    dbgLine_ = {};
    std::error_code ec;
    std::string path = !f.full.empty() ? f.full : f.file;
    if (path.empty() || f.line <= 0 || !fs::is_regular_file(path, ec)) return;
    path = fs::path(path).lexically_normal().string();
    dbgLine_ = {path, f.line};
    openFile(path);
    if (!docs_.empty() && doc().path == path) {
        doc().setCursor({f.line - 1, 0});
        scrollToCursor_ = true;
    }
    dirty_ = true;
}

void App::dbgRefreshStack() {
    if (!dbg_) return;
    const int gen = dbgGen_;
    dbg_->send("-stack-list-frames 0 255", [this, gen](const gdb::Record& r) {
        if (gen != dbgGen_ || !r.ok()) return;
        dbgFrames_.clear();
        if (const auto* a = r.data["stack"].array())
            for (const auto& v : *a) {
                DbgFrame f;
                f.level = std::atoi(v["level"].str().c_str()); f.func = v["func"].str(); f.file = v["file"].str();
                f.full = v["fullname"].str(); f.line = std::atoi(v["line"].str().c_str()); f.addr = v["addr"].str();
                dbgFrames_.push_back(std::move(f));
            }
        dirty_ = true;
    });
    dbg_->send("-thread-info", [this, gen](const gdb::Record& r) {
        if (gen != dbgGen_ || !r.ok()) return;
        dbgThreads_.clear();
        if (const auto* a = r.data["threads"].array())
            for (const auto& v : *a) {
                DbgThread t;
                t.id = std::atoi(v["id"].str().c_str()); t.name = v["name"].str(); t.state = v["state"].str(); t.func = v["frame"]["func"].str();
                dbgThreads_.push_back(std::move(t));
            }
        dirty_ = true;
    });
    dbgRefreshVars();
}

void App::dbgSelectFrame(int level) {
    if (!dbg_ || dbgState_ != DbgState::Stopped) return;
    dbg_->send("-stack-select-frame " + std::to_string(level), [this, level](const gdb::Record& r) {
        if (!r.ok()) return;
        dbgSel_ = level;
        for (const auto& f : dbgFrames_) if (f.level == level) dbgShowFrame(f);
        dbgRefreshVars();
    });
}

void App::dbgSelectThread(int id) {
    if (!dbg_ || dbgState_ != DbgState::Stopped) return;
    dbg_->send("-thread-select " + std::to_string(id), [this, id](const gdb::Record& r) {
        if (!r.ok()) return;
        dbgThread_ = id;
        dbgSel_ = 0;
        dbgRefreshStack();
        dbg_->send("-stack-info-frame", [this](const gdb::Record& fr) {
            const auto& f = fr.data["frame"];
            DbgFrame d;
            d.func = f["func"].str(); d.file = f["file"].str(); d.full = f["fullname"].str(); d.line = std::atoi(f["line"].str().c_str());
            dbgShowFrame(d);
        });
    });
}

void App::dbgRestart() {
    if (dbgLast_.exe.empty()) { cmakeBuild(true, true); return; }
    const DbgLaunch l = dbgLast_;
    debugLaunch(l);
}

void App::dbgRunToCursor() {
    if (!dbg_ || dbgState_ != DbgState::Stopped || docs_.empty() || doc().path.empty()) return;
    dbg_->send("-break-insert -t -f --source " + gdb::quote(doc().path) + " --line " + std::to_string(doc().cursor.line + 1), [this](const gdb::Record& r) {
        if (!r.ok()) { setMessage(r.msg(), true); return; }
        dbg_->send("-exec-continue");
    });
}

void App::dbgSetNextStatement() {
    if (!dbg_ || dbgState_ != DbgState::Stopped || docs_.empty() || doc().path.empty()) return;
    dbg_->send("-exec-jump " + gdb::quote(doc().path + ":" + std::to_string(doc().cursor.line + 1)), [this](const gdb::Record& r) {
        if (!r.ok()) setMessage(r.msg(), true);
    });
}

void App::dbgRefreshVars() {
    if (!dbg_ || dbgState_ != DbgState::Stopped) return;
    const int gen = ++dbgVarGen_;
    dbgPrev_.clear();
    dbgCollect(dbgLocals_, dbgPrev_);
    dbgCollect(dbgWatchVars_, dbgPrev_);
    for (const auto& v : dbgLocals_) if (!v->vo.empty()) dbg_->send("-var-delete " + v->vo);
    for (const auto& v : dbgWatchVars_) if (!v->vo.empty()) dbg_->send("-var-delete " + v->vo);
    dbgLocals_.clear();
    dbgWatchVars_.clear();
    for (std::size_t i = 0; i < watches_.size(); ++i) {
        auto v = std::make_shared<DbgVar>();
        v->exp = watches_[i];
        v->path = "watch/" + std::to_string(i) + ":" + v->exp;
        v->value = "...";
        dbgWatchVars_.push_back(v);
        dbgCreateVar(v, gen);
    }
    dbg_->send("-stack-list-variables --no-values", [this, gen](const gdb::Record& r) {
        if (gen != dbgVarGen_ || !r.ok()) return;
        if (const auto* a = r.data["variables"].array())
            for (const auto& e : *a) {
                auto v = std::make_shared<DbgVar>();
                v->exp = e["name"].str();
                if (v->exp.starts_with("__")) continue;   // compiler-generated (range-for temporaries ...)
                v->path = "var/" + v->exp;
                v->value = "...";
                dbgLocals_.push_back(v);
                dbgCreateVar(v, gen);
            }
        dirty_ = true;
    });
}

void App::dbgCreateVar(const VarP& v, int gen) {
    if (!dbg_) return;
    dbg_->send("-var-create - * " + gdb::quote(v->exp), [this, v, gen](const gdb::Record& r) {
        if (gen != dbgVarGen_) { if (r.ok() && dbg_) dbg_->send("-var-delete " + r.data["name"].str()); return; }
        if (!r.ok()) { v->value = r.msg().find("unable to create") != std::string::npos ? "<not available in this scope>" : "<" + r.msg() + ">"; v->numchild = 0; dirty_ = true; return; }
        v->vo = r.data["name"].str();
        v->type = r.data["type"].str();
        v->value = r.data["value"].str();
        v->numchild = std::atoi(r.data["numchild"].str().c_str());
        if (r.data["dynamic"].str() == "1") v->numchild = std::max(v->numchild, 1);   // pretty-printed containers list their elements lazily
        if (auto it = dbgPrev_.find(v->path); it != dbgPrev_.end()) v->changed = it->second != v->value;
        if (v->numchild > 0 && dbgExpanded_.contains(v->path)) dbgExpand(v, gen);
        dirty_ = true;
    });
}

void App::dbgFetchKids(const std::string& vo, const std::string& parentPath, int gen, std::function<void(std::vector<VarP>, bool)> done) {
    if (!dbg_) return;
    dbg_->send("-var-list-children --simple-values " + vo + " 0 300", [this, vo, parentPath, gen, done](const gdb::Record& r) {
        if (gen != dbgVarGen_) return;
        if (!r.ok()) { dbgConsole("Cannot list members: " + r.msg(), 1); done({}, false); return; }
        auto slots = std::make_shared<std::vector<std::vector<VarP>>>();
        auto left = std::make_shared<int>(0);
        auto fin = std::make_shared<std::function<void()>>();
        const bool more = r.data["has_more"].str() == "1";
        std::vector<std::pair<VarP, bool>> order;   // (child, pseudo)
        if (const auto* a = r.data["children"].array())
            for (const auto& c : *a) {
                auto k = std::make_shared<DbgVar>();
                k->exp = c["exp"].str(); k->vo = c["name"].str(); k->type = c["type"].str(); k->value = c["value"].str();
                k->numchild = std::atoi(c["numchild"].str().c_str());
                if (c["dynamic"].str() == "1") k->numchild = std::max(k->numchild, 1);
                k->path = parentPath + "/" + k->exp;
                if (auto it = dbgPrev_.find(k->path); it != dbgPrev_.end()) k->changed = it->second != k->value;
                const bool pseudo = (k->exp == "public" || k->exp == "private" || k->exp == "protected") && k->type.empty();
                order.push_back({k, pseudo});
            }
        slots->resize(order.size());
        *fin = [this, order, slots, done, more] {
            std::vector<VarP> out;
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i].second) out.insert(out.end(), (*slots)[i].begin(), (*slots)[i].end());
                else out.push_back(order[i].first);
            }
            done(std::move(out), more);
        };
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i].second) {
                ++*left;
                dbgFetchKids(order[i].first->vo, parentPath, gen, [slots, left, fin, i](std::vector<VarP> ks, bool) {
                    (*slots)[i] = std::move(ks);
                    if (--*left == 0) (*fin)();
                });
            }
        if (*left == 0) (*fin)();
    });
}

void App::dbgExpand(const VarP& v, int gen) {
    if (!dbg_ || v->vo.empty()) return;
    dbgFetchKids(v->vo, v->path, gen, [this, v, gen](std::vector<VarP> kids, bool more) {
        if (gen != dbgVarGen_) return;
        v->kids = std::move(kids);
        if (more) {
            auto m = std::make_shared<DbgVar>();
            m->exp = "..."; m->path = v->path + "/..."; m->value = "more items not shown";
            v->kids.push_back(m);
        }
        v->loaded = true;
        v->expanded = true;
        for (const auto& k : v->kids) if (k->numchild > 0 && dbgExpanded_.contains(k->path)) dbgExpand(k, gen);
        dirty_ = true;
    });
}

void App::dbgToggleVar(const VarP& v) {
    if (v->numchild <= 0) return;
    if (v->expanded) { v->expanded = false; dbgExpanded_.erase(v->path); return; }
    dbgExpanded_.insert(v->path);
    if (v->loaded) v->expanded = true; else dbgExpand(v, dbgVarGen_);
}

void App::dbgAddWatch(const std::string& e) {
    if (e.empty() || std::ranges::find(watches_, e) != watches_.end()) return;
    watches_.push_back(e);
    dbgSave();
    dbgSec_[1] = true;
    if (dbgState_ == DbgState::Stopped) dbgRefreshVars();
    dirty_ = true;
}

void App::dbgSetValue(const VarP& v, const std::string& value) {
    if (!dbg_ || dbgState_ != DbgState::Stopped || v->vo.empty()) return;
    dbg_->send("-var-assign " + v->vo + " " + gdb::quote(value), [this](const gdb::Record& r) {
        if (!r.ok()) { setMessage(r.msg(), true, 4); return; }
        dbgRefreshVars();
    });
}

void App::dbgEval(const std::string& text) {
    if (text.empty()) return;
    dbgEvalQ_.push_back(text);
    if (dbgHist_.empty() || dbgHist_.back() != text) dbgHist_.push_back(text);
    dbgHistAt_ = -1;
    if (!dbgEvalBusy_) dbgEvalNext();
}

void App::dbgEvalNext() {
    if (dbgEvalQ_.empty()) { dbgEvalBusy_ = false; return; }
    dbgEvalBusy_ = true;
    const std::string text = dbgEvalQ_.front();
    dbgEvalQ_.pop_front();
    dbgConsole(text, 2);
    if (!dbg_) { dbgConsole("Not debugging - press F5 to start", 1); dbgEvalNext(); return; }
    if (dbgState_ != DbgState::Stopped) { dbgConsole("The program is running; pause it first (F6)", 1); dbgEvalNext(); return; }
    dbg_->send("-data-evaluate-expression " + gdb::quote(text), [this, text](const gdb::Record& r) {
        if (r.ok()) { dbgConsole(r.data["value"].str(), 5); dbgEvalNext(); return; }
        if (!dbg_) { dbgEvalBusy_ = false; return; }
        const std::string evalError = r.msg();
        dbg_->send("-interpreter-exec console " + gdb::quote(text), [this, evalError](const gdb::Record& r2) {   // gdb commands: bt, info locals, p x, ...
            if (!r2.ok()) dbgConsole(r2.msg().starts_with("Undefined command") ? evalError : r2.msg(), 1);   // not a command: report the expression's error
            dirty_ = true;
            dbgEvalNext();
        });
    });
}

bool App::dbgConKey(const KeyEvent& k) {
    if (k.key == Key::Escape) { dbgConFocus_ = false; return true; }
    if (k.key == Key::Enter) { const std::string t = dbgIn_.text; dbgIn_ = {}; dbgEval(t); return true; }
    if (k.key == Key::Up) {
        if (dbgHist_.empty()) return true;
        dbgHistAt_ = dbgHistAt_ < 0 ? static_cast<int>(dbgHist_.size()) - 1 : std::max(0, dbgHistAt_ - 1);
        dbgIn_.text = dbgHist_[static_cast<std::size_t>(dbgHistAt_)]; dbgIn_.cur = dbgIn_.text.size();
        return true;
    }
    if (k.key == Key::Down) {
        if (dbgHistAt_ < 0) return true;
        ++dbgHistAt_;
        if (dbgHistAt_ >= static_cast<int>(dbgHist_.size())) { dbgHistAt_ = -1; dbgIn_ = {}; }
        else { dbgIn_.text = dbgHist_[static_cast<std::size_t>(dbgHistAt_)]; dbgIn_.cur = dbgIn_.text.size(); }
        return true;
    }
    if (k.ctrl && k.key == Key::Char && k.ch == 'l') { dbgCon_.clear(); return true; }
    if (k.ctrl && k.key != Key::Char) return false;
    if (k.key >= Key::F1) return false;
    if (k.ctrl && k.key == Key::Char && k.ch != 'v' && k.ch != 'a') return false;
    return editText(dbgIn_, k);
}

void App::drawDebugConsole(const Rect& body) {
    const float s = S(), lineH = std::round(lh() + 3 * s), inH = std::round(lineH + 14 * s), pad = 12 * s, headH = std::round(uiH() + 14 * s);
    // status strip
    {
        const Color dot = !dbg_ ? theme::lineNum : dbgState_ == DbgState::Stopped ? Color::rgb(0xe5c07b) : Color::rgb(0x73c990);
        const float d = std::round(8 * s);
        fill(body.x + pad, body.y + (headH - d) / 2, d, d, dot, d / 2);
        std::string t = !dbg_ ? "Not debugging" : dbgState_ == DbgState::Stopped ? dbgStopText_ : dbgState_ == DbgState::Running ? "Running" : "Starting";
        if (dbg_ && dbgState_ == DbgState::Stopped && dbgLine_.line > 0) t += "  -  " + fs::path(dbgLine_.file).filename().string() + ":" + std::to_string(dbgLine_.line);
        uiLabel(body.x + pad + d + 10 * s, body.y, headH, t, dbg_ ? theme::text : theme::dim, body.right() - pad);
        fill(body.x, body.y + headH - 1, body.w, std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 14));
    }
    const Rect list{body.x, body.y + headH, body.w, std::max(0.0f, body.h - headH - inH)};
    const float total = static_cast<float>(dbgCon_.size()) * lineH + 8 * s;
    const float maxS = std::max(0.0f, total - list.h);
    auto& sc = panelScroll_[4];
    sc = std::clamp(panelFollow_[4] ? maxS : sc, 0.0f, maxS);
    const float sy = std::round(sc);
    const int first = std::max(0, static_cast<int>(std::floor((sy - 4 * s) / lineH)));
    const int last = std::min(static_cast<int>(dbgCon_.size()) - 1, static_cast<int>(std::floor((sy + list.h) / lineH)));
    const int ip = std::max(10, static_cast<int>(std::round(lineH * 0.6f)));
    for (int i = first; i <= last; ++i) {
        const auto& l = dbgCon_[static_cast<std::size_t>(i)];
        const float y = list.y + 4 * s + static_cast<float>(i) * lineH - sy;
        if (y < list.y || y + lineH > list.bottom() + 1) continue;
        Color c = Color::rgb(0xc9ccd3);
        char32_t g = 0;
        switch (l.kind) {
            case 1: c = Color::rgb(0xf2777a); g = 0xF05D6; break;                       // error
            case 2: c = Color::rgb(0xffffff); g = 0xF0142; break;                       // what you typed
            case 3: c = Color::rgb(0xe5c07b); g = 0xF0B8A; break;                       // logpoint
            case 5: c = Color::rgb(0x9fe0b4); g = 0xF0141; break;                       // evaluation result
            default: break;
        }
        if (l.kind == 4) {   // session / info markers: a centred rule
            const float tw = uiW(l.text);
            const float cx = list.x + list.w / 2, ly = y + lineH / 2;
            fill(list.x + pad, ly, std::max(0.0f, cx - tw / 2 - 14 * s - list.x - pad), std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 22));
            fill(cx + tw / 2 + 14 * s, ly, std::max(0.0f, list.right() - pad - (cx + tw / 2 + 14 * s)), std::max(1.0f, std::round(s * 0.7f)), Color::rgb(0xffffff, 22));
            uiLabelC(cx, y, lineH, l.text, theme::dim);
            continue;
        }
        if (l.kind == 2) fill(list.x + 6 * s, y, list.w - 12 * s, lineH, Color::rgb(0xffffff, 10), 5 * s);
        if (g) Icons::glyph(dl_, font_, list.x + pad, y + (lineH - static_cast<float>(ip)) / 2, ip, g, c.alpha(200));
        label(list.x + pad + static_cast<float>(ip) + 8 * s, y + (lineH - lh()) / 2, cleanTerm(l.text), c, list.right() - pad);
    }
    if (dbgCon_.empty()) {
        const float cy = list.y + list.h / 2;
        uiLabelC(list.x + list.w / 2, cy - uiH(), uiH() + 8 * s, "Debug console", theme::dim);
        uiLabelC(list.x + list.w / 2, cy + 2 * s, uiH() + 8 * s, "Evaluate expressions (x + 1) or run gdb commands (bt, info locals) while paused", theme::lineNum);
    }
    // input row
    const Rect in{body.x + 8 * s, body.bottom() - inH + 5 * s, body.w - 16 * s, inH - 10 * s};
    fill(in, dbgConFocus_ ? theme::accent : Color::rgb(0xffffff, 30), in.h / 2);
    fill(in.x + 1, in.y + 1, in.w - 2, in.h - 2, Color::rgb(0x07080a), in.h / 2 - 1);
    const float x0 = in.x + 14 * s;
    const int cp = std::max(10, static_cast<int>(std::round(in.h * 0.5f)));
    Icons::glyph(dl_, font_, x0, in.y + (in.h - static_cast<float>(cp)) / 2, cp, 0xF0142, dbgConFocus_ ? theme::accent.mix(Color::rgb(0xffffff), 0.3f) : theme::dim);
    const float tx = x0 + static_cast<float>(cp) + 8 * s;
    std::size_t start = 0;
    const std::size_t cur = std::min(dbgIn_.cur, dbgIn_.text.size());
    while (start < cur && uiW(std::string_view(dbgIn_.text).substr(start, cur - start)) > in.right() - 14 * s - tx) start = utf8Next(dbgIn_.text, start);
    if (dbgIn_.text.empty()) uiLabel(tx, in.y, in.h, dbg_ ? "Evaluate an expression or run a gdb command" : "Start debugging to use the console", theme::lineNum, in.right() - 14 * s);
    else uiLabel(tx, in.y, in.h, std::string_view(dbgIn_.text).substr(start), theme::text, in.right() - 14 * s);
    if (dbgConFocus_ && caretOn_ && focused_) {
        const float cx = tx + uiW(std::string_view(dbgIn_.text).substr(start, cur - start));
        fill(std::round(cx), in.y + 7 * s, std::max(2.0f, std::round(1.4f * s)), in.h - 14 * s, theme::caret, s);
    }
    dbgConIn_ = in;
    if (maxS > 0) {
        const float th = std::max(24 * s, list.h * list.h / (total + list.h * 0.5f));
        fill(list.right() - 9 * s, list.y + (list.h - th) * std::clamp(sy / maxS, 0.0f, 1.0f), 5 * s, th, theme::scrollbar, 2.5f * s);
    }
}

void App::dbgBuildRows() {
    dbgRows_.clear();
    dbgRows_.push_back({7, 0, 0, nullptr});
    auto addVars = [&](auto&& self, const std::vector<VarP>& vs, int depth) -> void {
        for (const auto& v : vs) {
            dbgRows_.push_back({2, 0, depth, v});
            if (v->expanded) self(self, v->kids, depth + 1);
        }
    };
    dbgRows_.push_back({1, 0, 0, nullptr});
    if (dbgSec_[0]) {
        if (dbgState_ != DbgState::Stopped) dbgRows_.push_back({3, 0, 1, nullptr});
        else addVars(addVars, dbgLocals_, 0);
    }
    dbgRows_.push_back({1, 1, 0, nullptr});
    if (dbgSec_[1]) {
        if (watches_.empty()) dbgRows_.push_back({3, 1, 1, nullptr});
        else if (dbgState_ == DbgState::Stopped) addVars(addVars, dbgWatchVars_, 0);
        else for (std::size_t i = 0; i < watches_.size(); ++i) dbgRows_.push_back({3, 10 + static_cast<int>(i), 1, nullptr});
    }
    dbgRows_.push_back({1, 2, 0, nullptr});
    if (dbgSec_[2]) {
        if (dbgState_ != DbgState::Stopped) dbgRows_.push_back({3, 2, 1, nullptr});
        else {
            if (dbgThreads_.size() > 1) for (std::size_t i = 0; i < dbgThreads_.size(); ++i) dbgRows_.push_back({4, static_cast<int>(i), 0, nullptr});
            for (std::size_t i = 0; i < dbgFrames_.size(); ++i) dbgRows_.push_back({5, static_cast<int>(i), 0, nullptr});
        }
    }
    dbgRows_.push_back({1, 3, 0, nullptr});
    if (dbgSec_[3]) {
        if (bps_.empty()) dbgRows_.push_back({3, 3, 1, nullptr});
        for (std::size_t i = 0; i < bps_.size(); ++i) dbgRows_.push_back({6, static_cast<int>(i), 0, nullptr});
    }
}

std::vector<std::pair<int, char32_t>> App::dbgRowActions(const DRow& r) const {
    switch (r.kind) {
        case 1:
            if (r.idx == 1) return {{10, 0xF0415}};
            if (r.idx == 3) return {{11, 0xF0CE6}, {8, 0xF0295}, {9, 0xF0C62}};
            return {};
        case 2: {
            const bool isWatchRoot = r.var && r.var->path.starts_with("watch/") && r.var->path.find('/', 6) == std::string::npos;
            std::vector<std::pair<int, char32_t>> a = {{1, 0xF0CB6}, {7, 0xF018F}, {3, 0xF04FE}};
            if (isWatchRoot) a.push_back({4, 0xF09E7}); else a.insert(a.begin() + 2, {2, 0xF086C});
            return a;
        }
        case 6: return {{5, 0xF0CB6}, {6, 0xF09E7}};
        default: return {};
    }
}

float App::dbgRowH(const DRow& r) const {
    const float rh = L_.sideRow, s = S();
    switch (r.kind) {
        case 7: return std::round(rh + 30 * s);                        // status / start card
        case 1: return std::round(rh + (r.idx == 0 ? 4 : 12) * s);     // section header with breathing room above
        default: return rh;
    }
}

void App::dbgLayoutRows() {
    dbgY_.clear();
    float y = 0;
    for (const auto& r : dbgRows_) { dbgY_.push_back(y); y += dbgRowH(r); }
    dbgY_.push_back(y + 12 * S());   // total height incl. bottom padding
}

void App::drawDebugBody() {
    const Rect& sd = L_.side;
    const float s = S(), rh = L_.sideRow, hair = std::max(1.0f, std::round(0.8f * s));
    dbgBuildRows();
    dbgLayoutRows();
    dbgTop_ = L_.sideList.y + 4 * s;
    const float viewH = std::max(0.0f, L_.sideList.bottom() - dbgTop_);
    const float contentH = dbgY_.back();
    dbgMax_ = std::max(0.0f, contentH - viewH);
    dbgScroll_ = std::clamp(dbgScroll_, 0.0f, dbgMax_);
    const float sy = std::round(dbgScroll_);
    const float indent = std::round(14 * s);
    static const char* secNames[4] = {"Variables", "Watch", "Call stack", "Breakpoints"};
    const int nStack = static_cast<int>(dbgFrames_.size());
    const int counts[4] = {static_cast<int>(dbgLocals_.size()), static_cast<int>(watches_.size()), nStack, static_cast<int>(bps_.size())};
    for (std::size_t i = 0; i < dbgRows_.size(); ++i) {
        const DRow& r = dbgRows_[i];
        const float h = dbgRowH(r), y = dbgTop_ + dbgY_[i] - sy;
        if (y + h < L_.sideList.y || y > L_.sideList.bottom()) continue;
        const float top = r.kind == 1 && r.idx != 0 ? std::round(12 * s) : 0.0f;   // header rows keep their padding on top
        const float ry = y + top, rowH = h - top;
        const Rect rr{sd.x + 6 * s, ry + 1, sd.w - 12 * s, rowH - 2};
        const bool hot = L_.sideList.contains(mx_, my_) && my_ >= ry && my_ < ry + rowH && mx_ >= sd.x && mx_ < sd.right();
        if (hot && r.kind != 3 && r.kind != 7) fill(rr, r.kind == 1 ? Color::rgb(0xffffff, 12) : theme::sideHover, 6 * s);
        const float bs = std::round(rowH - 8 * s);
        float rightEdge = sd.right() - 14 * s;
        if (hot) {
            float bx = sd.right() - 10 * s - bs;
            for (const auto& [id, glyph] : dbgRowActions(r)) {
                const Rect b{bx, ry + 4 * s, bs, bs};
                if (b.contains(mx_, my_)) fill(b, Color::rgb(0xffffff, 36), 5 * s);
                const int ip = std::max(10, static_cast<int>(std::round(bs * 0.62f)));
                Icons::glyph(dl_, font_, std::round(b.x + (b.w - static_cast<float>(ip)) / 2), std::round(b.y + (b.h - static_cast<float>(ip)) / 2), ip, glyph, Color::rgb(0xffffff));
                bx -= bs + 2 * s;
            }
            rightEdge = bx + bs;
        }
        switch (r.kind) {
            case 7: {   // status / start card
                const Rect card{sd.x + 10 * s, y + 4 * s, sd.w - 20 * s, h - 8 * s};
                if (!dbg_) {
                    const Rect b{card.x, card.y, card.w, std::round(rh + 8 * s)};
                    const bool h2 = b.contains(mx_, my_);
                    fill(b, theme::accent.alpha(h2 ? 235 : 175), b.h / 2);
                    fill(b.x + hair, b.y + hair, b.w - 2 * hair, b.h - 2 * hair, h2 ? Color::rgb(0x15171b) : Color::rgb(0x0a0b0d), b.h / 2 - hair);
                    const int ip = std::max(14, static_cast<int>(std::round(b.h * 0.5f)));
                    Icons::glyph(dl_, font_, b.x + 16 * s, b.y + (b.h - static_cast<float>(ip)) / 2, ip, 0xF040A, theme::accent.mix(Color::rgb(0xffffff), 0.14f));
                    uiLabel(b.x + 16 * s + static_cast<float>(ip) + 10 * s, b.y, b.h, "Start Debugging", theme::text);
                    uiLabelR(b.right() - 16 * s, b.y, b.h, "F5", theme::lineNum);
                    uiLabel(card.x + 6 * s, b.bottom() + 4 * s, uiH() + 6 * s, "Builds the active target and runs it under gdb", theme::lineNum, card.right());
                } else {
                    const bool stopped = dbgState_ == DbgState::Stopped;
                    fill(card, Color::rgb(0xffffff, 12), 9 * s);
                    fill(card.x + hair, card.y + hair, card.w - 2 * hair, card.h - 2 * hair, Color::rgb(0x0a0b0d), 9 * s - hair);
                    const Color dot = stopped ? Color::rgb(0xe5c07b) : Color::rgb(0x73c990);
                    const float d = std::round(9 * s);
                    fill(card.x + 14 * s, card.y + (rh - d) / 2 + 4 * s, d, d, dot, d / 2);
                    const std::string t = dbgState_ == DbgState::Running ? "Running" : dbgState_ == DbgState::Starting ? "Starting gdb" : dbgStopText_;
                    uiLabel(card.x + 14 * s + d + 10 * s, card.y + 4 * s, rh, t, stopped ? theme::text : theme::dim, card.right() - 10 * s);
                    std::string sub = dbgLast_.name;
                    if (stopped && dbgLine_.line > 0) sub += "  -  " + fs::path(dbgLine_.file).filename().string() + ":" + std::to_string(dbgLine_.line);
                    uiLabel(card.x + 14 * s + d + 10 * s, card.y + 4 * s + rh - 4 * s, rh, sub, theme::lineNum, card.right() - 10 * s);
                }
                break;
            }
            case 1: {
                if (r.idx != 0) fill(sd.x + 12 * s, y + 4 * s, sd.w - 24 * s, hair, Color::rgb(0xffffff, 14));
                const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                Icons::chevron(dl_, font_, sd.x + 12 * s, ry + (rowH - static_cast<float>(cs)) / 2, cs, dbgSec_[r.idx], theme::dim);
                std::string title = secNames[r.idx];
                for (auto& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                uiLabel(sd.x + 12 * s + static_cast<float>(cs) + 8 * s, ry, rowH, title, Color::rgb(0xd4d7dd), rightEdge);
                if (!hot && counts[r.idx] > 0) {
                    const std::string cnt = std::to_string(counts[r.idx]);
                    const float bw = std::max(uiW(cnt) + 12 * s, 20 * s), bh = std::round(uiH() + 2 * s);
                    const Rect badge{sd.right() - 14 * s - bw, ry + (rowH - bh) / 2, bw, bh};
                    fill(badge, Color::rgb(0xffffff, 22), bh / 2);
                    uiLabelC(badge.x + badge.w / 2, badge.y, badge.h, cnt, theme::dim);
                }
                break;
            }
            case 3: {
                static const char* hints[4] = {"Not paused", "Add an expression with +", "Not paused", "Click the editor gutter or press F9"};
                const std::string t = r.idx >= 10 ? watches_[static_cast<std::size_t>(r.idx - 10)] : hints[r.idx];
                uiLabel(sd.x + 32 * s, ry, rowH, t, theme::lineNum, sd.right() - 12 * s);
                break;
            }
            case 2: {
                const VarP& v = r.var;
                float x = sd.x + 14 * s + static_cast<float>(r.depth) * indent;
                if (v->numchild > 0) {
                    const int cs = std::max(8, static_cast<int>(std::round(iconSize() * 0.55f)));
                    Icons::chevron(dl_, font_, x + (indent - static_cast<float>(cs)) / 2, ry + (rowH - static_cast<float>(cs)) / 2, cs, v->expanded, theme::dim);
                }
                x += indent;
                const bool isW = v->path.starts_with("watch/") && v->path.find('/', 6) == std::string::npos;
                if (v->changed) fill(rr.x, rr.y, std::round(2.5f * s), rr.h, Color::rgb(0xe5c07b), 1.5f * s);
                x = uiLabel(x, ry, rowH, v->exp, isW ? Color::rgb(0xe5c07b) : Color::rgb(0x8cc4ff), rightEdge - 20 * s);
                x = uiLabel(x, ry, rowH, "  ", theme::dim, rightEdge);
                std::string val = v->value;
                if (val.empty() && v->numchild > 0) val = "{...}";
                const bool err = val.starts_with("<");
                const bool str = !val.empty() && (val.front() == '"' || val.front() == '\'');
                Color vc = err ? Color::rgb(0xf2777a) : v->changed ? Color::rgb(0xe5c07b) : str ? Color::rgb(0xce9178) : (!val.empty() && (std::isdigit(static_cast<unsigned char>(val[0])) || val[0] == '-')) ? Color::rgb(0xb5cea8) : Color::rgb(0xd4d7dd);
                x = uiLabel(x, ry, rowH, val, vc, rightEdge);
                if (!v->type.empty() && !hot && rightEdge - x > 70 * s) uiLabel(x + 12 * s, ry, rowH, v->type, theme::lineNum, rightEdge);
                break;
            }
            case 4: {
                const DbgThread& t = dbgThreads_[static_cast<std::size_t>(r.idx)];
                const bool cur = t.id == dbgThread_;
                if (cur) fill(rr, theme::sideSel, 6 * s);
                const int ip = std::max(10, static_cast<int>(std::round(rowH * 0.55f)));
                Icons::glyph(dl_, font_, sd.x + 14 * s, ry + (rowH - static_cast<float>(ip)) / 2, ip, 0xF0A2E, cur ? Color::rgb(0xe5c07b) : theme::dim);   // chart-timeline-variant: thread
                uiLabel(sd.x + 14 * s + static_cast<float>(ip) + 8 * s, ry, rowH, std::format("Thread {}{}  {}", t.id, t.name.empty() ? "" : " " + t.name, t.func), cur ? theme::text : theme::dim, rightEdge);
                break;
            }
            case 5: {
                const DbgFrame& f = dbgFrames_[static_cast<std::size_t>(r.idx)];
                const bool cur = f.level == dbgSel_;
                if (cur) { fill(rr, theme::accent.alpha(40), 6 * s); fill(rr.x, rr.y + 4 * s, std::round(2.5f * s), rr.h - 8 * s, theme::accent, 1.5f * s); }
                const bool hasSrc = f.line > 0 && !(f.full.empty() && f.file.empty());
                const float x = sd.x + 16 * s;
                float xe = uiLabel(x, ry, rowH, f.func.empty() ? f.addr : f.func, cur ? Color::rgb(0xffffff) : hasSrc ? theme::text : theme::dim, rightEdge - 40 * s);
                if (hasSrc) uiLabel(xe + 10 * s, ry, rowH, fs::path(!f.full.empty() ? f.full : f.file).filename().string() + ":" + std::to_string(f.line), theme::lineNum, rightEdge);
                break;
            }
            case 6: {
                const Breakpoint& b = bps_[static_cast<std::size_t>(r.idx)];
                const float d = std::round(11 * s), cx = sd.x + 16 * s;
                const bool pending = dbg_ && !b.verified && b.id < 0 && dbgState_ != DbgState::Off;
                const Color bc = b.enabled ? Color::rgb(0xe5484d) : Color::rgb(0x6b6f78);
                if (!b.log.empty()) Icons::glyph(dl_, font_, cx - 1, ry + (rowH - d * 1.3f) / 2, static_cast<int>(std::round(d * 1.3f)), 0xF0B8A, bc);
                else if (b.enabled && !pending) fill(cx, ry + (rowH - d) / 2, d, d, bc, d / 2);
                else { fill(cx, ry + (rowH - d) / 2, d, d, bc, d / 2); fill(cx + 2 * s, ry + (rowH - d) / 2 + 2 * s, d - 4 * s, d - 4 * s, theme::sideBg, (d - 4 * s) / 2); }
                float x = uiLabel(cx + d + 10 * s, ry, rowH, dbgBpTitle(b), b.enabled ? theme::text : theme::dim, rightEdge - 20 * s);
                auto chip = [&](const std::string& txt, Color c) {
                    const float cw2 = uiW(txt) + 12 * s, ch = std::round(uiH() + 2 * s);
                    if (x + 8 * s + cw2 > rightEdge) return;
                    const Rect cr{x + 8 * s, ry + (rowH - ch) / 2, cw2, ch};
                    fill(cr, c.alpha(40), ch / 2);
                    uiLabelC(cr.x + cr.w / 2, cr.y, cr.h, txt, c);
                    x = cr.right();
                };
                if (!b.log.empty()) chip("log", Color::rgb(0xe5c07b));
                else if (!b.cond.empty()) chip("if " + b.cond, Color::rgb(0x8cc4ff));
                else if (b.ignore > 0) chip("skip " + std::to_string(b.ignore), Color::rgb(0x8cc4ff));
                if (b.hits > 0) chip(std::to_string(b.hits) + "x", theme::dim);
                break;
            }
            default: break;
        }
    }
    if (contentH > viewH) {
        const float thumbH = std::max(24 * s, viewH * viewH / (contentH + viewH * 0.5f));
        fill(sd.right() - 9 * s, dbgTop_ + (viewH - thumbH) * std::clamp(sy / std::max(1.0f, dbgMax_), 0.0f, 1.0f), 5 * s, thumbH, theme::scrollbar, 2.5f * s);
    }
    fill(sd.x, L_.sideList.y, sd.w, 4 * s, theme::sideBg);
    (void)rh;
}

void App::debugClick(Button b) {
    if (b != Button::Left) return;
    dbgBuildRows();
    dbgLayoutRows();
    const float rh = L_.sideRow, s = S();
    if (!L_.sideList.contains(mx_, my_) || my_ < dbgTop_) return;
    const int row = dbgRowAt(my_ - dbgTop_ + dbgScroll_);
    if (row < 0) return;
    const DRow r = dbgRows_[static_cast<std::size_t>(row)];
    const float h = dbgRowH(r), top = r.kind == 1 && r.idx != 0 ? std::round(12 * s) : 0.0f;
    const float y = dbgTop_ + dbgY_[static_cast<std::size_t>(row)] - dbgScroll_ + top, rowH = h - top, bs = std::round(rowH - 8 * s);
    (void)rh;
    float bx = L_.side.right() - 10 * s - bs;
    for (const auto& [id, glyph] : dbgRowActions(r)) {
        (void)glyph;
        if (Rect{bx, y + 4 * s, bs, bs}.contains(mx_, my_)) { dbgAction(id, r); return; }
        bx -= bs + 2 * s;
    }
    switch (r.kind) {
        case 7: if (!dbg_) dbgContinueOrStart(); break;
        case 1: if (my_ >= y) dbgSec_[r.idx] = !dbgSec_[r.idx]; break;
        case 2: dbgToggleVar(r.var); break;
        case 4: dbgSelectThread(dbgThreads_[static_cast<std::size_t>(r.idx)].id); break;
        case 5: dbgSelectFrame(dbgFrames_[static_cast<std::size_t>(r.idx)].level); break;
        case 6: {
            Breakpoint& bp = bps_[static_cast<std::size_t>(r.idx)];
            if (mx_ < L_.side.x + 16 * s + std::round(11 * s) + 8 * s) { dbgSetEnabled(bp.uid, !bp.enabled); break; }
            if (!bp.file.empty() && bp.line > 0) {
                openFile(bp.file);
                if (!docs_.empty() && doc().path == bp.file) { doc().setCursor({bp.line - 1, 0}); scrollToCursor_ = true; }
            }
            break;
        }
        default: break;
    }
    dirty_ = true;
}

void App::dbgVarExpr(const VarP& v, std::function<void(const std::string&)> done) {
    const bool root = v->path.find('/', v->path.find('/') + 1) == std::string::npos;
    if (root || !dbg_ || v->vo.empty()) { done(v->exp); return; }
    dbg_->send("-var-info-path-expression " + v->vo, [this, done, v](const gdb::Record& r) {
        done(r.ok() ? r.data["path_expr"].str() : v->exp);
        dirty_ = true;
    });
}

void App::dbgAction(int id, const DRow& r) {
    switch (id) {
        case 1: if (r.var && dbgState_ == DbgState::Stopped) { dbgEditVar_ = r.var; dbgPromptFor(PromptKind::DbgSetValue, "New value of " + r.var->exp, r.var->value); } break;
        case 2: if (r.var) dbgVarExpr(r.var, [this](const std::string& e) { dbgAddWatch(e); }); break;
        case 3: if (r.var) dbgVarExpr(r.var, [this](const std::string& e) { Breakpoint nb; nb.watch = e; dbgAddBp(std::move(nb)); setMessage("Data breakpoint on " + e + ": the program stops when it changes", false, 3); }); break;
        case 4: if (r.var) {
            const auto at = r.var->path.find(':');
            const int i = std::atoi(r.var->path.c_str() + 6);
            (void)at;
            if (i >= 0 && i < static_cast<int>(watches_.size())) { watches_.erase(watches_.begin() + i); dbgSave(); if (dbgState_ == DbgState::Stopped) dbgRefreshVars(); }
        } break;
        case 5: if (r.idx >= 0 && r.idx < static_cast<int>(bps_.size())) {
            const Breakpoint& b = bps_[static_cast<std::size_t>(r.idx)];
            dbgPromptUid_ = b.uid;
            dbgPromptFor(PromptKind::DbgBpEdit, "Condition, #N skip count, or log: message {expr}  -  " + dbgBpTitle(b), dbgBpEditText(b));
        } break;
        case 6: if (r.idx >= 0 && r.idx < static_cast<int>(bps_.size())) dbgRemoveBp(bps_[static_cast<std::size_t>(r.idx)].uid); break;
        case 7: if (r.var) { win_.setClipboard(r.var->value); setMessage("Copied value", false, 1.2f); } break;
        case 8: dbgPromptFor(PromptKind::DbgFuncBp, "Break on function (e.g. main, ns::fn)"); break;
        case 9: { auto uids = std::vector<int>(); for (const auto& b : bps_) uids.push_back(b.uid); for (int u : uids) dbgRemoveBp(u); break; }
        case 10: dbgPromptFor(PromptKind::DbgWatch, "Watch expression"); break;
        case 11: {   // break when a C++ exception is thrown
            auto it = std::ranges::find_if(bps_, [](const Breakpoint& b) { return b.exceptions; });
            if (it != bps_.end()) dbgRemoveBp(it->uid);
            else { Breakpoint nb; nb.exceptions = true; dbgAddBp(std::move(nb)); }
            break;
        }
        default: break;
    }
    dirty_ = true;
}

void App::dbgFinishPrompt(PromptKind k, const std::string& t) {
    switch (k) {
        case PromptKind::DbgWatch: dbgAddWatch(t); break;
        case PromptKind::DbgSetValue: if (dbgEditVar_) dbgSetValue(dbgEditVar_, t); break;
        case PromptKind::DbgBpEdit: dbgApplyBpEdit(dbgPromptUid_, t); break;
        case PromptKind::DbgFuncBp: if (!t.empty()) { Breakpoint nb; nb.func = t; dbgAddBp(std::move(nb)); } break;
        default: break;
    }
}

void App::drawDebugBar() {
    dbgBar_.clear();
    if (!dbg_ || L_.island.w < 200 * S()) return;
    const float s = S(), bs = std::round(uiH() + 14 * s), pad = std::round(5 * s), gap = std::round(2 * s);
    struct B { int cmd; char32_t glyph; const char* tip; bool enabled; Color col; };
    const bool st = dbgState_ == DbgState::Stopped, run = dbgState_ == DbgState::Running;
    const B btns[] = {
        {run ? 1 : 0, run ? 0xF03E4u : 0xF040Au, run ? "Pause (F6)" : "Continue (F5)", st || run, Color::rgb(0xffffff)},
        {2, 0xF01B7, "Step Over (F10)", st, Color::rgb(0xffffff)},
        {3, 0xF01B9, "Step Into (F11)", st, Color::rgb(0xffffff)},
        {4, 0xF01B8, "Step Out (Shift+F11)", st, Color::rgb(0xffffff)},
        {5, 0xF0709, "Restart (Ctrl+Shift+F5)", true, Color::rgb(0xffffff)},
        {6, 0xF04DB, "Stop (Shift+F5)", true, Color::rgb(0xf2777a)},
    };
    const float w = pad * 2 + 6 * bs + 5 * gap;
    const Rect bar{std::round(L_.island.x + (L_.island.w - w) / 2), L_.island.y + L_.tabs.h + 8 * s, w, bs + pad * 2};
    glassPanel(bar, bar.h / 2);
    float x = bar.x + pad;
    for (const auto& b : btns) {
        const Rect r{x, bar.y + pad, bs, bs};
        dbgBar_.push_back({r, b.cmd});
        const bool hot = r.contains(mx_, my_) && b.enabled;
        if (hot) fill(r, Color::rgb(0xffffff, 34), bs / 2);
        const int ip = std::max(14, static_cast<int>(std::round(bs * 0.58f)));
        Color c = b.col;
        if (!b.enabled) c = c.alpha(70);
        Icons::glyph(dl_, font_, std::round(r.x + (bs - static_cast<float>(ip)) / 2), std::round(r.y + (bs - static_cast<float>(ip)) / 2), ip, b.glyph, c);
        if (hot) dbgBarTip_ = b.tip, dbgBarTipRect_ = r;
        x += bs + gap;
    }
    if (!dbgBarTip_.empty() && dbgBarTipRect_.contains(mx_, my_)) {
        const float tw = uiW(dbgBarTip_) + 20 * s, th = std::round(uiH() + 10 * s);
        const Rect t{std::clamp(dbgBarTipRect_.x + dbgBarTipRect_.w / 2 - tw / 2, 4 * s, L_.status.w - tw - 4 * s), bar.bottom() + 6 * s, tw, th};
        glassPanel(t, 7 * s);
        uiLabelC(t.x + t.w / 2, t.y, t.h, dbgBarTip_, theme::text);
    } else dbgBarTip_.clear();
}

void App::dbgCommand(int cmd) {
    switch (cmd) {
        case 0: dbgContinueOrStart(); break;
        case 1: dbgPause(); break;
        case 2: dbgStep("-exec-next"); break;
        case 3: dbgStep("-exec-step"); break;
        case 4: dbgStep("-exec-finish"); break;
        case 5: dbgRestart(); break;
        case 6: debugStop(); break;
        default: break;
    }
    dirty_ = true;
}

bool App::dbgBarClick() {
    for (const auto& b : dbgBar_)
        if (b.r.contains(mx_, my_)) { dbgCommand(b.cmd); return true; }
    return false;
}

bool App::debugKey(const KeyEvent& k) {
    const bool ctrl = k.ctrl, shift = k.shift, alt = k.alt;
    if (alt) return false;
    switch (k.key) {
        case Key::F5:
            if (ctrl && shift) { dbgRestart(); return true; }
            if (shift) { debugStop(); return true; }
            if (ctrl) return false;
            dbgContinueOrStart(); return true;
        case Key::F6: if (ctrl || shift) return false; dbgPause(); return true;
        case Key::F9:
            if (ctrl || shift || docs_.empty()) return false;
            dbgToggleBpAt(doc().path, doc().cursor.line + 1); return true;
        case Key::F10:
            if (ctrl && shift) { dbgSetNextStatement(); return true; }
            if (ctrl) { dbgRunToCursor(); return true; }
            if (shift) return false;   // Shift+F10 stays "build and run"
            dbgStep("-exec-next"); return true;
        case Key::F11:
            if (ctrl) return false;
            dbgStep(shift ? "-exec-finish" : "-exec-step"); return true;
        default: return false;
    }
}

void App::openEditorMenu(Pos p) {
    if (docs_.empty()) return;
    Document& d = doc();
    const bool sel = d.hasSelection(), stopped = dbgState_ == DbgState::Stopped;
    const std::string path = d.path;
    const int line = p.line + 1;
    std::vector<std::tuple<std::string, std::string, bool, std::function<void()>>> items;
    items.emplace_back("Toggle Breakpoint", "F9", false, [this, path, line] { dbgToggleBpAt(path, line); });
    items.emplace_back("Add Conditional Breakpoint...", "", false, [this, path, line] {
        if (path.empty()) return;
        Breakpoint* b = dbgBpAt(path, line);
        if (!b) { Breakpoint nb; nb.file = path; nb.line = line; dbgAddBp(std::move(nb)); b = dbgBpAt(path, line); }
        if (b) { dbgPromptUid_ = b->uid; dbgPromptFor(PromptKind::DbgBpEdit, "Condition, #N skip count, or log: message {expr}", dbgBpEditText(*b)); }
    });
    items.emplace_back("Add Logpoint...", "", false, [this, path, line] {
        if (path.empty()) return;
        Breakpoint* b = dbgBpAt(path, line);
        if (!b) { Breakpoint nb; nb.file = path; nb.line = line; dbgAddBp(std::move(nb)); b = dbgBpAt(path, line); }
        if (b) { dbgPromptUid_ = b->uid; dbgPromptFor(PromptKind::DbgBpEdit, "Logpoint message, e.g. x = {x}", "log: "); }
    });
    if (stopped) {
        items.emplace_back("Run to Cursor", "Ctrl+F10", false, [this, p] { if (!docs_.empty()) { doc().setCursor(p); dbgRunToCursor(); } });
        items.emplace_back("Set Next Statement", "Ctrl+Shift+F10", false, [this, p] { if (!docs_.empty()) { doc().setCursor(p); dbgSetNextStatement(); } });
    }
    if (sel && d.selectedText().find('\n') == std::string::npos) {
        const std::string t = d.selectedText();
        items.emplace_back("Add to Watch", "", false, [this, t] { dbgAddWatch(t); sidebar_ = true; sideDebug_ = true; sideGit_ = false; sideSearch_ = false; });
        if (stopped) items.emplace_back("Evaluate in Debug Console", "", false, [this, t] { panelOpen_ = true; panelTab_ = 4; dbgEval(t); });
    }
    if (isMarkdownPath(d.path)) {
        const bool open = previewOf(d) != nullptr;
        items.emplace_back(open ? "Close Markdown Preview" : "Open Markdown Preview", "Ctrl+Shift+V", false, [this] { togglePreview(); });
    }
    items.emplace_back("Cut", "Ctrl+X", false, [this] { if (!docs_.empty()) copy(true); });
    items.emplace_back("Copy", "Ctrl+C", false, [this] { if (!docs_.empty()) copy(false); });
    items.emplace_back("Paste", "Ctrl+V", false, [this] { if (!docs_.empty()) paste(); });
    items.emplace_back("Select All", "Ctrl+A", false, [this] { if (!docs_.empty()) doc().selectAll(); });
    openCustomMenu(mx_, my_, items);
}

void App::drawDebugGutter(const Document& d, int ln, float y, float lineH) {
    const float s = S(), cell = cw();
    const Rect& e = L_.editor;
    for (const auto& b : bps_) {
        if (b.file != d.path || b.line - 1 != ln || !b.func.empty() || !b.watch.empty() || b.exceptions) continue;
        const float dsz = std::round(std::min(lineH - 6 * s, 1.1f * cell));
        const float cx = e.x + std::round(0.5f * cell), cy = y + (lineH - dsz) / 2;
        const bool pending = dbg_ && !b.verified && dbgState_ != DbgState::Off;
        const Color c = b.enabled ? Color::rgb(0xe5484d) : Color::rgb(0x6b6f78);
        if (!b.log.empty()) {   // logpoint: diamond
            const int ip = std::max(10, static_cast<int>(std::round(dsz * 1.25f)));
            Icons::glyph(dl_, font_, cx - 1, y + (lineH - static_cast<float>(ip)) / 2, ip, 0xF0B8A, c);
        } else if (pending || !b.cond.empty()) {
            fill(cx, cy, dsz, dsz, c, dsz / 2);
            fill(cx + 2 * s, cy + 2 * s, dsz - 4 * s, dsz - 4 * s, b.cond.empty() ? theme::editorBg : theme::editorBg.mix(c, 0.35f), (dsz - 4 * s) / 2);
        } else fill(cx, cy, dsz, dsz, c, dsz / 2);
    }
    if (dbgLine_.line > 0 && dbgLine_.file == d.path && dbgLine_.line - 1 == ln) {
        const int ip = std::max(12, static_cast<int>(std::round(lineH * 0.8f)));
        Icons::glyph(dl_, font_, e.x + std::round(0.2f * cell), y + (lineH - static_cast<float>(ip)) / 2 - 1, ip, 0xF0734, Color::rgb(0xe5c07b));
    }
}

}  // namespace ide
