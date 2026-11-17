// SPDX-License-Identifier: GPL-3.0-or-later
// process - implementation of module daemon.base.process.
module;
#include <fcntl.h>
#include <sys/resource.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
module daemon.base.process;
import std;
namespace fs = std::filesystem;

namespace ide {

std::optional<std::string> findExecutable(const std::string& name) {
    if (name.empty()) return std::nullopt;
    auto usable = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) return usable(name) ? std::optional(name) : std::nullopt;
    const char* path = std::getenv("PATH");
    std::string_view rest = path ? path : "/usr/local/bin:/usr/bin:/bin";
    while (!rest.empty()) {
        auto colon = rest.find(':');
        std::string dir(rest.substr(0, colon));
        if (!dir.empty() && usable(fs::path(dir) / name)) return (fs::path(dir) / name).string();
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return std::nullopt;
}

std::expected<void, std::string> Process::start(const ProcessOptions& o) {
    if (o.argv.empty()) return std::unexpected("empty command");
    auto exe = findExecutable(o.argv[0]);
    if (!exe) return std::unexpected("'" + o.argv[0] + "' not found");
    ::signal(SIGPIPE, SIG_IGN);

    int out[2] = {-1, -1}, in[2] = {-1, -1};
    int master = -1, slave = -1;
    if (o.usePty) {
        master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0) return std::unexpected(std::strerror(errno));
        char name[128];
        if (::grantpt(master) != 0 || ::unlockpt(master) != 0 || ::ptsname_r(master, name, sizeof name) != 0 ||
            (slave = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
            const std::string e = std::strerror(errno);
            ::close(master);
            return std::unexpected(e);
        }
        winsize ws{30, 120, 0, 0};
        ::ioctl(master, TIOCSWINSZ, &ws);
    } else {
        if (::pipe2(out, O_CLOEXEC) < 0) return std::unexpected(std::strerror(errno));
        if (o.pipeStdin && ::pipe2(in, O_CLOEXEC) < 0) { ::close(out[0]); ::close(out[1]); return std::unexpected(std::strerror(errno)); }
    }

    std::vector<char*> argv;
    for (const auto& a : o.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // Environment is assembled before fork (no allocation in the child).
    std::vector<std::string> envStore;
    std::vector<char*> envp;
    if (!o.env.empty()) {
        for (char** e = environ; e && *e; ++e) {
            std::string_view v(*e);
            const auto eq = v.find('=');
            const bool overridden = eq != std::string_view::npos &&
                std::ranges::any_of(o.env, [&](const auto& kv) { return v.substr(0, eq) == kv.first; });
            if (!overridden) envStore.emplace_back(v);
        }
        for (const auto& kv : o.env) envStore.push_back(kv.first + "=" + kv.second);
        for (auto& e : envStore) envp.push_back(e.data());
        envp.push_back(nullptr);
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        const std::string e = std::strerror(errno);
        if (o.usePty) { ::close(master); ::close(slave); }
        else {
            ::close(out[0]); ::close(out[1]);
            if (in[0] >= 0) { ::close(in[0]); ::close(in[1]); }
        }
        return std::unexpected(e);
    }
    if (pid == 0 && o.usePty) {  // child on a pseudo-terminal
        ::setsid();
        ::ioctl(slave, TIOCSCTTY, 0);
        ::dup2(slave, 0);
        ::dup2(slave, 1);
        ::dup2(slave, 2);
        if (!o.cwd.empty() && ::chdir(o.cwd.c_str()) != 0) ::_exit(126);
        if (!envp.empty()) ::execve(exe->c_str(), argv.data(), envp.data());
        else ::execv(exe->c_str(), argv.data());
        ::_exit(127);
    }
    if (pid == 0) {  // child
        ::setsid();  // own process group: lets us signal the whole tree
        if (o.niceLevel > 0) ::setpriority(PRIO_PROCESS, 0, o.niceLevel);
        int devnull = ::open("/dev/null", O_RDWR);
        ::dup2(o.pipeStdin ? in[0] : devnull, 0);
        ::dup2(out[1], 1);
        ::dup2(o.mergeStderr ? out[1] : devnull, 2);
        if (!o.cwd.empty() && ::chdir(o.cwd.c_str()) != 0) ::_exit(126);
        if (!envp.empty()) ::execve(exe->c_str(), argv.data(), envp.data());
        else ::execv(exe->c_str(), argv.data());
        ::_exit(127);
    }
    pid_ = pid;
    if (o.usePty) {
        ::close(slave);
        outFd_ = master;
        inFd_ = ::fcntl(master, F_DUPFD_CLOEXEC, 3);   // a second handle for writing keystrokes
        ::fcntl(outFd_, F_SETFL, ::fcntl(outFd_, F_GETFL) | O_NONBLOCK);   // (shared with inFd_)
        exited_ = false;
        return {};
    }
    ::close(out[1]);
    outFd_ = out[0];
    ::fcntl(outFd_, F_SETFL, ::fcntl(outFd_, F_GETFL) | O_NONBLOCK);
    if (o.pipeStdin) {
        ::close(in[0]);
        inFd_ = in[1];
        ::fcntl(inFd_, F_SETFL, ::fcntl(inFd_, F_GETFL) | O_NONBLOCK);
    }
    exited_ = false;
    return {};
}

std::expected<std::string, std::string> Process::openPtyOnly() {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) return std::unexpected(std::strerror(errno));
    char name[128];
    if (::grantpt(master) != 0 || ::unlockpt(master) != 0 || ::ptsname_r(master, name, sizeof name) != 0) {
        const std::string e = std::strerror(errno);
        ::close(master);
        return std::unexpected(e);
    }
    // Keep the slave open until the other program has opened it (otherwise the master would report EOF immediately).
    ptySlave_ = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    winsize ws{30, 120, 0, 0};
    ::ioctl(master, TIOCSWINSZ, &ws);
    outFd_ = master;
    inFd_ = ::fcntl(master, F_DUPFD_CLOEXEC, 3);
    ::fcntl(outFd_, F_SETFL, ::fcntl(outFd_, F_GETFL) | O_NONBLOCK);
    return std::string(name);
}

long Process::read(std::string& out) {
    if (outFd_ < 0) return 0;
    char buf[16384];
    long total = 0;
    while (true) {
        ssize_t n = ::read(outFd_, buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); total += n; if (n < static_cast<ssize_t>(sizeof buf)) return total; continue; }
        if (n == 0) { return total > 0 ? total : 0; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total > 0 ? total : -1;
        return total > 0 ? total : 0;
    }
}

bool Process::write(std::string_view data, int stallMs) {
    std::size_t off = 0;
    while (off < data.size()) {
        if (inFd_ < 0) return false;
        ssize_t n = ::write(inFd_, data.data() + off, data.size() - off);
        if (n > 0) { off += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{inFd_, POLLOUT, 0};
            if (::poll(&p, 1, stallMs) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

std::optional<int> Process::tryWait() {
    if (pid_ <= 0) return std::nullopt;
    if (exited_) return status_;
    int st = 0;
    pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == pid_) {
        exited_ = true;
        status_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        return status_;
    }
    if (r < 0) { exited_ = true; status_ = -1; return status_; }
    return std::nullopt;
}

auto Process::runCapture(const ProcessOptions& opts, int timeoutMs, const std::atomic<bool>* cancel) -> Result {
    Result res;
    Process p;
    if (auto r = p.start(opts); !r) { res.error = r.error(); return res; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        pollfd pf{p.outFd_, POLLIN, 0};
        long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { res.timedOut = true; p.terminate(true); break; }
        if (cancel && cancel->load()) { p.terminate(true); break; }
        if (::poll(&pf, 1, static_cast<int>(std::min<long>(left, 200))) > 0) {
            if (p.read(res.output) == 0) break;  // EOF
        }
        if (p.tryWait() && p.outFd_ < 0) break;
    }
    // Drain, then reap.
    for (int i = 0; i < 200 && !p.tryWait(); ++i) {
        p.read(res.output);
        ::usleep(5000);
    }
    p.read(res.output);
    res.exitCode = p.tryWait().value_or(-1);
    return res;
}

}  // namespace ide
